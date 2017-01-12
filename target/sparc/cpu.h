#ifndef SPARC_CPU_H
#define SPARC_CPU_H

#include "qemu-common.h"
#include "qemu/bswap.h"
#include "cpu-qom.h"

#define ALIGNED_ONLY

#if !defined(TARGET_SPARC64)
#define TARGET_LONG_BITS 32
#define TARGET_DPREGS 16
#define TARGET_PAGE_BITS 12 /* 4k */
#define TARGET_PHYS_ADDR_SPACE_BITS 36
#define TARGET_VIRT_ADDR_SPACE_BITS 32
#else
#define TARGET_LONG_BITS 64
#define TARGET_DPREGS 32
#define TARGET_PAGE_BITS 13 /* 8k */
#define TARGET_PHYS_ADDR_SPACE_BITS 41
# ifdef TARGET_ABI32
#  define TARGET_VIRT_ADDR_SPACE_BITS 32
# else
#  define TARGET_VIRT_ADDR_SPACE_BITS 44
# endif
#endif

#define CPUArchState struct CPUSPARCState

#include "exec/cpu-defs.h"

#include "fpu/softfloat.h"

/*#define EXCP_INTERRUPT 0x100*/

/* trap definitions */
#ifndef TARGET_SPARC64
#define TT_TFAULT   0x01
#define TT_ILL_INSN 0x02
#define TT_PRIV_INSN 0x03
#define TT_NFPU_INSN 0x04
#define TT_WIN_OVF  0x05
#define TT_WIN_UNF  0x06
#define TT_UNALIGNED 0x07
#define TT_FP_EXCP  0x08
#define TT_DFAULT   0x09
#define TT_TOVF     0x0a
#define TT_EXTINT   0x10
#define TT_CODE_ACCESS 0x21
#define TT_UNIMP_FLUSH 0x25
#define TT_DATA_ACCESS 0x29
#define TT_DIV_ZERO 0x2a
#define TT_NCP_INSN 0x24
#define TT_TRAP     0x80
#else
#define TT_POWER_ON_RESET 0x01
#define TT_TFAULT   0x08
#define TT_CODE_ACCESS 0x0a
#define TT_ILL_INSN 0x10
#define TT_UNIMP_FLUSH TT_ILL_INSN
#define TT_PRIV_INSN 0x11
#define TT_NFPU_INSN 0x20
#define TT_FP_EXCP  0x21
#define TT_TOVF     0x23
#define TT_CLRWIN   0x24
#define TT_DIV_ZERO 0x28
#define TT_DFAULT   0x30
#define TT_DATA_ACCESS 0x32
#define TT_UNALIGNED 0x34
#define TT_PRIV_ACT 0x37
#define TT_INSN_REAL_TRANSLATION_MISS 0x3e
#define TT_DATA_REAL_TRANSLATION_MISS 0x3f
#define TT_EXTINT   0x40
#define TT_IVEC     0x60
#define TT_TMISS    0x64
#define TT_DMISS    0x68
#define TT_DPROT    0x6c
#define TT_SPILL    0x80
#define TT_FILL     0xc0
#define TT_WOTHER   (1 << 5)
#define TT_TRAP     0x100
#define TT_HTRAP    0x180
#endif

#define PSR_NEG_SHIFT 23
#define PSR_NEG   (1 << PSR_NEG_SHIFT)
#define PSR_ZERO_SHIFT 22
#define PSR_ZERO  (1 << PSR_ZERO_SHIFT)
#define PSR_OVF_SHIFT 21
#define PSR_OVF   (1 << PSR_OVF_SHIFT)
#define PSR_CARRY_SHIFT 20
#define PSR_CARRY (1 << PSR_CARRY_SHIFT)
#define PSR_ICC   (PSR_NEG|PSR_ZERO|PSR_OVF|PSR_CARRY)
#if !defined(TARGET_SPARC64)
#define PSR_EF    (1<<12)
#define PSR_PIL   0xf00
#define PSR_S     (1<<7)
#define PSR_PS    (1<<6)
#define PSR_ET    (1<<5)
#define PSR_CWP   0x1f
#endif

#define CC_SRC (env->cc_src)
#define CC_SRC2 (env->cc_src2)
#define CC_DST (env->cc_dst)
#define CC_OP  (env->cc_op)

/* Even though lazy evaluation of CPU condition codes tends to be less
 * important on RISC systems where condition codes are only updated
 * when explicitly requested, SPARC uses it to update 32-bit and 64-bit
 * condition codes.
 */
