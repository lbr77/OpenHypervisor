// ohv_context.h - Layout of the user-mapped vCPU context (2 x 16 KiB pages).
//
// The kernel maps one arm_guest_context_t per vCPU into the process. Page 0
// is read/write scratch shared with the hypervisor, page 1 is the read-only
// exit/state page. Field order mirrors the kernel's own guest state layout;
// offsets pinned by static_assert against anchors observed in the framework
// binary and the kernel sources (see PROTOCOL.md).
#ifndef OHV_CONTEXT_H
#define OHV_CONTEXT_H

#include <stdint.h>
#include <stddef.h>
#include "ohv_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OHV_PAGE_SIZE       0x4000ull /* 16 KiB (arm64 Apple) */
#define OHV_CONTEXT_SIZE    (2 * OHV_PAGE_SIZE)
#define OHV_RO_PAGE_OFFSET  OHV_PAGE_SIZE

// Interface version word found at ro.ver: {magic " hyp"}<<32|major<<24|su<<16|minor.
#define OHV_STATE_VER_MAGIC 0x20687970ull
#define OHV_STATE_VER(major, su, minor) \
    ((OHV_STATE_VER_MAGIC << 32) | ((uint64_t)(major) << 24) | ((uint64_t)(su) << 16) | (uint64_t)(minor))

// ---- shared (unbanked) system registers -----------------------------------
typedef struct {
    uint64_t mdscr_el1;
    uint64_t tpidr_el1;
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    uint64_t sp_el0;
    uint64_t sp_el1;
    uint64_t par_el1;
    uint64_t csselr_el1;
    uint64_t apstate;
    uint64_t afpcr_el0;
    uint64_t scxtnum_el0;
    uint64_t tpidr2_el0;
    uint64_t smpri_el1;
} ohv_shared_sysregs_t;

// ---- banked EL1 sysregs ----------------------------------------------------
typedef struct {
    uint64_t ttbr0_el1;
    uint64_t ttbr1_el1;
    uint64_t tcr_el1;
    uint64_t elr_el1;
    uint64_t far_el1;
    uint64_t esr_el1;
    uint64_t mair_el1;
    uint64_t amair_el1;
    uint64_t vbar_el1;
    uint64_t cntv_cval_el0;
    uint64_t cntp_cval_el0;
    uint64_t actlr_el1;
    uint64_t sctlr_el1;
    uint64_t cpacr_el1;
    uint64_t spsr_el1;
    uint64_t afsr0_el1;
    uint64_t afsr1_el1;
    uint64_t contextidr_el1;
    uint64_t cntv_ctl_el0;
    uint64_t cntp_ctl_el0;
    uint64_t cntkctl_el1;
    uint64_t ich_vmcr_el2;   // aggregated view of ICV_*_EL1 RW fields
    uint64_t scxtnum_el1;
    uint64_t smcr_el1;
} ohv_banked_sysregs_t;

// ---- debug registers --------------------------------------------------------
typedef struct {
    struct { uint64_t bvr; uint64_t bcr; } bp[16];
    struct { uint64_t wvr; uint64_t wcr; } wp[16];
    uint64_t mdccint_el1;
    uint64_t osdtrrx_el1;
    uint64_t osdtrtx_el1;
    uint8_t  dbgclaim_el1;
} ohv_dbgregs_t;

// ---- VGIC CPU-interface pair ------------------------------------------------
typedef struct {
    uint64_t ich_ap0r0_el2;
    uint64_t ich_ap1r0_el2;
} ohv_vgic_sysregs_t;

// ---- run controls (RW page copy + RO page mirror) ---------------------------
enum {
    OHV_TIMER_MASK = 1ull << 0,
};
enum {
    OHV_ICH_LR_DIRTY = 1ull << 32,
};
typedef struct {
    uint64_t hcr_el2;
    uint64_t hacr_el2;
    uint64_t cptr_el2;
    uint64_t mdcr_el2;
    uint64_t vmpidr_el2;
    uint64_t vpidr_el2;
    uint64_t virtual_timer_offset;
    uint64_t hfgrtr_el2;
    uint64_t hfgwtr_el2;
    uint64_t hfgitr_el2;
    uint64_t hdfgrtr_el2;
    uint64_t hdfgwtr_el2;
    uint64_t cnthctl_el2;
    uint64_t timer;
    uint64_t vmkeyhi_el2;
    uint64_t vmkeylo_el2;
    uint64_t apsts_el1;
    uint64_t ich_hcr_el2;
    uint64_t ich_lr_el2[8];
    uint64_t hcrx_el2;
} ohv_controls_t;

