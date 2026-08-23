// ohv_vcpu.cpp - vCPU lifecycle, register access, run loop glue.
#include <mach/mach_time.h>
#include "ohv_internal.h"
#include "openhyp/ohv_trap.h"

using namespace ohv;

extern "C" uint64_t ohv_caps_field(const ohv_capabilities_t *, uint16_t enc);

struct hv_vcpu_config_s {
    uint64_t ccsidr_ptr[2];   // +16/+24 in framework layout
    uint64_t spare;           // +32 u16 observed; keep opaque
    bool fgt_enabled;
    bool tlbi_workaround;
    uint64_t vmkey;
    uint8_t pad[7];
};

static pthread_mutex_t g_cfg_mutex = PTHREAD_MUTEX_INITIALIZER;

// ------------------------------------------------------------- ownership --
namespace ohv {
VcpuSlot *owned_vcpu(hv_vcpu_t id) {
    VcpuSlot *s = find_vcpu(id);
    if (!s) return nullptr;
    // The framework binds a vcpu to its creating thread for state safety.
    if (tl_current_vcpu && tl_current_vcpu != s) return nullptr;
    return s;
}

} // namespace ohv

using namespace ohv;

static void exit_from_ro(VcpuSlot *s) {
    // Translate kernel vmexit classification into the public exit record.
    volatile ohv_vmexit_info_t *e = ohv_exit_info(s->ctx);
    uint32_t reason = e->vmexit_reason;
    hv_vcpu_exit_t *out = &s->pub_exit;
    out->exception.syndrome = e->vmexit_esr;
    out->exception.virtual_address = e->vmexit_far;
    uint64_t hpfar = e->vmexit_hpfar;
    out->exception.physical_address =
        ((hpfar << 8) & ~0xfffull) | (e->vmexit_far & 0xfff);
    switch (reason) {
        case OHV_VMEXIT_INTERRUPTED:
        case OHV_VMEXIT_HOST_AST:
            out->reason = HV_EXIT_REASON_CANCELED;
            break;
        case OHV_VMEXIT_HANDLED_FAULT: {
            // WFI-class handled faults return to us as normal exits only when
            // the vtimer fired; otherwise keep running is kernel-side, so this
            // surfaces as an exception with the recorded ESR.
            if (*ohv_rw_u64(s->ctx, OHV_RO_CONTROLS + offsetof(ohv_controls_t, timer)) & OHV_TIMER_MASK)
                out->reason = HV_EXIT_REASON_VTIMER_ACTIVATED;
            else
                out->reason = HV_EXIT_REASON_EXCEPTION;
            break;
        }
        case OHV_VMEXIT_SYNC:
        case OHV_VMEXIT_SERROR:
        case OHV_VMEXIT_IRQ:
        case OHV_VMEXIT_FIQ:
        case OHV_VMEXIT_UNHANDLED_FAULT:
        case OHV_VMEXIT_UNKNOWN_TRAP:
        case OHV_VMEXIT_MSR_TRAP:
        case OHV_VMEXIT_ILLEGAL_ERET:
        case OHV_VMEXIT_VGIC:
        case OHV_VMEXIT_SME_TRAP:
            out->reason = HV_EXIT_REASON_EXCEPTION;
            break;
        default:
            out->reason = HV_EXIT_REASON_UNKNOWN;
            break;
    }
}