enum {
    CC_OP_DYNAMIC, /* must use dynamic code to get cc_op */
    CC_OP_FLAGS,   /* all cc are back in status register */
    CC_OP_DIV,     /* modify N, Z and V, C = 0*/
    CC_OP_ADD,     /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_ADDX,    /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_TADD,    /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_TADDTV,  /* modify all flags except V, CC_DST = res, CC_SRC = src1 */
    CC_OP_SUB,     /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_SUBX,    /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_TSUB,    /* modify all flags, CC_DST = res, CC_SRC = src1 */
    CC_OP_TSUBTV,  /* modify all flags except V, CC_DST = res, CC_SRC = src1 */
    CC_OP_LOGIC,   /* modify N and Z, C = V = 0, CC_DST = res */
    CC_OP_NB,
};

/* Trap base register */
#define TBR_BASE_MASK 0xfffff000

#if defined(TARGET_SPARC64)
#define PS_TCT   (1<<12) /* UA2007, impl.dep. trap on control transfer */
#define PS_IG    (1<<11) /* v9, zero on UA2007 */
#define PS_MG    (1<<10) /* v9, zero on UA2007 */
#define PS_CLE   (1<<9) /* UA2007 */
#define PS_TLE   (1<<8) /* UA2007 */
#define PS_RMO   (1<<7)
#define PS_RED   (1<<5) /* v9, zero on UA2007 */
#define PS_PEF   (1<<4) /* enable fpu */
#define PS_AM    (1<<3) /* address mask */
#define PS_PRIV  (1<<2)
#define PS_IE    (1<<1)
#define PS_AG    (1<<0) /* v9, zero on UA2007 */

#define FPRS_FEF (1<<2)

#define HS_PRIV  (1<<2)
#endif

/* Fcc */
#define FSR_RD1        (1ULL << 31)
#define FSR_RD0        (1ULL << 30)
#define FSR_RD_MASK    (FSR_RD1 | FSR_RD0)
#define FSR_RD_NEAREST 0
#define FSR_RD_ZERO    FSR_RD0
#define FSR_RD_POS     FSR_RD1
#define FSR_RD_NEG     (FSR_RD1 | FSR_RD0)

#define FSR_NVM   (1ULL << 27)
#define FSR_OFM   (1ULL << 26)
#define FSR_UFM   (1ULL << 25)
#define FSR_DZM   (1ULL << 24)
#define FSR_NXM   (1ULL << 23)
#define FSR_TEM_MASK (FSR_NVM | FSR_OFM | FSR_UFM | FSR_DZM | FSR_NXM)

#define FSR_NVA   (1ULL << 9)
#define FSR_OFA   (1ULL << 8)
#define FSR_UFA   (1ULL << 7)
#define FSR_DZA   (1ULL << 6)
#define FSR_NXA   (1ULL << 5)
#define FSR_AEXC_MASK (FSR_NVA | FSR_OFA | FSR_UFA | FSR_DZA | FSR_NXA)

#define FSR_NVC   (1ULL << 4)
#define FSR_OFC   (1ULL << 3)
#define FSR_UFC   (1ULL << 2)
#define FSR_DZC   (1ULL << 1)
#define FSR_NXC   (1ULL << 0)
#define FSR_CEXC_MASK (FSR_NVC | FSR_OFC | FSR_UFC | FSR_DZC | FSR_NXC)

#define FSR_FTT2   (1ULL << 16)
#define FSR_FTT1   (1ULL << 15)
#define FSR_FTT0   (1ULL << 14)
//gcc warns about constant overflow for ~FSR_FTT_MASK
//#define FSR_FTT_MASK (FSR_FTT2 | FSR_FTT1 | FSR_FTT0)
#ifdef TARGET_SPARC64
#define FSR_FTT_NMASK      0xfffffffffffe3fffULL
#define FSR_FTT_CEXC_NMASK 0xfffffffffffe3fe0ULL
#define FSR_LDFSR_OLDMASK  0x0000003f000fc000ULL
#define FSR_LDXFSR_MASK    0x0000003fcfc00fffULL
#define FSR_LDXFSR_OLDMASK 0x00000000000fc000ULL
#else
#define FSR_FTT_NMASK      0xfffe3fffULL
#define FSR_FTT_CEXC_NMASK 0xfffe3fe0ULL
#define FSR_LDFSR_OLDMASK  0x000fc000ULL
#endif
#define FSR_LDFSR_MASK     0xcfc00fffULL
#define FSR_FTT_IEEE_EXCP (1ULL << 14)
#define FSR_FTT_UNIMPFPOP (3ULL << 14)
#define FSR_FTT_SEQ_ERROR (4ULL << 14)
#define FSR_FTT_INVAL_FPR (6ULL << 14)

