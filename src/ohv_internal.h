// ohv_internal.h - library-internal state and helpers.
#ifndef OHV_INTERNAL_H
#define OHV_INTERNAL_H

#include <atomic>
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
    /*
     * A cancel the VMM asked for, latched here.
     *
     * hv_vcpus_exit() is how the VMM says "come out, I have an interrupt for
     * you": QEMU's cpu_interrupt -> qemu_cpu_kick lands on it.  Forwarding it
     * straight to the run trap only works while the vCPU is actually inside
     * that trap.  One call to hv_vcpu_run() is many entries into the guest --
     * everything the framework can answer itself is answered and the guest is
     * re-entered without the VMM being told -- so a cancel that arrives during
     * one of those internal turnarounds lands on nobody, and the guest is
     * re-entered with the interrupt still uninjected.
     *
     * Latching it means the next turnaround sees it and returns instead.
     */
    std::atomic<bool> exit_requested{false};
    /*
     * The EL2 registers the context has no slot for.  Under NV2 the guest's
     * own accesses to these are redirected into the VNCR page in its own
     * memory, so what a VMM writes here is only ever read back by the VMM --
     * at reset and across migration.  The framework accepts them and shows
     * nothing in the context either; storing them keeps the pair honest
     * instead of refusing a write the caller treats as fatal.
     */
    uint64_t el2_shadow[32];
    /*
     * Set while HCR_EL2.NV is off because a guarded ERET was handed to the
     * hardware.  The guest is inside the guarded state it returned into and
     * is not acting as a hypervisor there; the bit goes back on when it
     * leaves, which is the guarded exit.
     */
    bool nv_off_for_geret;
    /*
     * VM_TMR_FIQ_ENA_EL2 (S3_5_C15_C1_3): bit 0 the guest's physical timer,
     * bit 1 its virtual one.  Both on until the guest says otherwise, which is
     * the state iBoot runs in.
     */
    uint64_t vm_tmr_fiq_ena = 3;
    /*
     * The two halves of the guest's virtual FIQ, kept apart so neither can
     * erase the other: the VMM asserts its own level once per entry, the
     * timers emulated here assert theirs on every internal one.
     */
    bool vmm_fiq = false;
    bool timer_fiq = false;
    /*
     * Which of the two timers has come due and is not masked at the timer
     * itself.  Kept apart from timer_fiq because the decision whether a due
     * timer may be delivered is not the timer's -- it depends on where the
     * guest is -- and that decision belongs with the level, not here.
     */
    bool timer_fired_v = false;
    bool timer_fired_p = false;
    /*
     * Where the monitor images live, learned rather than assumed.  SPTM, TXM
     * and the kernelcache are linked 0x10000000 apart -- 0xfffffff007004000,
     * 0xfffffff017004000, 0xfffffff027004000 -- and carry one slide between
     * them, so the first kernel-VA the guest is ever seen at, which is SPTM's
     * entry, fixes all three.  Below sptm_base + 0x20000000 is the monitor;
     * at or above it is XNU.
     */
    uint64_t sptm_base = 0;
    /*
     * The GL2 bank, s3_6_c15_c11_0..7, in the same order as GL1: SP, TPIDR,
     * VBAR, SPSR, ASPSR, ESR, ELR, FAR.  Kept here because the host's guest
     * state has nowhere to put it -- guest_thread_state.h carries *_gl1 and
     * nothing for GL2 -- so unlike the GL1 bank there is no silicon behind
     * these to disagree with.
     */
    uint64_t gl2_bank[8];
    /* The CTRR bank, whose two views share one shadow. */
    struct { uint8_t sel; uint64_t value; } ctrr[6];
    unsigned ctrr_used = 0;
    /* KTRACE_MESSAGE, HIST_TRIG and JCTL_EL0, per thread switch. */
    uint64_t ctx_trace[3] = {0, 0, 0};
    /* The HCR mirror the monitor entry writes to s3_7_c15_c15_7. */
    uint64_t monitor_hcr = 0;
    unsigned kernel_exits = 0;
    /* The VMM's virtual IRQ, held here so its level can be gated like the FIQ. */
    bool vmm_irq = false;
    /*
     * The Apple implementation-defined registers the guest touches that
     * nothing else answers.  They always trap -- HACR_EL2's THIDDVF, THIDCPU,
     * THIDLLC, THIDAMX and THIDDPC are in the kernel's fixed set -- and left
     * unanswered each one stops a boot stage dead, so they are kept here by
     * encoding: read gives back what was written, starting at zero.
     */
    struct { uint32_t enc; uint64_t value; } apple_shadow[96];
    unsigned apple_shadow_used;
/* The emulated physical timer control, kept out of the kernel's slot. */
#define OHV_SHADOW_CNTP_CTL 30
    /*
     * The ID registers.  They describe the machine, but they are the VMM's to
     * set: it builds a CPU model out of what the host reports, adjusts it,
     * writes the result back and then checks that reading it returns what it
     * wrote.  Serving them straight out of the capabilities and dropping
     * writes fails that check on the first exit.
     */
    uint64_t id_regs[20];
    bool id_regs_loaded;
    /*
     * The interrupt controller's CPU interface, as the guest sees it.  These
     * are system registers, and while the virtual interface is not serving
     * them every access leaves the guest; the framework answers them out of
     * its own model rather than passing them up, and so does this.
     */
    struct { uint16_t enc; uint64_t value; } gic_if[48];
    unsigned gic_if_count;
    bool gic_if_ready;
    /*
     * Which level the guest believes it is at.  HCR_EL2.NV is what makes an
     * EL1 guest look like a hypervisor to itself -- CurrentEL answers EL2 and
     * the EL2 registers redirect -- so it belongs on only while the guest
     * hypervisor is running its own code, and has to come off when the guest
     * drops into the guest it is hosting.
     */
    bool guest_at_el2;
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
    uint8_t kind;       // 0=shared 1=banked 2=extreg 3=debug 5=ro-id 6=raw offset 7=shadow
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