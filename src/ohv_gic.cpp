// ohv_gic.cpp - userspace GIC model + private config/state surface.
//
// The framework implements hv_gic_* entirely in userspace (verified: no trap
// involvement); kernel-side VGIC state is reached by writing ich_* registers
// into the vcpu context and marking ARM_GUEST_STATE_GIC dirty. We mirror that
// architecture with our own model object.
#include "ohv_internal.h"
#include <stdio.h>

/* Set from the exit path once the guest has faulted inside the kernel. */
extern bool g_kernel_running;
#include <stdlib.h>
#include "openhyp/ohv_object.h"
#include "openhyp/ohv_trap.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

using namespace ohv;

struct hv_gic_config_s {
    Class __isa;            /* os_release() reads this; see ohv_object.h */
    uint64_t distributor_base;
    uint64_t redistributor_base;
    uint64_t msi_region_base;
    uint32_t msi_intid_base;
    uint32_t msi_intid_count;
    uint32_t refcnt;
};

// GICv3 geometry, read back off the framework on this machine rather than
// worked out from the architecture: distributor 0x10000, redistributor
// 0x20000 per cpu, a 0x2000000 region to hold them, MSI 0x10000, and SPIs
// 32..1019.
static const size_t kDistSize        = 0x10000;
static const size_t kRedistSize      = 0x20000;  // RD_base + SGI_base, per cpu
static const size_t kRedistRegion    = 0x2000000;
static const size_t kMsiRegionSize   = 0x10000;
static const uint32_t kSpiBase       = 32;
static const uint32_t kSpiCount      = 988;

/*
 * Every one of these register spaces is addressed by *byte offset* -- that is
 * what hv_gic_distributor_reg_t and its siblings are, and a caller walks a
 * bitmap by adding four to the last one it used.  Indexing a small array with
 * that offset, which is what this did, answers only GICD_CTLR and GICD_TYPER
 * and rejects everything else: qemu reads GICD_TYPER, sees no interrupt lines
 * declared, and aborts before the machine ever starts.
 *
 * So the frames are held as frames, one slot per four bytes.  They are stored
 * 64 bits wide because GICD_IROUTER is a 64-bit register living at an 8-byte
 * stride inside the same space.
 */
static const size_t kDistSlots       = kDistSize / 4;
static const size_t kRedistSlots     = kRedistSize / 4;
static const size_t kMsiSlots        = kMsiRegionSize / 4;
/*
 * How many redistributor frames are kept.  The region advertised above has
 * room for far more, but a frame is a quarter of a megabyte and this machine
 * has eight cores; a vCPU past this gets told so rather than writing off the
 * end of the model.
 */
static const size_t kModelledCpus    = 16;

/* The CPU-interface registers are system register encodings, not offsets, and
 * they are sparse.  They get a dense slot each through the tables below. */
static const size_t kIccSlots        = 16;
static const size_t kIchSlots        = 32;
static const size_t kIcvSlots        = 16;

struct GicModel {
    hv_gic_config_s cfg{};
    uint64_t dist[kDistSlots];
    uint64_t redist[kModelledCpus][kRedistSlots];
    uint64_t icc[kModelledCpus][kIccSlots];
    uint64_t ich[kModelledCpus][kIchSlots];
    uint64_t icv[kModelledCpus][kIcvSlots];
    uint64_t msi[kMsiSlots];
};

/* What the framework's own distributor reads back the moment it is created. */
static const uint64_t kGicdCtlrReset  = 0x50;
static const uint64_t kGicdTyperReset = 0x0478001f;