#define FSR_FCC1_SHIFT 11
#define FSR_FCC1  (1ULL << FSR_FCC1_SHIFT)
#define FSR_FCC0_SHIFT 10
#define FSR_FCC0  (1ULL << FSR_FCC0_SHIFT)

/* MMU */
#define MMU_E     (1<<0)
#define MMU_NF    (1<<1)

#define PTE_ENTRYTYPE_MASK 3
#define PTE_ACCESS_MASK    0x1c
#define PTE_ACCESS_SHIFT   2
#define PTE_PPN_SHIFT      7
#define PTE_ADDR_MASK      0xffffff00

#define PG_ACCESSED_BIT 5
#define PG_MODIFIED_BIT 6
#define PG_CACHE_BIT    7

#define PG_ACCESSED_MASK (1 << PG_ACCESSED_BIT)
#define PG_MODIFIED_MASK (1 << PG_MODIFIED_BIT)
#define PG_CACHE_MASK    (1 << PG_CACHE_BIT)

/* 3 <= NWINDOWS <= 32. */
#define MIN_NWINDOWS 3
#define MAX_NWINDOWS 32

#if !defined(TARGET_SPARC64)
#define NB_MMU_MODES 3
#else
#define NB_MMU_MODES 7
typedef struct trap_state {
    uint64_t tpc;
    uint64_t tnpc;
    uint64_t tstate;
    uint32_t tt;
} trap_state;
#endif
#define TARGET_INSN_START_EXTRA_WORDS 1

typedef struct sparc_def_t {
    const char *name;
    target_ulong iu_version;
    uint32_t fpu_version;
    uint32_t mmu_version;
    uint32_t mmu_bm;
    uint32_t mmu_ctpr_mask;
    uint32_t mmu_cxr_mask;
    uint32_t mmu_sfsr_mask;
    uint32_t mmu_trcr_mask;
    uint32_t mxcc_version;
    uint32_t features;
    uint32_t nwindows;
    uint32_t maxtl;
} sparc_def_t;

#define CPU_FEATURE_FLOAT        (1 << 0)
#define CPU_FEATURE_FLOAT128     (1 << 1)
#define CPU_FEATURE_SWAP         (1 << 2)
#define CPU_FEATURE_MUL          (1 << 3)
#define CPU_FEATURE_DIV          (1 << 4)
#define CPU_FEATURE_FLUSH        (1 << 5)
#define CPU_FEATURE_FSQRT        (1 << 6)
#define CPU_FEATURE_FMUL         (1 << 7)
#define CPU_FEATURE_VIS1         (1 << 8)
#define CPU_FEATURE_VIS2         (1 << 9)
#define CPU_FEATURE_FSMULD       (1 << 10)
#define CPU_FEATURE_HYPV         (1 << 11)
#define CPU_FEATURE_CMT          (1 << 12)
#define CPU_FEATURE_GL           (1 << 13)
#define CPU_FEATURE_TA0_SHUTDOWN (1 << 14) /* Shutdown on "ta 0x0" */
#define CPU_FEATURE_ASR17        (1 << 15)
#define CPU_FEATURE_CACHE_CTRL   (1 << 16)
#define CPU_FEATURE_POWERDOWN    (1 << 17)
#define CPU_FEATURE_CASA         (1 << 18)

#ifndef TARGET_SPARC64
#define CPU_DEFAULT_FEATURES (CPU_FEATURE_FLOAT | CPU_FEATURE_SWAP |  \
                              CPU_FEATURE_MUL | CPU_FEATURE_DIV |     \
                              CPU_FEATURE_FLUSH | CPU_FEATURE_FSQRT | \
                              CPU_FEATURE_FMUL | CPU_FEATURE_FSMULD)
#else
#define CPU_DEFAULT_FEATURES (CPU_FEATURE_FLOAT | CPU_FEATURE_SWAP |  \
                              CPU_FEATURE_MUL | CPU_FEATURE_DIV |     \
                              CPU_FEATURE_FLUSH | CPU_FEATURE_FSQRT | \
                              CPU_FEATURE_FMUL | CPU_FEATURE_VIS1 |   \
                              CPU_FEATURE_VIS2 | CPU_FEATURE_FSMULD | \
                              CPU_FEATURE_CASA)
