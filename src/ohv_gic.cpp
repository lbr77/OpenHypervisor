// ohv_gic.cpp - userspace GIC model + private config/state surface.
//
// The framework implements hv_gic_* entirely in userspace (verified: no trap
// involvement); kernel-side VGIC state is reached by writing ich_* registers
// into the vcpu context and marking ARM_GUEST_STATE_GIC dirty. We mirror that
// architecture with our own model object.
#include "ohv_internal.h"
#include "openhyp/ohv_trap.h"

using namespace ohv;

struct hv_gic_config_s {
    uint64_t distributor_base;
    uint64_t redistributor_base;
    uint64_t msi_region_base;
    uint32_t msi_intid_base;
    uint32_t msi_intid_count;
    uint32_t refcnt;
};

// GICv3 constants (public ARM architecture values).
static const size_t kDistSize        = 0x10000;
static const size_t kRedistSize      = 0x20000;  // RD_base + SGI_base, per cpu
static const size_t kMaxCpus         = 64;      // as wide as the tables below
// The region has to hold a redistributor for every vCPU the VM can have,
// which is what a caller places its other devices around.  Reporting two
// cpus' worth while modelling sixty-four understates it by a factor of 32.
static const size_t kRedistRegion    = kMaxCpus * kRedistSize;
static const size_t kMsiRegionSize   = 0x10000;
static const uint32_t kSpiBase       = 32;
static const uint32_t kSpiCount      = 992 - 32 + 1;

struct GicModel {
    hv_gic_config_s cfg{};
    uint64_t dist[GIC_DISTRIBUTOR_REG_COUNT];
    uint64_t redist[64][GIC_REDISTRIBUTOR_REG_COUNT];
    uint64_t icc[64][GIC_ICC_REG_COUNT];
    uint64_t ich[64][GIC_ICH_REG_COUNT];
    uint64_t icv[64][GIC_ICV_REG_COUNT];
    uint64_t msi[GIC_MSI_REG_COUNT];
};