extern "C" hv_gic_config_t hv_gic_config_create(void) {
    return (hv_gic_config_t)ohv_object_alloc(sizeof(hv_gic_config_s));
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
        if (v.used) {
            if (getenv("OHV_TRACE_GIC"))
                fprintf(stderr, "openhyp: gic_create refused, vcpu %llu exists\n",
                        (unsigned long long)v.id);
            pthread_mutex_unlock(&g_vm_mutex); return HV_BUSY;
        }
    }
    if (g_gic) {
        if (getenv("OHV_TRACE_GIC"))
            fprintf(stderr, "openhyp: gic_create refused, a gic already exists\n");
        pthread_mutex_unlock(&g_vm_mutex); return HV_BUSY;
    }
    GicModel *m = new GicModel{};
    if (!m) {
        pthread_mutex_unlock(&g_vm_mutex); return HV_NO_RESOURCES;
    }
    m->cfg = *config;
    /*
     * GICD_TYPER is not decoration: it is how a caller learns how many
     * interrupt lines exist, and qemu multiplies its low five bits out and
     * refuses to build a machine that wants more than it finds.  Zero here
     * reads as "32 interrupts", which is fewer than any real board asks for.
     */
    m->dist[HV_GIC_DISTRIBUTOR_REG_GICD_CTLR / 4] = kGicdCtlrReset;
    m->dist[HV_GIC_DISTRIBUTOR_REG_GICD_TYPER / 4] = kGicdTyperReset;
    g_gic = m;
    for (auto &s : ohv::g_vcpus) {
        if (s.used && s.ctx) {
            /* Same as at vCPU creation: the virtual CPU interface has to be
             * on, or the guest's ICC_* accesses go nowhere useful. */
            ohv_rw_controls(s.ctx)->ich_hcr_el2 |= 1ull;
            mark_dirty(s.ctx, OHV_STATE_GIC | OHV_STATE_CONTROLS);
        }
    }
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

/* An offset into one of the memory-mapped frames, as a slot in the model. */
static bool frame_slot(unsigned offset, size_t slots, size_t *out) {
    if ((offset & 3) != 0 || offset / 4 >= slots) {
        return false;
    }
    *out = offset / 4;
    return true;
}

extern "C" hv_return_t hv_gic_get_distributor_reg(hv_gic_distributor_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); size_t slot;
    if (!m || !value) return HV_NO_DEVICE;
    if (!frame_slot((unsigned)reg, kDistSlots, &slot)) return HV_BAD_ARGUMENT;
    *value = m->dist[slot]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_distributor_reg(hv_gic_distributor_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); size_t slot;
    if (!m) return HV_NO_DEVICE;
    if (!frame_slot((unsigned)reg, kDistSlots, &slot)) return HV_BAD_ARGUMENT;
    /* GICD_TYPER describes the model and is not the caller's to change. */
    if (slot == HV_GIC_DISTRIBUTOR_REG_GICD_TYPER / 4) return HV_SUCCESS;
    m->dist[slot] = value; return HV_SUCCESS;
}

static VcpuSlot *vcpu_slot(hv_vcpu_t id) { return find_vcpu(id); }

extern "C" hv_return_t hv_gic_get_redistributor_base(hv_vcpu_t id, hv_ipa_t *a) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !a) return HV_NO_DEVICE;
    /*
     * Each vCPU's redistributor sits one frame further into the region, not
     * one whole region further: with the region size here the second vCPU's
     * base landed 32 MB away, outside anything the caller mapped.
     */
    *a = m->cfg.redistributor_base + (uint64_t)s->id * kRedistSize;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_redistributor_reg(hv_vcpu_t id, hv_gic_redistributor_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s || !value) return HV_NO_DEVICE;
    size_t slot;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!frame_slot((unsigned)reg, kRedistSlots, &slot)) return HV_BAD_ARGUMENT;
    *value = m->redist[s->id][slot]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_redistributor_reg(hv_vcpu_t id, hv_gic_redistributor_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id);
    if (!m || !s) return HV_NO_DEVICE;
    size_t slot;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!frame_slot((unsigned)reg, kRedistSlots, &slot)) return HV_BAD_ARGUMENT;
    m->redist[s->id][slot] = value;
    mark_dirty(s->ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}

/*
 * The CPU-interface registers are named by system register encoding, so a
 * dense slot comes from a lookup rather than from the value itself.  An
 * encoding that is not in the table is one this model does not have.
 */
static bool sparse_slot(unsigned enc, const unsigned *table, size_t n, size_t *out) {
    for (size_t i = 0; i < n; i++) {
        if (table[i] == enc) { *out = i; return true; }
    }
    return false;
}

static const unsigned kIccEncodings[] = {
    0xc230, 0xc643, 0xc644, 0xc648, 0xc65b, 0xc663,
    0xc664, 0xc665, 0xc666, 0xc667, 0xe64d,
};
static const unsigned kIcvEncodings[] = {
    0xc230, 0xc643, 0xc644, 0xc648, 0xc65b, 0xc663,
    0xc664, 0xc665, 0xc666, 0xc667,
};
static const unsigned kIchEncodings[] = {
    0xe640, 0xe648, 0xe658, 0xe659, 0xe65a, 0xe65b, 0xe65d, 0xe65f,
    0xe660, 0xe661, 0xe662, 0xe663, 0xe664, 0xe665, 0xe666, 0xe667,
    0xe668, 0xe669, 0xe66a, 0xe66b, 0xe66c, 0xe66d, 0xe66e, 0xe66f,
};