enum {
    mmu_us_12, // Ultrasparc < III (64 entry TLB)
    mmu_us_3,  // Ultrasparc III (512 entry TLB)
    mmu_us_4,  // Ultrasparc IV (several TLBs, 32 and 256MB pages)
    mmu_sun4v, // T1, T2
};
#endif

#define TTE_VALID_BIT       (1ULL << 63)
#define TTE_NFO_BIT         (1ULL << 60)
#define TTE_USED_BIT        (1ULL << 41)
#define TTE_LOCKED_BIT      (1ULL <<  6)
#define TTE_SIDEEFFECT_BIT  (1ULL <<  3)
#define TTE_PRIV_BIT        (1ULL <<  2)
#define TTE_W_OK_BIT        (1ULL <<  1)
#define TTE_GLOBAL_BIT      (1ULL <<  0)

#define TTE_NFO_BIT_UA2005  (1ULL << 62)
#define TTE_USED_BIT_UA2005 (1ULL << 47)
#define TTE_LOCKED_BIT_UA2005 (1ULL <<  61)
#define TTE_SIDEEFFECT_BIT_UA2005 (1ULL <<  11)
#define TTE_PRIV_BIT_UA2005 (1ULL <<  8)
#define TTE_W_OK_BIT_UA2005 (1ULL <<  6)

#define TTE_IS_VALID(tte)   ((tte) & TTE_VALID_BIT)
#define TTE_IS_NFO(tte)     ((tte) & TTE_NFO_BIT)
#define TTE_IS_USED(tte)    ((tte) & TTE_USED_BIT)
#define TTE_IS_LOCKED(tte)  ((tte) & TTE_LOCKED_BIT)
#define TTE_IS_SIDEEFFECT(tte) ((tte) & TTE_SIDEEFFECT_BIT)
#define TTE_IS_SIDEEFFECT_UA2005(tte) ((tte) & TTE_SIDEEFFECT_BIT_UA2005)
#define TTE_IS_PRIV(tte)    ((tte) & TTE_PRIV_BIT)
#define TTE_IS_W_OK(tte)    ((tte) & TTE_W_OK_BIT)

#define TTE_IS_NFO_UA2005(tte)     ((tte) & TTE_NFO_BIT_UA2005)
#define TTE_IS_USED_UA2005(tte)    ((tte) & TTE_USED_BIT_UA2005)
#define TTE_IS_LOCKED_UA2005(tte)  ((tte) & TTE_LOCKED_BIT_UA2005)
#define TTE_IS_SIDEEFFECT_UA2005(tte) ((tte) & TTE_SIDEEFFECT_BIT_UA2005)
#define TTE_IS_PRIV_UA2005(tte)    ((tte) & TTE_PRIV_BIT_UA2005)
#define TTE_IS_W_OK_UA2005(tte)    ((tte) & TTE_W_OK_BIT_UA2005)

#define TTE_IS_GLOBAL(tte)  ((tte) & TTE_GLOBAL_BIT)

#define TTE_SET_USED(tte)   ((tte) |= TTE_USED_BIT)
#define TTE_SET_UNUSED(tte) ((tte) &= ~TTE_USED_BIT)

#define TTE_PGSIZE(tte)     (((tte) >> 61) & 3ULL)
#define TTE_PGSIZE_UA2005(tte)     ((tte) & 7ULL)
#define TTE_PA(tte)         ((tte) & 0x1ffffffe000ULL)

/* UltraSPARC T1 specific */
#define TLB_UST1_IS_REAL_BIT   (1ULL << 9)  /* Real translation entry */
#define TLB_UST1_IS_SUN4V_BIT  (1ULL << 10) /* sun4u/sun4v TTE format switch */

#define SFSR_NF_BIT         (1ULL << 24)   /* JPS1 NoFault */
#define SFSR_TM_BIT         (1ULL << 15)   /* JPS1 TLB Miss */
#define SFSR_FT_VA_IMMU_BIT (1ULL << 13)   /* USIIi VA out of range (IMMU) */
#define SFSR_FT_VA_DMMU_BIT (1ULL << 12)   /* USIIi VA out of range (DMMU) */
#define SFSR_FT_NFO_BIT     (1ULL << 11)   /* NFO page access */
#define SFSR_FT_ILL_BIT     (1ULL << 10)   /* illegal LDA/STA ASI */
#define SFSR_FT_ATOMIC_BIT  (1ULL <<  9)   /* atomic op on noncacheable area */
#define SFSR_FT_NF_E_BIT    (1ULL <<  8)   /* NF access on side effect area */
#define SFSR_FT_PRIV_BIT    (1ULL <<  7)   /* privilege violation */
#define SFSR_PR_BIT         (1ULL <<  3)   /* privilege mode */
#define SFSR_WRITE_BIT      (1ULL <<  2)   /* write access mode */
#define SFSR_OW_BIT         (1ULL <<  1)   /* status overwritten */
#define SFSR_VALID_BIT      (1ULL <<  0)   /* status valid */

