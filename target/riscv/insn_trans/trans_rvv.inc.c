/*
 * RISC-V translation routines for the RVV Standard Extension.
 *
 * Copyright (c) 2020 T-Head Semiconductor Co., Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "tcg/tcg-op-gvec.h"
#include "tcg/tcg-gvec-desc.h"
#include "internals.h"

#define NVPR    32

#define require(x) if (unlikely(!(x))) { return false; }
#define require_align(val, pos) require(is_aligned(val, pos))

/* Destination vector register group cannot overlap source mask register. */
#define require_vm(vm, rd) do { if (vm == 0) require(rd != 0); } while (0)

#define require_noover(astart, asize, bstart, bsize) \
  require(!is_overlapped(astart, asize, bstart, bsize))
#define require_noover_widen(astart, asize, bstart, bsize) \
  require(!is_overlapped_widen(astart, asize, bstart, bsize))

#define REQUIRE_RVV do {    \
    if (s->mstatus_vs == 0) \
        return false;       \
} while (0)

static inline bool is_aligned(const unsigned val, const unsigned pos)
{
    return pos ? (val & (pos - 1)) == 0 : true;
}

static inline bool is_overlapped(const int astart, int asize,
                                 const int bstart, int bsize)
{
    asize = asize == 0 ? 1 : asize;
    bsize = bsize == 0 ? 1 : bsize;

    const int aend = astart + asize;
    const int bend = bstart + bsize;

    return MAX(aend, bend) - MIN(astart, bstart) < asize + bsize;
}

static inline bool is_overlapped_widen(const int astart, int asize,
                                       const int bstart, int bsize)
{
    asize = asize == 0 ? 1 : asize;
    bsize = bsize == 0 ? 1 : bsize;

    const int aend = astart + asize;
    const int bend = bstart + bsize;

    if (astart < bstart &&
        is_overlapped(astart, asize, bstart, bsize) &&
        !is_overlapped(astart, asize, bstart + bsize, bsize)) {
        return false;
    } else  {
        return MAX(aend, bend) - MIN(astart, bstart) < asize + bsize;
    }
}

static bool trans_vsetvl(DisasContext *s, arg_vsetvl *a)
{
    TCGv_i32 rd, rs1;
    TCGv s1, s2, dst;

    REQUIRE_RVV;
    if (!has_ext(s, RVV)) {
        return false;
    }

    rd = tcg_const_i32(a->rd);
    rs1 = tcg_const_i32(a->rs1);
    s1 = tcg_temp_new();
    s2 = tcg_temp_new();
    dst = tcg_temp_new();

    gen_get_gpr(s1, a->rs1);
    gen_get_gpr(s2, a->rs2);
    gen_helper_vsetvl(dst, cpu_env, rd, rs1, s1, s2);
    gen_set_gpr(a->rd, dst);
    tcg_gen_movi_tl(cpu_pc, s->pc_succ_insn);
    lookup_and_goto_ptr(s);
    s->base.is_jmp = DISAS_NORETURN;

    tcg_temp_free_i32(rd);
    tcg_temp_free_i32(rs1);
    tcg_temp_free(s1);
    tcg_temp_free(s2);
    tcg_temp_free(dst);
    mark_vs_dirty(s);
    return true;
}

static bool trans_vsetvli(DisasContext *s, arg_vsetvli *a)
{
    TCGv_i32 rd, rs1;
    TCGv s1, s2, dst;

    REQUIRE_RVV;
    if (!has_ext(s, RVV)) {
        return false;
    }

    rd = tcg_const_i32(a->rd);
    rs1 = tcg_const_i32(a->rs1);
    s1 = tcg_temp_new();
    s2 = tcg_const_tl(a->zimm);
    dst = tcg_temp_new();

    gen_get_gpr(s1, a->rs1);
    gen_helper_vsetvl(dst, cpu_env, rd, rs1, s1, s2);
    gen_set_gpr(a->rd, dst);
    gen_goto_tb(s, 0, s->pc_succ_insn);
    s->base.is_jmp = DISAS_NORETURN;

    tcg_temp_free_i32(rd);
    tcg_temp_free_i32(rs1);
    tcg_temp_free(s1);
    tcg_temp_free(s2);
    tcg_temp_free(dst);
    mark_vs_dirty(s);
    return true;
}

/* vector register offset from env */
static uint32_t vreg_ofs(DisasContext *s, int reg)
{
    return offsetof(CPURISCVState, vreg) + reg * s->vlen / 8;
}

/* check functions */

/*
 * Vector unit-stride, strided, unit-stride segment, strided segment
 * store check function.
 */
#define VEXT_CHECK_STORE(s, rd, nf) do {         \
    uint32_t emul_r = s->emul < 1 ? 1 : s->emul; \
    require(s->emul >= 0.125 && s->emul <= 8);   \
    require_align(rd, s->emul);                  \
    require((nf * emul_r) <= (NVPR / 4) &&       \
            (rd + nf * emul_r) <= NVPR);         \
} while (0)

/*
 * Vector unit-stride, strided, unit-stride segment, strided segment
 * load check function.
 */
#define VEXT_CHECK_LOAD(s, rd, nf, vm) do { \
    VEXT_CHECK_STORE(s, rd, nf);            \
    require_vm(vm, rd);                     \
} while (0)

/*
 * Vector indexed, indexed segment store check function.
 */
#define VEXT_CHECK_ST_INDEX(s, rd, rs2, nf) do {    \
    uint32_t flmul_r = s->flmul < 1 ? 1 : s->flmul; \
    require(s->emul >= 0.125 && s->emul <= 8);      \
    require_align(rs2, s->emul);                    \
    require_align(rd, s->flmul);                    \
    require((nf * flmul_r) <= (NVPR / 4) &&         \
            (rd + nf * flmul_r) <= NVPR);           \
} while (0)

/*
 * Vector indexed, indexed segment load check function
 */
#define VEXT_CHECK_LD_INDEX(s, rd, rs2, nf, vm) do {          \
    VEXT_CHECK_ST_INDEX(s, rd, rs2, nf);                      \
    if (s->eew > (1 << (s->sew + 3))) {                       \
        if (rd != rs2) {                                      \
            require_noover(rd, s->flmul, rs2, s->emul);       \
        }                                                     \
    } else if (s->eew < (1 << (s->sew + 3))) {                \
        if (s->emul < 1) {                                    \
            require_noover(rd, s->flmul, rs2, s->emul);       \
        } else {                                              \
            require_noover_widen(rd, s->flmul, rs2, s->emul); \
        }                                                     \
    }                                                         \
    if (nf > 1) {                                             \
        require_noover(rd, s->flmul, rs2, s->emul);           \
        require_noover(rd, nf, rs2, 1);                       \
    }                                                         \
    require_vm(vm, rd);                                       \
} while (0)

/*
 * Vector AMO check function.
 */
#define VEXT_CHECK_AMO(s, rd, rs2, wd, vm) do {                   \
    require(has_ext(s, RVA));                                     \
    require((1 << s->sew) >= 4);                                  \
    require((1 << s->sew) <= sizeof(target_ulong));               \
    require_align(rd, s->flmul);                                  \
    require_align(rs2, s->emul);                                  \
    require(s->emul >= 0.125 && s->emul <= 8);                    \
    if (wd) {                                                     \
        require_vm(vm, rd);                                       \
        if (s->eew > (1 << (s->sew + 3))) {                       \
            if (rd != rs2) {                                      \
                require_noover(rd, s->flmul, rs2, s->emul);       \
            }                                                     \
        } else if (s->eew < (1 << (s->sew + 3))) {                \
            if (s->emul < 1) {                                    \
                require_noover(rd, s->flmul, rs2, s->emul);       \
            } else {                                              \
                require_noover_widen(rd, s->flmul, rs2, s->emul); \
            }                                                     \
        }                                                         \
    }                                                             \
} while (0)

/*
 * Check function for vector instruction with format:
 * single-width result and single-width sources (SEW = SEW op SEW)
 *
 * is_vs1: indicates whether insn[19:15] is a vs1 field or not.
 */
#define VEXT_CHECK_SSS(s, rd, rs1, rs2, vm, is_vs1) do { \
    require_vm(vm, rd);                                  \
    if (s->flmul > 1) {                                  \
        require_align(rd, s->flmul);                     \
        require_align(rs2, s->flmul);                    \
        if (is_vs1) {                                    \
            require_align(rs1, s->flmul);                \
        }                                                \
    }                                                    \
} while (0)

/*
 * Check function for maskable vector instruction with format:
 * single-width result and single-width sources (SEW = SEW op SEW)
 *
 * is_vs1: indicates whether insn[19:15] is a vs1 field or not.
 */
#define VEXT_CHECK_MSS(s, rd, rs1, rs2, is_vs1) do { \
    if (rd != rs2) {                                 \
        require_noover(rd, 1, rs2, s->flmul);        \
    }                                                \
    require_align(rs2, s->flmul);                    \
    if (is_vs1) {                                    \
        if (rd != rs1) {                             \
            require_noover(rd, 1, rs1, s->flmul);    \
        }                                            \
        require_align(rs1, s->flmul);                \
    }                                                \
} while (0)

/* Common check function for vector widening instructions */
#define VEXT_WIDE_CHECK_COMMON(s, rd, vm) do { \
    require(s->flmul <= 4);                    \
    require(s->sew < 3);                       \
    require_align(rd, s->flmul * 2);           \
    require_vm(vm, rd);                        \
} while (0)

/* Common check function for vector narrowing instructions */
#define VEXT_NARROW_CHECK_COMMON(s, rd, rs2, vm) do { \
    require(s->flmul <= 4);                           \
    require(s->sew < 3);                              \
    require_align(rs2, s->flmul * 2);                 \
    require_align(rd, s->flmul);                      \
    require_vm(vm, rd);                               \
} while (0)

/*
 * Check function for vector instruction with format:
 * double-width result and single-width sources (2*SEW = SEW op SEW)
 *
 * is_vs1: indicates whether insn[19:15] is a vs1 field or not.
 */
#define VEXT_CHECK_DSS(s, rd, rs1, rs2, vm, is_vs1) do {           \
    VEXT_WIDE_CHECK_COMMON(s, rd, vm);                             \
    require_align(rs2, s->flmul);                                  \
    if (s->flmul < 1) {                                            \
        require_noover(rd, s->flmul * 2, rs2, s->flmul);           \
    } else {                                                       \
        require_noover_widen(rd, s->flmul * 2, rs2, s->flmul);     \
    }                                                              \
    if (is_vs1) {                                                  \
        require_align(rs1, s->flmul);                              \
        if (s->flmul < 1) {                                        \
            require_noover(rd, s->flmul * 2, rs1, s->flmul);       \
        } else {                                                   \
            require_noover_widen(rd, s->flmul * 2, rs1, s->flmul); \
        }                                                          \
    }                                                              \
} while (0)

/*
 * Check function for vector instruction with format:
 * double-width result and double-width source1 and single-width
 * source2 (2*SEW = 2*SEW op SEW)
 *
 * is_vs1: indicates whether insn[19:15] is a vs1 field or not.
 */
#define VEXT_CHECK_DDS(s, rd, rs1, rs2, vm, is_vs1) do {           \
    VEXT_WIDE_CHECK_COMMON(s, rd, vm);                             \
    require_align(rs2, s->flmul * 2);                              \
    if (is_vs1) {                                                  \
        require_align(rs1, s->flmul);                              \
        if (s->flmul < 1) {                                        \
            require_noover(rd, s->flmul * 2, rs1, s->flmul);       \
        } else {                                                   \
            require_noover_widen(rd, s->flmul * 2, rs1, s->flmul); \
        }                                                          \
    }                                                              \
} while (0)

/*
 * Check function for vector instruction with format:
 * single-width result and double-width source1 and single-width
 * source2 (SEW = 2*SEW op SEW)
 *
 * is_vs1: indicates whether insn[19:15] is a vs1 field or not.
 */
#define VEXT_CHECK_SDS(s, rd, rs1, rs2, vm, is_vs1) do { \
    VEXT_NARROW_CHECK_COMMON(s, rd, rs2, vm);            \
    if (rd != rs2) {                                     \
        require_noover(rd, s->flmul, rs2, s->flmul * 2); \
    }                                                    \
    if (is_vs1) {                                        \
        require_align(rs1, s->flmul);                    \
    }                                                    \
} while (0)

/*
 * Check function for vector reduction instructions
 *
 * 2. In widen instructions and some other insturctions, like vslideup.vx,
 *    there is no need to check whether LMUL=1.
 */
static bool vext_check_overlap_mask(DisasContext *s, uint32_t vd, bool vm,
    bool force)
{
    return (vm != 0 || vd != 0) || (!force && (s->lmul == 0));
}

/*
 * Check function for vector integer extension instructions.
 */
#define VEXT_CHECK_EXT(s, rd, rs2, vm, div) do {                 \
    uint32_t from = (1 << (s->sew + 3)) / div;                   \
    require(from >= 8 && from <= 64);                            \
    require(rd != rs2);                                          \
    require_align(rd, s->flmul);                                 \
    require_align(rs2, s->flmul / div);                          \
    if ((s->flmul / div) < 1) {                                  \
        require_noover(rd, s->flmul, rs2, s->flmul / div);       \
    } else {                                                     \
        require_noover_widen(rd, s->flmul, rs2, s->flmul / div); \
    }                                                            \
    require_vm(vm, rd);                                          \
} while (0)

/*
 * In cpu_get_tb_cpu_state(), set VILL if RVV was not present.
 * So RVV is also be checked in this function.
 */
#define VEXT_CHECK_ISA_ILL(s) do { \
    require(!s->vill)              \
} while (0)

/* common translation macro */
#define GEN_VEXT_TRANS(NAME, EEW, SEQ, ARGTYPE, OP, CHECK)   \
static bool trans_##NAME(DisasContext *s, arg_##ARGTYPE * a) \
{                                                            \
    s->eew = EEW;                                            \
    s->emul = (float)EEW / (1 << (s->sew + 3)) * s->flmul;   \
    if (CHECK(s, a)) {                                       \
        return OP(s, a, SEQ);                                \
    }                                                        \
    return false;                                            \
}

/*
 *** unit stride load and store
 */
typedef void gen_helper_ldst_us(TCGv_ptr, TCGv_ptr, TCGv,
                                TCGv_env, TCGv_i32);

