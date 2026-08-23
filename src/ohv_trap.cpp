// ohv_trap.cpp - the single supervisor-call gate.
//
// Every request to the kernel hypervisor funnels through one trap:
//   x16 = -5 (mach trap), svc #0x80, x0 = trap id, x1 = argument pointer.
// This is our own clean-room expression of that calling convention.
#include "openhyp/ohv_core.h"

extern "C" __attribute__((naked)) hv_return_t ohv_raw_trap(unsigned, void *) {
    __asm__ volatile(
        "mov  x16, #-5\n"
        "svc  #0x80\n"
        "ret\n");
}

namespace {
struct TrapArgs {
    unsigned id;
    void *arg;
};
}

// Convenience wrappers used across the library.
extern "C" hv_return_t ohv_trap(unsigned trap_id, void *arg) {
    return ohv_raw_trap(trap_id, arg);
}

extern "C" hv_return_t ohv_vm_create_trap(const ohv_vm_create_t *args) {
    return ohv_raw_trap(OHV_TRAP_VM_CREATE, (void *)args);
}