#define SFSR_ASI_SHIFT      16             /* 23:16 ASI value */
#define SFSR_ASI_MASK       (0xffULL << SFSR_ASI_SHIFT)
#define SFSR_CT_PRIMARY     (0ULL <<  4)   /* 5:4 context type */
#define SFSR_CT_SECONDARY   (1ULL <<  4)
#define SFSR_CT_NUCLEUS     (2ULL <<  4)
#define SFSR_CT_NOTRANS     (3ULL <<  4)
#define SFSR_CT_MASK        (3ULL <<  4)

/* Leon3 cache control */

/* Cache control: emulate the behavior of cache control registers but without
   any effect on the emulated */

#define CACHE_STATE_MASK 0x3
#define CACHE_DISABLED   0x0
#define CACHE_FROZEN     0x1
#define CACHE_ENABLED    0x3

/* Cache Control register fields */

#define CACHE_CTRL_IF (1 <<  4)  /* Instruction Cache Freeze on Interrupt */
#define CACHE_CTRL_DF (1 <<  5)  /* Data Cache Freeze on Interrupt */
#define CACHE_CTRL_DP (1 << 14)  /* Data cache flush pending */
#define CACHE_CTRL_IP (1 << 15)  /* Instruction cache flush pending */
#define CACHE_CTRL_IB (1 << 16)  /* Instruction burst fetch */
#define CACHE_CTRL_FI (1 << 21)  /* Flush Instruction cache (Write only) */
#define CACHE_CTRL_FD (1 << 22)  /* Flush Data cache (Write only) */
#define CACHE_CTRL_DS (1 << 23)  /* Data cache snoop enable */

typedef struct SparcTLBEntry {
    uint64_t tag;
    uint64_t tte;
} SparcTLBEntry;

struct CPUTimer
{
    const char *name;
    uint32_t    frequency;
    uint32_t    disabled;
    uint64_t    disabled_mask;
    uint32_t    npt;
    uint64_t    npt_mask;
    int64_t     clock_offset;
    QEMUTimer  *qtimer;
};

typedef struct CPUTimer CPUTimer;

typedef struct CPUSPARCState CPUSPARCState;

struct CPUSPARCState {
    target_ulong gregs[8]; /* general registers */
    target_ulong *regwptr; /* pointer to current register window */
    target_ulong pc;       /* program counter */
    target_ulong npc;      /* next program counter */
    target_ulong y;        /* multiply/divide register */

    /* emulator internal flags handling */
    target_ulong cc_src, cc_src2;
    target_ulong cc_dst;
    uint32_t cc_op;

    target_ulong cond; /* conditional branch result (XXX: save it in a
                          temporary register when possible) */

    uint32_t psr;      /* processor state register */
    target_ulong fsr;      /* FPU state register */
    CPU_DoubleU fpr[TARGET_DPREGS];  /* floating point registers */
    uint32_t cwp;      /* index of current register window (extracted
                          from PSR) */
#if !defined(TARGET_SPARC64) || defined(TARGET_ABI32)
    uint32_t wim;      /* window invalid mask */
#endif
    target_ulong tbr;  /* trap base register */
#if !defined(TARGET_SPARC64)
    int      psrs;     /* supervisor mode (extracted from PSR) */
    int      psrps;    /* previous supervisor mode */
    int      psret;    /* enable traps */
#endif
    uint32_t psrpil;   /* interrupt blocking level */
    uint32_t pil_in;   /* incoming interrupt level bitmap */
#if !defined(TARGET_SPARC64)
    int      psref;    /* enable fpu */
#endif
    int interrupt_index;
    /* NOTE: we allow 8 more registers to handle wrapping */
    target_ulong regbase[MAX_NWINDOWS * 16 + 8];

    CPU_COMMON

    /* Fields from here on are preserved across CPU reset. */
    target_ulong version;
    uint32_t nwindows;