static bool ldst_us_trans(uint32_t vd, uint32_t rs1, uint32_t data,
                          gen_helper_ldst_us *fn, DisasContext *s)
{
    TCGv_ptr dest, mask;
    TCGv base;
    TCGv_i32 desc;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    base = tcg_temp_new();

    /*
     * As simd_desc supports at most 256 bytes, and in this implementation,
     * the max vector group length is 2048 bytes. So split it into two parts.
     *
     * The first part is vlen in bytes, encoded in maxsz of simd_desc.
     * The second part is lmul, encoded in data of simd_desc.
     */
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    gen_get_gpr(base, rs1);
    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, base, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free(base);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

static bool ld_us_op(DisasContext *s, arg_r2nfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_us *fn;
    static gen_helper_ldst_us * const fns[2][4] = {
        /* masked unit stride load */
        { gen_helper_vle8_v_mask, gen_helper_vle16_v_mask,
          gen_helper_vle32_v_mask, gen_helper_vle64_v_mask },
        /* unmasked unit stride load */
        { gen_helper_vle8_v, gen_helper_vle16_v,
          gen_helper_vle32_v, gen_helper_vle64_v }
    };
    bool ret;

    fn =  fns[a->vm][seq];
    if (fn == NULL) {
        return false;
    }

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);
    ret = ldst_us_trans(a->rd, a->rs1, data, fn, s);
    mark_vs_dirty(s);
    return ret;
}

static bool ld_us_check(DisasContext *s, arg_r2nfvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_LOAD(s, a->rd, a->nf, a->vm);
    return true;
}

GEN_VEXT_TRANS(vle8_v,  8,  0, r2nfvm, ld_us_op, ld_us_check)
GEN_VEXT_TRANS(vle16_v, 16, 1, r2nfvm, ld_us_op, ld_us_check)
GEN_VEXT_TRANS(vle32_v, 32, 2, r2nfvm, ld_us_op, ld_us_check)
GEN_VEXT_TRANS(vle64_v, 64, 3, r2nfvm, ld_us_op, ld_us_check)

static bool st_us_op(DisasContext *s, arg_r2nfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_us *fn;
    static gen_helper_ldst_us * const fns[2][4] = {
        /* masked unit stride store */
        { gen_helper_vse8_v_mask, gen_helper_vse16_v_mask,
          gen_helper_vse32_v_mask, gen_helper_vse64_v_mask },
        /* unmasked unit stride store */
        { gen_helper_vse8_v, gen_helper_vse16_v,
          gen_helper_vse32_v, gen_helper_vse64_v }
    };

    fn =  fns[a->vm][seq];
    if (fn == NULL) {
        return false;
    }

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);
    return ldst_us_trans(a->rd, a->rs1, data, fn, s);
}

static bool st_us_check(DisasContext *s, arg_r2nfvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_STORE(s, a->rd, a->nf);
    return true;
}

GEN_VEXT_TRANS(vse8_v,  8,  0, r2nfvm, st_us_op, st_us_check)
GEN_VEXT_TRANS(vse16_v, 16, 1, r2nfvm, st_us_op, st_us_check)
GEN_VEXT_TRANS(vse32_v, 32, 2, r2nfvm, st_us_op, st_us_check)
GEN_VEXT_TRANS(vse64_v, 64, 3, r2nfvm, st_us_op, st_us_check)

/*
 *** stride load and store
 */
typedef void gen_helper_ldst_stride(TCGv_ptr, TCGv_ptr, TCGv,
                                    TCGv, TCGv_env, TCGv_i32);

static bool ldst_stride_trans(uint32_t vd, uint32_t rs1, uint32_t rs2,
                              uint32_t data, gen_helper_ldst_stride *fn,
                              DisasContext *s)
{
    TCGv_ptr dest, mask;
    TCGv base, stride;
    TCGv_i32 desc;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    base = tcg_temp_new();
    stride = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    gen_get_gpr(base, rs1);
    gen_get_gpr(stride, rs2);
    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, base, stride, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free(base);
    tcg_temp_free(stride);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

static bool ld_stride_op(DisasContext *s, arg_rnfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_stride *fn;
    static gen_helper_ldst_stride * const fns[4] = {
        gen_helper_vlse8_v, gen_helper_vlse16_v,
        gen_helper_vlse32_v, gen_helper_vlse64_v
    };
    bool ret;

    fn =  fns[seq];

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);
    ret  = ldst_stride_trans(a->rd, a->rs1, a->rs2, data, fn, s);
    mark_vs_dirty(s);
    return ret;
}

static bool ld_stride_check(DisasContext *s, arg_rnfvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_LOAD(s, a->rd, a->nf, a->vm);
    return true;
}

GEN_VEXT_TRANS(vlse8_v,  8,  0, rnfvm, ld_stride_op, ld_stride_check)
GEN_VEXT_TRANS(vlse16_v, 16, 1, rnfvm, ld_stride_op, ld_stride_check)
GEN_VEXT_TRANS(vlse32_v, 32, 2, rnfvm, ld_stride_op, ld_stride_check)
GEN_VEXT_TRANS(vlse64_v, 64, 3, rnfvm, ld_stride_op, ld_stride_check)

static bool st_stride_op(DisasContext *s, arg_rnfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_stride *fn;
    static gen_helper_ldst_stride * const fns[4] = {
        /* masked stride store */
        gen_helper_vsse8_v,  gen_helper_vsse16_v,
        gen_helper_vsse32_v,  gen_helper_vsse64_v
    };

    fn = fns[seq];

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);

    return ldst_stride_trans(a->rd, a->rs1, a->rs2, data, fn, s);
}

static bool st_stride_check(DisasContext *s, arg_rnfvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_STORE(s, a->rd, a->nf);
    return true;
}

GEN_VEXT_TRANS(vsse8_v,  8,  0, rnfvm, st_stride_op, st_stride_check)
GEN_VEXT_TRANS(vsse16_v, 16, 1, rnfvm, st_stride_op, st_stride_check)
GEN_VEXT_TRANS(vsse32_v, 32, 2, rnfvm, st_stride_op, st_stride_check)
GEN_VEXT_TRANS(vsse64_v, 64, 3, rnfvm, st_stride_op, st_stride_check)

/*
 *** index load and store
 */
typedef void gen_helper_ldst_index(TCGv_ptr, TCGv_ptr, TCGv,
                                   TCGv_ptr, TCGv_env, TCGv_i32);

static bool ldst_index_trans(uint32_t vd, uint32_t rs1, uint32_t vs2,
                             uint32_t data, gen_helper_ldst_index *fn,
                             DisasContext *s)
{
    TCGv_ptr dest, mask, index;
    TCGv base;
    TCGv_i32 desc;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    index = tcg_temp_new_ptr();
    base = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    gen_get_gpr(base, rs1);
    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(index, cpu_env, vreg_ofs(s, vs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, base, index, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(index);
    tcg_temp_free(base);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

static bool ld_index_op(DisasContext *s, arg_rnfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_index *fn;
    static gen_helper_ldst_index * const fns[4][4] = {
        /*
         * offset vector register group EEW = 8,
         * data vector register group EEW = SEW
         */
        { gen_helper_vlxei8_8_v,  gen_helper_vlxei8_16_v,
          gen_helper_vlxei8_32_v, gen_helper_vlxei8_64_v },
        /*
         * offset vector register group EEW = 16,
         * data vector register group EEW = SEW
         */
        { gen_helper_vlxei16_8_v, gen_helper_vlxei16_16_v,
          gen_helper_vlxei16_32_v, gen_helper_vlxei16_64_v },
        /*
         * offset vector register group EEW = 32,
         * data vector register group EEW = SEW
         */
        { gen_helper_vlxei32_8_v, gen_helper_vlxei32_16_v,
          gen_helper_vlxei32_32_v, gen_helper_vlxei32_64_v },
        /*
         * offset vector register group EEW = 64,
         * data vector register group EEW = SEW
         */
        { gen_helper_vlxei64_8_v, gen_helper_vlxei64_16_v,
          gen_helper_vlxei64_32_v, gen_helper_vlxei64_64_v }
    };
    bool ret;

    fn = fns[seq][s->sew];

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);
    ret = ldst_index_trans(a->rd, a->rs1, a->rs2, data, fn, s);
    mark_vs_dirty(s);
    return ret;
}

static bool ld_index_check(DisasContext *s, arg_rnfvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_LD_INDEX(s, a->rd, a->rs2, a->nf, a->vm);
    return true;
}

GEN_VEXT_TRANS(vlxei8_v,  8,  0, rnfvm, ld_index_op, ld_index_check)
GEN_VEXT_TRANS(vlxei16_v, 16, 1, rnfvm, ld_index_op, ld_index_check)
GEN_VEXT_TRANS(vlxei32_v, 32, 2, rnfvm, ld_index_op, ld_index_check)
GEN_VEXT_TRANS(vlxei64_v, 64, 3, rnfvm, ld_index_op, ld_index_check)

static bool st_index_op(DisasContext *s, arg_rnfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_index *fn;
    static gen_helper_ldst_index * const fns[4][4] = {
        /*
         * offset vector register group EEW = 8,
         * data vector register group EEW = SEW
         */
        { gen_helper_vsxei8_8_v,  gen_helper_vsxei8_16_v,
          gen_helper_vsxei8_32_v, gen_helper_vsxei8_64_v },
        /*
         * offset vector register group EEW = 16,
         * data vector register group EEW = SEW
         */
        { gen_helper_vsxei16_8_v, gen_helper_vsxei16_16_v,
          gen_helper_vsxei16_32_v, gen_helper_vsxei16_64_v },
        /*
         * offset vector register group EEW = 32,
         * data vector register group EEW = SEW
         */
        { gen_helper_vsxei32_8_v, gen_helper_vsxei32_16_v,
          gen_helper_vsxei32_32_v, gen_helper_vsxei32_64_v },
        /*
         * offset vector register group EEW = 64,
         * data vector register group EEW = SEW
         */
        { gen_helper_vsxei64_8_v, gen_helper_vsxei64_16_v,
          gen_helper_vsxei64_32_v, gen_helper_vsxei64_64_v }
    };

    fn = fns[seq][s->sew];

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);
    return ldst_index_trans(a->rd, a->rs1, a->rs2, data, fn, s);
}

static bool st_index_check(DisasContext *s, arg_rnfvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_ST_INDEX(s, a->rd, a->rs2, a->nf);
    return true;
}

GEN_VEXT_TRANS(vsxei8_v,  8,  0, rnfvm, st_index_op, st_index_check)
GEN_VEXT_TRANS(vsxei16_v, 16, 1, rnfvm, st_index_op, st_index_check)
GEN_VEXT_TRANS(vsxei32_v, 32, 2, rnfvm, st_index_op, st_index_check)
GEN_VEXT_TRANS(vsxei64_v, 64, 3, rnfvm, st_index_op, st_index_check)

/*
 *** unit stride fault-only-first load
 */
static bool ldff_trans(uint32_t vd, uint32_t rs1, uint32_t data,
                       gen_helper_ldst_us *fn, DisasContext *s)
{
    TCGv_ptr dest, mask;
    TCGv base;
    TCGv_i32 desc;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    base = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    gen_get_gpr(base, rs1);
    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, base, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free(base);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

static bool ldff_op(DisasContext *s, arg_r2nfvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_ldst_us *fn;
    static gen_helper_ldst_us * const fns[4] = {
        gen_helper_vle8ff_v, gen_helper_vle16ff_v,
        gen_helper_vle32ff_v, gen_helper_vle64ff_v
    };
    bool ret;

    fn = fns[seq];
    if (fn == NULL) {
        return false;
    }

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, NF, a->nf);
    ret = ldff_trans(a->rd, a->rs1, data, fn, s);
    mark_vs_dirty(s);
    return ret;
}

GEN_VEXT_TRANS(vle8ff_v,  8,  0, r2nfvm, ldff_op, ld_us_check)
GEN_VEXT_TRANS(vle16ff_v, 16, 1, r2nfvm, ldff_op, ld_us_check)
GEN_VEXT_TRANS(vle32ff_v, 32, 2, r2nfvm, ldff_op, ld_us_check)
GEN_VEXT_TRANS(vle64ff_v, 64, 3, r2nfvm, ldff_op, ld_us_check)

/*
 * load and store whole register instructions
 */
typedef void gen_helper_ldst_whole(TCGv_ptr, TCGv, TCGv_env, TCGv_i32);

static bool ldst_whole_trans(uint32_t vd, uint32_t rs1, uint32_t data,
                             gen_helper_ldst_whole *fn, DisasContext *s)
{
    TCGv_ptr dest;
    TCGv base;
    TCGv_i32 desc;

    dest = tcg_temp_new_ptr();
    base = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    gen_get_gpr(base, rs1);
    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));

    fn(dest, base, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free(base);
    tcg_temp_free_i32(desc);
    return true;
}

/*
 * load and store whole register instructions ignore vtype and vl setting.
 * Thus, we don't need to check vill bit. (Section 7.9)
 */
#define GEN_LDST_WHOLE_TRANS(NAME, EEW, SEQ, ARGTYPE, ARG_NF)          \
static bool trans_##NAME(DisasContext *s, arg_##ARGTYPE * a)           \
{                                                                      \
    s->eew = EEW;                                                      \
    s->emul = (float)EEW / (1 << (s->sew + 3)) * s->flmul;             \
                                                                       \
    REQUIRE_RVV;                                                       \
                                                                       \
    uint32_t data = 0;                                                 \
    bool ret;                                                          \
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);                     \
    data = FIELD_DP32(data, VDATA, SEW, s->sew);                       \
    data = FIELD_DP32(data, VDATA, VTA, s->vta);                       \
    data = FIELD_DP32(data, VDATA, VMA, s->vma);                       \
    data = FIELD_DP32(data, VDATA, NF, ARG_NF);                        \
    ret = ldst_whole_trans(a->rd, a->rs1, data, gen_helper_##NAME, s); \
    mark_vs_dirty(s);                                                  \
    return ret;                                                        \
}

GEN_LDST_WHOLE_TRANS(vl1r_v, 8, 0, vl1r_v, 1)

GEN_LDST_WHOLE_TRANS(vs1r_v, 8, 1, vs1r_v, 1)

/*
 *** vector atomic operation
 */
typedef void gen_helper_amo(TCGv_ptr, TCGv_ptr, TCGv, TCGv_ptr,
                            TCGv_env, TCGv_i32);

