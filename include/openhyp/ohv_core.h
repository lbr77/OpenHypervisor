// ohv_core.h - OpenHypervisor core types: traps, errors, ISA, exit reasons.
//
// Clean-room reimplementation of the Hypervisor.framework userspace/kernel
// protocol for arm64 macOS. See PROTOCOL.md for derivation.
#ifndef OHV_CORE_H
#define OHV_CORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- errors --
// Same numeric contract as the framework's return codes.
typedef int hv_return_t;

#define OHV_ERR_BASE 0xfae94000u
enum {
    HV_SUCCESS             = 0,
    HV_ERROR               = (int)(OHV_ERR_BASE | 0x01),
    HV_BUSY                = (int)(OHV_ERR_BASE | 0x02),
    HV_BAD_ARGUMENT        = (int)(OHV_ERR_BASE | 0x03),
    HV_ILLEGAL_GUEST_STATE = (int)(OHV_ERR_BASE | 0x04),
    HV_NO_RESOURCES        = (int)(OHV_ERR_BASE | 0x05),
    HV_NO_DEVICE           = (int)(OHV_ERR_BASE | 0x06),
    HV_DENIED              = (int)(OHV_ERR_BASE | 0x07),
    HV_FAULT               = (int)(OHV_ERR_BASE | 0x08),
    HV_UNSUPPORTED         = (int)(OHV_ERR_BASE | 0x0f),
};

#define _HV_VM_QUOTA 0xfae94fffull /* internal kernel quota marker */

// ----------------------------------------------------------------- traps --
// One supervisor call carries every request: x0 = trap id, x1 = arg pointer.
enum ohv_trap_id {
    OHV_TRAP_CAPABILITIES          = 0,
    OHV_TRAP_VM_CREATE             = 1,
    OHV_TRAP_VM_DESTROY            = 2,
    OHV_TRAP_VM_MAP                = 3,
    OHV_TRAP_VM_UNMAP              = 4,
    OHV_TRAP_VM_PROTECT            = 5,
    OHV_TRAP_VCPU_CREATE           = 6,
    OHV_TRAP_VCPU_DESTROY          = 7,
    OHV_TRAP_VCPU_SYSREGS_SYNC     = 8,  /* arg unused (NULL passes) */
    OHV_TRAP_VCPU_RUN              = 9,
    OHV_TRAP_VCPU_RUN_CANCEL       = 10, /* arg: uint64_t vcpu-id bitmask */
    OHV_TRAP_VCPU_SET_ADDRESS_SPACE= 11,
    OHV_TRAP_VM_ADDRESS_SPACE_CREATE = 12,
    OHV_TRAP_VM_INVALIDATE_TLB     = 13,
    OHV_TRAP_VM_STAGE1_TLB_OP      = 14,
    OHV_TRAP_VCPU_SET_SVCR         = 15,
    OHV_TRAP_VCPU_AMX_PREPARE      = 16,
    OHV_TRAP_VM_MONITOR_DATA_ABORT = 17,
    OHV_TRAP_COUNT                 = 18,
};

// Raw escape hatch: issue any hypervisor trap directly. Extended/private
// kernels may number further traps past OHV_TRAP_COUNT.
hv_return_t ohv_raw_trap(unsigned trap_id, void *arg);

// Trap argument structures (binary layout fixed by the kernel).
typedef struct __attribute__((packed)) {
    uint64_t min_ipa;
    uint64_t ipa_size;
    uint32_t granule;
    uint32_t flags;
    uint32_t isa;
    uint64_t __future[8]; /* newer kernels copyin their full sizeof */
} ohv_vm_create_t;

typedef struct __attribute__((packed)) {
    uint64_t uva;
    uint64_t ipa;
    uint64_t size;
    uint64_t flags;
    uint64_t asid;
} ohv_vm_map_item_t;

