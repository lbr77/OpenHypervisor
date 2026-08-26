// ohv_caps_field.cpp - capabilities snapshot bridge used by sysreg table.
#include "ohv_internal.h"
#include <cstddef>

/*
 * Offsets measured against a live trap 0 on this machine: CTR_EL0 reads
 * 0x9444c004 at +0xb0, CLIDR_EL1 0x80000023 at +0xc0, and the ID register
 * block runs from +0x148 in the same order the framework's feature_reg enum
 * uses.  If a field moves, this stops the library building rather than
 * letting it answer with the neighbouring register.
 */
static_assert(offsetof(ohv_capabilities_t, control_hcr) == OHV_CAPS_CONTROL_HCR_OFF,
              "control_hcr@0x40");
static_assert(offsetof(ohv_capabilities_t, ctr_el0) == 0xb0, "ctr_el0@0xb0");
static_assert(offsetof(ohv_capabilities_t, dczid_el0) == 0xb8, "dczid_el0@0xb8");
static_assert(offsetof(ohv_capabilities_t, clidr_el1) == 0xc0, "clidr_el1@0xc0");
static_assert(offsetof(ohv_capabilities_t, id_aa64dfr0_el1) == 0x148, "dfr0@0x148");
static_assert(offsetof(ohv_capabilities_t, id_aa64zfr0_el1) == 0x198, "zfr0@0x198");

static ohv_capabilities_t g_caps{};
static bool g_have = false;

const ohv_capabilities_t *ohv_caps_cached() {
    if (!g_have) {
        if (ohv_raw_trap(OHV_TRAP_CAPABILITIES, &g_caps) == HV_SUCCESS) g_have = true;
    }
    return &g_caps;
}