    /* MMU regs */
#if defined(TARGET_SPARC64)
    uint64_t lsu;
#define DMMU_E 0x8
#define IMMU_E 0x4
    //typedef struct SparcMMU
    union {
        uint64_t immuregs[16];
        struct {
            uint64_t tsb_tag_target;
            uint64_t unused_mmu_primary_context;   // use DMMU
            uint64_t unused_mmu_secondary_context; // use DMMU
            uint64_t sfsr;
            uint64_t sfar;
            uint64_t tsb;
            uint64_t tag_access;
            uint64_t virtual_watchpoint;
            uint64_t physical_watchpoint;
        } immu;
    };
    union {
        uint64_t dmmuregs[16];
        struct {
            uint64_t tsb_tag_target;
            uint64_t mmu_primary_context;
            uint64_t mmu_secondary_context;
            uint64_t sfsr;
            uint64_t sfar;
            uint64_t tsb;
            uint64_t tag_access;
            uint64_t virtual_watchpoint;
            uint64_t physical_watchpoint;
        } dmmu;
    };
    SparcTLBEntry itlb[64];
    SparcTLBEntry dtlb[64];
    uint32_t mmu_version;
#else
    uint32_t mmuregs[32];
    uint64_t mxccdata[4];
    uint64_t mxccregs[8];
    uint32_t mmubpctrv, mmubpctrc, mmubpctrs;
    uint64_t mmubpaction;
    uint64_t mmubpregs[4];
    uint64_t prom_addr;
#endif
    /* temporary float registers */
    float128 qt0, qt1;
    float_status fp_status;
#if defined(TARGET_SPARC64)
#define MAXTL_MAX 8
#define MAXTL_MASK (MAXTL_MAX - 1)
    trap_state ts[MAXTL_MAX];
    uint32_t xcc;               /* Extended integer condition codes */
    uint32_t asi;
    uint32_t pstate;
    uint32_t tl;
    uint32_t maxtl;
    uint32_t cansave, canrestore, otherwin, wstate, cleanwin;
    uint64_t agregs[8]; /* alternate general registers */
    uint64_t bgregs[8]; /* backup for normal global registers */
    uint64_t igregs[8]; /* interrupt general registers */
    uint64_t mgregs[8]; /* mmu general registers */
    uint64_t fprs;
    uint64_t tick_cmpr, stick_cmpr;
    CPUTimer *tick, *stick;
#define TICK_NPT_MASK        0x8000000000000000ULL
#define TICK_INT_DIS         0x8000000000000000ULL
    uint64_t gsr;
    uint32_t gl; // UA2005
    /* UA 2005 hyperprivileged registers */
    uint64_t hpstate, htstate[MAXTL_MAX], hintp, htba, hver, hstick_cmpr, ssr;
    uint64_t scratch[8];
    CPUTimer *hstick; // UA 2005
    /* Interrupt vector registers */
    uint64_t ivec_status;
    uint64_t ivec_data[3];
    uint32_t softint;
#define SOFTINT_TIMER   1
#define SOFTINT_STIMER  (1 << 16)
#define SOFTINT_INTRMASK (0xFFFE)
#define SOFTINT_REG_MASK (SOFTINT_STIMER|SOFTINT_INTRMASK|SOFTINT_TIMER)
#endif
    sparc_def_t *def;

    void *irq_manager;
    void (*qemu_irq_ack)(CPUSPARCState *env, void *irq_manager, int intno);

    /* Leon3 cache control */
    uint32_t cache_control;
};

/**
 * SPARCCPU:
 * @env: #CPUSPARCState
 *
 * A SPARC CPU.
 */
struct SPARCCPU {
    /*< private >*/
    CPUState parent_obj;
    /*< public >*/

    CPUSPARCState env;
};

static inline SPARCCPU *sparc_env_get_cpu(CPUSPARCState *env)
{
    return container_of(env, SPARCCPU, env);
}

#define ENV_GET_CPU(e) CPU(sparc_env_get_cpu(e))

#define ENV_OFFSET offsetof(SPARCCPU, env)

#ifndef CONFIG_USER_ONLY
extern const struct VMStateDescription vmstate_sparc_cpu;
#endif

void sparc_cpu_do_interrupt(CPUState *cpu);
void sparc_cpu_dump_state(CPUState *cpu, FILE *f,
                          fprintf_function cpu_fprintf, int flags);
hwaddr sparc_cpu_get_phys_page_debug(CPUState *cpu, vaddr addr);
int sparc_cpu_gdb_read_register(CPUState *cpu, uint8_t *buf, int reg);
int sparc_cpu_gdb_write_register(CPUState *cpu, uint8_t *buf, int reg);
void QEMU_NORETURN sparc_cpu_do_unaligned_access(CPUState *cpu, vaddr addr,
                                                 MMUAccessType access_type,
                                                 int mmu_idx,
                                                 uintptr_t retaddr);