typedef struct __attribute__((packed)) {
    uint64_t id;        /* in: requested vcpu id */
    uint64_t interface; /* out: user-mapped context (2 x 16 KiB pages) */
} ohv_vcpu_create_t;

typedef struct __attribute__((packed)) {
    uint64_t min_ipa;
    uint64_t ipa_size;
    uint32_t granule;
    uint32_t flags;
    uint64_t out_asid;
    uint64_t __future[8];
} ohv_vm_addrspace_create_t;

typedef struct __attribute__((packed)) {
    uint64_t asid;
    uint64_t ipa;
    uint64_t nbytes;
} ohv_vm_tlbi_item_t;

typedef struct __attribute__((packed)) {
    uint64_t asid;
    uint64_t op;
    uint64_t param;
} ohv_vm_stage1_tlb_op_t;

typedef struct __attribute__((packed)) {
    uint64_t cmd;          /* 0=add, 1=remove */
    uint64_t asid;
    uint64_t context;
    uint64_t base_ipa;
    uint64_t size;
    uint64_t access_type;
    uint32_t port_name;
} ohv_vm_monitor_data_abort_t;

typedef struct __attribute__((packed)) {
    /*
     * What trap 0 copies out starts with a header the fields below do not
     * describe: the first word counts something the kernel version decides,
     * and the rest reads as zero here.  It was left out when this was first
     * written, which put every named field 0x40 bytes early -- control_hcr
     * landed on the header, ctr_el0 on a CCSIDR entry, the ID registers on
     * each other -- so the one caller that needed control_hcr reached it by
     * raw offset instead, and everything else quietly read the wrong word.
     */
    uint64_t __header[8];
    uint64_t control_hcr;
    uint64_t control_hacr;
    uint64_t control_cptr;
    uint64_t control_mdcr;
    uint64_t control_ich_hcr;
    uint64_t control_timer;
    uint64_t control_apsts;
    uint64_t control_hfgrtr;
    uint64_t control_hfgwtr;
    uint64_t control_hfgitr;
    uint64_t control_hdfgrtr;
    uint64_t control_hdfgwtr;
    uint64_t control_cnthctl;
    uint64_t actlr_el1;
    uint64_t ctr_el0;
    uint64_t dczid_el0;
    uint64_t clidr_el1;
    uint64_t ccsidr_el1_inst[8];
    uint64_t ccsidr_el1_data_or_unified[8];
    uint64_t id_aa64dfr0_el1;
    uint64_t id_aa64dfr1_el1;
    uint64_t id_aa64isar0_el1;
    uint64_t id_aa64isar1_el1;
    uint64_t id_aa64mmfr0_el1;
    uint64_t id_aa64mmfr1_el1;
    uint64_t id_aa64mmfr2_el1;
    uint64_t id_aa64pfr0_el1;
    uint64_t id_aa64pfr1_el1;
    uint64_t id_aa64smfr0_el1;
    uint64_t id_aa64zfr0_el1;
    uint8_t  gic_npie_active_pending_bug;
    uint64_t ipa_bits_4k;
    uint64_t ipa_bits_16k;
    uint16_t svl_b_max;
    // Newer kernels append fields and copyout their full sizeof; keep a
    // generous tail so the copy always lands inside our buffer.
    uint8_t __future[512];
} ohv_capabilities_t;

// ------------------------------------------------------------------- ISA --
enum ohv_vm_isa {
    OHV_VM_ISA_NONE               = 0,
    OHV_VM_ISA_GENERIC            = 1,
    OHV_VM_ISA_APPLE_COPROCESSOR  = 2,
    OHV_VM_ISA_APPLE              = 3,
    OHV_VM_ISA_INTERNAL           = 4,
};

// ------------------------------------------------- nested virtualisation --
/*
 * Guest EL2 is FEAT_NV.  The guest never reaches hardware EL2: it runs at EL1
 * and is told it is at EL2 -- CurrentEL and the EL2 registers are answered for
 * it, through the VNCR page with NV2 -- while PSTATE.M names EL1 throughout.
 *
 * These are also the three bits the framework tests before it accepts
 * el2_enabled at all: it reads the mask of settable HCR_EL2 bits out of the
 * capabilities and requires every one of them.
 */