static bool amo_trans(uint32_t vd, uint32_t rs1, uint32_t vs2,
                      uint32_t data, gen_helper_amo *fn, DisasContext *s)
{
    TCGv_ptr dest, mask, index;
    TCGv base;
    TCGv_i32 desc;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    index = tcg_temp_new_ptr();
    base = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    gen_get_gpr(base, rs1);
    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(index, cpu_env, vreg_ofs(s, vs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, base, index, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(index);
    tcg_temp_free(base);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

static bool amo_op(DisasContext *s, arg_rwdvm *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_amo *fn;
    static gen_helper_amo *const fns[27][2] = {
        /* no atomic operation */
        { gen_helper_vamoswapei8_32_v, gen_helper_vamoswapei8_64_v },
        { gen_helper_vamoswapei16_32_v, gen_helper_vamoswapei16_64_v },
        { gen_helper_vamoswapei32_32_v, gen_helper_vamoswapei32_64_v },
        { gen_helper_vamoaddei8_32_v, gen_helper_vamoaddei8_64_v },
        { gen_helper_vamoaddei16_32_v, gen_helper_vamoaddei16_64_v },
        { gen_helper_vamoaddei32_32_v, gen_helper_vamoaddei32_64_v },
        { gen_helper_vamoxorei8_32_v, gen_helper_vamoxorei8_64_v },
        { gen_helper_vamoxorei16_32_v, gen_helper_vamoxorei16_64_v },
        { gen_helper_vamoxorei32_32_v, gen_helper_vamoxorei32_64_v },
        { gen_helper_vamoandei8_32_v, gen_helper_vamoandei8_64_v },
        { gen_helper_vamoandei16_32_v, gen_helper_vamoandei16_64_v },
        { gen_helper_vamoandei32_32_v, gen_helper_vamoandei32_64_v },
        { gen_helper_vamoorei8_32_v, gen_helper_vamoorei8_64_v },
        { gen_helper_vamoorei16_32_v, gen_helper_vamoorei16_64_v },
        { gen_helper_vamoorei32_32_v, gen_helper_vamoorei32_64_v },
        { gen_helper_vamominei8_32_v, gen_helper_vamominei8_64_v },
        { gen_helper_vamominei16_32_v, gen_helper_vamominei16_64_v },
        { gen_helper_vamominei32_32_v, gen_helper_vamominei32_64_v },
        { gen_helper_vamomaxei8_32_v, gen_helper_vamomaxei8_64_v },
        { gen_helper_vamomaxei16_32_v, gen_helper_vamomaxei16_64_v },
        { gen_helper_vamomaxei32_32_v, gen_helper_vamomaxei32_64_v },
        { gen_helper_vamominuei8_32_v, gen_helper_vamominuei8_64_v },
        { gen_helper_vamominuei16_32_v, gen_helper_vamominuei16_64_v },
        { gen_helper_vamominuei32_32_v, gen_helper_vamominuei32_64_v },
        { gen_helper_vamomaxuei8_32_v, gen_helper_vamomaxuei8_64_v },
        { gen_helper_vamomaxuei16_32_v, gen_helper_vamomaxuei16_64_v },
        { gen_helper_vamomaxuei32_32_v, gen_helper_vamomaxuei32_64_v }
    };

#ifdef TARGET_RISCV64
    static gen_helper_amo *const fns64[9][2] = {
        { gen_helper_vamoswapei64_32_v, gen_helper_vamoswapei64_64_v },
        { gen_helper_vamoaddei64_32_v, gen_helper_vamoaddei64_64_v },
        { gen_helper_vamoxorei64_32_v, gen_helper_vamoxorei64_64_v },
        { gen_helper_vamoandei64_32_v, gen_helper_vamoandei64_64_v },
        { gen_helper_vamoorei64_32_v, gen_helper_vamoorei64_64_v },
        { gen_helper_vamominei64_32_v, gen_helper_vamominei64_64_v },
        { gen_helper_vamomaxei64_32_v, gen_helper_vamomaxei64_64_v },
        { gen_helper_vamominuei64_32_v, gen_helper_vamominuei64_64_v },
        { gen_helper_vamomaxuei64_32_v, gen_helper_vamomaxuei64_64_v }
    };
#endif
    bool ret;

    if (tb_cflags(s->base.tb) & CF_PARALLEL) {
        gen_helper_exit_atomic(cpu_env);
        s->base.is_jmp = DISAS_NORETURN;
        return true;
    } else {
        if (s->eew == 64) {
#ifdef TARGET_RISCV64
            /* EEW == 64. */
            fn = fns64[seq][s->sew - 2];
#else
            /* RV32 does not support EEW = 64 AMO insns. */
            g_assert_not_reached();
#endif
        } else if (s->sew == 3) {
#ifdef TARGET_RISCV64
            /* EEW <= 32 && SEW == 64. */
            fn = fns[seq][s->sew - 2];
#else
            /* Check done in amo_check(). */
            g_assert_not_reached();
#endif
        } else {
            /* EEW <= 32 && SEW == 32. */
            fn = fns[seq][s->sew - 2];
        }
    }

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, SEW, s->sew);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    data = FIELD_DP32(data, VDATA, WD, a->wd);
    ret = amo_trans(a->rd, a->rs1, a->rs2, data, fn, s);
    mark_vs_dirty(s);
    return ret;
}


static bool amo_check(DisasContext *s, arg_rwdvm* a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_AMO(s, a->rd, a->rs2, a->wd, a->vm);
    return true;
}

GEN_VEXT_TRANS(vamoswapei8_v,  8,  0,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoswapei16_v, 16, 1,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoswapei32_v, 32, 2,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoaddei8_v,   8,  3,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoaddei16_v,  16, 4,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoaddei32_v,  32, 5,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoxorei8_v,   8,  6,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoxorei16_v,  16, 7,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoxorei32_v,  32, 8,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoandei8_v,   8,  9,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoandei16_v,  16, 10, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoandei32_v,  32, 11, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoorei8_v,    8,  12, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoorei16_v,   16, 13, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoorei32_v,   32, 14, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominei8_v,   8,  15, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominei16_v,  16, 16, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominei32_v,  32, 17, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxei8_v,   8,  18, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxei16_v,  16, 19, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxei32_v,  32, 20, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominuei8_v,  8,  21, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominuei16_v, 16, 22, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominuei32_v, 32, 23, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxuei8_v,  8,  24, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxuei16_v, 16, 25, rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxuei32_v, 32, 26, rwdvm, amo_op, amo_check)

/*
 * Index EEW cannot be greater than XLEN,
 * else an illegal instruction is raised (Section 8)
 */
#ifdef TARGET_RISCV64
GEN_VEXT_TRANS(vamoswapei64_v, 64, 0,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoaddei64_v,  64, 1,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoxorei64_v,  64, 2,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoandei64_v,  64, 3,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamoorei64_v,   64, 4,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominei64_v,  64, 5,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxei64_v,  64, 6,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamominuei64_v, 64, 7,  rwdvm, amo_op, amo_check)
GEN_VEXT_TRANS(vamomaxuei64_v, 64, 8,  rwdvm, amo_op, amo_check)
#endif

/*
 *** Vector Integer Arithmetic Instructions
 */

/*
 * MAXSZ returns the maximum vector size can be operated in bytes,
 * which is used in GVEC IR when vl_eq_vlmax flag is set to true
 * to accerlate vector operation.
 */
static inline uint32_t MAXSZ(DisasContext *s)
{
    return (s->vlen >> 3) * s->flmul;
}

static bool opivv_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_SSS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    return true;
}

typedef void GVecGen3Fn(unsigned, uint32_t, uint32_t,
                        uint32_t, uint32_t, uint32_t);

static inline bool
do_opivv_gvec(DisasContext *s, arg_rmrr *a, GVecGen3Fn *gvec_fn,
              gen_helper_gvec_4_ptr *fn)
{
    TCGLabel *over = gen_new_label();
    if (!opivv_check(s, a)) {
        return false;
    }

    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    if (a->vm && s->vl_eq_vlmax) {
        gvec_fn(s->sew, vreg_ofs(s, a->rd),
                vreg_ofs(s, a->rs2), vreg_ofs(s, a->rs1),
                MAXSZ(s), MAXSZ(s));
    } else {
        uint32_t data = 0;

        data = FIELD_DP32(data, VDATA, VM, a->vm);
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
        data = FIELD_DP32(data, VDATA, VTA, s->vta);
        data = FIELD_DP32(data, VDATA, VMA, s->vma);
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                           vreg_ofs(s, a->rs1), vreg_ofs(s, a->rs2),
                           cpu_env, 0, s->vlen / 8, data, fn);
    }
    mark_vs_dirty(s);
    gen_set_label(over);
    return true;
}

/* OPIVV with GVEC IR */
#define GEN_OPIVV_GVEC_TRANS(NAME, SUF) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    static gen_helper_gvec_4_ptr * const fns[4] = {                \
        gen_helper_##NAME##_b, gen_helper_##NAME##_h,              \
        gen_helper_##NAME##_w, gen_helper_##NAME##_d,              \
    };                                                             \
    return do_opivv_gvec(s, a, tcg_gen_gvec_##SUF, fns[s->sew]);   \
}

GEN_OPIVV_GVEC_TRANS(vadd_vv, add)
GEN_OPIVV_GVEC_TRANS(vsub_vv, sub)

typedef void gen_helper_opivx(TCGv_ptr, TCGv_ptr, TCGv, TCGv_ptr,
                              TCGv_env, TCGv_i32);

static bool opivx_trans(uint32_t vd, uint32_t rs1, uint32_t vs2, uint32_t vm,
                        gen_helper_opivx *fn, DisasContext *s)
{
    TCGv_ptr dest, src2, mask;
    TCGv src1;
    TCGv_i32 desc;
    uint32_t data = 0;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    src2 = tcg_temp_new_ptr();
    src1 = tcg_temp_new();
    gen_get_gpr(src1, rs1);

    data = FIELD_DP32(data, VDATA, VM, vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(src2, cpu_env, vreg_ofs(s, vs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, src1, src2, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(src2);
    tcg_temp_free(src1);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

static bool opivx_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_SSS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    return true;
}

typedef void GVecGen2sFn(unsigned, uint32_t, uint32_t, TCGv_i64,
                         uint32_t, uint32_t);

static inline bool
do_opivx_gvec(DisasContext *s, arg_rmrr *a, GVecGen2sFn *gvec_fn,
              gen_helper_opivx *fn)
{
    bool ret;
    if (!opivx_check(s, a)) {
        return false;
    }

    if (a->vm && s->vl_eq_vlmax) {
        TCGv_i64 src1 = tcg_temp_new_i64();
        TCGv tmp = tcg_temp_new();

        gen_get_gpr(tmp, a->rs1);
        tcg_gen_ext_tl_i64(src1, tmp);
        gvec_fn(s->sew, vreg_ofs(s, a->rd), vreg_ofs(s, a->rs2),
                src1, MAXSZ(s), MAXSZ(s));

        tcg_temp_free_i64(src1);
        tcg_temp_free(tmp);
        mark_vs_dirty(s);
        return true;
    }
    ret = opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fn, s);
    mark_vs_dirty(s);
    return ret;
}

/* OPIVX with GVEC IR */
#define GEN_OPIVX_GVEC_TRANS(NAME, SUF) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    static gen_helper_opivx * const fns[4] = {                     \
        gen_helper_##NAME##_b, gen_helper_##NAME##_h,              \
        gen_helper_##NAME##_w, gen_helper_##NAME##_d,              \
    };                                                             \
    return do_opivx_gvec(s, a, tcg_gen_gvec_##SUF, fns[s->sew]);   \
}

GEN_OPIVX_GVEC_TRANS(vadd_vx, adds)
GEN_OPIVX_GVEC_TRANS(vsub_vx, subs)

static void gen_vec_rsub8_i64(TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    tcg_gen_vec_sub8_i64(d, b, a);
}

static void gen_vec_rsub16_i64(TCGv_i64 d, TCGv_i64 a, TCGv_i64 b)
{
    tcg_gen_vec_sub16_i64(d, b, a);
}

static void gen_rsub_i32(TCGv_i32 ret, TCGv_i32 arg1, TCGv_i32 arg2)
{
    tcg_gen_sub_i32(ret, arg2, arg1);
}

static void gen_rsub_i64(TCGv_i64 ret, TCGv_i64 arg1, TCGv_i64 arg2)
{
    tcg_gen_sub_i64(ret, arg2, arg1);
}

static void gen_rsub_vec(unsigned vece, TCGv_vec r, TCGv_vec a, TCGv_vec b)
{
    tcg_gen_sub_vec(vece, r, b, a);
}

static void tcg_gen_gvec_rsubs(unsigned vece, uint32_t dofs, uint32_t aofs,
                               TCGv_i64 c, uint32_t oprsz, uint32_t maxsz)
{
    static const TCGOpcode vecop_list[] = { INDEX_op_sub_vec, 0 };
    static const GVecGen2s rsub_op[4] = {
        { .fni8 = gen_vec_rsub8_i64,
          .fniv = gen_rsub_vec,
          .fno = gen_helper_vec_rsubs8,
          .opt_opc = vecop_list,
          .vece = MO_8 },
        { .fni8 = gen_vec_rsub16_i64,
          .fniv = gen_rsub_vec,
          .fno = gen_helper_vec_rsubs16,
          .opt_opc = vecop_list,
          .vece = MO_16 },
        { .fni4 = gen_rsub_i32,
          .fniv = gen_rsub_vec,
          .fno = gen_helper_vec_rsubs32,
          .opt_opc = vecop_list,
          .vece = MO_32 },
        { .fni8 = gen_rsub_i64,
          .fniv = gen_rsub_vec,
          .fno = gen_helper_vec_rsubs64,
          .opt_opc = vecop_list,
          .prefer_i64 = TCG_TARGET_REG_BITS == 64,
          .vece = MO_64 },
    };

    tcg_debug_assert(vece <= MO_64);
    tcg_gen_gvec_2s(dofs, aofs, oprsz, maxsz, c, &rsub_op[vece]);
}

GEN_OPIVX_GVEC_TRANS(vrsub_vx, rsubs)

static bool opivi_trans(uint32_t vd, uint32_t imm, uint32_t vs2, uint32_t vm,
                        gen_helper_opivx *fn, DisasContext *s, int zx)
{
    TCGv_ptr dest, src2, mask;
    TCGv src1;
    TCGv_i32 desc;
    uint32_t data = 0;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    src2 = tcg_temp_new_ptr();
    if (zx) {
        src1 = tcg_const_tl(imm);
    } else {
        src1 = tcg_const_tl(sextract64(imm, 0, 5));
    }
    data = FIELD_DP32(data, VDATA, VM, vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(src2, cpu_env, vreg_ofs(s, vs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, src1, src2, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(src2);
    tcg_temp_free(src1);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

typedef void GVecGen2iFn(unsigned, uint32_t, uint32_t, int64_t,
                         uint32_t, uint32_t);

static inline bool
do_opivi_gvec(DisasContext *s, arg_rmrr *a, GVecGen2iFn *gvec_fn,
              gen_helper_opivx *fn, int zx)
{
    bool ret;
    if (!opivx_check(s, a)) {
        return false;
    }

    if (a->vm && s->vl_eq_vlmax) {
        if (zx) {
            gvec_fn(s->sew, vreg_ofs(s, a->rd), vreg_ofs(s, a->rs2),
                    extract64(a->rs1, 0, 5), MAXSZ(s), MAXSZ(s));
        } else {
            gvec_fn(s->sew, vreg_ofs(s, a->rd), vreg_ofs(s, a->rs2),
                    sextract64(a->rs1, 0, 5), MAXSZ(s), MAXSZ(s));
        }
        ret = true;
    } else {
        ret = opivi_trans(a->rd, a->rs1, a->rs2, a->vm, fn, s, zx);
    }
    mark_vs_dirty(s);
    return ret;
}

/* OPIVI with GVEC IR */
#define GEN_OPIVI_GVEC_TRANS(NAME, ZX, OPIVX, SUF) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    static gen_helper_opivx * const fns[4] = {                     \
        gen_helper_##OPIVX##_b, gen_helper_##OPIVX##_h,            \
        gen_helper_##OPIVX##_w, gen_helper_##OPIVX##_d,            \
    };                                                             \
    return do_opivi_gvec(s, a, tcg_gen_gvec_##SUF,                 \
                         fns[s->sew], ZX);                         \
}

GEN_OPIVI_GVEC_TRANS(vadd_vi, 0, vadd_vx, addi)

static void tcg_gen_gvec_rsubi(unsigned vece, uint32_t dofs, uint32_t aofs,
                               int64_t c, uint32_t oprsz, uint32_t maxsz)
{
    TCGv_i64 tmp = tcg_const_i64(c);
    tcg_gen_gvec_rsubs(vece, dofs, aofs, tmp, oprsz, maxsz);
    tcg_temp_free_i64(tmp);
}

GEN_OPIVI_GVEC_TRANS(vrsub_vi, 0, vrsub_vx, rsubi)

/* Vector Widening Integer Add/Subtract */

/* OPIVV with WIDEN */
static bool opivv_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DSS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    return true;
}

static bool do_opivv_widen(DisasContext *s, arg_rmrr *a,
                           gen_helper_gvec_4_ptr *fn,
                           bool (*checkfn)(DisasContext *, arg_rmrr *))
{
    if (checkfn(s, a)) {
        uint32_t data = 0;
        TCGLabel *over = gen_new_label();
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

        data = FIELD_DP32(data, VDATA, VM, a->vm);
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
        data = FIELD_DP32(data, VDATA, VTA, s->vta);
        data = FIELD_DP32(data, VDATA, VMA, s->vma);
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                           vreg_ofs(s, a->rs1),
                           vreg_ofs(s, a->rs2),
                           cpu_env, 0, s->vlen / 8,
                           data, fn);
        mark_vs_dirty(s);
        gen_set_label(over);
        return true;
    }
    return false;
}

#define GEN_OPIVV_WIDEN_TRANS(NAME, CHECK) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)       \
{                                                            \
    static gen_helper_gvec_4_ptr * const fns[3] = {          \
        gen_helper_##NAME##_b,                               \
        gen_helper_##NAME##_h,                               \
        gen_helper_##NAME##_w                                \
    };                                                       \
    return do_opivv_widen(s, a, fns[s->sew], CHECK);         \
}

GEN_OPIVV_WIDEN_TRANS(vwaddu_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwadd_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwsubu_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwsub_vv, opivv_widen_check)

/* OPIVX with WIDEN */
static bool opivx_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DSS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    return true;
}