// ---- frozen bits ------------------------------------------------------------
typedef struct {
    uint64_t actlr_el1; // only EnTSO lockable
} ohv_frozen_t;

// ---- Apple private SPRs (GXF / SPRR / CTRR / PPL keys / GL1 bank / JIT) -----
typedef struct {
    uint64_t amx_state_t_el1;
    uint64_t amx_config_el1;
    uint64_t aspsr_el1;
    uint64_t ctrr_ctl_el1;
    uint64_t ctrr_lock_el1;
    uint64_t ctrr_a_lwr_el1;
    uint64_t ctrr_a_upr_el1;
    uint64_t ctrr_b_lwr_el1;
    uint64_t ctrr_b_upr_el1;
    uint64_t ctrr_c_lwr_el1;
    uint64_t ctrr_c_upr_el1;
    uint64_t ctrr_c_ctl_el1;
    uint64_t ctrr_d_lwr_el1;
    uint64_t ctrr_d_upr_el1;
    uint64_t ctrr_d_ctl_el1;
    uint64_t ctxr_a_lwr_el1;
    uint64_t ctxr_a_upr_el1;
    uint64_t ctxr_a_ctl_el1;
    uint64_t ctxr_b_lwr_el1;
    uint64_t ctxr_b_upr_el1;
    uint64_t ctxr_b_ctl_el1;
    uint64_t ctxr_c_lwr_el1;
    uint64_t ctxr_c_upr_el1;
    uint64_t ctxr_c_ctl_el1;
    uint64_t ctxr_d_lwr_el1;
    uint64_t ctxr_d_upr_el1;
    uint64_t ctxr_d_ctl_el1;
    uint64_t vmsa_lock_el1;
    uint64_t pmcr1_el1;
    uint64_t apctl_el1;
    uint64_t apgakeyhi_el1;
    uint64_t apgakeylo_el1;
    uint64_t apiakeyhi_el1;
    uint64_t apiakeylo_el1;
    uint64_t apibkeyhi_el1;
    uint64_t apibkeylo_el1;
    uint64_t apdakeyhi_el1;
    uint64_t apdakeylo_el1;
    uint64_t apdbkeyhi_el1;
    uint64_t apdbkeylo_el1;
    uint64_t kernkeyhi_el1;
    uint64_t kernkeylo_el1;
    uint64_t gxf_config_el1;
    uint64_t gxf_entry_el1;
    uint64_t gxf_pabentry_el1;
    uint64_t sp_gl1;
    uint64_t tpidr_gl1;
    uint64_t aspsr_gl1;
    uint64_t vbar_gl1;
    uint64_t far_gl1;
    uint64_t esr_gl1;
    uint64_t elr_gl1;
    uint64_t spsr_gl1;
    uint64_t pmcr1_gl1;
    uint64_t afsr1_gl1;
    uint64_t sprr_config_el1;
    uint64_t sprr_amrange_el1;
    uint64_t sprr_pperm_el1;
    uint64_t sprr_uperm_el0;
    uint64_t sprr_pmprr_el1;
    uint64_t sprr_umprr_el1;
    uint64_t sprr_pperm_sh1_el1;
    uint64_t sprr_pperm_sh2_el1;
    uint64_t sprr_pperm_sh3_el1;
    uint64_t sprr_pperm_sh4_el1;
    uint64_t sprr_pperm_sh5_el1;
    uint64_t sprr_pperm_sh6_el1;
    uint64_t sprr_pperm_sh7_el1;
    uint64_t sprr_uperm_sh1_el1;
    uint64_t sprr_uperm_sh2_el1;
    uint64_t sprr_uperm_sh3_el1;
    uint64_t sprr_uperm_sh4_el1;
    uint64_t sprr_uperm_sh5_el1;
    uint64_t sprr_uperm_sh6_el1;
    uint64_t sprr_uperm_sh7_el1;
    uint64_t acfg_el1;
    uint64_t jrange_el1;
    uint64_t jctl_el1;
    uint64_t japiakeyhi_el1;
    uint64_t japiakeylo_el1;
    uint64_t japibkeyhi_el1;
    uint64_t japibkeylo_el1;
    uint64_t agtcntrdir_el1;
} ohv_extregs_t;

