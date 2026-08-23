// ohv_internal.h - library-internal state and helpers.
#ifndef OHV_INTERNAL_H
#define OHV_INTERNAL_H

#include <pthread.h>
#include <stdint.h>
#include "openhyp/ohv_context.h"
#include "openhyp/hv_compat_protos.h"

// Process-global VM singleton: the kernel binds exactly one VM per task.
namespace ohv {

extern pthread_mutex_t g_vm_mutex;
extern pthread_mutex_t g_vcpus_mutex;
extern bool g_vm_alive;
extern uint32_t g_vm_isa;
extern bool g_vm_el2;
extern bool g_vm_vhe;
extern void *g_gic;         // userspace GIC model (opaque)
extern uint32_t g_max_vcpus;

constexpr uint64_t kMaxVcpuIds = 64; // vcpus_exit bitmask width

// Userspace GIC model geometry (our own; kernel sees only context writes).
#define GIC_DISTRIBUTOR_REG_COUNT    32
#define GIC_REDISTRIBUTOR_REG_COUNT  16
#define GIC_ICC_REG_COUNT            24
#define GIC_ICH_REG_COUNT            20
#define GIC_ICV_REG_COUNT            24
#define GIC_MSI_REG_COUNT            8

struct VcpuSlot {
    bool used;
    pthread_t owner;       // thread that created / currently runs it
    void *ctx;             // mapped interface (2 pages)
    uint64_t id;
    uint64_t run_count;
    uint64_t wfi_ticks;
    hv_vcpu_exit_t pub_exit;
};

extern VcpuSlot g_vcpus[kMaxVcpuIds];

// thread-local binding like the framework's thread_local vcpu guard
extern thread_local VcpuSlot *tl_current_vcpu;

inline VcpuSlot *find_vcpu(hv_vcpu_t id) {
    if (id >= kMaxVcpuIds) return nullptr;
    VcpuSlot &s = g_vcpus[id];
    return s.used ? &s : nullptr;
}

VcpuSlot *owned_vcpu(hv_vcpu_t id);
hv_return_t require_vm();
void mark_dirty(void *ctx, uint64_t bit);

// sysreg dispatch (ohv_sysreg.cpp)
struct SysRegDesc {
    uint16_t encoding;
    uint8_t kind;       // 0=shared 1=banked(sync+dirty) 2=extreg 3=debug 5=ro-id
    uint16_t index;     // element index within its class
    uint64_t dirty_bit; // state_dirty bit to set on write (0 = none)
    bool sync_before;   // issue TRAP_HV_VCPU_SYSREGS_SYNC before access
};
const SysRegDesc *sysreg_lookup(uint16_t encoding);
/* The Apple private encodings, which the generator has no source for. */
const SysRegDesc *sysreg_lookup_apple(uint16_t encoding);

} // namespace ohv
void *ohv_guest_ptr(uint64_t ipa, uint64_t len);
/* The nested address spaces a guest hypervisor runs in, if any. */
uint64_t ohv_nested_asid(unsigned index);

#endif