static bool do_opivx_widen(DisasContext *s, arg_rmrr *a,
                           gen_helper_opivx *fn)
{
    if (opivx_widen_check(s, a)) {
        return opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fn, s);
    }
    return false;
}

#define GEN_OPIVX_WIDEN_TRANS(NAME) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)       \
{                                                            \
    static gen_helper_opivx * const fns[3] = {               \
        gen_helper_##NAME##_b,                               \
        gen_helper_##NAME##_h,                               \
        gen_helper_##NAME##_w                                \
    };                                                       \
    return do_opivx_widen(s, a, fns[s->sew]);                \
}

GEN_OPIVX_WIDEN_TRANS(vwaddu_vx)
GEN_OPIVX_WIDEN_TRANS(vwadd_vx)
GEN_OPIVX_WIDEN_TRANS(vwsubu_vx)
GEN_OPIVX_WIDEN_TRANS(vwsub_vx)

/* WIDEN OPIVV with WIDEN */
static bool opiwv_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DDS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    return true;
}

static bool do_opiwv_widen(DisasContext *s, arg_rmrr *a,
                           gen_helper_gvec_4_ptr *fn)
{
    if (opiwv_widen_check(s, a)) {
        uint32_t data = 0;
        TCGLabel *over = gen_new_label();
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

        data = FIELD_DP32(data, VDATA, VM, a->vm);
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
        data = FIELD_DP32(data, VDATA, VTA, s->vta);
        data = FIELD_DP32(data, VDATA, VMA, s->vma);
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                           vreg_ofs(s, a->rs1),
                           vreg_ofs(s, a->rs2),
                           cpu_env, 0, s->vlen / 8, data, fn);
        mark_vs_dirty(s);
        gen_set_label(over);
        return true;
    }
    return false;
}

#define GEN_OPIWV_WIDEN_TRANS(NAME) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)       \
{                                                            \
    static gen_helper_gvec_4_ptr * const fns[3] = {          \
        gen_helper_##NAME##_b,                               \
        gen_helper_##NAME##_h,                               \
        gen_helper_##NAME##_w                                \
    };                                                       \
    return do_opiwv_widen(s, a, fns[s->sew]);                \
}

GEN_OPIWV_WIDEN_TRANS(vwaddu_wv)
GEN_OPIWV_WIDEN_TRANS(vwadd_wv)
GEN_OPIWV_WIDEN_TRANS(vwsubu_wv)
GEN_OPIWV_WIDEN_TRANS(vwsub_wv)

/* WIDEN OPIVX with WIDEN */
static bool opiwx_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DDS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    return true;
}

static bool do_opiwx_widen(DisasContext *s, arg_rmrr *a,
                           gen_helper_opivx *fn)
{
    if (opiwx_widen_check(s, a)) {
        return opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fn, s);
    }
    return false;
}

#define GEN_OPIWX_WIDEN_TRANS(NAME) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)       \
{                                                            \
    static gen_helper_opivx * const fns[3] = {               \
        gen_helper_##NAME##_b,                               \
        gen_helper_##NAME##_h,                               \
        gen_helper_##NAME##_w                                \
    };                                                       \
    return do_opiwx_widen(s, a, fns[s->sew]);                \
}

GEN_OPIWX_WIDEN_TRANS(vwaddu_wx)
GEN_OPIWX_WIDEN_TRANS(vwadd_wx)
GEN_OPIWX_WIDEN_TRANS(vwsubu_wx)
GEN_OPIWX_WIDEN_TRANS(vwsub_wx)

/* Vector Integer Add-with-Carry / Subtract-with-Borrow Instructions */
/* OPIVV without GVEC IR */
#define GEN_OPIVV_TRANS(NAME, CHECK)                               \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    if (CHECK(s, a)) {                                             \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_4_ptr * const fns[4] = {            \
            gen_helper_##NAME##_b, gen_helper_##NAME##_h,          \
            gen_helper_##NAME##_w, gen_helper_##NAME##_d,          \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs1),                    \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew]);        \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}

/*
 * For vadc and vsbc, an illegal instruction exception is raised if the
 * destination vector register is v0 and LMUL > 1. (Section 12.3)
 */
static bool opivv_vadc_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require(a->rd != 0);
    VEXT_CHECK_SSS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    return true;
}

GEN_OPIVV_TRANS(vadc_vvm, opivv_vadc_check)
GEN_OPIVV_TRANS(vsbc_vvm, opivv_vadc_check)

/*
 * For vmadc and vmsbc, an illegal instruction exception is raised if the
 * destination vector register overlaps a source vector register group.
 */
static bool opivv_vmadc_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_MSS(s, a->rd, a->rs1, a->rs2, true);
    return true;
}

GEN_OPIVV_TRANS(vmadc_vvm, opivv_vmadc_check)
GEN_OPIVV_TRANS(vmsbc_vvm, opivv_vmadc_check)

static bool opivx_vadc_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require(a->rd != 0);
    VEXT_CHECK_SSS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    return true;
}

/* OPIVX without GVEC IR */
#define GEN_OPIVX_TRANS(NAME, CHECK)                                     \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)                   \
{                                                                        \
    if (CHECK(s, a)) {                                                   \
        static gen_helper_opivx * const fns[4] = {                       \
            gen_helper_##NAME##_b, gen_helper_##NAME##_h,                \
            gen_helper_##NAME##_w, gen_helper_##NAME##_d,                \
        };                                                               \
                                                                         \
        return opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fns[s->sew], s);\
    }                                                                    \
    return false;                                                        \
}

GEN_OPIVX_TRANS(vadc_vxm, opivx_vadc_check)
GEN_OPIVX_TRANS(vsbc_vxm, opivx_vadc_check)

static bool opivx_vmadc_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_MSS(s, a->rd, a->rs1, a->rs2, false);
    return true;
}

GEN_OPIVX_TRANS(vmadc_vxm, opivx_vmadc_check)
GEN_OPIVX_TRANS(vmsbc_vxm, opivx_vmadc_check)

/* OPIVI without GVEC IR */
#define GEN_OPIVI_TRANS(NAME, ZX, OPIVX, CHECK)                          \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)                   \
{                                                                        \
    if (CHECK(s, a)) {                                                   \
        static gen_helper_opivx * const fns[4] = {                       \
            gen_helper_##OPIVX##_b, gen_helper_##OPIVX##_h,              \
            gen_helper_##OPIVX##_w, gen_helper_##OPIVX##_d,              \
        };                                                               \
        return opivi_trans(a->rd, a->rs1, a->rs2, a->vm,                 \
                           fns[s->sew], s, ZX);                          \
    }                                                                    \
    return false;                                                        \
}

GEN_OPIVI_TRANS(vadc_vim, 0, vadc_vxm, opivx_vadc_check)
GEN_OPIVI_TRANS(vmadc_vim, 0, vmadc_vxm, opivx_vmadc_check)

/* Vector Bitwise Logical Instructions */
GEN_OPIVV_GVEC_TRANS(vand_vv, and)
GEN_OPIVV_GVEC_TRANS(vor_vv,  or)
GEN_OPIVV_GVEC_TRANS(vxor_vv, xor)
GEN_OPIVX_GVEC_TRANS(vand_vx, ands)
GEN_OPIVX_GVEC_TRANS(vor_vx,  ors)
GEN_OPIVX_GVEC_TRANS(vxor_vx, xors)
GEN_OPIVI_GVEC_TRANS(vand_vi, 0, vand_vx, andi)
GEN_OPIVI_GVEC_TRANS(vor_vi, 0, vor_vx,  ori)
GEN_OPIVI_GVEC_TRANS(vxor_vi, 0, vxor_vx, xori)

/* Vector Single-Width Bit Shift Instructions */
GEN_OPIVV_GVEC_TRANS(vsll_vv,  shlv)
GEN_OPIVV_GVEC_TRANS(vsrl_vv,  shrv)
GEN_OPIVV_GVEC_TRANS(vsra_vv,  sarv)

typedef void GVecGen2sFn32(unsigned, uint32_t, uint32_t, TCGv_i32,
                           uint32_t, uint32_t);

static inline bool
do_opivx_gvec_shift(DisasContext *s, arg_rmrr *a, GVecGen2sFn32 *gvec_fn,
                    gen_helper_opivx *fn)
{
    if (!opivx_check(s, a)) {
        return false;
    }

    if (a->vm && s->vl_eq_vlmax) {
        TCGv_i32 src1 = tcg_temp_new_i32();
        TCGv tmp = tcg_temp_new();

        gen_get_gpr(tmp, a->rs1);
        tcg_gen_trunc_tl_i32(src1, tmp);
        tcg_gen_extract_i32(src1, src1, 0, s->sew + 3);
        gvec_fn(s->sew, vreg_ofs(s, a->rd), vreg_ofs(s, a->rs2),
                src1, MAXSZ(s), MAXSZ(s));

        tcg_temp_free_i32(src1);
        tcg_temp_free(tmp);
        mark_vs_dirty(s);
        return true;
    }
    return opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fn, s);
}

#define GEN_OPIVX_GVEC_SHIFT_TRANS(NAME, SUF) \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)                    \
{                                                                         \
    static gen_helper_opivx * const fns[4] = {                            \
        gen_helper_##NAME##_b, gen_helper_##NAME##_h,                     \
        gen_helper_##NAME##_w, gen_helper_##NAME##_d,                     \
    };                                                                    \
                                                                          \
    return do_opivx_gvec_shift(s, a, tcg_gen_gvec_##SUF, fns[s->sew]);    \
}

GEN_OPIVX_GVEC_SHIFT_TRANS(vsll_vx,  shls)
GEN_OPIVX_GVEC_SHIFT_TRANS(vsrl_vx,  shrs)
GEN_OPIVX_GVEC_SHIFT_TRANS(vsra_vx,  sars)

GEN_OPIVI_TRANS(vsll_vi, 1, vsll_vx, opivx_check)
GEN_OPIVI_TRANS(vsrl_vi, 1, vsrl_vx, opivx_check)
GEN_OPIVI_TRANS(vsra_vi, 1, vsra_vx, opivx_check)

/* Vector Narrowing Integer Right Shift Instructions */
static bool opivv_narrow_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_SDS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    return true;
}