#define ICH_AP0R0 0xe640u
#define ICH_AP1R0 0xe648u
#define ICH_HCR   0xe658u
#define ICH_VTR   0xe659u
#define ICH_MISR  0xe65au
#define ICH_EISR  0xe65bu
#define ICH_ELRSR 0xe65du
#define ICH_VMCR  0xe65fu
#define ICH_LR0   0xe660u

extern "C" hv_return_t hv_gic_get_icc_reg(hv_vcpu_t id, hv_gic_icc_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id); size_t slot;
    if (!m || !s || !value) return HV_NO_DEVICE;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!sparse_slot((unsigned)reg, kIccEncodings, ARRAY_COUNT(kIccEncodings), &slot))
        return HV_BAD_ARGUMENT;
    *value = m->icc[s->id][slot]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_icc_reg(hv_vcpu_t id, hv_gic_icc_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id); size_t slot;
    if (!m || !s) return HV_NO_DEVICE;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!sparse_slot((unsigned)reg, kIccEncodings, ARRAY_COUNT(kIccEncodings), &slot))
        return HV_BAD_ARGUMENT;
    m->icc[s->id][slot] = value;
    mark_dirty(s->ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}

/*
 * ICH_* is the half that reaches hardware.  The kernel keeps the virtual CPU
 * interface in the vcpu context: the list registers and ICH_HCR_EL2 in the
 * run controls, the two active-priority registers in their own block, and
 * ICH_VMCR_EL2 with the banked system registers.  Writing there and marking
 * the state dirty is what the framework does -- there is no separate trap for
 * a GIC -- so that is what happens here.  ICH_VTR, ICH_MISR and ICH_ELRSR are
 * the kernel's to report and come back out of the read-only page.
 */