// ------------------------------------------------------------ create/free --
extern "C" hv_return_t hv_vcpu_create(hv_vcpu_t *vcpu_out, hv_vcpu_exit_t **exit, hv_vcpu_config_t config) {
    if (!vcpu_out || !exit) return HV_BAD_ARGUMENT;
    if (!g_vm_alive) return HV_NO_DEVICE;
    if (tl_current_vcpu) return HV_BUSY; // one vcpu per thread

    pthread_mutex_lock(&g_vcpus_mutex);
    hv_vcpu_t id = kMaxVcpuIds;
    for (uint64_t i = 0; i < kMaxVcpuIds; i++)
        if (!g_vcpus[i].used) { id = i; break; }
    if (id == kMaxVcpuIds) { pthread_mutex_unlock(&g_vcpus_mutex); return HV_NO_RESOURCES; }

    ohv_vcpu_create_t args{ id, 0 };
    hv_return_t r = ohv_raw_trap(OHV_TRAP_VCPU_CREATE, &args);
    if (r != HV_SUCCESS) { pthread_mutex_unlock(&g_vcpus_mutex); return r; }

    void *ctx = (void *)(uintptr_t)args.interface;
    uint64_t ver = *(volatile uint64_t *)((uint8_t *)ctx + OHV_RO_VER);
    if ((ver >> 32) != OHV_STATE_VER_MAGIC) {
        ohv_raw_trap(OHV_TRAP_VCPU_DESTROY, nullptr);
        pthread_mutex_unlock(&g_vcpus_mutex);
        return HV_ERROR;
    }

    VcpuSlot &s = g_vcpus[id];
    s.used = true;
    s.ctx = ctx;
    s.id = id;
    s.owner = pthread_self();
    s.run_count = 0;
    tl_current_vcpu = &s;
    *vcpu_out = id;
    *exit = &s.pub_exit;
    pthread_mutex_unlock(&g_vcpus_mutex);
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_destroy(hv_vcpu_t id) {
    VcpuSlot *s = find_vcpu(id);
    if (!s) return HV_BAD_ARGUMENT;
    hv_return_t r = ohv_raw_trap(OHV_TRAP_VCPU_DESTROY, nullptr);
    pthread_mutex_lock(&g_vcpus_mutex);
    if (tl_current_vcpu == s) tl_current_vcpu = nullptr;
    s = nullptr;
    g_vcpus[id] = VcpuSlot{};
    pthread_mutex_unlock(&g_vcpus_mutex);
    return r;
}

// ------------------------------------------------------------- GPR / SIMD --
extern "C" hv_return_t hv_vcpu_get_reg(hv_vcpu_t id, hv_reg_t reg, uint64_t *value) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || !value) return HV_BAD_ARGUMENT;
    ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
    switch (reg) {
        case HV_REG_CPSR: *value = rw->regs.cpsr; break;
        case HV_REG_FPCR: *value = rw->neon.fpcr; break;
        case HV_REG_FPSR: *value = rw->neon.fpsr; break;
        default: {
            unsigned n = (unsigned)reg;
            switch (n) {
                case 29: *value = rw->regs.fp; break;
                case 30: *value = rw->regs.lr; break;
                case 31: *value = rw->regs.pc; break;
                default:
                    if (n > 31) return HV_BAD_ARGUMENT;
                    *value = rw->regs.x[n];
            }
        }
    }
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_set_reg(hv_vcpu_t id, hv_reg_t reg, uint64_t value) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s) return HV_BAD_ARGUMENT;
    ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
    switch (reg) {
        case HV_REG_CPSR: rw->regs.cpsr = (uint32_t)value; break;
        case HV_REG_FPCR: rw->neon.fpcr = (uint32_t)value; break;
        case HV_REG_FPSR: rw->neon.fpsr = (uint32_t)value; break;
        default: {
            unsigned n = (unsigned)reg;
            switch (n) {
                case 29: rw->regs.fp = value; break;
                case 30: rw->regs.lr = value; break;
                case 31: rw->regs.pc = value; break;
                default:
                    if (n > 31) return HV_BAD_ARGUMENT;
                    rw->regs.x[n] = value;
            }
        }
    }
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_get_simd_fp_reg(hv_vcpu_t id, hv_simd_fp_reg_t reg, hv_simd_fp_uchar16_t *value) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || !value || (unsigned)reg > 31) return HV_BAD_ARGUMENT;
    __builtin_memcpy(value, (void *)&ohv_rw(s->ctx)->neon.q[(unsigned)reg], 16);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_simd_fp_reg(hv_vcpu_t id, hv_simd_fp_reg_t reg, hv_simd_fp_uchar16_t value) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || (unsigned)reg > 31) return HV_BAD_ARGUMENT;
    __builtin_memcpy((void *)&ohv_rw(s->ctx)->neon.q[(unsigned)reg], &value, 16);
    return HV_SUCCESS;
}