void cpu_raise_exception_ra(CPUSPARCState *, int, uintptr_t) QEMU_NORETURN;

#ifndef NO_CPU_IO_DEFS
/* cpu_init.c */
SPARCCPU *cpu_sparc_init(const char *cpu_model);
void cpu_sparc_set_id(CPUSPARCState *env, unsigned int cpu);
void sparc_cpu_list(FILE *f, fprintf_function cpu_fprintf);
/* mmu_helper.c */
int sparc_cpu_handle_mmu_fault(CPUState *cpu, vaddr address, int rw,
                               int mmu_idx);
target_ulong mmu_probe(CPUSPARCState *env, target_ulong address, int mmulev);
void dump_mmu(FILE *f, fprintf_function cpu_fprintf, CPUSPARCState *env);

#if !defined(TARGET_SPARC64) && !defined(CONFIG_USER_ONLY)
int sparc_cpu_memory_rw_debug(CPUState *cpu, vaddr addr,
                              uint8_t *buf, int len, bool is_write);
#endif


/* translate.c */
void gen_intermediate_code_init(CPUSPARCState *env);

/* cpu-exec.c */

/* win_helper.c */
target_ulong cpu_get_psr(CPUSPARCState *env1);
void cpu_put_psr(CPUSPARCState *env1, target_ulong val);
void cpu_put_psr_raw(CPUSPARCState *env1, target_ulong val);
#ifdef TARGET_SPARC64
target_ulong cpu_get_ccr(CPUSPARCState *env1);
void cpu_put_ccr(CPUSPARCState *env1, target_ulong val);
target_ulong cpu_get_cwp64(CPUSPARCState *env1);
void cpu_put_cwp64(CPUSPARCState *env1, int cwp);
void cpu_change_pstate(CPUSPARCState *env1, uint32_t new_pstate);
#endif
int cpu_cwp_inc(CPUSPARCState *env1, int cwp);
int cpu_cwp_dec(CPUSPARCState *env1, int cwp);
void cpu_set_cwp(CPUSPARCState *env1, int new_cwp);

/* int_helper.c */
void leon3_irq_manager(CPUSPARCState *env, void *irq_manager, int intno);

/* sun4m.c, sun4u.c */
void cpu_check_irqs(CPUSPARCState *env);

/* leon3.c */
void leon3_irq_ack(void *irq_manager, int intno);

#if defined (TARGET_SPARC64)

static inline int compare_masked(uint64_t x, uint64_t y, uint64_t mask)
{
    return (x & mask) == (y & mask);
}

#define MMU_CONTEXT_BITS 13
#define MMU_CONTEXT_MASK ((1 << MMU_CONTEXT_BITS) - 1)

static inline int tlb_compare_context(const SparcTLBEntry *tlb,
                                      uint64_t context)
{
    return compare_masked(context, tlb->tag, MMU_CONTEXT_MASK);
}

#endif
#endif

/* cpu-exec.c */
#if !defined(CONFIG_USER_ONLY)
void sparc_cpu_unassigned_access(CPUState *cpu, hwaddr addr,
                                 bool is_write, bool is_exec, int is_asi,
                                 unsigned size);
#if defined(TARGET_SPARC64)
hwaddr cpu_get_phys_page_nofault(CPUSPARCState *env, target_ulong addr,
                                           int mmu_idx);
#endif
#endif
int cpu_sparc_signal_handler(int host_signum, void *pinfo, void *puc);

#ifndef NO_CPU_IO_DEFS
#define cpu_init(cpu_model) CPU(cpu_sparc_init(cpu_model))
#endif

#define cpu_signal_handler cpu_sparc_signal_handler
#define cpu_list sparc_cpu_list

/* MMU modes definitions */
#if defined (TARGET_SPARC64)
#define MMU_USER_IDX   0
#define MMU_USER_SECONDARY_IDX   1
#define MMU_KERNEL_IDX 2
#define MMU_KERNEL_SECONDARY_IDX 3
#define MMU_NUCLEUS_IDX 4
#define MMU_HYPV_IDX   5
#define MMU_PHYS_IDX   6
#else
#define MMU_USER_IDX   0
#define MMU_KERNEL_IDX 1
#define MMU_PHYS_IDX   2
#endif

#if defined (TARGET_SPARC64)
static inline int cpu_has_hypervisor(CPUSPARCState *env1)
{
    return env1->def->features & CPU_FEATURE_HYPV;
}

