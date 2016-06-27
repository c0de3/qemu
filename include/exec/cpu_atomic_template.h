#include "tcg/tcg.h"

static inline DATA_TYPE
glue(glue(glue(cpu_cmpxchg, SUFFIX), MEMSUFFIX),
     _ra)(CPUArchState *env, target_ulong ptr, DATA_TYPE old, DATA_TYPE new,
          uintptr_t ra)
{
    target_ulong addr;
    TCGMemOpIdx oi;
    int page_index;
    DATA_TYPE ret;
    int mmu_idx;

    addr = ptr;
    page_index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    mmu_idx = CPU_MMU_INDEX;
    if (unlikely(env->tlb_table[mmu_idx][page_index].addr_write !=
                 (addr & (TARGET_PAGE_MASK | (DATA_SIZE - 1))))) {
        oi = make_memop_idx(SHIFT, mmu_idx);
        ret = glue(glue(helper_cmpxchg, SUFFIX), MMUSUFFIX)(env, addr, old, new,
                                                            oi, ra);
    } else {
        uintptr_t hostaddr = addr + env->tlb_table[mmu_idx][page_index].addend;

        ret = atomic_cmpxchg((DATA_TYPE *)hostaddr, old, new);
    }
    return ret;
}

/* define cmpxchgo once ldq and stq have been defined */
#if DATA_SIZE == 8
/* returns true on success, false on failure */
static inline bool
glue(glue(cpu_cmpxchgo, MEMSUFFIX), _ra)(CPUArchState *env, target_ulong ptr,
                                         uint64_t *old_lo, uint64_t *old_hi,
                                         uint64_t new_lo, uint64_t new_hi,
                                         uintptr_t retaddr)
{
    uint64_t orig_lo, orig_hi;
    bool ret = true;

    tcg_cmpxchg_lock(ptr);
    orig_lo = glue(glue(cpu_ldq, MEMSUFFIX), _ra)(env, ptr, retaddr);
    orig_hi = glue(glue(cpu_ldq, MEMSUFFIX), _ra)(env, ptr + 8, retaddr);
    if (orig_lo == *old_lo && orig_hi == *old_hi) {
        glue(glue(cpu_stq, MEMSUFFIX), _ra)(env, ptr, new_lo, retaddr);
        glue(glue(cpu_stq, MEMSUFFIX), _ra)(env, ptr + 8, new_hi, retaddr);
    } else {
        *old_lo = orig_lo;
        *old_hi = orig_hi;
        ret = false;
    }
    tcg_cmpxchg_unlock();
    return ret;
}
#endif

#define GEN_ATOMIC_HELPER(NAME)                                         \
static inline DATA_TYPE                                                 \
glue(glue(glue(glue(cpu_atomic_, NAME), SUFFIX), MEMSUFFIX),            \
     _ra)(CPUArchState *env, target_ulong ptr, DATA_TYPE val, uintptr_t ra) \
{                                                                       \
    int page_index;                                                     \
    target_ulong addr;                                                  \
    int mmu_idx;                                                        \
    TCGMemOpIdx oi;                                                     \
                                                                        \
    addr = ptr;                                                         \
    page_index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);       \
    mmu_idx = CPU_MMU_INDEX;                                            \
    if (unlikely(env->tlb_table[mmu_idx][page_index].addr_write !=      \
                 (addr & (TARGET_PAGE_MASK | (DATA_SIZE - 1))))) {      \
        oi = make_memop_idx(SHIFT, mmu_idx);                            \
        return glue(glue(glue(helper_atomic_, NAME), SUFFIX),           \
                    MMUSUFFIX)(env, addr, val, oi, ra);                 \
    } else {                                                            \
        uintptr_t hostaddr = addr +                                     \
            env->tlb_table[mmu_idx][page_index].addend;                 \
                                                                        \
        return glue(atomic_, NAME)((DATA_TYPE *)hostaddr, val);         \
    }                                                                   \
}

GEN_ATOMIC_HELPER(fetch_add)
GEN_ATOMIC_HELPER(fetch_sub)
GEN_ATOMIC_HELPER(fetch_and)
GEN_ATOMIC_HELPER(fetch_or)
GEN_ATOMIC_HELPER(fetch_xor)

GEN_ATOMIC_HELPER(add_fetch)
GEN_ATOMIC_HELPER(sub_fetch)
GEN_ATOMIC_HELPER(and_fetch)
GEN_ATOMIC_HELPER(or_fetch)
GEN_ATOMIC_HELPER(xor_fetch)

GEN_ATOMIC_HELPER(xchg)
#undef GEN_ATOMIC_HELPER