/* OPIVV with NARROW */
#define GEN_OPIVV_NARROW_TRANS(NAME)                               \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    if (opivv_narrow_check(s, a)) {                                \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_4_ptr * const fns[3] = {            \
            gen_helper_##NAME##_b,                                 \
            gen_helper_##NAME##_h,                                 \
            gen_helper_##NAME##_w,                                 \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs1),                    \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew]);        \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}
GEN_OPIVV_NARROW_TRANS(vnsra_vv)
GEN_OPIVV_NARROW_TRANS(vnsrl_vv)

static bool opivx_narrow_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_SDS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    return true;
}

/* OPIVX with NARROW */
#define GEN_OPIVX_NARROW_TRANS(NAME)                                     \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)                   \
{                                                                        \
    if (opivx_narrow_check(s, a)) {                                      \
        static gen_helper_opivx * const fns[3] = {                       \
            gen_helper_##NAME##_b,                                       \
            gen_helper_##NAME##_h,                                       \
            gen_helper_##NAME##_w,                                       \
        };                                                               \
        return opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fns[s->sew], s);\
    }                                                                    \
    return false;                                                        \
}

GEN_OPIVX_NARROW_TRANS(vnsra_vx)
GEN_OPIVX_NARROW_TRANS(vnsrl_vx)

/* OPIVI with NARROW */
#define GEN_OPIVI_NARROW_TRANS(NAME, ZX, OPIVX)                          \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)                   \
{                                                                        \
    if (opivx_narrow_check(s, a)) {                                      \
        static gen_helper_opivx * const fns[3] = {                       \
            gen_helper_##OPIVX##_b,                                      \
            gen_helper_##OPIVX##_h,                                      \
            gen_helper_##OPIVX##_w,                                      \
        };                                                               \
        return opivi_trans(a->rd, a->rs1, a->rs2, a->vm,                 \
                           fns[s->sew], s, ZX);                          \
    }                                                                    \
    return false;                                                        \
}

GEN_OPIVI_NARROW_TRANS(vnsra_vi, 1, vnsra_vx)
GEN_OPIVI_NARROW_TRANS(vnsrl_vi, 1, vnsrl_vx)

/* Vector Integer Comparison Instructions */
/*
 * For all comparison instructions, an illegal instruction exception is raised
 * if the destination vector register overlaps a source vector register group
 * and LMUL > 1.
 */
static bool opivv_cmp_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_MSS(s, a->rd, a->rs1, a->rs2, true);
    return true;
}

GEN_OPIVV_TRANS(vmseq_vv, opivv_cmp_check)
GEN_OPIVV_TRANS(vmsne_vv, opivv_cmp_check)
GEN_OPIVV_TRANS(vmsltu_vv, opivv_cmp_check)
GEN_OPIVV_TRANS(vmslt_vv, opivv_cmp_check)
GEN_OPIVV_TRANS(vmsleu_vv, opivv_cmp_check)
GEN_OPIVV_TRANS(vmsle_vv, opivv_cmp_check)

static bool opivx_cmp_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_MSS(s, a->rd, a->rs1, a->rs2, false);
    return true;
}

GEN_OPIVX_TRANS(vmseq_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmsne_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmsltu_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmslt_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmsleu_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmsle_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmsgtu_vx, opivx_cmp_check)
GEN_OPIVX_TRANS(vmsgt_vx, opivx_cmp_check)

GEN_OPIVI_TRANS(vmseq_vi, 0, vmseq_vx, opivx_cmp_check)
GEN_OPIVI_TRANS(vmsne_vi, 0, vmsne_vx, opivx_cmp_check)
GEN_OPIVI_TRANS(vmsleu_vi, 1, vmsleu_vx, opivx_cmp_check)
GEN_OPIVI_TRANS(vmsle_vi, 0, vmsle_vx, opivx_cmp_check)
GEN_OPIVI_TRANS(vmsgtu_vi, 1, vmsgtu_vx, opivx_cmp_check)
GEN_OPIVI_TRANS(vmsgt_vi, 0, vmsgt_vx, opivx_cmp_check)

/* Vector Integer Min/Max Instructions */
GEN_OPIVV_GVEC_TRANS(vminu_vv, umin)
GEN_OPIVV_GVEC_TRANS(vmin_vv,  smin)
GEN_OPIVV_GVEC_TRANS(vmaxu_vv, umax)
GEN_OPIVV_GVEC_TRANS(vmax_vv,  smax)
GEN_OPIVX_TRANS(vminu_vx, opivx_check)
GEN_OPIVX_TRANS(vmin_vx,  opivx_check)
GEN_OPIVX_TRANS(vmaxu_vx, opivx_check)
GEN_OPIVX_TRANS(vmax_vx,  opivx_check)

/* Vector Single-Width Integer Multiply Instructions */
GEN_OPIVV_GVEC_TRANS(vmul_vv,  mul)
GEN_OPIVV_TRANS(vmulh_vv, opivv_check)
GEN_OPIVV_TRANS(vmulhu_vv, opivv_check)
GEN_OPIVV_TRANS(vmulhsu_vv, opivv_check)
GEN_OPIVX_GVEC_TRANS(vmul_vx,  muls)
GEN_OPIVX_TRANS(vmulh_vx, opivx_check)
GEN_OPIVX_TRANS(vmulhu_vx, opivx_check)
GEN_OPIVX_TRANS(vmulhsu_vx, opivx_check)

/* Vector Integer Divide Instructions */
GEN_OPIVV_TRANS(vdivu_vv, opivv_check)
GEN_OPIVV_TRANS(vdiv_vv, opivv_check)
GEN_OPIVV_TRANS(vremu_vv, opivv_check)
GEN_OPIVV_TRANS(vrem_vv, opivv_check)
GEN_OPIVX_TRANS(vdivu_vx, opivx_check)
GEN_OPIVX_TRANS(vdiv_vx, opivx_check)
GEN_OPIVX_TRANS(vremu_vx, opivx_check)
GEN_OPIVX_TRANS(vrem_vx, opivx_check)

/* Vector Widening Integer Multiply Instructions */
GEN_OPIVV_WIDEN_TRANS(vwmul_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwmulu_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwmulsu_vv, opivv_widen_check)
GEN_OPIVX_WIDEN_TRANS(vwmul_vx)
GEN_OPIVX_WIDEN_TRANS(vwmulu_vx)
GEN_OPIVX_WIDEN_TRANS(vwmulsu_vx)

/* Vector Single-Width Integer Multiply-Add Instructions */
GEN_OPIVV_TRANS(vmacc_vv, opivv_check)
GEN_OPIVV_TRANS(vnmsac_vv, opivv_check)
GEN_OPIVV_TRANS(vmadd_vv, opivv_check)
GEN_OPIVV_TRANS(vnmsub_vv, opivv_check)
GEN_OPIVX_TRANS(vmacc_vx, opivx_check)
GEN_OPIVX_TRANS(vnmsac_vx, opivx_check)
GEN_OPIVX_TRANS(vmadd_vx, opivx_check)
GEN_OPIVX_TRANS(vnmsub_vx, opivx_check)

/* Vector Widening Integer Multiply-Add Instructions */
GEN_OPIVV_WIDEN_TRANS(vwmaccu_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwmacc_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwmaccsu_vv, opivv_widen_check)
GEN_OPIVX_WIDEN_TRANS(vwmaccu_vx)
GEN_OPIVX_WIDEN_TRANS(vwmacc_vx)
GEN_OPIVX_WIDEN_TRANS(vwmaccsu_vx)
GEN_OPIVX_WIDEN_TRANS(vwmaccus_vx)

/* Vector Integer Merge and Move Instructions */
static bool trans_vmv_v_v(DisasContext *s, arg_vmv_v_v *a)
{
    if (vext_check_isa_ill(s) &&
        vext_check_reg(s, a->rd, false) &&
        vext_check_reg(s, a->rs1, false)) {

        if (s->vl_eq_vlmax) {
            tcg_gen_gvec_mov(s->sew, vreg_ofs(s, a->rd),
                             vreg_ofs(s, a->rs1),
                             MAXSZ(s), MAXSZ(s));
        } else {
            uint32_t data = 0;
            data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
            data = FIELD_DP32(data, VDATA, VTA, s->vta);
            static gen_helper_gvec_2_ptr * const fns[4] = {
                gen_helper_vmv_v_v_b, gen_helper_vmv_v_v_h,
                gen_helper_vmv_v_v_w, gen_helper_vmv_v_v_d,
            };
            TCGLabel *over = gen_new_label();
            tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

            tcg_gen_gvec_2_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, a->rs1),
                               cpu_env, 0, s->vlen / 8, data, fns[s->sew]);
            gen_set_label(over);
        }
        return true;
    }
    return false;
}

typedef void gen_helper_vmv_vx(TCGv_ptr, TCGv_i64, TCGv_env, TCGv_i32);
static bool trans_vmv_v_x(DisasContext *s, arg_vmv_v_x *a)
{
    if (vext_check_isa_ill(s) &&
        vext_check_reg(s, a->rd, false)) {

        TCGv s1;
        TCGLabel *over = gen_new_label();
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

        s1 = tcg_temp_new();
        gen_get_gpr(s1, a->rs1);

        if (s->vl_eq_vlmax) {
            tcg_gen_gvec_dup_tl(s->sew, vreg_ofs(s, a->rd),
                                MAXSZ(s), MAXSZ(s), s1);
        } else {
            TCGv_i32 desc ;
            TCGv_i64 s1_i64 = tcg_temp_new_i64();
            TCGv_ptr dest = tcg_temp_new_ptr();
            uint32_t data = 0;
            data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
            data = FIELD_DP32(data, VDATA, VTA, s->vta);
            static gen_helper_vmv_vx * const fns[4] = {
                gen_helper_vmv_v_x_b, gen_helper_vmv_v_x_h,
                gen_helper_vmv_v_x_w, gen_helper_vmv_v_x_d,
            };

            tcg_gen_ext_tl_i64(s1_i64, s1);
            desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));
            tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, a->rd));
            fns[s->sew](dest, s1_i64, cpu_env, desc);

            tcg_temp_free_ptr(dest);
            tcg_temp_free_i32(desc);
            tcg_temp_free_i64(s1_i64);
        }

        tcg_temp_free(s1);
        gen_set_label(over);
        return true;
    }
    return false;
}

static bool trans_vmv_v_i(DisasContext *s, arg_vmv_v_i *a)
{
    if (vext_check_isa_ill(s) &&
        vext_check_reg(s, a->rd, false)) {

        int64_t simm = sextract64(a->rs1, 0, 5);
        if (s->vl_eq_vlmax) {
            tcg_gen_gvec_dup_imm(s->sew, vreg_ofs(s, a->rd),
                                 MAXSZ(s), MAXSZ(s), simm);
        } else {
            TCGv_i32 desc;
            TCGv_i64 s1;
            TCGv_ptr dest;
            uint32_t data = 0;
            data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
            data = FIELD_DP32(data, VDATA, VTA, s->vta);
            data = FIELD_DP32(data, VDATA, VMA, s->vma);
            static gen_helper_vmv_vx * const fns[4] = {
                gen_helper_vmv_v_x_b, gen_helper_vmv_v_x_h,
                gen_helper_vmv_v_x_w, gen_helper_vmv_v_x_d,
            };
            TCGLabel *over = gen_new_label();
            tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

            s1 = tcg_const_i64(simm);
            dest = tcg_temp_new_ptr();
            desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));
            tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, a->rd));
            fns[s->sew](dest, s1, cpu_env, desc);

            tcg_temp_free_ptr(dest);
            tcg_temp_free_i32(desc);
            tcg_temp_free_i64(s1);
            gen_set_label(over);
        }
        return true;
    }
    return false;
}

GEN_OPIVV_TRANS(vmerge_vvm, opivv_vadc_check)
GEN_OPIVX_TRANS(vmerge_vxm, opivx_vadc_check)
GEN_OPIVI_TRANS(vmerge_vim, 0, vmerge_vxm, opivx_vadc_check)

/*
 *** Vector Fixed-Point Arithmetic Instructions
 */

/* Vector Single-Width Saturating Add and Subtract */
GEN_OPIVV_TRANS(vsaddu_vv, opivv_check)
GEN_OPIVV_TRANS(vsadd_vv,  opivv_check)
GEN_OPIVV_TRANS(vssubu_vv, opivv_check)
GEN_OPIVV_TRANS(vssub_vv,  opivv_check)
GEN_OPIVX_TRANS(vsaddu_vx,  opivx_check)
GEN_OPIVX_TRANS(vsadd_vx,  opivx_check)
GEN_OPIVX_TRANS(vssubu_vx,  opivx_check)
GEN_OPIVX_TRANS(vssub_vx,  opivx_check)
GEN_OPIVI_TRANS(vsaddu_vi, 1, vsaddu_vx, opivx_check)
GEN_OPIVI_TRANS(vsadd_vi, 0, vsadd_vx, opivx_check)

/* Vector Single-Width Averaging Add and Subtract */
GEN_OPIVV_TRANS(vaadd_vv, opivv_check)
GEN_OPIVV_TRANS(vaaddu_vv, opivv_check)
GEN_OPIVV_TRANS(vasub_vv, opivv_check)
GEN_OPIVV_TRANS(vasubu_vv, opivv_check)
GEN_OPIVX_TRANS(vaadd_vx,  opivx_check)
GEN_OPIVX_TRANS(vaaddu_vx,  opivx_check)
GEN_OPIVX_TRANS(vasub_vx,  opivx_check)
GEN_OPIVX_TRANS(vasubu_vx,  opivx_check)

/* Vector Single-Width Fractional Multiply with Rounding and Saturation */
GEN_OPIVV_TRANS(vsmul_vv, opivv_check)
GEN_OPIVX_TRANS(vsmul_vx,  opivx_check)

/* Vector Widening Saturating Scaled Multiply-Add */
GEN_OPIVV_WIDEN_TRANS(vwsmaccu_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwsmacc_vv, opivv_widen_check)
GEN_OPIVV_WIDEN_TRANS(vwsmaccsu_vv, opivv_widen_check)
GEN_OPIVX_WIDEN_TRANS(vwsmaccu_vx)
GEN_OPIVX_WIDEN_TRANS(vwsmacc_vx)
GEN_OPIVX_WIDEN_TRANS(vwsmaccsu_vx)
GEN_OPIVX_WIDEN_TRANS(vwsmaccus_vx)