#define OHV_HCR_NV                (1ull << 42)
#define OHV_HCR_NV1               (1ull << 43)
#define OHV_HCR_NV2               (1ull << 45)
#define OHV_HCR_E2H               (1ull << 34)
#define OHV_HCR_TIDCP             (1ull << 20)
#define OHV_HCR_TWE               (1ull << 14)
#define OHV_HCR_FMO  (1ull << 3)
#define OHV_HCR_IMO  (1ull << 4)
#define OHV_HCR_AMO  (1ull << 5)
#define OHV_HCR_TID3              (1ull << 18)
#define OHV_HCR_TSC               (1ull << 19)
/* HACR_EL2 is Apple's, and this is what the framework runs its vCPUs with. */
#define OHV_HACR_APPLE_DEFAULT    0x0100000000000000ull
/*
 * Trap the guarded execution registers.  The kernel's own default has this on
 * and lists it as overridable, so a VMM that hands it a HACR without the bit
 * turns the traps off -- which is what leaves SPTM's MSR ELR_GL1 / SPSR_GL1
 * invisible, and with them invisible the ERET that follows returns to whatever
 * the guest's EL2 pair happens to hold.
 */
#define OHV_HACR_TGXF             (1ull << 13)
/*
 * NV and NV2, and deliberately not NV1 -- which is what the framework runs
 * this machine's guest hypervisor with: its HCR_EL2 reads 0x00202704021c0000,
 * and bit 43 is clear in it.
 *
 * NV1 does change what the guest sees: with it on, the boot ROM's write to
 * VBAR_EL2 stops arriving as a trapped register access and the core runs on
 * past the point it otherwise dies at.  That is not evidence for it.  The
 * guarded-level probe, which is the thing that actually exercises guest EL2,
 * stops working entirely with NV1 on -- its guest takes an exception straight
 * after setting its vector base and never reaches GENTER -- and the framework
 * gets the same redirection without it.  Whatever is missing here is missing
 * somewhere else.
 */
#define OHV_HCR_NESTED            (OHV_HCR_NV | OHV_HCR_NV2)

/* The settable-HCR mask inside what trap 0 copies out
 * (Arm::HypervisorCapabilities::get_control_hcr reads it at this offset).
 * Now that the header is declared this is just where the field is, and the
 * static assert below is what keeps the two from drifting apart again. */
#define OHV_CAPS_CONTROL_HCR_OFF  0x40

// ----------------------------------------------------------- vmexit info --
// Kernel-side exit classification, found in the read-only context page.
enum ohv_vmexit_reason {
    OHV_VMEXIT_NONE            = 0,
    OHV_VMEXIT_SYNC            = 1,
    OHV_VMEXIT_SERROR          = 2,
    OHV_VMEXIT_IRQ             = 3,
    OHV_VMEXIT_FIQ             = 4,
    OHV_VMEXIT_HANDLED_FAULT   = 5,
    OHV_VMEXIT_UNHANDLED_FAULT = 6,
    OHV_VMEXIT_UNKNOWN_TRAP    = 7,
    OHV_VMEXIT_MSR_TRAP        = 8,
    OHV_VMEXIT_INTERRUPTED     = 9,
    OHV_VMEXIT_ILLEGAL_ERET    = 10,
    OHV_VMEXIT_HOST_AST        = 11,
    OHV_VMEXIT_VGIC            = 12,
    OHV_VMEXIT_SME_TRAP        = 13,
    OHV_VMEXIT_PRIVATE         = 1u << 31,
    OHV_VMEXIT_AMX_DISABLED    = (int)(unsigned)OHV_VMEXIT_PRIVATE,
};

#ifdef __cplusplus
}
#endif
#endif // OHV_CORE_H