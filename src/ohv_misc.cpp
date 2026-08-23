// ohv_misc.cpp - SME/AMX, spaces, capabilities, private vcpu-config surface.
#include <sys/mman.h>
#include "ohv_internal.h"
#include "openhyp/ohv_trap.h"

using namespace ohv;

// ------------------------------------------------------------------ caps --
struct CapsEntry { uint16_t enc; size_t off; };
static const CapsEntry kCapsMap[] = {
#define CAP_OFF(fld) offsetof(ohv_capabilities_t, fld)
    {HV_SYS_REG_ID_AA64DFR0_EL1, CAP_OFF(id_aa64dfr0_el1)},
    {HV_SYS_REG_ID_AA64DFR1_EL1, CAP_OFF(id_aa64dfr1_el1)},
    {HV_SYS_REG_ID_AA64ISAR0_EL1, CAP_OFF(id_aa64isar0_el1)},
    {HV_SYS_REG_ID_AA64ISAR1_EL1, CAP_OFF(id_aa64isar1_el1)},
    {HV_SYS_REG_ID_AA64MMFR0_EL1, CAP_OFF(id_aa64mmfr0_el1)},
    {HV_SYS_REG_ID_AA64MMFR1_EL1, CAP_OFF(id_aa64mmfr1_el1)},
    {HV_SYS_REG_ID_AA64MMFR2_EL1, CAP_OFF(id_aa64mmfr2_el1)},
    {HV_SYS_REG_ID_AA64PFR0_EL1, CAP_OFF(id_aa64pfr0_el1)},
    {HV_SYS_REG_ID_AA64PFR1_EL1, CAP_OFF(id_aa64pfr1_el1)},
#undef CAP_OFF
};
extern "C" uint64_t ohv_caps_field(const ohv_capabilities_t *c, uint16_t enc) {
    // Serve read-only id registers from the kernel capabilities snapshot.
    for (auto &e : kCapsMap)
        if (e.enc == enc) return *(const uint64_t *)((const uint8_t *)c + e.off);
    return 0;
}
#if 0
    switch (enc) {
        case HV_SYS_REG_ID_AA64DFR0_EL1: return c->id_aa64dfr0_el1;
        case HV_SYS_REG_ID_AA64DFR1_EL1: return c->id_aa64dfr1_el1;
        case HV_SYS_REG_ID_AA64ISAR0_EL1: return c->id_aa64isar0_el1;
        case HV_SYS_REG_ID_AA64ISAR1_EL1: return c->id_aa64isar1_el1;
        case HV_SYS_REG_ID_AA64MMFR0_EL1: return c->id_aa64mmfr0_el1;
        case HV_SYS_REG_ID_AA64MMFR1_EL1: return c->id_aa64mmfr1_el1;
        case HV_SYS_REG_ID_AA64MMFR2_EL1: return c->id_aa64mmfr2_el1;
        case HV_SYS_REG_ID_AA64PFR0_EL1: return c->id_aa64pfr0_el1;
        case HV_SYS_REG_ID_AA64PFR1_EL1: return c->id_aa64pfr1_el1;
        case HV_SYS_REG_ID_AA64SMFR0_EL1: return c->id_aa64smfr0_el1;
        case HV_SYS_REG_ID_AA64ZFR0_EL1: return c->id_aa64zfr0_el1;
        default: return 0;
    }
}
#endif