// ---------------------------------------------------------------- sysreg --
static hv_return_t sysreg_access(hv_vcpu_t id, uint16_t enc, uint64_t *value, bool write) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || !value) return HV_BAD_ARGUMENT;
    const SysRegDesc *d = sysreg_lookup(enc);
    if (!d) return HV_UNSUPPORTED;
    uint8_t *base = (uint8_t *)s->ctx;
    // Generic slot resolution by region base + index.
    uint64_t region = 0;
    switch (d->kind) {
        case 0: region = OHV_RW_BANKED_SYSREGS; break;
        case 1: region = OHV_RW_SHARED_SYSREGS; break;
        case 2: region = OHV_RW_EXTREGS; break;
        case 3: region = OHV_RW_DBGREGS; break;
        case 5: { // read-only machine/id registers from capabilities cache
            static ohv_capabilities_t caps{};
            static bool have = false;
            if (!have) { ohv_raw_trap(OHV_TRAP_CAPABILITIES, &caps); have = true; }
            // serve from the capabilities snapshot by name hash
            *value = ohv_caps_field(&caps, enc);
            return HV_SUCCESS;
        }
        default: return HV_UNSUPPORTED;
    }
    volatile uint64_t *p = (volatile uint64_t *)(base + region + (uint64_t)d->index * 8);
    if (d->sync_before && write) {
        hv_return_t r = ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);
        if (r != HV_SUCCESS) return r;
    }
    if (write) {
        *p = *value;
        if (d->dirty_bit) mark_dirty(s->ctx, d->dirty_bit);
    } else {
        *value = *p;
    }
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_get_sys_reg(hv_vcpu_t id, hv_sys_reg_t reg, uint64_t *value) {
    return sysreg_access(id, (uint16_t)reg, value, false);
}
extern "C" hv_return_t hv_vcpu_set_sys_reg(hv_vcpu_t id, hv_sys_reg_t reg, uint64_t value) {
    return sysreg_access(id, (uint16_t)reg, &value, true);
}

// --------------------------------------------- interrupts / timers / misc --
extern "C" hv_return_t hv_vcpu_get_serror(hv_vcpu_t id, bool *pending) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || !pending) return HV_BAD_ARGUMENT;
    *pending = ohv_exit_info(s->ctx)->vmexit_reason == OHV_VMEXIT_SERROR;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_serror(hv_vcpu_t, bool) {
    // Injection path requires the VGIC model; see ohv_gic.cpp notes.
    return HV_UNSUPPORTED;
}

extern "C" hv_return_t hv_vcpu_get_pending_interrupt(hv_vcpu_t id, hv_interrupt_type_t type, bool *pending) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || !pending) return HV_BAD_ARGUMENT;
    uint32_t reason = ohv_exit_info(s->ctx)->vmexit_reason;
    *pending = type == HV_INTERRUPT_TYPE_IRQ ? reason == OHV_VMEXIT_IRQ : reason == OHV_VMEXIT_FIQ;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_pending_interrupt(hv_vcpu_t, hv_interrupt_type_t, bool) {
    return HV_UNSUPPORTED; // routed through the GIC model when enabled
}

extern "C" hv_return_t hv_vcpu_get_trap_debug_exceptions(hv_vcpu_t id, bool *v) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    *v = (*ohv_rw_u64(s->ctx, OHV_RW_CONTROLS + 0x20) >> 14) & 1; // mdcr via ctrl map idx3
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_trap_debug_exceptions(hv_vcpu_t id, bool on) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    volatile uint64_t *mdcr = ohv_rw_u64(s->ctx, OHV_RW_CONTROLS + 0x20);
    *mdcr = on ? (*mdcr | (1ull << 14)) : (*mdcr & ~(1ull << 14));
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_trap_debug_reg_accesses(hv_vcpu_t id, bool *v) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    *v = (*ohv_rw_u64(s->ctx, OHV_RW_CONTROLS + 0x20) >> 9) & 1; // TDA|TDOSA|TDRA collapsed
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_trap_debug_reg_accesses(hv_vcpu_t id, bool on) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    volatile uint64_t *mdcr = ohv_rw_u64(s->ctx, OHV_RW_CONTROLS + 0x20);
    const uint64_t mask = (1ull << 9) | (1ull << 10) | (1ull << 12);
    *mdcr = on ? (*mdcr | mask) : (*mdcr & ~mask);
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_get_exec_time(hv_vcpu_t id, uint64_t *t) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !t) return HV_BAD_ARGUMENT;
    *t = *ohv_rw_u64(s->ctx, OHV_RW_GUEST_TICKS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_wait_for_interrupt_time(hv_vcpu_t id, uint64_t *t) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !t) return HV_BAD_ARGUMENT;
    *t = s->wfi_ticks;
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_get_vtimer_mask(hv_vcpu_t id, bool *masked) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !masked) return HV_BAD_ARGUMENT;
    *masked = (*ohv_rw_u64(s->ctx, OHV_RW_TIMER) & OHV_TIMER_MASK) != 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_vtimer_mask(hv_vcpu_t id, bool masked) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    volatile uint64_t *t = ohv_rw_u64(s->ctx, OHV_RW_TIMER);
    *t = masked ? (*t | OHV_TIMER_MASK) : (*t & ~OHV_TIMER_MASK);
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_vtimer_offset(hv_vcpu_t id, uint64_t *off) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !off) return HV_BAD_ARGUMENT;
    *off = *ohv_rw_u64(s->ctx, OHV_RW_VTIMER_OFFSET);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_vtimer_offset(hv_vcpu_t id, uint64_t off) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    *ohv_rw_u64(s->ctx, OHV_RW_VTIMER_OFFSET) = off;
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_invalidate_tlb(hv_vcpu_t id, hv_tlbi_op_t op, uint64_t param) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    ohv_vm_stage1_tlb_op_t a{ 0, (uint64_t)op, param };
    return ohv_raw_trap(OHV_TRAP_VM_STAGE1_TLB_OP, &a);
}