// Anchors from the framework binary (see PROTOCOL.md §2):
// Binary-verified anchors (framework internals, PROTOCOL.md section 2):
#define OHV_RW_REGS_X0          0x000
#define OHV_RW_FP               0x0e8
#define OHV_RW_LR               0x0f0
#define OHV_RW_SP               0x0f8
#define OHV_RW_PC               0x100
#define OHV_RW_CPSR             0x108
#define OHV_RW_NEON_Q0          0x140
#define OHV_RW_FPSR             0x340
#define OHV_RW_FPCR             0x344
#define OHV_RW_SHARED_SYSREGS   0x350
#define OHV_RW_BANKED_SYSREGS   0x3b8
#define OHV_RW_DBGREGS          0x478
#define OHV_RW_VGIC_SYSREGS     0x698
// Controls base measured twice independently: public-field table entry 5
// lands on vpidr at +0x28 (NESTED.md's field-5 MIDR anchor) and entry 17 on
// hcrx at +0xd0 -- both match the kernel's sequential controls order from
// this base.  The vtimer words sit inside it at their kernel slots.
#define OHV_RW_CONTROLS         0x920
#define OHV_RW_VTIMER_OFFSET    0x950   /* controls + 0x30 */
#define OHV_RW_TIMER            0x988   /* controls + 0x68 */
#define OHV_RW_FROZEN           0x9f8   /* controls end exactly at the state word */
#define OHV_RW_STATE_DIRTY      0xa00
#define OHV_RW_GUEST_TICKS      0xa08
#define OHV_RW_EXTREGS          0xa10
#define OHV_RW_VNCR             0x1000  /* 4 KiB aligned inside the 16 KiB page */
#define OHV_RW_AVNCR            0x2000

#define OHV_RO_VER              0x4000
#define OHV_RO_EXIT             0x4008
#define OHV_RO_EXIT_REASON      0x4008
#define OHV_RO_EXIT_ESR         0x400c
#define OHV_RO_EXIT_INSTR       0x4010
#define OHV_RO_EXIT_FAR         0x4018
#define OHV_RO_EXIT_HPFAR       0x4020
#define OHV_RO_CONTROLS         0x4028  /* framework mirrors controls here too */
#define OHV_RO_STATE_VALID      0x4100
#define OHV_RO_STATE_DIRTY      0x4108
#define OHV_RO_STATE_USED       0x4110
#define OHV_RO_ICH_VTR          0x4118
#define OHV_RO_ICH_MISR         0x411c
#define OHV_RO_ICH_ELRSR        0x4120
#define OHV_RO_SVCR             0x4128
#define OHV_RO_SVL_B            0x4130
#define OHV_RO_SME_PTR          0x4138
#define OHV_RO_AMX_PTR          0x4140

// state_dirty / state_used bits
enum {
    OHV_STATE_SYSREGS      = 1ull << 0,
    OHV_STATE_DEBUG        = 1ull << 1,
    OHV_STATE_CONTROLS     = 1ull << 2,
    OHV_STATE_GIC          = 1ull << 3,
    OHV_STATE_SME_CONTEXT  = 1ull << 4,
    OHV_STATE_AMX_CONTEXT  = 1ull << 54,
    OHV_STATE_APPLE_GENERIC_TIMER = 1ull << 55,
    OHV_STATE_JITBOX       = 1ull << 56,
    OHV_STATE_AMX          = 1ull << 57,
    OHV_STATE_GXF          = 1ull << 58,
    OHV_STATE_SPRR         = 1ull << 59,
    OHV_STATE_PTRAUTH_APPLE= 1ull << 60,
    OHV_STATE_PTRAUTH_ARM  = 1ull << 61,
    OHV_STATE_CTRR         = 1ull << 62,
    OHV_STATE_APPLE        = 1ull << 63,
};