// ------------------------------------------------------------- vcpu config --
struct hv_vcpu_config_s {
    uint64_t feature_overrides[2]; // +16/+24 in framework layout
    uint16_t misc;                 // +32
    bool fgt_enabled;
    bool tlbi_workaround;
    uint64_t vmkey;
};
extern "C" hv_vcpu_config_t hv_vcpu_config_create(void) { return new hv_vcpu_config_s{}; }
extern "C" hv_return_t __hv_vcpu_config_get_vmkey(hv_vcpu_config_t c, uint64_t *k) { if (!c || !k) return HV_BAD_ARGUMENT; *k = c->vmkey; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vcpu_config_set_vmkey(hv_vcpu_config_t c, uint64_t k) { if (!c) return HV_BAD_ARGUMENT; c->vmkey = k; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vcpu_config_get_fgt_enabled(hv_vcpu_config_t c, bool *v) { if (!c || !v) return HV_BAD_ARGUMENT; *v = c->fgt_enabled; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vcpu_config_set_fgt_enabled(hv_vcpu_config_t c, bool v) { if (!c) return HV_BAD_ARGUMENT; c->fgt_enabled = v; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vcpu_config_get_tlbi_workaround_enabled(hv_vcpu_config_t c, bool *v) { if (!c || !v) return HV_BAD_ARGUMENT; *v = c->tlbi_workaround; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vcpu_config_set_tlbi_workaround_enabled(hv_vcpu_config_t c, bool v) { if (!c) return HV_BAD_ARGUMENT; c->tlbi_workaround = v; return HV_SUCCESS; }
extern "C" hv_return_t hv_vcpu_config_get_feature_reg(hv_vcpu_config_t c, hv_feature_reg_t reg, uint64_t *v) {
    if (!c || !v) return HV_BAD_ARGUMENT;
    if ((unsigned)reg >= 2) return HV_BAD_ARGUMENT;
    *v = c->feature_overrides[(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_config_get_ccsidr_el1_sys_reg_values(hv_vcpu_config_t c, hv_cache_type_t t, uint64_t v[8]) {
    if (!c || !v) return HV_BAD_ARGUMENT;
    ohv_capabilities_t caps{};
    if (ohv_raw_trap(OHV_TRAP_CAPABILITIES, &caps) != HV_SUCCESS) return HV_ERROR;
    const uint64_t *src = (t == HV_CACHE_TYPE_INSTRUCTION) ? caps.ccsidr_el1_inst : caps.ccsidr_el1_data_or_unified;
    for (int i = 0; i < 8; i++) v[i] = src[i];
    return HV_SUCCESS;
}

// -------------------------------------------------------------- SME/AMX --
extern "C" hv_return_t hv_sme_config_get_max_svl_bytes(size_t *v) {
    if (!v) return HV_BAD_ARGUMENT;
    VcpuSlot *s = tl_current_vcpu;
    if (!s) { *v = 0; return HV_NO_DEVICE; }
    *v = ohv_ro(s->ctx)->svl_b; // SVL in bytes
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_sme_state(hv_vcpu_t id, hv_vcpu_sme_state_t *st) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !st) return HV_BAD_ARGUMENT;
    uint64_t svcr = *ohv_ro_u64(s->ctx, OHV_RO_SVCR);
    st->streaming_sve_mode_enabled = (svcr & 1) != 0;
    st->za_storage_enabled = (svcr & 2) != 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_sme_state(hv_vcpu_t id, const hv_vcpu_sme_state_t *st) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !st) return HV_BAD_ARGUMENT;
    uint64_t svcr = (st->streaming_sve_mode_enabled ? 1 : 0) | (st->za_storage_enabled ? 2 : 0);
    return ohv_raw_trap(OHV_TRAP_VCPU_SET_SVCR, (void *)svcr);
}
extern "C" hv_return_t hv_vcpu_get_sme_z_reg(hv_vcpu_t id, hv_sme_z_reg_t reg, uint8_t *v, size_t len) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    void *sme = ohv_ro(s->ctx)->sme;
    if (!sme) return HV_ILLEGAL_GUEST_STATE;
    size_t svl = ohv_ro(s->ctx)->svl_b;
    if (len < svl || (unsigned)reg > 31) return HV_BAD_ARGUMENT;
    __builtin_memcpy(v, (uint8_t *)sme + (size_t)reg * svl, svl);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_sme_z_reg(hv_vcpu_t id, hv_sme_z_reg_t reg, const uint8_t *v, size_t len) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    void *sme = ohv_ro(s->ctx)->sme;
    if (!sme) return HV_ILLEGAL_GUEST_STATE;
    size_t svl = ohv_ro(s->ctx)->svl_b;
    if (len < svl || (unsigned)reg > 31) return HV_BAD_ARGUMENT;
    __builtin_memcpy((uint8_t *)sme + (size_t)reg * svl, v, svl);
    mark_dirty(s->ctx, OHV_STATE_SME_CONTEXT);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_sme_p_reg(hv_vcpu_t, hv_sme_p_reg_t, uint8_t *, size_t) { return HV_UNSUPPORTED; }
extern "C" hv_return_t hv_vcpu_set_sme_p_reg(hv_vcpu_t, hv_sme_p_reg_t, const uint8_t *, size_t) { return HV_UNSUPPORTED; }
extern "C" hv_return_t hv_vcpu_get_sme_za_reg(hv_vcpu_t id, uint8_t *v, size_t len) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    void *sme = ohv_ro(s->ctx)->sme; if (!sme) return HV_ILLEGAL_GUEST_STATE;
    size_t svl = ohv_ro(s->ctx)->svl_b;
    if (len < svl * svl) return HV_BAD_ARGUMENT;
    __builtin_memcpy(v, (uint8_t *)sme + 32 * svl, svl * svl); // after z-array + predicates
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_sme_za_reg(hv_vcpu_t id, const uint8_t *v, size_t len) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    void *sme = ohv_ro(s->ctx)->sme; if (!sme) return HV_ILLEGAL_GUEST_STATE;
    size_t svl = ohv_ro(s->ctx)->svl_b;
    if (len < svl * svl) return HV_BAD_ARGUMENT;
    __builtin_memcpy((uint8_t *)sme + 32 * svl, v, svl * svl);
    mark_dirty(s->ctx, OHV_STATE_SME_CONTEXT);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_sme_zt0_reg(hv_vcpu_t id, hv_sme_zt0_uchar64_t *v) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    void *sme = ohv_ro(s->ctx)->sme; if (!sme) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(v, (uint8_t *)sme, 64);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_sme_zt0_reg(hv_vcpu_t id, const hv_sme_zt0_uchar64_t *v) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    void *sme = ohv_ro(s->ctx)->sme; if (!sme) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy((uint8_t *)sme, v, 64);
    mark_dirty(s->ctx, OHV_STATE_SME_CONTEXT);
    return HV_SUCCESS;
}

struct arm_guest_amx_context {
    uint8_t x[8][64];
    uint8_t y[8][64];
    uint8_t z[64][64];
    uint64_t amx_state_t_el1;
} __attribute__((aligned(64)));

extern "C" hv_return_t __hv_vcpu_amx_prepare(hv_vcpu_t id) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    return ohv_raw_trap(OHV_TRAP_VCPU_AMX_PREPARE, nullptr);
}
extern "C" hv_return_t __hv_vcpu_amx_query_active_context(hv_vcpu_t id, bool *active) {
    VcpuSlot *s = ohv::owned_vcpu(id); if (!s || !active) return HV_BAD_ARGUMENT;
    *active = ohv_ro(s->ctx)->amx != nullptr;
    return HV_SUCCESS;
}
static arm_guest_amx_context *amx_of(hv_vcpu_t id) {
    VcpuSlot *s = ohv::owned_vcpu(id);
    return s ? (arm_guest_amx_context *)ohv_ro(s->ctx)->amx : nullptr;
}
extern "C" hv_return_t __hv_vcpu_get_amx_x_space(hv_vcpu_t id, uint8_t out[8][64]) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(out, a->x, sizeof(a->x)); return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_set_amx_x_space(hv_vcpu_t id, const uint8_t in[8][64]) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(a->x, in, sizeof(a->x)); return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_get_amx_y_space(hv_vcpu_t id, uint8_t out[8][64]) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(out, a->y, sizeof(a->y)); return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_set_amx_y_space(hv_vcpu_t id, const uint8_t in[8][64]) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(a->y, in, sizeof(a->y)); return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_get_amx_z_space(hv_vcpu_t id, uint8_t out[64][64]) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(out, a->z, sizeof(a->z)); return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_set_amx_z_space(hv_vcpu_t id, const uint8_t in[64][64]) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    __builtin_memcpy(a->z, in, sizeof(a->z)); return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_get_amx_state_t_el1(hv_vcpu_t id, uint64_t *v) {
    auto *a = amx_of(id); if (!a || !v) return HV_ILLEGAL_GUEST_STATE;
    *v = a->amx_state_t_el1; return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vcpu_set_amx_state_t_el1(hv_vcpu_t id, uint64_t v) {
    auto *a = amx_of(id); if (!a) return HV_ILLEGAL_GUEST_STATE;
    a->amx_state_t_el1 = v; return HV_SUCCESS;
}

// ---------------------------------------------------------------- spaces --
struct hv_vm_space_config_s {
    uint64_t ipa_base;
    uint64_t ipa_size;
    uint64_t ipa_granule;
};
extern "C" hv_vm_space_config_t __hv_vm_space_config_create(void) { return new hv_vm_space_config_s{}; }
extern "C" hv_return_t __hv_vm_space_config_set_ipa_base(hv_vm_space_config_t c, hv_ipa_t v) { if (!c) return HV_BAD_ARGUMENT; c->ipa_base = v; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vm_space_config_get_ipa_base(hv_vm_space_config_t c, hv_ipa_t *v) { if (!c || !v) return HV_BAD_ARGUMENT; *v = c->ipa_base; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vm_space_config_set_ipa_size(hv_vm_space_config_t c, hv_ipa_t v) { if (!c) return HV_BAD_ARGUMENT; c->ipa_size = v; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vm_space_config_get_ipa_size(hv_vm_space_config_t c, hv_ipa_t *v) { if (!c || !v) return HV_BAD_ARGUMENT; *v = c->ipa_size; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vm_space_config_set_ipa_granule(hv_vm_space_config_t c, hv_ipa_t v) { if (!c) return HV_BAD_ARGUMENT; c->ipa_granule = v; return HV_SUCCESS; }
extern "C" hv_return_t __hv_vm_space_config_get_ipa_granule(hv_vm_space_config_t c, hv_ipa_t *v) { if (!c || !v) return HV_BAD_ARGUMENT; *v = c->ipa_granule; return HV_SUCCESS; }

extern "C" hv_return_t __hv_vm_space_create(hv_vm_space_config_t c, hv_vm_space_t *out) {
    if (!c || !out) return HV_BAD_ARGUMENT;
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r == HV_SUCCESS) {
        ohv_vm_addrspace_create_t a{ c->ipa_base, c->ipa_size, (uint32_t)c->ipa_granule, 0, 0 };
        r = ohv_raw_trap(OHV_TRAP_VM_ADDRESS_SPACE_CREATE, &a);
        if (r == HV_SUCCESS) *out = a.out_asid;
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}
extern "C" hv_return_t __hv_vm_space_destroy(hv_vm_space_t space) {
    (void)space; // kernel frees spaces with the VM; per-space destroy has no trap
    return HV_SUCCESS;
}
extern "C" hv_return_t __hv_vm_map_space(hv_vm_space_t space, void *addr, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r == HV_SUCCESS) {
        ohv_vm_map_item_t item{ (uint64_t)(uintptr_t)addr, ipa, size, flags, space };
        r = ohv_raw_trap(OHV_TRAP_VM_MAP, &item);
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}
extern "C" hv_return_t __hv_vm_unmap_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r == HV_SUCCESS) {
        ohv_vm_map_item_t item{ 0, ipa, size, 0, space };
        r = ohv_raw_trap(OHV_TRAP_VM_UNMAP, &item);
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}
extern "C" hv_return_t __hv_vm_protect_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r == HV_SUCCESS) {
        ohv_vm_map_item_t item{ 0, ipa, size, flags, space };
        r = ohv_raw_trap(OHV_TRAP_VM_PROTECT, &item);
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}
extern "C" hv_return_t __hv_vm_stage1_tlb_op(hv_vm_space_t space, uint64_t op, uint64_t param) {
    ohv_vm_stage1_tlb_op_t a{ space, op, param };
    return ohv_raw_trap(OHV_TRAP_VM_STAGE1_TLB_OP, &a);
}

// ---------------------------------------------------------- capabilities --
extern "C" hv_return_t __hv_capability(hv_capability_t cap, uint64_t *value) {
    if (!value) return HV_BAD_ARGUMENT;
    ohv_capabilities_t caps{};
    hv_return_t r = ohv_raw_trap(OHV_TRAP_CAPABILITIES, &caps);
    if (r != HV_SUCCESS) return r;
    switch (cap) {
        case HV_CAP_VCPUMAX: *value = 64; break;
        case HV_CAP_ADDRSPACEMAX: *value = caps.ipa_bits_4k ? caps.ipa_bits_4k : 40; break;
        default: return HV_BAD_ARGUMENT;
    }
    return HV_SUCCESS;
}

// --------------------------------------------- private monitor data abort --
extern "C" hv_return_t ohv_monitor_data_abort(bool add, hv_vm_space_t asid, uint64_t context,
                                              hv_ipa_t base, size_t size, mach_port_t port) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r == HV_SUCCESS) {
        ohv_vm_monitor_data_abort_t m{
            add ? 0ull : 1ull, asid, context, base, size, HV_MEMORY_WRITE, (uint32_t)port
        };
        r = ohv_raw_trap(OHV_TRAP_VM_MONITOR_DATA_ABORT, &m);
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}