/* Vector Single-Width Scaling Shift Instructions */
GEN_OPIVV_TRANS(vssrl_vv, opivv_check)
GEN_OPIVV_TRANS(vssra_vv, opivv_check)
GEN_OPIVX_TRANS(vssrl_vx,  opivx_check)
GEN_OPIVX_TRANS(vssra_vx,  opivx_check)
GEN_OPIVI_TRANS(vssrl_vi, 1, vssrl_vx, opivx_check)
GEN_OPIVI_TRANS(vssra_vi, 0, vssra_vx, opivx_check)

/* Vector Narrowing Fixed-Point Clip Instructions */
GEN_OPIVV_NARROW_TRANS(vnclipu_vv)
GEN_OPIVV_NARROW_TRANS(vnclip_vv)
GEN_OPIVX_NARROW_TRANS(vnclipu_vx)
GEN_OPIVX_NARROW_TRANS(vnclip_vx)
GEN_OPIVI_NARROW_TRANS(vnclipu_vi, 1, vnclipu_vx)
GEN_OPIVI_NARROW_TRANS(vnclip_vi, 1, vnclip_vx)

/*
 *** Vector Float Point Arithmetic Instructions
 */
/* Vector Single-Width Floating-Point Add/Subtract Instructions */

/*
 * If the current SEW does not correspond to a supported IEEE floating-point
 * type, an illegal instruction exception is raised.
 */
static bool opfvv_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_SSS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    require(s->sew != 0);
    return true;
}

/* OPFVV without GVEC IR */
#define GEN_OPFVV_TRANS(NAME, CHECK)                               \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    if (CHECK(s, a)) {                                             \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_4_ptr * const fns[3] = {            \
            gen_helper_##NAME##_h,                                 \
            gen_helper_##NAME##_w,                                 \
            gen_helper_##NAME##_d,                                 \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        gen_set_rm(s, 7);                                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs1),                    \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew - 1]);    \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}
GEN_OPFVV_TRANS(vfadd_vv, opfvv_check)
GEN_OPFVV_TRANS(vfsub_vv, opfvv_check)

typedef void gen_helper_opfvf(TCGv_ptr, TCGv_ptr, TCGv_i64, TCGv_ptr,
                              TCGv_env, TCGv_i32);

static bool opfvf_trans(uint32_t vd, uint32_t rs1, uint32_t vs2,
                        uint32_t data, gen_helper_opfvf *fn, DisasContext *s)
{
    TCGv_ptr dest, src2, mask;
    TCGv_i32 desc;

    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    dest = tcg_temp_new_ptr();
    mask = tcg_temp_new_ptr();
    src2 = tcg_temp_new_ptr();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, vd));
    tcg_gen_addi_ptr(src2, cpu_env, vreg_ofs(s, vs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    fn(dest, mask, cpu_fpr[rs1], src2, cpu_env, desc);

    tcg_temp_free_ptr(dest);
    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(src2);
    tcg_temp_free_i32(desc);
    gen_set_label(over);
    return true;
}

/*
 * If the current SEW does not correspond to a supported IEEE floating-point
 * type, an illegal instruction exception is raised
 */
static bool opfvf_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_SSS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    require(s->sew != 0);
    return true;
}

/* OPFVF without GVEC IR */
#define GEN_OPFVF_TRANS(NAME, CHECK)                              \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)            \
{                                                                 \
    if (CHECK(s, a)) {                                            \
        uint32_t data = 0;                                        \
        static gen_helper_opfvf *const fns[3] = {                 \
            gen_helper_##NAME##_h,                                \
            gen_helper_##NAME##_w,                                \
            gen_helper_##NAME##_d,                                \
        };                                                        \
        gen_set_rm(s, 7);                                         \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);            \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);              \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);              \
        return opfvf_trans(a->rd, a->rs1, a->rs2, data,           \
                           fns[s->sew - 1], s);                   \
    }                                                             \
    return false;                                                 \
}

GEN_OPFVF_TRANS(vfadd_vf,  opfvf_check)
GEN_OPFVF_TRANS(vfsub_vf,  opfvf_check)
GEN_OPFVF_TRANS(vfrsub_vf,  opfvf_check)

/* Vector Widening Floating-Point Add/Subtract Instructions */
static bool opfvv_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DSS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    require(s->sew != 0);
    return true;
}

/* OPFVV with WIDEN */
#define GEN_OPFVV_WIDEN_TRANS(NAME, CHECK)                       \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)           \
{                                                                \
    if (CHECK(s, a)) {                                           \
        uint32_t data = 0;                                       \
        static gen_helper_gvec_4_ptr * const fns[2] = {          \
            gen_helper_##NAME##_h, gen_helper_##NAME##_w,        \
        };                                                       \
        TCGLabel *over = gen_new_label();                        \
        gen_set_rm(s, 7);                                        \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);        \
                                                                 \
        data = FIELD_DP32(data, VDATA, VM, a->vm);               \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);           \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);             \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);             \
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),   \
                           vreg_ofs(s, a->rs1),                  \
                           vreg_ofs(s, a->rs2), cpu_env, 0,      \
                           s->vlen / 8, data, fns[s->sew - 1]);  \
        mark_vs_dirty(s);                                      \
        gen_set_label(over);                                     \
        return true;                                             \
    }                                                            \
    return false;                                                \
}

GEN_OPFVV_WIDEN_TRANS(vfwadd_vv, opfvv_widen_check)
GEN_OPFVV_WIDEN_TRANS(vfwsub_vv, opfvv_widen_check)

static bool opfvf_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DSS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    require(s->sew != 0);
    return true;
}

/* OPFVF with WIDEN */
#define GEN_OPFVF_WIDEN_TRANS(NAME)                              \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)           \
{                                                                \
    if (opfvf_widen_check(s, a)) {                               \
        uint32_t data = 0;                                       \
        static gen_helper_opfvf *const fns[2] = {                \
            gen_helper_##NAME##_h, gen_helper_##NAME##_w,        \
        };                                                       \
        gen_set_rm(s, 7);                                        \
        data = FIELD_DP32(data, VDATA, VM, a->vm);               \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);           \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);             \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);             \
        return opfvf_trans(a->rd, a->rs1, a->rs2, data,          \
                           fns[s->sew - 1], s);                  \
    }                                                            \
    return false;                                                \
}

GEN_OPFVF_WIDEN_TRANS(vfwadd_vf)
GEN_OPFVF_WIDEN_TRANS(vfwsub_vf)

static bool opfwv_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DDS(s, a->rd, a->rs1, a->rs2, a->vm, true);
    require(s->sew != 0);
    return true;
}

/* WIDEN OPFVV with WIDEN */
#define GEN_OPFWV_WIDEN_TRANS(NAME)                                \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)             \
{                                                                  \
    if (opfwv_widen_check(s, a)) {                                 \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_4_ptr * const fns[2] = {            \
            gen_helper_##NAME##_h, gen_helper_##NAME##_w,          \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        gen_set_rm(s, 7);                                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs1),                    \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew - 1]);    \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}

GEN_OPFWV_WIDEN_TRANS(vfwadd_wv)
GEN_OPFWV_WIDEN_TRANS(vfwsub_wv)

static bool opfwf_widen_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_DDS(s, a->rd, a->rs1, a->rs2, a->vm, false);
    require(s->sew != 0);
    return true;
}

/* WIDEN OPFVF with WIDEN */
#define GEN_OPFWF_WIDEN_TRANS(NAME)                              \
static bool trans_##NAME(DisasContext *s, arg_rmrr *a)           \
{                                                                \
    if (opfwf_widen_check(s, a)) {                               \
        uint32_t data = 0;                                       \
        static gen_helper_opfvf *const fns[2] = {                \
            gen_helper_##NAME##_h, gen_helper_##NAME##_w,        \
        };                                                       \
        gen_set_rm(s, 7);                                        \
        data = FIELD_DP32(data, VDATA, VM, a->vm);               \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);           \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);             \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);             \
        return opfvf_trans(a->rd, a->rs1, a->rs2, data,          \
                           fns[s->sew - 1], s);                  \
    }                                                            \
    return false;                                                \
}

GEN_OPFWF_WIDEN_TRANS(vfwadd_wf)
GEN_OPFWF_WIDEN_TRANS(vfwsub_wf)

/* Vector Single-Width Floating-Point Multiply/Divide Instructions */
GEN_OPFVV_TRANS(vfmul_vv, opfvv_check)
GEN_OPFVV_TRANS(vfdiv_vv, opfvv_check)
GEN_OPFVF_TRANS(vfmul_vf,  opfvf_check)
GEN_OPFVF_TRANS(vfdiv_vf,  opfvf_check)
GEN_OPFVF_TRANS(vfrdiv_vf,  opfvf_check)

/* Vector Widening Floating-Point Multiply */
GEN_OPFVV_WIDEN_TRANS(vfwmul_vv, opfvv_widen_check)
GEN_OPFVF_WIDEN_TRANS(vfwmul_vf)

/* Vector Single-Width Floating-Point Fused Multiply-Add Instructions */
GEN_OPFVV_TRANS(vfmacc_vv, opfvv_check)
GEN_OPFVV_TRANS(vfnmacc_vv, opfvv_check)
GEN_OPFVV_TRANS(vfmsac_vv, opfvv_check)
GEN_OPFVV_TRANS(vfnmsac_vv, opfvv_check)
GEN_OPFVV_TRANS(vfmadd_vv, opfvv_check)
GEN_OPFVV_TRANS(vfnmadd_vv, opfvv_check)
GEN_OPFVV_TRANS(vfmsub_vv, opfvv_check)
GEN_OPFVV_TRANS(vfnmsub_vv, opfvv_check)
GEN_OPFVF_TRANS(vfmacc_vf, opfvf_check)
GEN_OPFVF_TRANS(vfnmacc_vf, opfvf_check)
GEN_OPFVF_TRANS(vfmsac_vf, opfvf_check)
GEN_OPFVF_TRANS(vfnmsac_vf, opfvf_check)
GEN_OPFVF_TRANS(vfmadd_vf, opfvf_check)
GEN_OPFVF_TRANS(vfnmadd_vf, opfvf_check)
GEN_OPFVF_TRANS(vfmsub_vf, opfvf_check)
GEN_OPFVF_TRANS(vfnmsub_vf, opfvf_check)

/* Vector Widening Floating-Point Fused Multiply-Add Instructions */
GEN_OPFVV_WIDEN_TRANS(vfwmacc_vv, opfvv_widen_check)
GEN_OPFVV_WIDEN_TRANS(vfwnmacc_vv, opfvv_widen_check)
GEN_OPFVV_WIDEN_TRANS(vfwmsac_vv, opfvv_widen_check)
GEN_OPFVV_WIDEN_TRANS(vfwnmsac_vv, opfvv_widen_check)
GEN_OPFVF_WIDEN_TRANS(vfwmacc_vf)
GEN_OPFVF_WIDEN_TRANS(vfwnmacc_vf)
GEN_OPFVF_WIDEN_TRANS(vfwmsac_vf)
GEN_OPFVF_WIDEN_TRANS(vfwnmsac_vf)

/* Vector Floating-Point Square-Root Instruction */

/*
 * If the current SEW does not correspond to a supported IEEE floating-point
 * type, an illegal instruction exception is raised
 */
static bool opfv_check(DisasContext *s, arg_rmr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    /* OPFV instructions ignore vs1 check */
    VEXT_CHECK_SSS(s, a->rd, 0, a->rs2, a->vm, false);
    require(s->sew != 0);
    return true;
}

#define GEN_OPFV_TRANS(NAME, CHECK)                                \
static bool trans_##NAME(DisasContext *s, arg_rmr *a)              \
{                                                                  \
    if (CHECK(s, a)) {                                             \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_3_ptr * const fns[3] = {            \
            gen_helper_##NAME##_h,                                 \
            gen_helper_##NAME##_w,                                 \
            gen_helper_##NAME##_d,                                 \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        gen_set_rm(s, 7);                                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_3_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew - 1]);    \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}

GEN_OPFV_TRANS(vfsqrt_v, opfv_check)

/* Vector Floating-Point MIN/MAX Instructions */
GEN_OPFVV_TRANS(vfmin_vv, opfvv_check)
GEN_OPFVV_TRANS(vfmax_vv, opfvv_check)
GEN_OPFVF_TRANS(vfmin_vf, opfvf_check)
GEN_OPFVF_TRANS(vfmax_vf, opfvf_check)

/* Vector Floating-Point Sign-Injection Instructions */
GEN_OPFVV_TRANS(vfsgnj_vv, opfvv_check)
GEN_OPFVV_TRANS(vfsgnjn_vv, opfvv_check)
GEN_OPFVV_TRANS(vfsgnjx_vv, opfvv_check)
GEN_OPFVF_TRANS(vfsgnj_vf, opfvf_check)
GEN_OPFVF_TRANS(vfsgnjn_vf, opfvf_check)
GEN_OPFVF_TRANS(vfsgnjx_vf, opfvf_check)

/* Vector Floating-Point Compare Instructions */
static bool opfvv_cmp_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_MSS(s, a->rd, a->rs1, a->rs2, true);
    require(s->sew != 0);
    return true;
}

GEN_OPFVV_TRANS(vmfeq_vv, opfvv_cmp_check)
GEN_OPFVV_TRANS(vmfne_vv, opfvv_cmp_check)
GEN_OPFVV_TRANS(vmflt_vv, opfvv_cmp_check)
GEN_OPFVV_TRANS(vmfle_vv, opfvv_cmp_check)
GEN_OPFVV_TRANS(vmford_vv, opfvv_cmp_check)

static bool opfvf_cmp_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    VEXT_CHECK_MSS(s, a->rd, a->rs1, a->rs2, false);
    require(s->sew != 0);
    return true;
}

GEN_OPFVF_TRANS(vmfeq_vf, opfvf_cmp_check)
GEN_OPFVF_TRANS(vmfne_vf, opfvf_cmp_check)
GEN_OPFVF_TRANS(vmflt_vf, opfvf_cmp_check)
GEN_OPFVF_TRANS(vmfle_vf, opfvf_cmp_check)
GEN_OPFVF_TRANS(vmfgt_vf, opfvf_cmp_check)
GEN_OPFVF_TRANS(vmfge_vf, opfvf_cmp_check)
GEN_OPFVF_TRANS(vmford_vf, opfvf_cmp_check)

/* Vector Floating-Point Classify Instruction */
GEN_OPFV_TRANS(vfclass_v, opfv_check)

/* Vector Floating-Point Merge Instruction */
GEN_OPFVF_TRANS(vfmerge_vfm,  opfvf_check)

static bool trans_vfmv_v_f(DisasContext *s, arg_vfmv_v_f *a)
{
    if (vext_check_isa_ill(s) &&
        vext_check_reg(s, a->rd, false) &&
        (s->sew != 0)) {

        if (s->vl_eq_vlmax) {
            tcg_gen_gvec_dup_i64(s->sew, vreg_ofs(s, a->rd),
                                 MAXSZ(s), MAXSZ(s), cpu_fpr[a->rs1]);
        } else {
            TCGv_ptr dest;
            TCGv_i32 desc;
            uint32_t data = FIELD_DP32(0, VDATA, LMUL, s->lmul);
            static gen_helper_vmv_vx * const fns[3] = {
                gen_helper_vmv_v_x_h,
                gen_helper_vmv_v_x_w,
                gen_helper_vmv_v_x_d,
            };
            TCGLabel *over = gen_new_label();
            tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

            dest = tcg_temp_new_ptr();
            desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));
            tcg_gen_addi_ptr(dest, cpu_env, vreg_ofs(s, a->rd));
            fns[s->sew - 1](dest, cpu_fpr[a->rs1], cpu_env, desc);

            tcg_temp_free_ptr(dest);
            tcg_temp_free_i32(desc);
            gen_set_label(over);
        }
        return true;
    }
    return false;
}