// Typed views over the raw mapped pages.
typedef struct {
    struct {
        uint64_t x[29];      // x0 @ 0x000 .. x28 @ 0xe0
        uint64_t fp;         // 0xe8
        uint64_t lr;         // 0xf0
        uint64_t sp;         // 0xf8
        uint64_t pc;         // 0x100
        uint32_t cpsr;       // 0x108
        uint32_t pad;
    } regs;
    uint8_t __pad_to_neon[OHV_RW_NEON_Q0 - 0x110];
    struct {
        __uint128_t q[32];   // 0x140 .. 0x33f
        uint32_t fpsr;       // 0x340
        uint32_t fpcr;       // 0x344
        uint32_t pad[2];
    } neon;
    ohv_shared_sysregs_t shared_sysregs;  // 0x350 (saved/restored every entry)
    ohv_banked_sysregs_t banked_sysregs;  // 0x3b8 (dirty-tracked, opportunistic)
    ohv_dbgregs_t dbgregs;                // 0x478
    ohv_vgic_sysregs_t vgic_sysregs;      // 0x698
    uint8_t __pad_to_controls[OHV_RW_CONTROLS - (OHV_RW_VGIC_SYSREGS + sizeof(ohv_vgic_sysregs_t))];
    ohv_controls_t controls;              // 0x8e8 (timer word at 0x950)
    ohv_frozen_t frozen;                  // 0x9c0
    static_assert(OHV_RW_FROZEN + sizeof(ohv_frozen_t) <= OHV_RW_STATE_DIRTY, "frozen fits");
    uint8_t __pad_to_state_dirty[OHV_RW_STATE_DIRTY - (OHV_RW_FROZEN + sizeof(ohv_frozen_t))];
    volatile uint64_t state_dirty;        // 0xa00
    uint64_t guest_tick_count;            // 0xa08
    ohv_extregs_t extregs;                // 0xa10
    uint8_t __pad_to_vncr[OHV_RW_VNCR - (OHV_RW_EXTREGS + sizeof(ohv_extregs_t))];
} ohv_rw_page_head_t; // RW page head; vncr/avncr accessed by raw offset

typedef struct {
    uint32_t vmexit_reason;
    uint32_t vmexit_esr;
    uint32_t vmexit_instr;
    uint32_t __pad;
    uint64_t vmexit_far;
    uint64_t vmexit_hpfar;
} ohv_vmexit_info_t;

typedef struct {
    uint64_t ver;
    ohv_vmexit_info_t exit;
    ohv_controls_t controls;
    volatile uint64_t state_valid;
    volatile uint64_t state_dirty;
    volatile uint64_t state_used;
    uint32_t ich_vtr_el2;
    uint32_t ich_misr_el2;
    uint32_t ich_elrsr_el2;
    uint32_t __pad0;
    uint64_t svcr;
    uint16_t svl_b;
    uint16_t padding[3];
    void *sme;
    void *amx;
} ohv_ro_page_t;