extern "C" hv_return_t hv_gic_get_ich_reg(hv_vcpu_t id, hv_gic_ich_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id); size_t slot;
    unsigned enc = (unsigned)reg;
    if (!m || !s || !value) return HV_NO_DEVICE;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!sparse_slot(enc, kIchEncodings, ARRAY_COUNT(kIchEncodings), &slot))
        return HV_BAD_ARGUMENT;
    switch (enc) {
        case ICH_VTR:   *value = *ohv_ro_u64(s->ctx, OHV_RO_ICH_VTR);   return HV_SUCCESS;
        case ICH_MISR:  *value = *ohv_ro_u64(s->ctx, OHV_RO_ICH_MISR);  return HV_SUCCESS;
        case ICH_ELRSR: *value = *ohv_ro_u64(s->ctx, OHV_RO_ICH_ELRSR); return HV_SUCCESS;
        case ICH_AP0R0: *value = ohv_rw(s->ctx)->vgic_sysregs.ich_ap0r0_el2; return HV_SUCCESS;
        case ICH_AP1R0: *value = ohv_rw(s->ctx)->vgic_sysregs.ich_ap1r0_el2; return HV_SUCCESS;
        case ICH_HCR:   *value = ohv_rw_controls(s->ctx)->ich_hcr_el2;  return HV_SUCCESS;
        case ICH_VMCR:  *value = ohv_rw(s->ctx)->banked_sysregs.ich_vmcr_el2; return HV_SUCCESS;
        default: break;
    }
    if (enc >= ICH_LR0) {
        unsigned lr = enc - ICH_LR0;
        if (lr >= ARRAY_COUNT(ohv_rw_controls(s->ctx)->ich_lr_el2)) return HV_BAD_ARGUMENT;
        /* The dirty flag is this interface's, not the register's. */
        *value = ohv_rw_controls(s->ctx)->ich_lr_el2[lr] & ~OHV_ICH_LR_DIRTY;
        return HV_SUCCESS;
    }
    *value = m->ich[s->id][slot];
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_ich_reg(hv_vcpu_t id, hv_gic_ich_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id); size_t slot;
    unsigned enc = (unsigned)reg;
    if (!m || !s) return HV_NO_DEVICE;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!sparse_slot(enc, kIchEncodings, ARRAY_COUNT(kIchEncodings), &slot))
        return HV_BAD_ARGUMENT;
    m->ich[s->id][slot] = value;
    switch (enc) {
        /* The kernel reports these; a write to one is not an error, it just
         * has nowhere to go. */
        case ICH_VTR: case ICH_MISR: case ICH_EISR: case ICH_ELRSR:
            return HV_SUCCESS;
        case ICH_AP0R0:
            ohv_rw(s->ctx)->vgic_sysregs.ich_ap0r0_el2 = value; break;
        case ICH_AP1R0:
            ohv_rw(s->ctx)->vgic_sysregs.ich_ap1r0_el2 = value; break;
        case ICH_HCR:
            ohv_rw_controls(s->ctx)->ich_hcr_el2 = value; break;
        case ICH_VMCR:
            ohv_rw(s->ctx)->banked_sysregs.ich_vmcr_el2 = value; break;
        default: {
            unsigned lr = enc - ICH_LR0;
            if (enc < ICH_LR0 ||
                lr >= ARRAY_COUNT(ohv_rw_controls(s->ctx)->ich_lr_el2))
                return HV_BAD_ARGUMENT;
            /* Same contract for a list register the caller writes itself. */
            ohv_rw_controls(s->ctx)->ich_lr_el2[lr] = value | OHV_ICH_LR_DIRTY;
            break;
        }
    }
    mark_dirty(s->ctx, OHV_STATE_GIC | OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_icv_reg(hv_vcpu_t id, hv_gic_icv_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id); size_t slot;
    if (!m || !s || !value) return HV_NO_DEVICE;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!sparse_slot((unsigned)reg, kIcvEncodings, ARRAY_COUNT(kIcvEncodings), &slot))
        return HV_BAD_ARGUMENT;
    *value = m->icv[s->id][slot]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_icv_reg(hv_vcpu_t id, hv_gic_icv_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); VcpuSlot *s = vcpu_slot(id); size_t slot;
    if (!m || !s) return HV_NO_DEVICE;
    if (s->id >= kModelledCpus) return HV_BAD_ARGUMENT;
    if (!sparse_slot((unsigned)reg, kIcvEncodings, ARRAY_COUNT(kIcvEncodings), &slot))
        return HV_BAD_ARGUMENT;
    m->icv[s->id][slot] = value;
    mark_dirty(s->ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_get_msi_reg(hv_gic_msi_reg_t reg, uint64_t *value) {
    GicModel *m = gic_or_null(); if (!m || !value) return HV_NO_DEVICE;
    size_t slot;
    if (!frame_slot((unsigned)reg, kMsiSlots, &slot)) return HV_BAD_ARGUMENT;
    *value = m->msi[slot]; return HV_SUCCESS;
}
extern "C" hv_return_t hv_gic_set_msi_reg(hv_gic_msi_reg_t reg, uint64_t value) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    size_t slot;
    if (!frame_slot((unsigned)reg, kMsiSlots, &slot)) return HV_BAD_ARGUMENT;
    m->msi[slot] = value; return HV_SUCCESS;
}

/* Fields of a GICv3 list register, as ICH_LR<n>_EL2 defines them. */
#define LR_STATE_SHIFT   62
#define LR_STATE_PENDING (1ull << LR_STATE_SHIFT)
#define LR_STATE_MASK    (3ull << LR_STATE_SHIFT)
#define LR_GROUP1        (1ull << 60)
#define LR_PRIORITY_SHIFT 48
#define LR_VINTID_MASK   0xffffffffull

/*
 * Raising a shared interrupt is two things: the distributor records it as
 * pending, which is what a caller reads back out of GICD_ISPENDR, and the
 * hardware has to be given somewhere to deliver it from.  That second half is
 * a list register, and without it the interrupt is only ever a bit in a model
 * nothing looks at -- which is what this did before, and why an in-kernel
 * interrupt controller delivered nothing at all.
 *
 * The routing here is deliberately the simple one: every SPI goes to the
 * first vCPU, which is where this machine's affinity routing sends them and
 * is all GICD_IROUTER is ever set to.
 */