/* Single-Width Floating-Point/Integer Type-Convert Instructions */
GEN_OPFV_TRANS(vfcvt_xu_f_v, opfv_check)
GEN_OPFV_TRANS(vfcvt_x_f_v, opfv_check)
GEN_OPFV_TRANS(vfcvt_f_xu_v, opfv_check)
GEN_OPFV_TRANS(vfcvt_f_x_v, opfv_check)

/* Widening Floating-Point/Integer Type-Convert Instructions */

/*
 * If the current SEW does not correspond to a supported IEEE floating-point
 * type, an illegal instruction exception is raised
 */
static bool opfv_widen_check(DisasContext *s, arg_rmr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    /* OPFV widening instructions ignore vs1 check */
    VEXT_CHECK_DSS(s, a->rd, 0, a->rs2, a->vm, false);
    require(s->sew != 0);
    return true;
}

#define GEN_OPFV_WIDEN_TRANS(NAME)                                 \
static bool trans_##NAME(DisasContext *s, arg_rmr *a)              \
{                                                                  \
    if (opfv_widen_check(s, a)) {                                  \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_3_ptr * const fns[2] = {            \
            gen_helper_##NAME##_h,                                 \
            gen_helper_##NAME##_w,                                 \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        gen_set_rm(s, 7);                                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_3_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew - 1]);    \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}

GEN_OPFV_WIDEN_TRANS(vfwcvt_xu_f_v)
GEN_OPFV_WIDEN_TRANS(vfwcvt_x_f_v)
GEN_OPFV_WIDEN_TRANS(vfwcvt_f_xu_v)
GEN_OPFV_WIDEN_TRANS(vfwcvt_f_x_v)
GEN_OPFV_WIDEN_TRANS(vfwcvt_f_f_v)

/* Narrowing Floating-Point/Integer Type-Convert Instructions */

/*
 * If the current SEW does not correspond to a supported IEEE floating-point
 * type, an illegal instruction exception is raised
 */
static bool opfv_narrow_check(DisasContext *s, arg_rmr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    /* OPFV narrowing instructions ignore vs1 check */
    VEXT_CHECK_SDS(s, a->rd, 0, a->rs2, a->vm, false);
    require(s->sew != 0);
    return true;
}

#define GEN_OPFV_NARROW_TRANS(NAME)                                \
static bool trans_##NAME(DisasContext *s, arg_rmr *a)              \
{                                                                  \
    if (opfv_narrow_check(s, a)) {                                 \
        uint32_t data = 0;                                         \
        static gen_helper_gvec_3_ptr * const fns[2] = {            \
            gen_helper_##NAME##_h,                                 \
            gen_helper_##NAME##_w,                                 \
        };                                                         \
        TCGLabel *over = gen_new_label();                          \
        gen_set_rm(s, 7);                                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, VM, a->vm);                 \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VTA, s->vta);               \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_3_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fns[s->sew - 1]);    \
        mark_vs_dirty(s);                                        \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}

GEN_OPFV_NARROW_TRANS(vfncvt_xu_f_v)
GEN_OPFV_NARROW_TRANS(vfncvt_x_f_v)
GEN_OPFV_NARROW_TRANS(vfncvt_f_xu_v)
GEN_OPFV_NARROW_TRANS(vfncvt_f_x_v)
GEN_OPFV_NARROW_TRANS(vfncvt_f_f_v)

/*
 *** Vector Reduction Operations
 */
/* Vector Single-Width Integer Reduction Instructions */
static bool reduction_check(DisasContext *s, arg_rmrr *a)
{
    return vext_check_isa_ill(s) && vext_check_reg(s, a->rs2, false);
}

GEN_OPIVV_TRANS(vredsum_vs, reduction_check)
GEN_OPIVV_TRANS(vredmaxu_vs, reduction_check)
GEN_OPIVV_TRANS(vredmax_vs, reduction_check)
GEN_OPIVV_TRANS(vredminu_vs, reduction_check)
GEN_OPIVV_TRANS(vredmin_vs, reduction_check)
GEN_OPIVV_TRANS(vredand_vs, reduction_check)
GEN_OPIVV_TRANS(vredor_vs, reduction_check)
GEN_OPIVV_TRANS(vredxor_vs, reduction_check)

/* Vector Widening Integer Reduction Instructions */
GEN_OPIVV_WIDEN_TRANS(vwredsum_vs, reduction_check)
GEN_OPIVV_WIDEN_TRANS(vwredsumu_vs, reduction_check)

/* Vector Single-Width Floating-Point Reduction Instructions */
GEN_OPFVV_TRANS(vfredsum_vs, reduction_check)
GEN_OPFVV_TRANS(vfredmax_vs, reduction_check)
GEN_OPFVV_TRANS(vfredmin_vs, reduction_check)

/* Vector Widening Floating-Point Reduction Instructions */
GEN_OPFVV_WIDEN_TRANS(vfwredsum_vs, reduction_check)

/*
 *** Vector Mask Operations
 */

/* Vector Mask-Register Logical Instructions */
#define GEN_MM_TRANS(NAME)                                         \
static bool trans_##NAME(DisasContext *s, arg_r *a)                \
{                                                                  \
    if (vext_check_isa_ill(s)) {                                   \
        uint32_t data = 0;                                         \
        gen_helper_gvec_4_ptr *fn = gen_helper_##NAME;             \
        TCGLabel *over = gen_new_label();                          \
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);          \
                                                                   \
        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);             \
        data = FIELD_DP32(data, VDATA, VMA, s->vma);               \
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),     \
                           vreg_ofs(s, a->rs1),                    \
                           vreg_ofs(s, a->rs2), cpu_env, 0,        \
                           s->vlen / 8, data, fn);                 \
        gen_set_label(over);                                       \
        return true;                                               \
    }                                                              \
    return false;                                                  \
}

GEN_MM_TRANS(vmand_mm)
GEN_MM_TRANS(vmnand_mm)
GEN_MM_TRANS(vmandnot_mm)
GEN_MM_TRANS(vmxor_mm)
GEN_MM_TRANS(vmor_mm)
GEN_MM_TRANS(vmnor_mm)
GEN_MM_TRANS(vmornot_mm)
GEN_MM_TRANS(vmxnor_mm)

/* Vector mask population count vpopc */
static bool trans_vpopc_m(DisasContext *s, arg_rmr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);

    TCGv_ptr src2, mask;
    TCGv dst;
    TCGv_i32 desc;
    uint32_t data = 0;
    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);

    mask = tcg_temp_new_ptr();
    src2 = tcg_temp_new_ptr();
    dst = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    tcg_gen_addi_ptr(src2, cpu_env, vreg_ofs(s, a->rs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    gen_helper_vpopc_m(dst, mask, src2, cpu_env, desc);
    gen_set_gpr(a->rd, dst);

    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(src2);
    tcg_temp_free(dst);
    tcg_temp_free_i32(desc);

    return true;
}

/* vmfirst find-first-set mask bit */
static bool trans_vfirst_m(DisasContext *s, arg_rmr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);

    TCGv_ptr src2, mask;
    TCGv dst;
    TCGv_i32 desc;
    uint32_t data = 0;
    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);

    mask = tcg_temp_new_ptr();
    src2 = tcg_temp_new_ptr();
    dst = tcg_temp_new();
    desc = tcg_const_i32(simd_desc(0, s->vlen / 8, data));

    tcg_gen_addi_ptr(src2, cpu_env, vreg_ofs(s, a->rs2));
    tcg_gen_addi_ptr(mask, cpu_env, vreg_ofs(s, 0));

    gen_helper_vfirst_m(dst, mask, src2, cpu_env, desc);
    gen_set_gpr(a->rd, dst);

    tcg_temp_free_ptr(mask);
    tcg_temp_free_ptr(src2);
    tcg_temp_free(dst);
    tcg_temp_free_i32(desc);

    return true;
}

/* vmsbf.m set-before-first mask bit */
/* vmsif.m set-includ-first mask bit */
/* vmsof.m set-only-first mask bit */
#define GEN_M_TRANS(NAME)                                          \
static bool trans_##NAME(DisasContext *s, arg_rmr *a)              \
{                                                                  \
    REQUIRE_RVV;                                                   \
    VEXT_CHECK_ISA_ILL(s);                                         \
    require_vm(a->vm, a->rd);                                      \
    require(a->rd != a->rs2);                                      \
                                                                   \
    uint32_t data = 0;                                             \
    gen_helper_gvec_3_ptr *fn = gen_helper_##NAME;                 \
    TCGLabel *over = gen_new_label();                              \
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);              \
                                                                   \
    data = FIELD_DP32(data, VDATA, VM, a->vm);                     \
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);                 \
    data = FIELD_DP32(data, VDATA, VMA, s->vma);                   \
    tcg_gen_gvec_3_ptr(vreg_ofs(s, a->rd),                         \
                       vreg_ofs(s, 0), vreg_ofs(s, a->rs2),        \
                       cpu_env, 0, s->vlen / 8, data, fn);         \
    gen_set_label(over);                                           \
    return true;                                                   \
}

GEN_M_TRANS(vmsbf_m)
GEN_M_TRANS(vmsif_m)
GEN_M_TRANS(vmsof_m)

/* Vector Iota Instruction */
static bool trans_viota_m(DisasContext *s, arg_viota_m *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require_noover(a->rd, s->flmul, a->rs2, 1);
    require_vm(a->vm, a->rd);
    require_align(a->rd, s->flmul);

    uint32_t data = 0;
    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    static gen_helper_gvec_3_ptr * const fns[4] = {
        gen_helper_viota_m_b, gen_helper_viota_m_h,
        gen_helper_viota_m_w, gen_helper_viota_m_d,
    };
    tcg_gen_gvec_3_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                       vreg_ofs(s, a->rs2), cpu_env, 0,
                       s->vlen / 8, data, fns[s->sew]);
    gen_set_label(over);
    return true;
}

/* Vector Element Index Instruction */
static bool trans_vid_v(DisasContext *s, arg_vid_v *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require_align(a->rd, s->flmul);
    require_vm(a->vm, a->rd);

    uint32_t data = 0;
    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);
    static gen_helper_gvec_2_ptr * const fns[4] = {
        gen_helper_vid_v_b, gen_helper_vid_v_h,
        gen_helper_vid_v_w, gen_helper_vid_v_d,
    };
    tcg_gen_gvec_2_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                       cpu_env, 0, s->vlen / 8, data, fns[s->sew]);
    gen_set_label(over);
    return true;
}

/*
 *** Vector Permutation Instructions
 */

/* Integer Extract Instruction */

static void load_element(TCGv_i64 dest, TCGv_ptr base,
                         int ofs, int sew)
{
    switch (sew) {
    case MO_8:
        tcg_gen_ld8u_i64(dest, base, ofs);
        break;
    case MO_16:
        tcg_gen_ld16u_i64(dest, base, ofs);
        break;
    case MO_32:
        tcg_gen_ld32u_i64(dest, base, ofs);
        break;
    case MO_64:
        tcg_gen_ld_i64(dest, base, ofs);
        break;
    default:
        g_assert_not_reached();
        break;
    }
}

/* offset of the idx element with base regsiter r */
static uint32_t endian_ofs(DisasContext *s, int r, int idx)
{
#ifdef HOST_WORDS_BIGENDIAN
    return vreg_ofs(s, r) + ((idx ^ (7 >> s->sew)) << s->sew);
#else
    return vreg_ofs(s, r) + (idx << s->sew);
#endif
}

/* adjust the index according to the endian */
static void endian_adjust(TCGv_i32 ofs, int sew)
{
#ifdef HOST_WORDS_BIGENDIAN
    tcg_gen_xori_i32(ofs, ofs, 7 >> sew);
#endif
}

/* Load idx >= VLMAX ? 0 : vreg[idx] */
static void vec_element_loadx(DisasContext *s, TCGv_i64 dest,
                              int vreg, TCGv idx, int vlmax)
{
    TCGv_i32 ofs = tcg_temp_new_i32();
    TCGv_ptr base = tcg_temp_new_ptr();
    TCGv_i64 t_idx = tcg_temp_new_i64();
    TCGv_i64 t_vlmax, t_zero;

    /*
     * Mask the index to the length so that we do
     * not produce an out-of-range load.
     */
    tcg_gen_trunc_tl_i32(ofs, idx);
    tcg_gen_andi_i32(ofs, ofs, vlmax - 1);

    /* Convert the index to an offset. */
    endian_adjust(ofs, s->sew);
    tcg_gen_shli_i32(ofs, ofs, s->sew);

    /* Convert the index to a pointer. */
    tcg_gen_ext_i32_ptr(base, ofs);
    tcg_gen_add_ptr(base, base, cpu_env);

    /* Perform the load. */
    load_element(dest, base,
                 vreg_ofs(s, vreg), s->sew);
    tcg_temp_free_ptr(base);
    tcg_temp_free_i32(ofs);

    /* Flush out-of-range indexing to zero.  */
    t_vlmax = tcg_const_i64(vlmax);
    t_zero = tcg_const_i64(0);
    tcg_gen_extu_tl_i64(t_idx, idx);

    tcg_gen_movcond_i64(TCG_COND_LTU, dest, t_idx,
                        t_vlmax, dest, t_zero);

    tcg_temp_free_i64(t_vlmax);
    tcg_temp_free_i64(t_zero);
    tcg_temp_free_i64(t_idx);
}

