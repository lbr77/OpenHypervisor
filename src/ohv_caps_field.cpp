// ohv_caps_field.cpp - capabilities snapshot bridge used by sysreg table.
#include "ohv_internal.h"

static ohv_capabilities_t g_caps{};
static bool g_have = false;

const ohv_capabilities_t *ohv_caps_cached() {
    if (!g_have) {
        if (ohv_raw_trap(OHV_TRAP_CAPABILITIES, &g_caps) == HV_SUCCESS) g_have = true;
    }
    return &g_caps;
}