static inline int cpu_hypervisor_mode(CPUSPARCState *env1)
{
    return cpu_has_hypervisor(env1) && (env1->hpstate & HS_PRIV);
}

static inline int cpu_supervisor_mode(CPUSPARCState *env1)
{
    return env1->pstate & PS_PRIV;
}
#else
static inline int cpu_supervisor_mode(CPUSPARCState *env1)
{
    return env1->psrs;
}
#endif

static inline int cpu_mmu_index(CPUSPARCState *env, bool ifetch)
{
#if defined(CONFIG_USER_ONLY)
    return MMU_USER_IDX;
#elif !defined(TARGET_SPARC64)
    if ((env->mmuregs[0] & MMU_E) == 0) { /* MMU disabled */
        return MMU_PHYS_IDX;
    } else {
        return env->psrs;
    }
#else
    /* IMMU or DMMU disabled.  */
    if (ifetch
        ? (env->lsu & IMMU_E) == 0 || (env->pstate & PS_RED) != 0
        : (env->lsu & DMMU_E) == 0) {
        return MMU_PHYS_IDX;
    } else if (cpu_hypervisor_mode(env)) {
        return MMU_HYPV_IDX;
    } else if (env->tl > 0) {
        return MMU_NUCLEUS_IDX;
    } else if (cpu_supervisor_mode(env)) {
        return MMU_KERNEL_IDX;
    } else {
        return MMU_USER_IDX;
    }
#endif
}

static inline int cpu_interrupts_enabled(CPUSPARCState *env1)
{
#if !defined (TARGET_SPARC64)
    if (env1->psret != 0)
        return 1;
#else
    if ((env1->pstate & PS_IE) && !cpu_hypervisor_mode(env1)) {
        return 1;
    }
#endif

    return 0;
}

static inline int cpu_pil_allowed(CPUSPARCState *env1, int pil)
{
#if !defined(TARGET_SPARC64)
    /* level 15 is non-maskable on sparc v8 */
    return pil == 15 || pil > env1->psrpil;
#else
    return pil > env1->psrpil;
#endif
}

#include "exec/cpu-all.h"

#ifdef TARGET_SPARC64
/* sun4u.c */
void cpu_tick_set_count(CPUTimer *timer, uint64_t count);
uint64_t cpu_tick_get_count(CPUTimer *timer);
void cpu_tick_set_limit(CPUTimer *timer, uint64_t limit);
trap_state* cpu_tsptr(CPUSPARCState* env);
#endif

#define TB_FLAG_MMU_MASK     7
#define TB_FLAG_FPU_ENABLED  (1 << 4)
#define TB_FLAG_AM_ENABLED   (1 << 5)
#define TB_FLAG_SUPER        (1 << 6)
#define TB_FLAG_HYPER        (1 << 7)
#define TB_FLAG_ASI_SHIFT    24

static inline void cpu_get_tb_cpu_state(CPUSPARCState *env, target_ulong *pc,
                                        target_ulong *cs_base, uint32_t *pflags)
{
    uint32_t flags;
    *pc = env->pc;
    *cs_base = env->npc;
    flags = cpu_mmu_index(env, false);
#ifndef CONFIG_USER_ONLY
    if (cpu_supervisor_mode(env)) {
        flags |= TB_FLAG_SUPER;
    }
#endif
#ifdef TARGET_SPARC64
#ifndef CONFIG_USER_ONLY
    if (cpu_hypervisor_mode(env)) {
        flags |= TB_FLAG_HYPER;
    }
#endif
    if (env->pstate & PS_AM) {
        flags |= TB_FLAG_AM_ENABLED;
    }
    if ((env->def->features & CPU_FEATURE_FLOAT)
        && (env->pstate & PS_PEF)
        && (env->fprs & FPRS_FEF)) {
        flags |= TB_FLAG_FPU_ENABLED;
    }
    flags |= env->asi << TB_FLAG_ASI_SHIFT;
#else
    if ((env->def->features & CPU_FEATURE_FLOAT) && env->psref) {
        flags |= TB_FLAG_FPU_ENABLED;
    }
#endif
    *pflags = flags;
}

static inline bool tb_fpu_enabled(int tb_flags)
{
#if defined(CONFIG_USER_ONLY)
    return true;
#else
    return tb_flags & TB_FLAG_FPU_ENABLED;
#endif
}

static inline bool tb_am_enabled(int tb_flags)
{
#ifndef TARGET_SPARC64
    return false;
#else
    return tb_flags & TB_FLAG_AM_ENABLED;
#endif
}

#endif