// -------------------------------------------------------------------- run --
extern "C" hv_return_t hv_vcpu_run(hv_vcpu_t id) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s) return HV_BAD_ARGUMENT;
    uint64_t t0 = mach_absolute_time();
    hv_return_t r = ohv_raw_trap(OHV_TRAP_VCPU_RUN, nullptr);
    uint64_t t1 = mach_absolute_time();
    s->wfi_ticks += (t1 - t0); // coarse: full run window; refined below when ESR=WFI
    s->run_count++;
    exit_from_ro(s);
    return r;
}

extern "C" hv_return_t hv_vcpus_exit(hv_vcpu_t *vcpus, uint32_t count) {
    if (!vcpus && count) return HV_BAD_ARGUMENT;
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (vcpus[i] >= kMaxVcpuIds) return HV_BAD_ARGUMENT;
        mask |= 1ull << vcpus[i];
    }
    return ohv_raw_trap(OHV_TRAP_VCPU_RUN_CANCEL, (void *)mask);
}

// -------------------------------------------------- private control/ext API --
/*
 * Apple's public control-field index -> offset from the controls base, and
 * the validity mask the framework applies (0x3FBEF: 9 and 15 invalid), both
 * lifted from Hv::Vcpu::get/set_control_field.  The order is the framework's
 * own and does not follow the kernel's member sequence.
 */
static const uint64_t kCtrlMap[18] = {
    0x00, 0x10, 0x18, 0x20, 0x00 /*invalid*/, 0x28, 0x08,
    0x70, 0x78, 0x80, 0x00 /*invalid*/, 0x38, 0x40,
    0x48 /*invalid*/, 0x50, 0x58, 0x60, 0xd0,
};
#define OHV_CTRL_VALID_MASK 0x3fbefull

static hv_return_t control_field_access(hv_vcpu_t id, _hv_control_field_t f, uint64_t *v, bool w) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    if ((unsigned)f >= 18 || !((OHV_CTRL_VALID_MASK >> f) & 1)) return HV_BAD_ARGUMENT;
    volatile uint64_t *c = (volatile uint64_t *)((uint8_t *)s->ctx + OHV_RW_CONTROLS);
    if (w) { c[kCtrlMap[f] / 8] = *v; mark_dirty(s->ctx, OHV_STATE_CONTROLS); }
    else *v = c[kCtrlMap[f] / 8];
    return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_get_control_field(hv_vcpu_t id, _hv_control_field_t f, uint64_t *v) {
    return control_field_access(id, f, v, false);
}
extern "C" hv_return_t __hv_vcpu_set_control_field(hv_vcpu_t id, _hv_control_field_t f, uint64_t v) {
    return control_field_access(id, f, &v, true);
}
extern "C" hv_return_t __hv_vcpu_get_context(hv_vcpu_t id, void **context) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !context) return HV_BAD_ARGUMENT;
    *context = s->ctx; return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_get_ext_reg(hv_vcpu_t id, _hv_ext_reg_t reg, uint64_t *value) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !value) return HV_BAD_ARGUMENT;
    if ((unsigned)reg >= sizeof(ohv_extregs_t) / 8) return HV_BAD_ARGUMENT;
    *value = ((volatile uint64_t *)(s ? (uint8_t *)s->ctx + OHV_RW_EXTREGS : nullptr))[(unsigned)reg];
    return HV_SUCCESS;
}