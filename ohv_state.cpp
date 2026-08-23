// ohv_state.cpp - process-global state.
#include "ohv_internal.h"

namespace ohv {

pthread_mutex_t g_vm_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_vcpus_mutex = PTHREAD_MUTEX_INITIALIZER;
bool g_vm_alive = false;
uint32_t g_vm_isa = OHV_VM_ISA_NONE;
bool g_vm_el2 = false;
bool g_vm_vhe = false;
void *g_gic = nullptr;
uint32_t g_max_vcpus = 64;
VcpuSlot g_vcpus[kMaxVcpuIds] = {};
thread_local VcpuSlot *tl_current_vcpu = nullptr;

hv_return_t require_vm() {
    return g_vm_alive ? HV_SUCCESS : HV_NO_DEVICE;
}

void mark_dirty(void *ctx, uint64_t bit) {
    if (ctx) *ohv_rw_u64(ctx, OHV_RW_STATE_DIRTY) |= bit;
}

} // namespace ohv