static void vec_element_loadi(DisasContext *s, TCGv_i64 dest,
                              int vreg, int idx)
{
    load_element(dest, cpu_env, endian_ofs(s, vreg, idx), s->sew);
}

static bool trans_vext_x_v(DisasContext *s, arg_r *a)
{
    TCGv_i64 tmp = tcg_temp_new_i64();
    TCGv dest = tcg_temp_new();

    if (a->rs1 == 0) {
        /* Special case vmv.x.s rd, vs2. */
        vec_element_loadi(s, tmp, a->rs2, 0);
    } else {
        /* This instruction ignores LMUL and vector register groups */
        int vlmax = s->vlen >> (3 + s->sew);
        vec_element_loadx(s, tmp, a->rs2, cpu_gpr[a->rs1], vlmax);
    }
    tcg_gen_trunc_i64_tl(dest, tmp);
    gen_set_gpr(a->rd, dest);

    tcg_temp_free(dest);
    tcg_temp_free_i64(tmp);
    return true;
}

/* Integer Scalar Move Instruction */

static void store_element(TCGv_i64 val, TCGv_ptr base,
                          int ofs, int sew)
{
    switch (sew) {
    case MO_8:
        tcg_gen_st8_i64(val, base, ofs);
        break;
    case MO_16:
        tcg_gen_st16_i64(val, base, ofs);
        break;
    case MO_32:
        tcg_gen_st32_i64(val, base, ofs);
        break;
    case MO_64:
        tcg_gen_st_i64(val, base, ofs);
        break;
    default:
        g_assert_not_reached();
        break;
    }
}

/*
 * Store vreg[idx] = val.
 * The index must be in range of VLMAX.
 */
static void vec_element_storei(DisasContext *s, int vreg,
                               int idx, TCGv_i64 val)
{
    store_element(val, cpu_env, endian_ofs(s, vreg, idx), s->sew);
}

/* vmv.x.s rd, vs2 # x[rd] = vs2[0] */
static bool trans_vmv_x_s(DisasContext *s, arg_vmv_x_s *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);

    TCGv_i64 t1;
    TCGv dest;

    t1 = tcg_temp_new_i64();
    dest = tcg_temp_new();
    /*
     * load vreg and sign-extend to 64 bits,
     * then truncate to XLEN bits before storing to gpr.
     */
    vec_element_loadi(s, t1, a->rs2, 0, true);
    tcg_gen_trunc_i64_tl(dest, t1);
    gen_set_gpr(a->rd, dest);
    tcg_temp_free_i64(t1);
    tcg_temp_free(dest);
    mark_vs_dirty(s);

    return true;
}

/* vmv.s.x vd, rs1 # vd[0] = rs1 */
static bool trans_vmv_s_x(DisasContext *s, arg_vmv_s_x *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);

    /* This instruction ignores LMUL and vector register groups */
    TCGv_i64 t1;
    TCGLabel *over = gen_new_label();

    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);
    if (a->rs1 == 0) {
        goto done;
    }

    t1 = tcg_temp_new_i64();
    tcg_gen_extu_tl_i64(t1, cpu_gpr[a->rs1]);
    vec_element_storei(s, a->rd, 0, t1);
    tcg_temp_free_i64(t1);
done:
    gen_set_label(over);
    return true;
}

/* Floating-Point Scalar Move Instructions */
static bool trans_vfmv_f_s(DisasContext *s, arg_vfmv_f_s *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require(has_ext(s, RVF));
    require(s->mstatus_fs != 0);
    require(s->sew != 0);

    unsigned int len = 8 << s->sew;

    vec_element_loadi(s, cpu_fpr[a->rd], a->rs2, 0, false);
    if (len < 64) {
        tcg_gen_ori_i64(cpu_fpr[a->rd], cpu_fpr[a->rd],
                        MAKE_64BIT_MASK(len, 64 - len));
    }

    mark_fs_dirty(s);
    return true;
}

/* vfmv.s.f vd, rs1 # vd[0] = rs1 (vs2=0) */
static bool trans_vfmv_s_f(DisasContext *s, arg_vfmv_s_f *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require(has_ext(s, RVF));
    require(s->sew != 0);

    /* The instructions ignore LMUL and vector register group. */
    TCGv_i64 t1;
    TCGLabel *over = gen_new_label();

    /* if vl == 0, skip vector register write back */
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    /* NaN-box f[rs1] as necessary for SEW */
    t1 = tcg_temp_new_i64();
    if (s->sew == MO_64 && !has_ext(s, RVD)) {
        tcg_gen_ori_i64(t1, cpu_fpr[a->rs1], MAKE_64BIT_MASK(32, 32));
    } else {
        tcg_gen_mov_i64(t1, cpu_fpr[a->rs1]);
    }
    vec_element_storei(s, a->rd, 0, t1);
    tcg_temp_free_i64(t1);
    gen_set_label(over);
    return true;
}

/* Vector Slide Instructions */
static bool slideup_check(DisasContext *s, arg_rmrr *a)
{
    return (vext_check_isa_ill(s) &&
            vext_check_overlap_mask(s, a->rd, a->vm, true) &&
            vext_check_reg(s, a->rd, false) &&
            vext_check_reg(s, a->rs2, false) &&
            (a->rd != a->rs2));
}

GEN_OPIVX_TRANS(vslideup_vx, slideup_check)
GEN_OPIVX_TRANS(vslide1up_vx, slideup_check)
GEN_OPIVI_TRANS(vslideup_vi, 1, vslideup_vx, slideup_check)

GEN_OPIVX_TRANS(vslidedown_vx, opivx_check)
GEN_OPIVX_TRANS(vslide1down_vx, opivx_check)
GEN_OPIVI_TRANS(vslidedown_vi, 1, vslidedown_vx, opivx_check)

/* Vector Register Gather Instruction */
static bool vrgather_vv_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require_align(a->rd, s->flmul);
    require_align(a->rs1, s->flmul);
    require_align(a->rs2, s->flmul);
    require(a->rd != a->rs2 && a->rd != a->rs1);
    require_vm(a->vm, a->rd);
    return true;
}

GEN_OPIVV_TRANS(vrgather_vv, vrgather_vv_check)

static bool vrgather_vx_check(DisasContext *s, arg_rmrr *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require_align(a->rd, s->flmul);
    require_align(a->rs2, s->flmul);
    require(a->rd != a->rs2);
    require_vm(a->vm, a->rd);
    return true;
}

/* vrgather.vx vd, vs2, rs1, vm # vd[i] = (x[rs1] >= VLMAX) ? 0 : vs2[rs1] */
static bool trans_vrgather_vx(DisasContext *s, arg_rmrr *a)
{
    if (!vrgather_vx_check(s, a)) {
        return false;
    }

    if (a->vm && s->vl_eq_vlmax) {
        int vlmax = s->vlen;
        TCGv_i64 dest = tcg_temp_new_i64();

        if (a->rs1 == 0) {
            vec_element_loadi(s, dest, a->rs2, 0);
        } else {
            vec_element_loadx(s, dest, a->rs2, cpu_gpr[a->rs1], vlmax);
        }

        tcg_gen_gvec_dup_i64(s->sew, vreg_ofs(s, a->rd),
                             MAXSZ(s), MAXSZ(s), dest);
        tcg_temp_free_i64(dest);
        mark_vs_dirty(s);
    } else {
        static gen_helper_opivx * const fns[4] = {
            gen_helper_vrgather_vx_b, gen_helper_vrgather_vx_h,
            gen_helper_vrgather_vx_w, gen_helper_vrgather_vx_d
        };
        return opivx_trans(a->rd, a->rs1, a->rs2, a->vm, fns[s->sew], s);
    }
    return true;
}

/* vrgather.vi vd, vs2, imm, vm # vd[i] = (imm >= VLMAX) ? 0 : vs2[imm] */
static bool trans_vrgather_vi(DisasContext *s, arg_rmrr *a)
{
    if (!vrgather_vx_check(s, a)) {
        return false;
    }

    if (a->vm && s->vl_eq_vlmax) {
        if (a->rs1 >= s->vlen) {
            tcg_gen_gvec_dup_imm(SEW64, vreg_ofs(s, a->rd),
                                 MAXSZ(s), MAXSZ(s), 0);
        } else {
            tcg_gen_gvec_dup_mem(s->sew, vreg_ofs(s, a->rd),
                                 endian_ofs(s, a->rs2, a->rs1),
                                 MAXSZ(s), MAXSZ(s));
        }
        mark_vs_dirty(s);
    } else {
        static gen_helper_opivx * const fns[4] = {
            gen_helper_vrgather_vx_b, gen_helper_vrgather_vx_h,
            gen_helper_vrgather_vx_w, gen_helper_vrgather_vx_d
        };
        return opivi_trans(a->rd, a->rs1, a->rs2, a->vm, fns[s->sew], s, 1);
    }
    return true;
}

/* Vector Compress Instruction */
static bool vcompress_vm_check(DisasContext *s, arg_r *a)
{
    REQUIRE_RVV;
    VEXT_CHECK_ISA_ILL(s);
    require_align(a->rd, s->flmul);
    require_align(a->rs2, s->flmul);
    require(a->rd != a->rs2);
    require_noover(a->rd, s->flmul, a->rs1, 1);
    return true;
}

static bool trans_vcompress_vm(DisasContext *s, arg_r *a)
{
    if (vcompress_vm_check(s, a)) {
        uint32_t data = 0;
        static gen_helper_gvec_4_ptr * const fns[4] = {
            gen_helper_vcompress_vm_b, gen_helper_vcompress_vm_h,
            gen_helper_vcompress_vm_w, gen_helper_vcompress_vm_d,
        };
        TCGLabel *over = gen_new_label();
        tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

        data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
        data = FIELD_DP32(data, VDATA, VTA, s->vta);
        data = FIELD_DP32(data, VDATA, VMA, s->vma);
        tcg_gen_gvec_4_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                           vreg_ofs(s, a->rs1), vreg_ofs(s, a->rs2),
                           cpu_env, 0, s->vlen / 8, data, fns[s->sew]);
        mark_vs_dirty(s);
        gen_set_label(over);
        return true;
    }
    return false;
}

/*
 * Whole Vector Register Move Instructions ignore vtype and vl setting.
 * Thus, we don't need to check vill bit. (Section 17.6)
 */
#define GEN_VMV_WHOLE_TRANS(NAME, LEN)                    \
static bool trans_##NAME(DisasContext *s, arg_##NAME * a) \
{                                                         \
    REQUIRE_RVV;                                          \
    require((a->rd & ((LEN) - 1)) == 0);                  \
    require((a->rs2 & ((LEN) - 1)) == 0);                 \
                                                          \
    for (int i = 0; i < LEN; ++i) {                       \
        /* EEW = 8 */                                     \
        tcg_gen_gvec_mov(8, vreg_ofs(s, a->rd + i),       \
                         vreg_ofs(s, a->rs2 + i),         \
                         s->vlen / 8, s->vlen / 8);       \
    }                                                     \
    mark_vs_dirty(s);                                     \
    return true;                                          \
}

GEN_VMV_WHOLE_TRANS(vmv1r_v, 1)
GEN_VMV_WHOLE_TRANS(vmv2r_v, 2)
GEN_VMV_WHOLE_TRANS(vmv4r_v, 4)
GEN_VMV_WHOLE_TRANS(vmv8r_v, 8)

static bool int_ext_check(DisasContext *s, arg_rmr *a, uint8_t div)
{
    REQUIRE_RVV;
    VEXT_CHECK_EXT(s, a->rd, a->rs2, a->vm, div);
    return true;
}

static bool int_ext_op(DisasContext *s, arg_rmr *a, uint8_t seq)
{
    uint32_t data = 0;
    gen_helper_gvec_3_ptr *fn;
    TCGLabel *over = gen_new_label();
    tcg_gen_brcondi_tl(TCG_COND_EQ, cpu_vl, 0, over);

    static gen_helper_gvec_3_ptr * const fns[6][4] = {
        {
            NULL, gen_helper_vzext_vf2_h,
            gen_helper_vzext_vf2_w, gen_helper_vzext_vf2_d
        },
        {
            NULL, NULL,
            gen_helper_vzext_vf4_w, gen_helper_vzext_vf4_d,
        },
        {
            NULL, NULL,
            NULL, gen_helper_vzext_vf8_d
        },
        {
            NULL, gen_helper_vsext_vf2_h,
            gen_helper_vsext_vf2_w, gen_helper_vsext_vf2_d
        },
        {
            NULL, NULL,
            gen_helper_vsext_vf4_w, gen_helper_vsext_vf4_d,
        },
        {
            NULL, NULL,
            NULL, gen_helper_vsext_vf8_d
        }
    };

    fn = fns[seq][s->sew];
    if (fn == NULL) {
        return false;
    }

    data = FIELD_DP32(data, VDATA, VM, a->vm);
    data = FIELD_DP32(data, VDATA, LMUL, s->lmul);
    data = FIELD_DP32(data, VDATA, VTA, s->vta);
    data = FIELD_DP32(data, VDATA, VMA, s->vma);

    tcg_gen_gvec_3_ptr(vreg_ofs(s, a->rd), vreg_ofs(s, 0),
                       vreg_ofs(s, a->rs2), cpu_env, 0,
                       s->vlen / 8, data, fn);

    mark_vs_dirty(s);
    gen_set_label(over);
    return true;
}

/* Vector Integer Extension */
#define GEN_INT_EXT_TRANS(NAME, DIV, SEQ)             \
static bool trans_##NAME(DisasContext *s, arg_rmr *a) \
{                                                     \
    if (int_ext_check(s, a, DIV)) {                   \
        return int_ext_op(s, a, SEQ);                 \
    }                                                 \
    return false;                                     \
}

GEN_INT_EXT_TRANS(vzext_vf2, 2, 0)
GEN_INT_EXT_TRANS(vzext_vf4, 4, 1)
GEN_INT_EXT_TRANS(vzext_vf8, 8, 2)
GEN_INT_EXT_TRANS(vsext_vf2, 2, 3)
GEN_INT_EXT_TRANS(vsext_vf4, 4, 4)
GEN_INT_EXT_TRANS(vsext_vf8, 8, 5)