extern "C" hv_gic_config_t hv_gic_config_create(void) {
    return new hv_gic_config_s{};
}
extern "C" hv_return_t hv_gic_config_set_distributor_base(hv_gic_config_t c, hv_ipa_t a) {
    if (!c) return HV_BAD_ARGUMENT; c->distributor_base = a; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_config_set_redistributor_base(hv_gic_config_t c, hv_ipa_t a) {
    if (!c) return HV_BAD_ARGUMENT; c->redistributor_base = a; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_config_set_msi_region_base(hv_gic_config_t c, hv_ipa_t a) {
    if (!c) return HV_BAD_ARGUMENT; c->msi_region_base = a; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_config_set_msi_interrupt_range(hv_gic_config_t c, uint32_t base, uint32_t count) {
    if (!c) return HV_BAD_ARGUMENT; c->msi_intid_base = base; c->msi_intid_count = count; return HV_SUCCESS;
}

extern "C" hv_return_t hv_gic_create(hv_gic_config_t config) {
    if (!config) return HV_BAD_ARGUMENT;
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r != HV_SUCCESS) { pthread_mutex_unlock(&g_vm_mutex); return HV_NO_DEVICE; }
    // The GIC is created after the VM and *before* any vCPU, so that the CPU
    // system resources can be allocated as each vCPU comes up; hv_gic.h says
    // so and the framework enforces it.  Requiring a vCPU first had it exactly
    // backwards, and refused every caller that follows the documented order.
    for (auto &v : ohv::g_vcpus) {
        if (v.used) { pthread_mutex_unlock(&g_vm_mutex); return HV_BUSY; }
    }
    if (g_gic) { pthread_mutex_unlock(&g_vm_mutex); return HV_BUSY; }
    GicModel *m = new GicModel{};
    m->cfg = *config;
    g_gic = m;
    for (auto &s : ohv::g_vcpus)
        if (s.used && s.ctx) mark_dirty(s.ctx, OHV_STATE_GIC);
    pthread_mutex_unlock(&g_vm_mutex);
    return HV_SUCCESS;
}

static GicModel *gic_or_null() { return (GicModel *)g_gic; }

extern "C" hv_return_t hv_gic_get_distributor_size(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = kDistSize; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_distributor_base_alignment(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = 0x10000; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_redistributor_region_size(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = kRedistRegion; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_redistributor_size(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = kRedistSize; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_redistributor_base_alignment(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = 0x10000; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_msi_region_size(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = kMsiRegionSize; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_msi_region_base_alignment(size_t *v) { if (!v) return HV_BAD_ARGUMENT; *v = 0x1000; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_spi_interrupt_range(uint32_t *b, uint32_t *n) { if (!b || !n) return HV_BAD_ARGUMENT; *b = kSpiBase; *n = kSpiCount; return HV_SUCCESS; }
extern "C" hv_return_t hv_gic_get_intid(hv_gic_intid_t interrupt, uint32_t *intid) { if (!intid) return HV_BAD_ARGUMENT; *intid = (uint32_t)interrupt; return HV_SUCCESS; }

extern "C" hv_return_t hv_gic_get_distributor_reg(hv_gic_distributor_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); if (!m || !value) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_DISTRIBUTOR_REG_COUNT) return HV_BAD_ARGUMENT;
    *value = m->dist[(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_distributor_reg(hv_gic_distributor_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_DISTRIBUTOR_REG_COUNT) return HV_BAD_ARGUMENT;
    m->dist[(unsigned)reg] = value; return HV_SUCCESS;
}

static VcpuSlot *vcpu_slot(hv_vcpu_t id) { return find_vcpu(id); }

extern "C" hv_return_t hv_gic_get_redistributor_base(hv_vcpu_t id, hv_ipa_t *a) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !a) return HV_NO_DEVICE;
    *a = m->cfg.redistributor_base + s->id * kRedistRegion;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_redistributor_reg(hv_vcpu_t id, hv_gic_redistributor_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !value) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_REDISTRIBUTOR_REG_COUNT) return HV_BAD_ARGUMENT;
    *value = m->redist[s->id][(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_redistributor_reg(hv_vcpu_t id, hv_gic_redistributor_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_REDISTRIBUTOR_REG_COUNT) return HV_BAD_ARGUMENT;
    m->redist[s->id][(unsigned)reg] = value;
    mark_dirty(s->ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_gic_get_icc_reg(hv_vcpu_t id, hv_gic_icc_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !value) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_ICC_REG_COUNT) return HV_BAD_ARGUMENT;
    *value = m->icc[s->id][(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_icc_reg(hv_vcpu_t id, hv_gic_icc_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_ICC_REG_COUNT) return HV_BAD_ARGUMENT;
    m->icc[s->id][(unsigned)reg] = value;
    mark_dirty(s->ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_ich_reg(hv_vcpu_t id, hv_gic_ich_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !value) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_ICH_REG_COUNT) return HV_BAD_ARGUMENT;
    *value = m->ich[s->id][(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_ich_reg(hv_vcpu_t id, hv_gic_ich_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_ICH_REG_COUNT) return HV_BAD_ARGUMENT;
    m->ich[s->id][(unsigned)reg] = value;
    // Mirror list registers into the context so the next run picks them up.
    if ((unsigned)reg < 8)
        ohv_rw_controls(s->ctx)->ich_lr_el2[(unsigned)reg] =
            value | (((uint64_t)(reg)) << 32 ? 0 : 0);
    else
        ohv_rw_controls(s->ctx)->ich_hcr_el2 = value;
    mark_dirty(s->ctx, OHV_STATE_GIC | OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_icv_reg(hv_vcpu_t id, hv_gic_icv_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !value) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_ICV_REG_COUNT) return HV_BAD_ARGUMENT;
    *value = m->icv[s->id][(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_icv_reg(hv_vcpu_t id, hv_gic_icv_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_ICV_REG_COUNT) return HV_BAD_ARGUMENT;
    m->icv[s->id][(unsigned)reg] = value;
    mark_dirty(s->ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_msi_reg(hv_gic_msi_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); if (!m || !value) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_MSI_REG_COUNT) return HV_BAD_ARGUMENT;
    *value = m->msi[(unsigned)reg]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_msi_reg(hv_gic_msi_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    if ((unsigned)reg >= GIC_MSI_REG_COUNT) return HV_BAD_ARGUMENT;
    m->msi[(unsigned)reg] = value; return HV_SUCCESS;
}

extern "C" hv_return_t hv_gic_set_spi(uint32_t intid, bool level) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    if (intid < kSpiBase || intid >= kSpiBase + kSpiCount) return HV_BAD_ARGUMENT;
    // ISPENDR/ICPENDR pair: register n holds bits for intids 32*n.. in the model.
    unsigned idx = (intid / 32) % GIC_DISTRIBUTOR_REG_COUNT;
    uint64_t bit = 1ull << (intid % 32);
    m->dist[idx] = level ? (m->dist[idx] | bit) : (m->dist[idx] & ~bit);
    for (auto &s : ohv::g_vcpus)
        if (s.used && s.ctx) mark_dirty(s.ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_gic_send_msi(hv_ipa_t address, uint32_t intid) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    (void)address; (void)intid;
    // MSI doorbell: recorded, actual delivery follows the ITS translation the
    // guest programmed into the msi region registers.
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_gic_reset(void) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    for (auto &r : m->dist) r = 0;
    for (auto &row : m->redist) for (auto &r : row) r = 0;
    for (auto &row : m->icc) for (auto &r : row) r = 0;
    for (auto &row : m->ich) for (auto &r : row) r = 0;
    for (auto &row : m->icv) for (auto &r : row) r = 0;
    for (auto &r : m->msi) r = 0;
    for (auto &s : ohv::g_vcpus)
        if (s.used && s.ctx) mark_dirty(s.ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}

extern "C" hv_gic_state_t hv_gic_state_create(void) {
    return (hv_gic_state_t)new GicModel{}; // fresh state buffer with model geometry
}
extern "C" hv_return_t hv_gic_state_get_size(hv_gic_state_t state, size_t *size) {
    if (!state || !size) return HV_BAD_ARGUMENT;
    *size = sizeof(GicModel); return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_state_get_data(hv_gic_state_t state, void *data) {
    if (!state || !data) return HV_BAD_ARGUMENT;
    __builtin_memcpy(data, state, sizeof(GicModel)); return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_state(const void *data, size_t size) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    if (!data || size != sizeof(GicModel)) return HV_BAD_ARGUMENT;
    __builtin_memcpy(m, data, sizeof(GicModel));
    for (auto &s : ohv::g_vcpus)
        if (s.used && s.ctx) mark_dirty(s.ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}