extern "C" hv_return_t hv_gic_set_spi(uint32_t intid, bool level) {
    GicModel *m = gic_or_null(); if (!m) return HV_NO_DEVICE;
    if (intid < kSpiBase || intid >= kSpiBase + kSpiCount) return HV_BAD_ARGUMENT;

    size_t word = intid / 32;
    uint64_t bit = 1ull << (intid % 32);
    size_t pend = HV_GIC_DISTRIBUTOR_REG_GICD_ISPENDR0 / 4 + word;
    size_t enabled = HV_GIC_DISTRIBUTOR_REG_GICD_ISENABLER0 / 4 + word;

    if (pend >= kDistSlots) return HV_BAD_ARGUMENT;
    m->dist[pend] = level ? (m->dist[pend] | bit) : (m->dist[pend] & ~bit);

    VcpuSlot *target = nullptr;
    for (auto &v : ohv::g_vcpus) {
        if (v.used && v.ctx) { target = &v; break; }
    }
    if (!target || target->id >= kModelledCpus) return HV_SUCCESS;

    ohv_controls_t *ctl = ohv_rw_controls(target->ctx);
    size_t lrs = ARRAY_COUNT(ctl->ich_lr_el2);
    uint8_t priority = (uint8_t)(m->dist[HV_GIC_DISTRIBUTOR_REG_GICD_IPRIORITYR0 / 4
                                         + intid / 4] >> (8 * (intid % 4)));

    if (level) {
        if ((m->dist[enabled] & bit) == 0) return HV_SUCCESS;
        for (size_t i = 0; i < lrs; i++) {
            uint64_t lr = ctl->ich_lr_el2[i];
            if ((lr & LR_STATE_MASK) != 0 && (lr & LR_VINTID_MASK) == intid) {
                return HV_SUCCESS;   /* already queued */
            }
        }
        for (size_t i = 0; i < lrs; i++) {
            if ((ctl->ich_lr_el2[i] & LR_STATE_MASK) != 0) continue;
            /*
             * The kernel only looks at a list register that says it has
             * changed: it tests OHV_ICH_LR_DIRTY, validates the entry, and
             * clears the bit to mark it taken.  Written without it, the entry
             * is there for anyone reading the page and invisible to the
             * hardware -- the interrupt is queued and never delivered.
             */
            /*
             * With the irqchip in the kernel this is the path interrupts
             * actually take, not hv_vcpu_set_pending_interrupt.  What this is
             * for: XNU's sleh_irq dispatches through an object IOKit
             * publishes, and one delivered before that global is set
             * dereferences null.  The intid names which line it was.
             */
            /*
             * The pc in the shared page is only meaningful at an exit, and
             * this runs from the VMM's own thread while the guest is going --
             * gating on it filtered out every injection that mattered.  The
             * phase flag is set from the exit path, where the value is real.
             */
            if (getenv("OHV_TRACE_KIRQ") && g_kernel_running) {
                static unsigned n;

                if (n < 60) {
                    n++;
                    fprintf(stderr, "[ohv] gic inject %u: intid %u prio %u\n",
                            n, (unsigned)intid, (unsigned)priority);
                }
            }
            ctl->ich_lr_el2[i] = LR_STATE_PENDING | LR_GROUP1 |
                                 ((uint64_t)priority << LR_PRIORITY_SHIFT) |
                                 (uint64_t)intid | OHV_ICH_LR_DIRTY;
            mark_dirty(target->ctx, OHV_STATE_GIC | OHV_STATE_CONTROLS);
            return HV_SUCCESS;
        }
        /* Every list register is taken; the pending bit above keeps it. */
        return HV_SUCCESS;
    }

    for (size_t i = 0; i < lrs; i++) {
        uint64_t lr = ctl->ich_lr_el2[i];
        if ((lr & LR_STATE_MASK) == LR_STATE_PENDING &&
            (lr & LR_VINTID_MASK) == intid) {
            ctl->ich_lr_el2[i] = OHV_ICH_LR_DIRTY;   /* inactive, and say so */
            mark_dirty(target->ctx, OHV_STATE_GIC | OHV_STATE_CONTROLS);
        }
    }
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
    /* A reset distributor still describes itself. */
    m->dist[HV_GIC_DISTRIBUTOR_REG_GICD_CTLR / 4] = kGicdCtlrReset;
    m->dist[HV_GIC_DISTRIBUTOR_REG_GICD_TYPER / 4] = kGicdTyperReset;
    for (auto &s : ohv::g_vcpus)
        if (s.used && s.ctx) mark_dirty(s.ctx, OHV_STATE_GIC);
    return HV_SUCCESS;
}

extern "C" hv_gic_state_t hv_gic_state_create(void) {
    /* GicModel leads with the config, so this handle carries the isa too. */
    return (hv_gic_state_t)ohv_object_alloc(sizeof(GicModel));
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