// Compile-time pinning of the load-bearing offsets.
_Static_assert(offsetof(ohv_rw_page_head_t, regs.x[0]) == OHV_RW_REGS_X0, "x0 offset");
_Static_assert(offsetof(ohv_rw_page_head_t, regs.fp) == OHV_RW_FP, "fp offset");
_Static_assert(offsetof(ohv_rw_page_head_t, regs.pc) == OHV_RW_PC, "pc offset");
_Static_assert(offsetof(ohv_rw_page_head_t, regs.cpsr) == OHV_RW_CPSR, "cpsr offset");
_Static_assert(offsetof(ohv_rw_page_head_t, neon.q[0]) == OHV_RW_NEON_Q0, "q0 offset");
_Static_assert(offsetof(ohv_rw_page_head_t, neon.q[0]) == OHV_RW_NEON_Q0, "q0 offset");
_Static_assert(offsetof(ohv_rw_page_head_t, neon.fpsr) == OHV_RW_FPSR, "fpsr offset");
_Static_assert(offsetof(ohv_rw_page_head_t, neon.fpcr) == OHV_RW_FPCR, "fpcr offset");
_Static_assert(offsetof(ohv_rw_page_head_t, shared_sysregs) == OHV_RW_SHARED_SYSREGS, "shared base");
_Static_assert(offsetof(ohv_rw_page_head_t, banked_sysregs) == OHV_RW_BANKED_SYSREGS, "banked base");
_Static_assert(offsetof(ohv_rw_page_head_t, dbgregs) == OHV_RW_DBGREGS, "dbg base");
_Static_assert(offsetof(ohv_rw_page_head_t, vgic_sysregs) == OHV_RW_VGIC_SYSREGS, "vgic base");
_Static_assert(offsetof(ohv_rw_page_head_t, controls) == OHV_RW_CONTROLS, "controls base");
_Static_assert(offsetof(ohv_rw_page_head_t, controls) + offsetof(ohv_controls_t, timer) == OHV_RW_TIMER, "timer anchor");
_Static_assert(offsetof(ohv_rw_page_head_t, guest_tick_count) == OHV_RW_GUEST_TICKS, "ticks anchor");
_Static_assert(offsetof(ohv_rw_page_head_t, state_dirty) == OHV_RW_STATE_DIRTY, "state_dirty anchor");
_Static_assert(offsetof(ohv_rw_page_head_t, extregs) == OHV_RW_EXTREGS, "extregs base");
_Static_assert(offsetof(ohv_extregs_t, gxf_entry_el1) == 0xb68 - OHV_RW_EXTREGS, "gxf_entry anchor (HANDOFF)");
_Static_assert(offsetof(ohv_extregs_t, sp_gl1) == 0xb78 - OHV_RW_EXTREGS, "sp_gl1 anchor (HANDOFF)");
_Static_assert(offsetof(ohv_extregs_t, spsr_gl1) == 0xbb0 - OHV_RW_EXTREGS, "spsr_gl1 anchor (HANDOFF)");
_Static_assert(sizeof(ohv_extregs_t) == 83 * 8, "extregs size");

// In-page accessors (all bounds-checked callers-side).
static inline volatile uint8_t *ohv_rw_byte(void *ctx, uint64_t off) { return (volatile uint8_t *)((uint8_t *)ctx + off); }
static inline volatile uint64_t *ohv_rw_u64(void *ctx, uint64_t off) { return (volatile uint64_t *)((uint8_t *)ctx + off); }
static inline volatile uint32_t *ohv_rw_u32(void *ctx, uint64_t off) { return (volatile uint32_t *)((uint8_t *)ctx + off); }
static inline volatile uint64_t *ohv_ro_u64(void *ctx, uint64_t off) { return (volatile uint64_t *)((uint8_t *)ctx + off); }
static inline volatile uint32_t *ohv_ro_u32(void *ctx, uint64_t off) { return (volatile uint32_t *)((uint8_t *)ctx + off); }

static inline ohv_rw_page_head_t *ohv_rw(void *ctx) { return (ohv_rw_page_head_t *)ctx; }
static inline ohv_vmexit_info_t *ohv_exit_info(void *ctx) { return (ohv_vmexit_info_t *)((uint8_t *)ctx + OHV_RO_EXIT); }
static inline ohv_ro_page_t *ohv_ro(void *ctx) { return (ohv_ro_page_t *)((uint8_t *)ctx + OHV_RO_PAGE_OFFSET); }
static inline ohv_controls_t *ohv_rw_controls(void *ctx) { return &ohv_rw(ctx)->controls; }
static inline bool ohv_ver_ok(void *ctx, uint64_t expect) { return *(volatile uint64_t *)((uint8_t *)ctx + OHV_RO_VER) == expect; }

#ifdef __cplusplus
}
#endif
#endif // OHV_CONTEXT_H