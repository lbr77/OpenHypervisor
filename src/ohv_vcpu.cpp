// ohv_vcpu.cpp - vCPU lifecycle, register access, run loop glue.
#include <new>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "openhyp/ohv_object.h"
#include "ohv_internal.h"
#include <cstdlib>
#include <cstdio>
#include "openhyp/ohv_trap.h"

/*
 * getenv(), remembered.
 *
 * Every trace switch in this file is read with getenv(), and some of them sit
 * on paths that run on each and every exit -- the timer sweep and the virtual
 * FIQ level are evaluated once per turnaround, and the gate rule below reads
 * one more.  getenv() takes a lock inside libc, and a sample of a stalled run
 * shows the exit path down in _os_unfair_lock_lock_slow because of it.
 *
 * The environment does not change while a VM is running, so each thread keeps
 * its own answers.  Thread-local means no sharing and so no race; the keys are
 * string literals and are compared by address, and a name that somehow arrives
 * at two addresses only costs a second slot.
 */
static const char *ohv_env(const char *name) {
    struct Entry { const char *key; const char *value; };
    static thread_local Entry cache[128];
    static thread_local unsigned used;

    for (unsigned i = 0; i < used; i++) {
        if (cache[i].key == name) {
            return cache[i].value;
        }
    }
    const char *value = getenv(name);
    if (used < 128) {
        cache[used].key = name;
        cache[used].value = value;
        used++;
    }
    return value;
}

/* Set once the guest's PC first lands in the kernel; defined below. */
extern bool g_kernel_running;


using namespace ohv;

extern "C" uint64_t ohv_caps_field(const ohv_capabilities_t *, uint16_t enc);
extern "C" uint64_t ohv_id_reg_for(const ohv_capabilities_t *, unsigned which, bool for_config);
/* Which slot in VcpuSlot::id_regs an encoding names, or -1 for none. */
extern "C" int ohv_id_slot(uint16_t encoding);
#define OHV_ID_SLOT_MIDR  18
#define OHV_ID_SLOT_MPIDR 19

struct hv_vcpu_config_s {
    /*
     * The same handle ohv_misc.cpp allocates, so the isa has to be declared
     * here too or every field below is read eight bytes out of place.
     */
    Class __isa;
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

/*
 * What the kernel's classification means, taken from the framework's own
 * handler rather than from the names.  Three outcomes, not one: hand the exit
 * to the VMM, tell the VMM nothing happened, or do not return to the VMM at
 * all and enter the guest again.
 *
 * That last one is the part that was missing.  Several classifications are
 * the kernel asking to be re-entered -- they carry no exception and mean
 * nothing to a VMM -- and answering them with an exit record leaves the
 * caller looking at a reason it has no case for.  qemu's is a switch that
 * ends in g_assert_not_reached().
 */
enum ExitAction {
    EXIT_TO_VMM,
    EXIT_KEEP_RUNNING,
};

static void synth_msr_trap_syndrome(VcpuSlot *s, hv_vcpu_exit_t *out) {
    /*
     * The kernel says a system register access trapped and stops there: no
     * syndrome, no opcode, and nothing about it anywhere in the shared pages.
     * A VMM cannot answer an access it cannot name, so read the instruction
     * the guest is sitting on and build the syndrome the exception class is
     * defined to carry.
     *
     * PC is a virtual address in general; this reaches it as a guest physical
     * one, which is right while the guest MMU is off and is where a monitor
     * being brought up spends its first instructions.  With translation on
     * there is a stage-1 walk to do first, and until that exists the syndrome
     * stays zero rather than wrong.
     */
    uint64_t pc = ohv_rw(s->ctx)->regs.pc;
    const uint32_t *insn = (const uint32_t *)ohv_guest_ptr(pc, 4);

    if (insn && (*insn & 0xffd00000u) == 0xd5100000u) {
        uint32_t i = *insn;
        uint32_t read = (i >> 21) & 1;
        uint32_t op0 = 2 + ((i >> 19) & 1);
        uint32_t op1 = (i >> 16) & 7;
        uint32_t crn = (i >> 12) & 0xf;
        uint32_t crm = (i >> 8) & 0xf;
        uint32_t op2 = (i >> 5) & 7;
        uint32_t rt = i & 0x1f;

        out->exception.syndrome =
            (0x18ull << 26) | (1ull << 25) |
            (op0 << 20) | (op2 << 17) | (op1 << 14) |
            (crn << 10) | (rt << 5) | (crm << 1) | read;
    }
}

static void synth_guarded_exit_syndrome(VcpuSlot *s, hv_vcpu_exit_t *out) {
    /*
     * A guarded exit arrives as a plain synchronous exit with no syndrome:
     * the kernel does not perform GEXIT for a guest at EL2 and does not
     * describe it either.  The framework answers its caller with EC 0x3f, and
     * a VMM that has to complete the guarded return needs to be told which
     * exit this is -- so read the instruction the guest stopped on and say so.
     *
     * Only a guest actually in guarded state can execute it; outside guarded
     * state the same encoding is undefined, which is how the exception class
     * earns its meaning.
     */
    uint64_t pc = ohv_rw(s->ctx)->regs.pc;
    const uint32_t *insn = (const uint32_t *)ohv_guest_ptr(pc, 4);

    if (insn && *insn == 0x00201400u) {   /* GEXIT */
        out->exception.syndrome = (0x3full << 26) | (1ull << 25) | 0x22;
    }
}

/*
 * Which EL2 register an access names, as an offset into the VNCR page.  These
 * are the registers FEAT_NV2 redirects: at EL1 with HCR_EL2.{NV,NV2} set the
 * hardware turns the access into a memory access at VNCR_EL2 plus the offset,
 * and the guest never knows.  When it arrives here as a trapped register
 * access instead, doing the memory access is doing what was meant to happen.
 */
struct Nv2Reg { uint16_t enc; uint16_t ctx_off; bool banked; };

/*
 * Where a guest hypervisor's EL2 register actually lives.
 *
 * Two kinds.  The ones that describe the translation the guest runs under --
 * and the exception state it takes an exception with -- are the EL1 registers
 * themselves: with the host extensions on and the guest at hardware EL1, its
 * "EL2" MMU is the EL1 MMU, and a copy kept anywhere else is a value the
 * hardware never sees.  That is measurable: a guest at its EL2 writing
 * ELR_EL2, ESR_EL2 or FAR_EL2 lands on the banked EL1 slots without anything
 * here doing it.
 *
 * The rest have no EL1 counterpart -- the stage-2 registers, the traps, the
 * identification registers -- and go to the VNCR page, where the guest reads
 * them back and this library reads them when it has to enter or leave the
 * guest's EL2 on its behalf.
 */
#define VNCR(x)  (uint16_t)(0x1000 + (x)), false
#define BANKED(i) (uint16_t)(OHV_RW_BANKED_SYSREGS + (i) * 8), true

static const Nv2Reg kNv2Regs[] = {
    /* The EL1&0 translation regime, which is the guest's EL2&0 regime. */
    {0xe080, BANKED(12)},  /* SCTLR_EL2      -> SCTLR_EL1      */
    {0xe08a, BANKED(13)},  /* CPTR_EL2       -> CPACR_EL1      */
    {0xe100, BANKED(0)},   /* TTBR0_EL2      -> TTBR0_EL1      */
    {0xe101, BANKED(1)},   /* TTBR1_EL2      -> TTBR1_EL1      */
    {0xe102, BANKED(2)},   /* TCR_EL2        -> TCR_EL1        */
    {0xe510, BANKED(6)},   /* MAIR_EL2       -> MAIR_EL1       */
    {0xe518, BANKED(7)},   /* AMAIR_EL2      -> AMAIR_EL1      */
    {0xe681, BANKED(17)},  /* CONTEXTIDR_EL2 -> CONTEXTIDR_EL1 */
    {0xe200, BANKED(14)},  /* SPSR_EL2       -> SPSR_EL1       */
    {0xe201, BANKED(3)},   /* ELR_EL2        -> ELR_EL1        */
    {0xe300, BANKED(4)},   /* FAR_EL2        -> FAR_EL1        */
    {0xe290, BANKED(5)},   /* ESR_EL2        -> ESR_EL1        */


    /* The ones that are the guest hypervisor's alone. */
    {0xe000, VNCR(0x088)}, /* VPIDR_EL2   */
    {0xe005, VNCR(0x050)}, /* VMPIDR_EL2  */
    {0xe088, VNCR(0x078)}, /* HCR_EL2     */
    {0xe089, VNCR(0x130)}, /* MDCR_EL2    */
    {0xe08b, VNCR(0x080)}, /* HSTR_EL2    */
    {0xe108, VNCR(0x020)}, /* VTTBR_EL2   */
    {0xe10a, VNCR(0x040)}, /* VTCR_EL2    */
    {0xe304, VNCR(0x1f8)}, /* HPFAR_EL2   */
    {0xe682, VNCR(0x090)}, /* TPIDR_EL2   */
    {0xe703, VNCR(0x060)}, /* CNTVOFF_EL2 */
    {0xe708, VNCR(0x0e0)}, /* CNTHCTL_EL2 */
    /*
     * VBAR_EL2 stays here rather than aliasing onto VBAR_EL1, because a guest
     * hypervisor has both and uses them for different things: the boot ROM
     * installs an EL2 table, drops to EL1, and installs a second one there.
     * Folding the two together loses whichever it wrote first, and the entry
     * this library builds when it takes an exception into the guest's EL2
     * then lands in the wrong table.
     */
    {0xe600, VNCR(0x250)}, /* VBAR_EL2    */
};


static const Nv2Reg *nv2_lookup(unsigned enc) {
    for (const Nv2Reg &r : kNv2Regs) {
        if (r.enc == enc) return &r;
    }
    return nullptr;
}

/*
 * Carry out a trapped EL2 register access against the VNCR page.  Answers
 * false when the register is not one of them, and the exit goes to the VMM.
 */
/*
 * The CPU interface registers, kept per vCPU.  A guest that is only resetting
 * the controller -- which is what this machine's firmware does, having its
 * own interrupt controller elsewhere -- needs them to read back what it
 * wrote and nothing more.
 */
static uint64_t *gic_if_slot(VcpuSlot *s, unsigned enc) {
    if (!s->gic_if_ready) {
        s->gic_if_ready = true;
        s->gic_if_count = 0;
        /*
         * ICC_SRE_EL1 says the interface is reachable as system registers,
         * which is how the guest got here in the first place; reading back
         * zero would tell it to go looking for a memory-mapped one instead.
         */
        s->gic_if[s->gic_if_count].enc = 0xc665;
        s->gic_if[s->gic_if_count].value = 0x7;
        s->gic_if_count++;
    }
    for (unsigned i = 0; i < s->gic_if_count; i++) {
        if (s->gic_if[i].enc == enc) return &s->gic_if[i].value;
    }
    if (s->gic_if_count >= sizeof(s->gic_if) / sizeof(s->gic_if[0])) {
        return nullptr;
    }
    s->gic_if[s->gic_if_count].enc = (uint16_t)enc;
    s->gic_if[s->gic_if_count].value = 0;
    return &s->gic_if[s->gic_if_count++].value;
}

static bool is_gic_interface_reg(unsigned op0, unsigned op1, unsigned crn,
                                 unsigned crm, unsigned op2) {
    if (op0 != 3) {
        return false;
    }
    /* ICC_* and ICV_* at EL1, and ICH_* at EL2, all of which sit in c12. */
    if (crn == 12 && (op1 == 0 || op1 == 4) && crm >= 8 && crm <= 12) {
        return true;
    }
    /* ICC_PMR_EL1 and ICV_PMR_EL1 are the exception: they live in c4. */
    if (crn == 4 && op1 == 0 && crm == 6 && op2 == 0) {
        return true;
    }
    return false;
}

/*
 * A stage-1 walk of the guest's own tables, as AT would do it.  Returns what
 * PAR_EL1 should read: bit 0 clear and the physical address in place on a
 * hit, bit 0 set on a fault, which is the shape the caller checks first.
 *
 * Only what the guest actually uses is modelled -- a single TTBR0 regime with
 * one of the three granules, blocks and pages, no second stage.  A second
 * stage is the kernel's and does not appear in this answer: at EL1 an AT
 * reports the address its own tables produce.
 */
#define OHV_PAR_FAULT 1ull

static uint64_t ohv_translate_stage1(VcpuSlot *s, uint64_t va) {
    uint64_t tcr = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 2 * 8);
    uint64_t ttbr0 = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 0 * 8);
    uint64_t sctlr = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 12 * 8);
    unsigned page_bits, level_bits;

    if ((sctlr & 1) == 0) {
        /* Translation off: the address is its own answer. */
        return va & 0x0000fffffffff000ull;
    }
    /*
     * Which half of the address space, and so which base register and size.
     * The payload runs in the EL2&0 regime with the host extensions on, and
     * that regime has both halves -- its stack is in the upper one, so a walk
     * that only knows TTBR0 answers "fault" for the address it is standing
     * on.
     */
    bool upper = (va >> 63) != 0;
    unsigned tsz = upper ? (unsigned)((tcr >> 16) & 0x3f)
                         : (unsigned)(tcr & 0x3f);
    unsigned tg = upper ? (unsigned)((tcr >> 30) & 3)
                        : (unsigned)((tcr >> 14) & 3);

    if (upper) {
        ttbr0 = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 1 * 8);
        /* TG1 spells the granules in a different order than TG0 does. */
        switch (tg) {
            case 1: page_bits = 14; break;   /* 16 KiB */
            case 2: page_bits = 12; break;   /* 4 KiB  */
            case 3: page_bits = 16; break;   /* 64 KiB */
            default: return OHV_PAR_FAULT;
        }
    } else {
        switch (tg) {
            case 0: page_bits = 12; break;   /* 4 KiB  */
            case 1: page_bits = 16; break;   /* 64 KiB */
            case 2: page_bits = 14; break;   /* 16 KiB */
            default: return OHV_PAR_FAULT;
        }
    }
    unsigned t0sz = tsz;
    level_bits = page_bits - 3;

    unsigned va_bits = 64 - t0sz;
    if (va_bits < page_bits || va_bits > 52) {
        return OHV_PAR_FAULT;
    }
    /* The level the walk starts at, given how many bits are left to resolve. */
    unsigned level = 3;
    unsigned resolved = page_bits;
    while (level > 0 && resolved + level_bits < va_bits) {
        resolved += level_bits;
        level--;
    }

    uint64_t table = ttbr0 & 0x0000fffffffffffeull & ~((1ull << page_bits) - 1);
    for (;;) {
        unsigned shift = page_bits + (3 - level) * level_bits;
        unsigned bits = (shift + level_bits > va_bits) ? (va_bits - shift)
                                                       : level_bits;
        uint64_t index = (va >> shift) & ((1ull << bits) - 1);
        (void)upper;
        const uint64_t *entry =
            (const uint64_t *)ohv_guest_ptr(table + index * 8, 8);

        if (!entry) {
            return OHV_PAR_FAULT;
        }
        uint64_t desc = *entry;

        if ((desc & 1) == 0) {
            return OHV_PAR_FAULT;              /* invalid */
        }
        uint64_t next = desc & 0x0000fffffffff000ull;

        if ((desc & 3) == 1 || level == 3) {
            /* A block, or the last level, which is the page itself. */
            uint64_t mask = (1ull << shift) - 1;

            return (next & ~mask) | (va & mask & 0x0000fffffffff000ull);
        }
        table = next & ~((1ull << page_bits) - 1);
        level++;
        if (level > 3) {
            return OHV_PAR_FAULT;
        }
    }
}

static bool service_nv2_access(VcpuSlot *s, uint64_t esr) {
    unsigned op0 = (unsigned)((esr >> 20) & 3);
    unsigned op2 = (unsigned)((esr >> 17) & 7);
    unsigned op1 = (unsigned)((esr >> 14) & 7);
    unsigned crn = (unsigned)((esr >> 10) & 0xf);
    unsigned rt  = (unsigned)((esr >> 5) & 0x1f);
    unsigned crm = (unsigned)((esr >> 1) & 0xf);
    bool read = (esr & 1) != 0;
    unsigned enc = (op0 << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2;

    /*
     * AMXIDR_EL1, which the kernel insists on being told about.
     *
     * The Apple hidden-ID registers always trap -- HACR_EL2's THIDDVF,
     * THIDCPU, THIDLLC, THIDAMX and THIDDPC are in the kernel's fixed set --
     * so this one is the VMM's to answer, and XNU does not treat it as
     * optional.  configure_misc_apple_regs reads it and panics two ways: with
     * any of bits 5..15 set it is "Unknown AMX feature ID bit has been set",
     * and with bits 0..3 all clear it is "AMXIDR_EL1 doesn't advertise any
     * known ...".  Exactly one of the low four says which version, counting
     * downward from bit 3, so the lowest that satisfies it is one.
     *
     * Unanswered, the read takes an exception the kernel has no handler for
     * and the machine parks on its own synchronous vector at EL1 -- measured,
     * three thousand entries at the same instruction without retiring it.
     */
    if (op0 == 3 && op1 == 6 && crn == 15 && crm == 2 && op2 == 7 && read) {
        const char *want = ohv_env("OHV_AMXIDR");
        uint64_t v = want ? strtoull(want, nullptr, 0) : 1;
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        if (rt != 31) rw->regs.x[rt] = v;
        rw->regs.pc += 4;
        return true;
    }

    /*
     * The boot ROM's handoff saying the monitor runs next.
     *
     * s3_6_c15_c15_7 is not a register anything implements; it is a word our
     * own handoff writes at the one instant that knows SPTM is about to
     * start.  Arming NV here is what makes SPTM's guarded ERET trap, which is
     * the only chance to mirror ELR_GL1/SPSR_GL1 onto the pair a hardware
     * return actually consumes.  Left to chance it works only when the
     * guest's last transition happened to leave NV set.
     */
    if (op0 == 3 && op1 == 6 && crn == 15 && crm == 15 && op2 == 7 && !read) {
        ohv_rw_controls(s->ctx)->hcr_el2 |= OHV_HCR_NV;
        mark_dirty(s->ctx, OHV_STATE_CONTROLS);
        s->guest_at_el2 = true;
        if (ohv_env("OHV_TRACE_ERET")) {
            fprintf(stderr, "[ohv] handoff: monitor next, NV armed, hcr %#llx\n",
                    (unsigned long long)ohv_rw_controls(s->ctx)->hcr_el2);
        }
        ohv_rw(s->ctx)->regs.pc += 4;
        return true;
    }

    /*
     * The three registers HACR_EL2.TGXF actually covers.
     *
     * It is not the guarded bank -- ELR_GL1 and SPSR_GL1 are never trapped by
     * anything, so there is nowhere to see SPTM set them.  What TGXF traps is
     * GXF_CONFIG_EL1, GXF_ENTRY_EL1 and GXF_PABENTRY_EL1, once: the kernel
     * clears the bit after servicing the first one.  That once is what
     * matters.  Servicing it marks ARM_GUEST_STATE_GXF used, and from then on
     * the kernel saves and restores the whole guarded bank around every entry
     * -- SP_GL1, ASPSR_GL12, VBAR_GL12, ELR_GL12, SPSR_GL12 and the rest.
     * Without it that bank is never carried at all, which is why the copy in
     * the shared page reads zero however often it is asked.
     */
    if (op0 == 3 && op1 == 6 && crn == 15 &&
        ((crm == 1 && op2 == 2) || (crm == 8 && (op2 == 1 || op2 == 2)))) {
        unsigned ext = (crm == 1) ? 42 : (op2 == 1 ? 43 : 44);
        volatile uint64_t *slot =
            ohv_rw_u64(s->ctx, OHV_RW_EXTREGS + ext * 8);
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        if (read) {
            if (rt != 31) rw->regs.x[rt] = *slot;
        } else {
            *slot = (rt == 31) ? 0 : rw->regs.x[rt];
            mark_dirty(s->ctx, OHV_STATE_GXF);
        }
        if (ohv_env("OHV_TRACE_ERET")) {
            fprintf(stderr, "[ohv] gxf %s ext[%u] = %#llx pc %#llx\n",
                    read ? "read" : "write", ext,
                    (unsigned long long)*slot,
                    (unsigned long long)rw->regs.pc);
        }
        rw->regs.pc += 4;
        return true;
    }

    /*
     * The cache identification registers.  HCR_EL2.TID2 is in the kernel's
     * fixed set, so every CLIDR_EL1, CCSIDR_EL1 and CSSELR_EL1 access traps
     * here whatever this library asks for -- and a stage's entry code cleans
     * the data caches by set and way, which means reading both of the first
     * two before it has done anything else.  Left unanswered, the very first
     * instruction of the next stage takes an exception it has no handler for:
     * measured, iBEC reaches its fourth instruction and the machine resets.
     *
     * The answers are the host's, which is what the capabilities carry, and
     * which level and which of the two caches is being asked about comes from
     * CSSELR_EL1 -- a banked-per-guest value, so it is read back from the
     * shared page rather than from the host register.
     */
    if (op0 == 3 && crn == 0 && crm == 0 &&
        ((op1 == 1 && (op2 == 0 || op2 == 1)) || (op1 == 2 && op2 == 0))) {
        static ohv_capabilities_t caps{};
        static bool have = false;
        volatile uint64_t *csselr =
            ohv_rw_u64(s->ctx, OHV_RW_SHARED_SYSREGS + 7 * 8);
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
        uint64_t v = 0;

        if (!have) {
            ohv_raw_trap(OHV_TRAP_CAPABILITIES, &caps);
            have = true;
        }
        if (op1 == 2) {                       /* CSSELR_EL1 */
            if (read) {
                v = *csselr;
            } else {
                *csselr = (rt == 31) ? 0 : rw->regs.x[rt];
                mark_dirty(s->ctx, OHV_STATE_SYSREGS);
            }
        } else if (op2 == 1) {                /* CLIDR_EL1 */
            if (!read) return false;
            v = caps.clidr_el1;
        } else {                              /* CCSIDR_EL1 */
            unsigned level = (unsigned)((*csselr >> 1) & 7);

            if (!read) return false;
            v = (*csselr & 1) ? caps.ccsidr_el1_inst[level]
                              : caps.ccsidr_el1_data_or_unified[level];
        }
        if (read && rt != 31) rw->regs.x[rt] = v;
        if (ohv_env("OHV_TRACE_TID2")) {
            fprintf(stderr, "[ohv] tid2 %s op1=%u op2=%u csselr %#llx"
                    " -> %#llx pc %#llx\n", read ? "read" : "write", op1, op2,
                    (unsigned long long)*csselr, (unsigned long long)v,
                    (unsigned long long)rw->regs.pc);
        }
        rw->regs.pc += 4;
        return true;
    }

    /*
     * The counters.  A guest that believes it is at EL2 expects to read these
     * without asking anyone, and letting it -- by opening CNTHCTL_EL2 -- hands
     * it the host's raw count.  That is a different clock from the one the
     * VMM's devices run on, and firmware that waits by comparing a device's
     * tick against the counter then waits on two clocks that never agree.
     * Answering here keeps it on the VMM's, which is the offset the VMM
     * already told us about.
     */
    if (op0 == 3 && op1 == 3 && crn == 14 && crm == 0 && read) {
        uint64_t v = 0;

        if (op2 == 1 || op2 == 2) {          /* CNTPCT_EL0, CNTVCT_EL0 */
            v = mach_absolute_time() -
                ohv_rw_controls(s->ctx)->virtual_timer_offset;
        } else if (op2 == 0) {               /* CNTFRQ_EL0 */
            uint64_t freq;

            __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
            v = freq;
        } else {
            return false;
        }
        if (rt != 31) ohv_rw(s->ctx)->regs.x[rt] = v;
        ohv_rw(s->ctx)->regs.pc += 4;
        return true;
    }

    /*
     * The timers themselves.  Their state lives in the banked system
     * registers, which is where the kernel loads them from on the way into
     * the guest; TVAL is not stored at all, it is the distance from now to
     * the compare value and is read and written as such.
     */
    if (op0 == 3 && op1 == 3 && crn == 14 && (crm == 2 || crm == 3)) {
        bool physical = (crm == 2);

        /*
         * Gated on where the guest is rather than on a count.  The firmware
         * reads these registers in loops, so any budget is spent long before
         * the kernel runs -- and the question worth answering is whether the
         * kernel's own accesses arrive here at all, or are redirected to the
         * EL2 timers behind this side's back.
         */
        /*
         * Every write the guest makes to the physical timer, whatever era it
         * is in.  A trace of the interrupt's edges says when the timer fires
         * and when it is masked, but not whether the firmware ever arms it
         * again -- and arming with a deadline still in the future moves no
         * edge at all, so it is invisible from there.
         */
        if (ohv_env("OHV_TRACE_TIMERREG") && !read && physical) {
            fprintf(stderr, "[ohv] cntp %s = %#llx pc %#llx\n",
                    op2 == 0 ? "tval" : (op2 == 1 ? "ctl " : "cval"),
                    (unsigned long long)(rt == 31 ? 0
                                         : ohv_rw(s->ctx)->regs.x[rt]),
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc);
        }
        unsigned ctl_index = physical ? 19 : 18;
        unsigned cval_index = physical ? 10 : 9;
        /*
         * The physical timer is emulated, so the guest's control word is kept
         * here and the banked slot -- which the kernel loads straight into the
         * hardware timer -- is left disabled.  Armed for real, the hardware
         * timer keeps asserting into the host from the moment the compare
         * value passes until the guest re-arms it, and a guest that is waiting
         * for this very interrupt never gets a cycle in which to do so: every
         * entry leaves again before it retires an instruction.  The waking is
         * the VMM's job now, off its own clock.
         */
        volatile uint64_t *ctl = physical
            ? (volatile uint64_t *)&s->el2_shadow[OHV_SHADOW_CNTP_CTL]
            : ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + ctl_index * 8);

        if (physical) {
            *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + ctl_index * 8) = 0;
        }
        volatile uint64_t *cval =
            ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + cval_index * 8);
        /*
         * The compare value is kept in the host's counter, not the guest's.
         * The kernel restores this slot straight into the hardware timer,
         * which compares it against CNTPCT -- so a guest-relative value, tiny
         * beside the host's count, reads as already expired the moment the
         * guest enables the timer, and the interrupt that fires on every
         * entry from then on leaves no cycle for the guest to retire even the
         * instruction after the one that armed it.  Translate on the way in
         * and out instead: the guest keeps its own clock, the hardware keeps
         * the one it can act on.
         */
        uint64_t off = ohv_rw_controls(s->ctx)->virtual_timer_offset;
        uint64_t hnow = mach_absolute_time();
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
        uint64_t v = (rt == 31) ? 0 : rw->regs.x[rt];

        switch (op2) {
            case 0:                                   /* TVAL */
                if (read) v = (uint64_t)((int64_t)*cval - (int64_t)hnow);
                else *cval = hnow + (uint64_t)(int32_t)(uint32_t)v;
                break;
            case 1:                                   /* CTL  */
                if (read) v = *ctl;
                else *ctl = v;
                break;
            case 2:                                   /* CVAL */
                if (read) v = *cval - off;
                else *cval = v + off;
                break;
            default:
                return false;
        }
        if (read) {
            if (rt != 31) rw->regs.x[rt] = v;
        } else {
            mark_dirty(s->ctx, OHV_STATE_SYSREGS);
        }
        rw->regs.pc += 4;
        return true;
    }

    if (is_gic_interface_reg(op0, op1, crn, crm, op2)) {
        uint64_t *slot = gic_if_slot(s, enc);
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        if (!slot) return false;
        if (read) {
            if (rt != 31) rw->regs.x[rt] = *slot;
        } else {
            *slot = (rt == 31) ? 0 : rw->regs.x[rt];
        }
        rw->regs.pc += 4;
        return true;
    }
    /*
     * The EL2 cache and TLB maintenance instructions -- TLBI, DC, IC, AT with
     * Op0 of 1 and Op1 of 4.  A guest hypervisor issues them against its own
     * translation regime, which here is the EL1 one the hardware is already
     * keeping coherent: the kernel invalidates on every change it makes to
     * the stage-2 tables and to the registers that name them, so there is
     * nothing left for these to do.  What matters is that they complete --
     * left to fall through, the boot ROM's `tlbi alle2` on the way to
     * handing over the machine becomes an exception the guest takes with no
     * vector table to take it with.
     */
    if (ohv_env("OHV_TRACE_AT") && op0 == 1 && crn == 7) {
        fprintf(stderr, "[ohv] vcpu %llu system insn op1=%u crn=%u crm=%u"
                " op2=%u rt=%u pc=%#llx\n", (unsigned long long)s->id,
                op1, crn, crm, op2, rt,
                (unsigned long long)ohv_rw(s->ctx)->regs.pc);
    }
    /*
     * AT is not maintenance: it answers.  The address translation
     * instructions sit at CRn 7 with CRm 8 or 9, and each one is a question
     * whose reply is PAR_EL1 -- iBoot asks its own EL2 regime to translate an
     * address and compares the answer with the walk it did itself, so an AT
     * that is quietly stepped over leaves a stale PAR, the two disagree, and
     * it panics on the mismatch.  Leave it for the VMM.
     */
    /*
     * AT is a question, not maintenance: it translates an address and leaves
     * the answer in PAR_EL1.  iBoot asks its own EL2 regime to translate an
     * address and compares the answer against the walk it did itself; an AT
     * that is stepped over leaves PAR holding whatever it held before, the
     * two disagree, and it panics on the mismatch.
     *
     * The guest's EL2 translation regime is the EL1 one -- its TTBR0_EL2 and
     * TCR_EL2 are kept in the real EL1 registers, which is what makes its
     * page tables the ones the hardware is walking -- so answering means
     * walking those same tables here.  Being the same tables, the answer is
     * the same by construction.
     */
    /*
     * The kernel's own AT S1E1R comes here too, for a reason of our making:
     * with HCR_EL2.NV set the address-translation instructions trap out of
     * EL1, and NV is set because SPTM's guarded return had to be trapped to be
     * completed.  So once XNU is running, every translation it asks for is
     * ours to answer -- measured, one AT S1E1R at arm_init time arrives as an
     * undefined instruction and handle_uncategorized turns it into a panic.
     * The tables are the same ones the hardware walks, so the same walk
     * answers both.
     */
    if (op0 == 1 && (op1 == 4 || op1 == 0) && crn == 7 &&
        (crm == 8 || crm == 9)) {
        uint64_t va = (rt == 31) ? 0 : ohv_rw(s->ctx)->regs.x[rt];

        *ohv_rw_u64(s->ctx, OHV_RW_SHARED_SYSREGS + 6 * 8) =
            ohv_translate_stage1(s, va);
        mark_dirty(s->ctx, OHV_STATE_SYSREGS);
        ohv_rw(s->ctx)->regs.pc += 4;
        return true;
    }
    if (op0 == 1 && op1 == 4) {
        ohv_rw(s->ctx)->regs.pc += 4;
        return true;
    }

    /*
     * A PSTATE field this host does not have.
     *
     * The kernel's thread switch writes S3_3_C4_C2_6 from a byte it keeps per
     * thread.  CRn 4 is where the PSTATE fields live, and a write to one the
     * silicon does not implement is UNDEFINED -- which is what the guest gets,
     * and handle_uncategorized turns it into a panic.  The firmware is built
     * for an A15 and the host is an M4; the field is simply not there.
     *
     * Swallowing it is the honest answer: there is no bit to set, so the guest
     * reads back what it wrote and nothing pretends otherwise.
     */
    if (op0 == 3 && op1 == 3 && crn == 4 && crm == 2 && op2 == 6) {
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
        static uint64_t held;

        if (read) {
            if (rt != 31) rw->regs.x[rt] = held;
        } else {
            held = (rt == 31) ? 0 : rw->regs.x[rt];
        }
        rw->regs.pc += 4;
        return true;
    }

    /*
     * Everything else Apple defines at CRn 15.
     *
     * These are the hidden ID and chicken registers, and the kernel is not
     * shy with them: AMXIDR_EL1 decides whether it panics about AMX, EHID4 and
     * HID4 get a bit cleared and written back, ACNTVCT_EL0 is read inside
     * DebuggerXCallEnter.  Each one that goes unanswered is an exception the
     * guest has no handler for at that point, and the boot stops there --
     * answering them one run at a time costs a run each, so answer the class.
     *
     * A shadow is the honest model for the ID and chicken words: the guest
     * reads back what it wrote, and nothing pretends the silicon changed.  The
     * counters are the exception, since a counter that does not advance is
     * worse than one that is approximate.
     */
    /*
     * VM_TMR_FIQ_ENA_EL2, which is how Apple gates delivery of the guest's own
     * timers.  Linux names it SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2 = sys_reg(3, 5,
     * 15, 1, 3), with VM_TMR_FIQ_ENABLE_P in bit 0 and ..._V in bit 1, and its
     * AIC driver masks a guest timer FIQ by clearing the matching bit -- that
     * clear is the acknowledgement, there is no other end-of-interrupt.
     *
     * The guest writes it once, with zero, in the middle of bringing up its
     * own interrupt handling: "do not deliver guest timer interrupts".  Left
     * unanswered here the write went out to the VMM, which kept the value and
     * could do nothing with it, and ohv_expire_timers() carried on raising one
     * anyway -- which is the interrupt the kernel dies on, with no controller
     * line raised anywhere to explain it.  Answering it separates the two
     * phases on its own: iBoot never writes it and keeps what it relies on.
     */
    /*
     * The GL2 bank.  SPTM runs at GL2 and keeps its per-CPU block in
     * TPIDR_GL2 (c11_1); refused here it went out to the VMM, which kept the
     * value where nothing could reach it, and SPTM's guarded handler was left
     * reading a stack that was never its own.  Its own assertion names the
     * result: "[SPTM] Synchronous exception taken while SP0 selected in GL2".
     *
     * Answering these is not the same call as the GL1 bank next door.  Those
     * go to the extended registers the kernel loads into hardware, and
     * shadowing them would tell the guest something the silicon never heard.
     * For GL2 there is no such register to disagree with: the host's guest
     * state models GL1 and only GL1, so this is the only place the value can
     * live.
     */
    if (op0 == 3 && op1 == 6 && crn == 15 && crm == 11 && op2 < 8) {
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        if (read) {
            if (rt != 31) rw->regs.x[rt] = s->gl2_bank[op2];
        } else {
            s->gl2_bank[op2] = rt == 31 ? 0 : rw->regs.x[rt];
        }
        rw->regs.pc += 4;
        return true;
    }
    if (op0 == 3 && op1 == 5 && crn == 15 && crm == 1 && op2 == 3) {
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        if (read) {
            if (rt != 31) rw->regs.x[rt] = s->vm_tmr_fiq_ena;
        } else {
            uint64_t was = s->vm_tmr_fiq_ena;

            s->vm_tmr_fiq_ena = rt == 31 ? 0 : rw->regs.x[rt];
            /*
             * Every write, with nowhere to hide.  Clearing a bit here stops
             * ohv_expire_timers raising that timer at all, and the kernel
             * boot has been ending with the virtual bit clear and nothing
             * setting it back -- so what is worth knowing is which address
             * turns it off, and whether the same one ever turns it on.  These
             * writes are rare enough that a budget would only hide them.
             */
            if (ohv_env("OHV_TRACE_TMRENA")) {
                fprintf(stderr, "[ohv] vm_tmr_fiq_ena %#llx -> %#llx"
                        " pc %#llx\n", (unsigned long long)was,
                        (unsigned long long)s->vm_tmr_fiq_ena,
                        (unsigned long long)rw->regs.pc);
            }
        }
        rw->regs.pc += 4;
        return true;
    }
    /*
     * The CTRR bank belongs here too, and did not reach it.
     *
     * SPTM writes CTRR_A_LWR/UPR/CTL/LOCK at op1 4, CRm 2 and XNU reads the
     * same silicon back as CTRR_A_LWR/UPR_EL2 at CRm 6.  Neither was covered
     * by the condition below -- it took only CRm 10 -- so the fold that puts
     * the two views in one place was written inside a block that never ran,
     * and what SPTM wrote was still not what XNU read:
     *
     *     zalloc_ro_mut failed: source (...) not from RO zone map (...),
     *     current stack (...) or const memory (phys 0 - 0)
     *
     * with pid 1 as the thread, because machine_init stashes those two
     * registers as the kernel's read-only region and every zalloc_ro_mut out
     * of the kernel's own const data is refused when that range is empty.
     */
    /*
     * The kernel's read-only region, which is one register with two names.
     *
     * SPTM programs it as CTRR_A_LWR/UPR_EL1 -- op1 4, CRn 15, CRm 2, op2 3
     * and 4 -- and XNU reads it back as CTRR_A_LWR/UPR_EL2, which are CRm 6,
     * op2 4 and 5.  Two encodings, one piece of silicon.  Kept apart, what
     * SPTM wrote was never what XNU read and machine_init stashed a range of
     * nothing:
     *
     *     zalloc_ro_mut failed: source (...) not from RO zone map (...),
     *     current stack (...) or const memory (phys 0 - 0)
     *
     * with pid 1 as the thread, because every zalloc_ro_mut out of the
     * kernel's own const data is refused when that range is empty.
     *
     * Only these six encodings, and not the CRm they sit in: the block next
     * door takes whole CRms, and taking CRm 2 that way stopped the machine
     * before iBSS ever answered its USB.
     */
    if (op0 == 3 && op1 == 4 && crn == 15 &&
        ((crm == 2 && op2 >= 2 && op2 <= 5) ||
         (crm == 6 && (op2 == 4 || op2 == 5)))) {
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
        unsigned slot = (crm == 6) ? (op2 - 1) : op2;   /* 6/4 -> 2/3, 6/5 -> 2/4 */
        unsigned i;

        for (i = 0; i < s->ctrr_used; i++) {
            if (s->ctrr[i].sel == slot) break;
        }
        if (i == s->ctrr_used) {
            if (i >= sizeof(s->ctrr) / sizeof(s->ctrr[0])) {
                return false;
            }
            s->ctrr[i].sel = (uint8_t)slot;
            s->ctrr[i].value = 0;
            s->ctrr_used++;
        }
        if (read) {
            if (rt != 31) rw->regs.x[rt] = s->ctrr[i].value;
        } else {
            s->ctrr[i].value = (rt == 31) ? 0 : rw->regs.x[rt];
        }
        if (ohv_env("OHV_TRACE_CTRR")) {
            fprintf(stderr, "[ohv] ctrr %s c%u_%u slot %u = %#llx pc %#llx\n",
                    read ? "read " : "write", crm, op2, slot,
                    (unsigned long long)s->ctrr[i].value,
                    (unsigned long long)rw->regs.pc);
        }
        rw->regs.pc += 4;
        return true;
    }
    /*
     * The three registers every thread switch touches.
     *
     * XNU's context switch writes KTRACE_MESSAGE (s3_5_c15_c1_4), HIST_TRIG
     * (s3_5_c15_c10_1) and JCTL_EL0 (s3_4_c15_c15_6) on the way through, and
     * each one left this library, crossed into the VMM, and was kept in a
     * register file nothing reads -- three exits per switch, on a machine
     * that switches threads to wait for everything.  It is why drivers report
     * starts of fifty seconds, and why AppleT8110PMGR's own timing assertion
     * fires:
     *
     *     _waitAPSCPending takes too long exeution time 8176 us domain 2
     *
     * with its counter at zero, meaning the register it waited on was ready
     * at once and only the wall clock ran over.
     *
     * The first two are trace: written zero every time, never read, and
     * answering them where the trap lands costs nothing and saves the
     * crossing.
     *
     * **JCTL_EL0 is not.**  thread_invoke reads it into the outgoing thread
     * and writes the incoming thread's value back, so it is real per-thread
     * state -- and it is the JIT control, whose extended registers include
     * japibkeylo/hi.  Held here it never reaches the silicon, the B key stops
     * being the one the kernel signed with, and the switch itself dies:
     *
     *     PAC failure from kernel with IB key at pc ... (thread_invoke)
     *
     * deterministically, at the same pc and lr every round.  It goes out.
     */
    if (op0 == 3 && crn == 15 && op1 == 5 &&
        ((crm == 1 && op2 == 4) || (crm == 10 && op2 == 1))) {
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
        unsigned slot = (crm == 1) ? 0 : 1;

        if (read) {
            if (rt != 31) rw->regs.x[rt] = s->ctx_trace[slot];
        } else {
            s->ctx_trace[slot] = (rt == 31) ? 0 : rw->regs.x[rt];
        }
        rw->regs.pc += 4;
        return true;
    }
    if (op0 == 3 && (op1 == 0 || (op1 == 4 && crm == 10))) {
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        /*
         * Op1 of six is left alone on purpose: that is the guarded bank and
         * the SPRR and GXF controls, which go to the extended registers the
         * kernel loads into hardware.  Shadowing those would answer the guest
         * while the silicon never heard, which is the whole failure this file
         * spends its length avoiding.
         */
        if (op1 == 4 && crm == 10 && (op2 == 5 || op2 == 6)) {
            if (read) {
                uint64_t v = mach_absolute_time() -
                    ohv_rw_controls(s->ctx)->virtual_timer_offset;

                if (rt != 31) rw->regs.x[rt] = v;
            }
            rw->regs.pc += 4;
            return true;
        }

        unsigned i;

        if (crn != 15) {
            return false;
        }
        for (i = 0; i < s->apple_shadow_used; i++) {
            if (s->apple_shadow[i].enc == enc) break;
        }
        if (i == s->apple_shadow_used) {
            if (i >= sizeof(s->apple_shadow) / sizeof(s->apple_shadow[0])) {
                return false;               /* out of room: let the VMM see it */
            }
            s->apple_shadow[i].enc = enc;
            s->apple_shadow[i].value = 0;
            s->apple_shadow_used++;
        }
        if (read) {
            if (rt != 31) rw->regs.x[rt] = s->apple_shadow[i].value;
        } else {
            s->apple_shadow[i].value = (rt == 31) ? 0 : rw->regs.x[rt];
        }
        if (ohv_env("OHV_TRACE_IMPDEF")) {
            fprintf(stderr, "[ohv] impdef %s s3_%u_c15_c%u_%u = %#llx pc %#llx\n",
                    read ? "read " : "write", op1, crm, op2,
                    (unsigned long long)s->apple_shadow[i].value,
                    (unsigned long long)rw->regs.pc);
        }
        rw->regs.pc += 4;
        return true;
    }

    /*
     * The three registers that decide which key a pointer is signed with.
     *
     * TXM authenticates a static pointer out of its own image and takes an
     * FPAC -- the value is right, the signature is not:
     *
     *     [TXM/SK] Unhandled synchronous exception taken from GL0/GL1
     *     esr 0x72000000  (EC 0x1c)
     *
     * SPTM writes APCTL_EL1 eleven times and KERNELKEY once each, and XNU
     * writes KERNELKEY six times more; TXM writes none of them and only uses
     * what it was given.  These go out to the VMM rather than being answered
     * here, so nothing in this library sees them without asking.  What is
     * worth knowing is the order: which of them is written, with what, and
     * where the authentication that fails sits among them.
     */
    if (ohv_env("OHV_TRACE_PAC") && op0 == 3 && op1 == 4 && crn == 15 &&
        ((crm == 0 && op2 == 4) || (crm == 1 && op2 < 2))) {
        static const char *const name[] = { "KERNELKEYLO", "KERNELKEYHI" };
        ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

        fprintf(stderr, "[ohv] vcpu %llu pac %s %s = %#llx pc %#llx\n",
                (unsigned long long)s->id,
                read ? "read " : "write",
                crm == 0 ? "APCTL_EL1" : name[op2],
                (unsigned long long)((read || rt == 31) ? 0 : rw->regs.x[rt]),
                (unsigned long long)rw->regs.pc);
    }

    const Nv2Reg *r = (op1 == 4 && op0 == 3) ? nv2_lookup(enc) : nullptr;

    if (!r) {
        if (ohv_env("OHV_TRACE_UNMATCHED")) {
            static unsigned n;

            if (n < 40) {
                n++;
                fprintf(stderr, "[ohv] unmatched sysreg %s op0=%u op1=%u"
                        " crn=%u crm=%u op2=%u rt=%u pc %#llx\n",
                        read ? "read" : "write", op0, op1, crn, crm, op2, rt,
                        (unsigned long long)ohv_rw(s->ctx)->regs.pc);
            }
        }
        return false;
    }
    volatile uint64_t *slot = ohv_rw_u64(s->ctx, r->ctx_off);
    ohv_rw_page_head_t *rw = ohv_rw(s->ctx);

    if (read) {
        uint64_t v = *slot;

        if (rt != 31) rw->regs.x[rt] = v;
    } else {
        *slot = (rt == 31) ? 0 : rw->regs.x[rt];
        if (r->banked) mark_dirty(s->ctx, OHV_STATE_SYSREGS);
    }
    rw->regs.pc += 4;
    return true;
}

/* Where in a vector table an exception from a lower AArch64 EL lands. */
#define OHV_VECT_LOWER64_SYNC 0x400

/*
 * Take an exception into the guest hypervisor's own EL2.  While the guest is
 * running the guest it hosts, HCR_EL2.NV is off and the hardware knows
 * nothing about the level the guest believes in, so the entry has to be built
 * here: the return state into the EL2 registers the guest reads, the reason
 * into ESR_EL2, and the program counter to the guest's own vector table.
 *
 * Without this the guest hypervisor can be left but never re-entered.  The
 * boot ROM drops itself to EL1 to do its work and comes back with an HVC to
 * hand the machine to the next stage; with nowhere to come back to, the HVC
 * leaves the guest for good and the payload is never entered.
 */
/*
 * Which SPSR values a hardware exception return refuses.  An illegal one does
 * not fault where it is written: the return completes, hardware sets
 * PSTATE.IL, and then the *next* instruction fetched anywhere raises EC 0x0e.
 * The report therefore names a victim three subsystems away from the cause --
 * a kext thunk whose first `stp` is illegal, with nothing wrong with the
 * thunk.  Checking the value at the moment it is written is the only way to
 * put the two together.
 *
 * M[4] is part of this: the software path below reads the mode with a 4-bit
 * mask, so an AArch32 SPSR would pass for a legal AArch64 one there.
 */
static const char *spsr_illegal_reason(uint64_t spsr) {
    if (spsr & 0x10ull) {
        return "AArch32, M[4] set";
    }
    switch ((unsigned)(spsr & 0xfull)) {
        case 0x0:   /* EL0t */
        case 0x4:   /* EL1t */
        case 0x5:   /* EL1h */
        case 0x8:   /* EL2t */
        case 0x9:   /* EL2h */
            return nullptr;
        case 0xc:
        case 0xd:
            return "EL3, which this guest does not have";
        default:
            return "reserved M[3:0]";
    }
}

/*
 * `from_el1` is the difference between the two paths.  The software one
 * rewrites EL2t/EL2h into the hardware EL1 pair before the guest sees it, so
 * those values are ordinary there.  The guarded one takes NV off and hands the
 * return to the silicon while the hardware is still at EL1 -- and a return
 * from EL1 to EL2 is illegal, whatever the guest believes its level to be.
 */
static void report_illegal_eret(VcpuSlot *s, const char *how, uint64_t elr,
                                uint64_t spsr, bool from_el1) {
    const char *why = spsr_illegal_reason(spsr);
    unsigned m = (unsigned)(spsr & 0xfull);

    if (why == nullptr && from_el1 && (m == 0x8 || m == 0x9)) {
        why = "EL2 from hardware EL1, with NV off";
    }
    if (why == nullptr) {
        return;
    }
    fprintf(stderr, "[ohv] vcpu %llu illegal exception return (%s): spsr %#llx"
            " -- %s -- elr %#llx pc %#llx\n",
            (unsigned long long)s->id, how, (unsigned long long)spsr, why,
            (unsigned long long)elr,
            (unsigned long long)ohv_rw(s->ctx)->regs.pc);
}

/*
 * Whether a trapped ERET is a return into a guarded level, which is the only
 * kind the hardware may be handed with NV off.
 *
 * `SPSR_GL1 != 0` on its own is not that test.  The register does not clear
 * itself: once SPTM has entered a guarded level it keeps whatever was written
 * there, so every ordinary return that comes afterwards reads non-zero too.
 * Measured -- XNU's exception_return going back to its own EL2 with SPSR
 * 0x604003c8 was taken for a guarded one, handed to the silicon from hardware
 * EL1, and refused: a return to a higher EL sets PSTATE.IL, and the EC 0x0e
 * that follows lands on whatever instruction comes next.  It was a kext thunk
 * with nothing wrong with it, three subsystems from the cause.
 *
 * What the hardware can actually carry out from EL1 names EL0 or EL1.  A
 * return that names EL2 is the guest hypervisor going back into its own
 * guest, and rewriting that is the software path's job.
 */
static bool eret_is_guarded(VcpuSlot *s, uint64_t *spsr_out) {
    ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);

    uint64_t spsr = *ohv_rw_u64(s->ctx, OHV_RW_EXTREGS + 52 * 8);
    unsigned m = (unsigned)(spsr & 0xfull);

    if (spsr_out != nullptr) {
        *spsr_out = spsr;
    }
    return spsr != 0 && spsr_illegal_reason(spsr) == nullptr &&
           m != 0x8 && m != 0x9;
}

/*
 * Set once the guest has taken an exception inside the kernel image.  It is a
 * phase marker, not a sampling window: iBoot raises far more interrupts than
 * the kernel does, and a trace that spends its budget there never reaches the
 * part being looked at.  Nothing before this point is of interest to the
 * interrupt question.
 */
bool g_kernel_running;

static bool g_trace_armed;

/* Defined with the FIQ gate, used by the GXF probe well above it. */
static bool ohv_guest_in_monitor(VcpuSlot *s);

static void enter_guest_el2(VcpuSlot *s, uint64_t esr, uint64_t return_pc) {
    g_trace_armed = true;
    ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
    uint64_t vbar = *ohv_rw_u64(s->ctx, OHV_VNCR_BASE + 0x250);

    /*
     * With the host extensions on, a guest hypervisor's ELR_EL2, SPSR_EL2,
     * ESR_EL2 and FAR_EL2 are the EL1 exception registers -- it is running at
     * EL1 and those are the ones its exceptions use.  Measured: a guest at
     * its EL2 writing each of them lands on the banked EL1 slots, while
     * VBAR_EL2 goes to the VNCR page.  Writing the VNCR copies instead left
     * the guest reading whatever was in them, and its own vector code turned
     * away because the syndrome did not name an HVC.
     */
    *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 3 * 8) = return_pc;
    *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 5 * 8) = esr;
    *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 14 * 8) = rw->regs.cpsr;
    mark_dirty(s->ctx, OHV_STATE_SYSREGS);

    rw->regs.pc = vbar + OHV_VECT_LOWER64_SYNC;
    /*
     * The guest believes it is at EL2; the hardware keeps it at EL1 and
     * HCR_EL2.NV is what makes the two agree.  DAIF is masked on entry, as
     * taking an exception does.
     */
    rw->regs.cpsr = 0x3c5;
    ohv_rw_controls(s->ctx)->hcr_el2 |= OHV_HCR_NV;
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    s->guest_at_el2 = true;
}

static ExitAction exit_from_ro(VcpuSlot *s) {
    volatile ohv_vmexit_info_t *e = ohv_exit_info(s->ctx);

    if (ohv_env("OHV_TRACE_HCR")) {
        static unsigned shown[8];

        if (s->id < 8 && shown[s->id] < 40 && e->vmexit_reason != 6 &&
            e->vmexit_reason != 11) {
            shown[s->id]++;
            fprintf(stderr, "[ohv] vcpu %llu reason %u esr %#llx pc %#llx"
                    " cntv_ctl %#llx cntv_cval %#llx cntp_ctl %#llx"
                    " cntp_cval %#llx timer %#llx\n",
                    (unsigned long long)s->id,
                    (unsigned)e->vmexit_reason,
                    (unsigned long long)e->vmexit_esr,
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 18 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 9 * 8),
                    (unsigned long long)s->el2_shadow[OHV_SHADOW_CNTP_CTL],
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 10 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx, OHV_RW_TIMER));
        }
    }
    if (ohv_env("OHV_TRACE_ALL")) {
        uint64_t tpc = ohv_rw(s->ctx)->regs.pc;
        /*
         * Tracing every exit costs more than the guest does, so a run under it
         * never reaches the point worth looking at.  Hold the trace until the
         * firmware has had its uninteresting seconds.
         */
        static time_t t0;
        const char *after = ohv_env("OHV_TRACE_AFTER");
        bool armed = true;

        if (!t0) t0 = time(nullptr);
        if (after) armed = time(nullptr) - t0 >= atoi(after);
        static uint64_t floor = ~0ull;

        if (floor == ~0ull) {
            const char *f = ohv_env("OHV_TRACE_FROM");

            floor = f ? strtoull(f, nullptr, 0) : 0x1fc000000ull;
        }
        static unsigned emitted;
        static unsigned cap = 0;

        if (!cap) {
            const char *c = ohv_env("OHV_TRACE_MAX");

            cap = c ? (unsigned)atoi(c) : ~0u;
        }
        static long only = -2;

        if (only == -2) {
            const char *o = ohv_env("OHV_TRACE_VCPU");

            only = o ? strtol(o, nullptr, 0) : -1;
        }
        if (armed && emitted < cap && tpc >= floor &&
            (only < 0 || (long)s->id == only)) {
            emitted++;
            fprintf(stderr, "[ohv] vcpu %llu exit reason %u esr %#llx pc %#llx"
                    " far %#llx ipa %#llx\n",
                    (unsigned long long)s->id,
                    (unsigned)e->vmexit_reason,
                    (unsigned long long)e->vmexit_esr,
                    (unsigned long long)tpc,
                    (unsigned long long)e->vmexit_far,
                    (unsigned long long)(e->vmexit_hpfar << 8));
        }
    }
    /*
     * Put NV back when the guest leaves the guarded state a hardware ERET
     * carried it into.  Inside it the guest is the monitor, not a hypervisor,
     * and trapping its returns there is what broke the entry in the first
     * place; outside it every ERET is a guest hypervisor's again.  The guarded
     * exit is the event that says which side of that line the guest is on --
     * the guarded level itself reads back zero here whatever it is.
     */
    if (s->nv_off_for_geret &&
        ((e->vmexit_esr >> 26) & 0x3f) == 0x3f) {
        ohv_rw_controls(s->ctx)->hcr_el2 |= OHV_HCR_NV;
        mark_dirty(s->ctx, OHV_STATE_CONTROLS);
        s->nv_off_for_geret = false;
        if (ohv_env("OHV_TRACE_ERET")) {
            fprintf(stderr, "[ohv] guarded exit: NV restored at pc %#llx\n",
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc);
        }
    }
    /*
     * What level the hardware actually returned into.  SPTM asks for guarded
     * with bit 12 of the value it writes; the bit is not kept, so the only way
     * to know where the guest landed is to look afterwards -- and SPTM's own
     * guarded lower-EL handler is the thing that notices when it is wrong.
     */
    if (ohv_env("OHV_TRACE_GLVL") && s->nv_off_for_geret) {
        static unsigned n;

        if (n < 12) {
            n++;
            ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);
            fprintf(stderr, "[ohv] after geret: pc %#llx esr %#llx"
                    " aspsr %#llx apsts %#llx spsr_gl1 %#llx cpsr %#llx\n",
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc,
                    (unsigned long long)e->vmexit_esr,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_EXTREGS + 47 * 8),
                    (unsigned long long)ohv_rw_controls(s->ctx)->apsts_el1,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_EXTREGS + 52 * 8),
                    (unsigned long long)ohv_rw(s->ctx)->regs.cpsr);
        }
    }
    /*
     * Keep the guest's EL2 return pair holding the guarded one.
     *
     * SPTM hands the monitor over with MSR ELR_GL1 / MSR SPSR_GL1 and an
     * ERET.  Those two writes are not trapped -- the hardware takes them --
     * and by then HCR_EL2.NV is usually off, so the ERET is not trapped
     * either: the hardware performs it at EL1, where it consumes ELR_EL1 and
     * SPSR_EL1, which under the host extensions are the pair the guest calls
     * its EL2 one.  Those still hold zero, so the monitor is entered at
     * address zero and takes an instruction abort on its first fetch --
     * measured, ESR_GL1 0x82000005 with FAR and ELR both zero.
     *
     * There is nowhere to intercept the writes, so mirror instead: whenever
     * the guarded bank names a return that exists, put it where a hardware
     * ERET will look.  The window is real -- SPTM touches SPRR_UPERM_EL0,
     * MDSCR_EL1, APCTL_EL1 and SCTLR_EL1 between setting the pair and the
     * ERET, and those do trap.
     */
    if (ohv_env("OHV_MIRROR_GL1") &&
        ((e->vmexit_esr >> 26) & 0x3f) == 0x18) {
        ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);
        uint64_t gelr = *ohv_rw_u64(s->ctx, OHV_RW_EXTREGS + 51 * 8);
        uint64_t gspsr = *ohv_rw_u64(s->ctx, OHV_RW_EXTREGS + 52 * 8);

        if (gelr != 0 && gspsr != 0) {
            volatile uint64_t *elr =
                ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 3 * 8);
            volatile uint64_t *spsr =
                ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 14 * 8);

            if (*elr != gelr || *spsr != gspsr) {
                *elr = gelr;
                *spsr = gspsr;
                mark_dirty(s->ctx, OHV_STATE_SYSREGS);
                if (ohv_env("OHV_TRACE_ERET")) {
                    static unsigned n;

                    if (n < 8) {
                        n++;
                        fprintf(stderr, "[ohv] mirrored guarded return"
                                " %#llx/%#llx at pc %#llx\n",
                                (unsigned long long)gelr,
                                (unsigned long long)gspsr,
                                (unsigned long long)ohv_rw(s->ctx)->regs.pc);
                    }
                }
            }
        }
    }
    /*
     * Whether the kernel is carrying the guarded bank at all.  It only does so
     * while ARM_GUEST_STATE_GXF (bit 58) is in state_used, and it sets that
     * bit when it services a trapped GXF_CONFIG/ENTRY/PABENTRY access -- so
     * this says, in one word, whether the traps we asked for are happening.
     */
    if (ohv_env("OHV_TRACE_GXFSTATE")) {
        static unsigned n;

        static uint64_t last = ~0ull;
        uint64_t now = *ohv_ro_u64(s->ctx, OHV_RO_STATE_USED);

        if (n < 60 && now != last) {
            last = now;
            n++;
            fprintf(stderr, "[ohv] state_used %#llx (GXF %s, SPRR %s)"
                    " hacr %#llx pc %#llx\n",
                    (unsigned long long)*ohv_ro_u64(s->ctx, OHV_RO_STATE_USED),
                    (*ohv_ro_u64(s->ctx, OHV_RO_STATE_USED) & (1ull << 58))
                        ? "yes" : "no",
                    (*ohv_ro_u64(s->ctx, OHV_RO_STATE_USED) & (1ull << 59))
                        ? "yes" : "no",
                    (unsigned long long)ohv_rw_controls(s->ctx)->hacr_el2,
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc);
        }
    }
    /*
     * Put NV back on once the monitor is running.
     *
     * SPTM's handover to TXM is MSR ELR_GL1 / MSR SPSR_GL1 and an ERET.  Those
     * writes are never trapped, so the only chance to put that pair where a
     * hardware ERET will look for it -- the guest's EL2 pair, which under the
     * host extensions is the banked EL1 one -- is to trap the ERET itself, and
     * that needs HCR_EL2.NV.  iBoot leaves NV off: the emulation above clears
     * it whenever the guest hypervisor returns to its EL1, and iBoot enters
     * SPTM with a branch, not an ERET, so nothing turns it back on.
     *
     * Measured: with the ERET trapped the monitor is entered correctly and TXM
     * runs to its secure boot check; without, it is entered at zero and takes
     * an instruction abort on its first fetch.  The difference is only whether
     * NV happened to still be set, which is why it worked once and not again.
     */
    if (!ohv_env("OHV_NO_MONITOR_NV") &&
        ohv_rw(s->ctx)->regs.pc >= 0xfffffff000000000ull &&
        (ohv_rw_controls(s->ctx)->hcr_el2 & OHV_HCR_NV) == 0) {
        ohv_rw_controls(s->ctx)->hcr_el2 |= OHV_HCR_NV;
        mark_dirty(s->ctx, OHV_STATE_CONTROLS);
        if (ohv_env("OHV_TRACE_ERET")) {
            fprintf(stderr, "[ohv] NV re-armed for the monitor at pc %#llx\n",
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc);
        }
    }
    /*
     * What the kernel is faulting on.  Once XNU is running its exceptions are
     * its own -- they never reach here -- so the only account of them is the
     * banked pair it took them with: ELR_EL1 where it faulted, FAR_EL1 the
     * address, ESR_EL1 why.  Printed when the reason changes, so a loop says
     * so once rather than a million times.
     */
    if (ohv_env("OHV_TRACE_KFAULT") &&
        ohv_rw(s->ctx)->regs.pc >= 0xfffffff000000000ull) {
        static uint64_t last_esr = ~0ull;
        uint64_t esr = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 5 * 8);

        if (esr != last_esr) {
            last_esr = esr;
            fprintf(stderr, "[ohv] kernel esr %#llx far %#llx elr %#llx"
                    " (pc %#llx)\n",
                    (unsigned long long)esr,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 4 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 3 * 8),
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc);
        }
    }
    /*
     * The guest's own exception state, whenever it changes.
     *
     * A nested panic prints nothing: the first one has not reached a console
     * yet when the second halts the machine, so the halt tells you only that
     * it happened.  What the kernel took is in its banked EL1 registers, and
     * they are in the shared page -- ESR at slot 5, FAR at 4, ELR at 3.
     * Printing on change gives the first fault as well as the last.
     */
    {
        /*
         * Opened by the first exit taken anywhere in the kernel image, not by
         * the first fault there.  Gating on a fault opened it far too late:
         * the interrupt this is meant to catch is one the kernel takes before
         * it has faulted even once, so the trace stayed silent and read as
         * "no interrupt was delivered".
         */
        uint64_t pc_now = ohv_rw(s->ctx)->regs.pc;

        if (pc_now >= 0xfffffff06b000000ull &&
            pc_now < 0xfffffff070000000ull) {
            g_kernel_running = true;
        }
        /*
         * What the timers look like as the kernel comes up, and for a few
         * exits after.  No software path delivers an interrupt in this era --
         * measured, on both the pending-interrupt call and the vGIC list
         * registers, with the AIC line never rising -- yet the kernel takes
         * one and dies dispatching it.  A timer the hardware still has armed
         * is the only source left, and this side keeps CNTP_CVAL in *host*
         * counter units with the guest's control word in a shadow, so a value
         * left over from before the handoff would fire at a moment nothing
         * expects.
         */
        /*
         * Any exit at which a virtual interrupt is pending, once the kernel is
         * up.  Measured at the kernel's first instructions, both are clear and
         * no controller, injection call or timer accounts for one -- so the
         * one it dies on appears later, and only a level check catches the
         * moment it does.
         */
        if (ohv_env("OHV_TRACE_KTIMER")) {
            uint64_t h = ohv_rw_controls(s->ctx)->hcr_el2;

            /*
             * Bounded by where the guest is, not by a count.  SPTM services
             * USB the whole time with a virtual IRQ legitimately pending, and
             * a capped trace spends itself entirely on that before the kernel
             * ever runs -- the range is what separates the two.
             */
            if ((h & (3ull << 6)) != 0 &&
                pc_now >= 0xfffffff06b000000ull &&
                pc_now < 0xfffffff070000000ull) {
                fprintf(stderr, "[ohv] vint: pc %#llx VI %d VF %d\n",
                        (unsigned long long)pc_now,
                        (h & (1ull << 7)) ? 1 : 0, (h & (1ull << 6)) ? 1 : 0);
            }
        }
        if (ohv_env("OHV_TRACE_KTIMER") && g_kernel_running) {
            static unsigned n;

            if (n < 8) {
                uint64_t pct;

                __asm__ volatile("mrs %0, cntpct_el0" : "=r"(pct));
                n++;
                /*
                 * VI and VF as a level, not as a transition.  A virtual
                 * interrupt left pending from before the kernel started is
                 * invisible to every edge-triggered check: no controller
                 * raises a line, no injection call changes state, and the
                 * guest still takes one the moment it unmasks.
                 */
                fprintf(stderr, "[ohv] ktimer %u: pc %#llx cntpct %#llx"
                        " hcr %#llx VI %d VF %d"
                        " p_ctl_shadow %#llx p_ctl_banked %#llx p_cval %#llx"
                        " v_ctl %#llx v_cval %#llx\n",
                        n, (unsigned long long)pc_now,
                        (unsigned long long)pct,
                        (unsigned long long)ohv_rw_controls(s->ctx)->hcr_el2,
                        (ohv_rw_controls(s->ctx)->hcr_el2 & (1ull << 7)) ? 1 : 0,
                        (ohv_rw_controls(s->ctx)->hcr_el2 & (1ull << 6)) ? 1 : 0,
                        (unsigned long long)s->el2_shadow[OHV_SHADOW_CNTP_CTL],
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 19 * 8),
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 10 * 8),
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 18 * 8),
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 9 * 8));
            }
        }
    }
    if (ohv_env("OHV_TRACE_FAULT")) {
        static uint64_t last_esr, last_elr;
        uint64_t esr = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 5 * 8);

        uint64_t felr = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 3 * 8);

        /*
         * Only the monitor's and the kernel's own faults.  iBoot raises a
         * million supervisor calls on the way here, and printing them costs
         * more than the guest does -- enough to push the run into a stall
         * before it reaches the part worth seeing.
         */
        /*
         * A platform error is worth reporting whether or not the syndrome has
         * changed since the last exit.  XNU calls one of these fatal before it
         * looks at the exception class at all -- `is_platform_error` in
         * sleh_synchronous -- and the panic it raises names only the error
         * registers, not what was touched.  The syndrome that caused it is
         * usually overwritten by the abort the panic path takes next, so the
         * change-triggered print below never sees it.
         *
         * These are the fault codes XNU treats that way: a synchronous
         * external abort, the same on a translation table walk, and parity.
         */
        {
            uint64_t ec = (esr >> 26) & 0x3f;
            uint64_t fsc = esr & 0x3f;

            /*
             * EC 0x2f is the same story from the asynchronous side: XNU's
             * fleh_serror hands it to sleh_serror, which reports it as an
             * implementation specific error and panics.  Nothing here injects
             * a virtual SError -- hv_vcpu_set_serror answers HV_UNSUPPORTED --
             * so one arriving in the guest came from the machine.
             */
            if (ec == 0x2f ||
                ((ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) &&
                 (fsc == 0x10 || (fsc >= 0x14 && fsc <= 0x18) ||
                  (fsc >= 0x1c && fsc <= 0x1f)))) {
                fprintf(stderr, "[ohv] platform error: esr %#llx (ec %#llx"
                        " fsc %#llx) far %#llx elr %#llx pc %#llx\n",
                        (unsigned long long)esr, (unsigned long long)ec,
                        (unsigned long long)fsc,
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 4 * 8),
                        (unsigned long long)felr,
                        (unsigned long long)ohv_rw(s->ctx)->regs.pc);
            }
        }
        if ((esr != last_esr || felr != last_elr) && esr != 0 &&
            felr >= 0xfffffff000000000ull) {
            last_esr = esr;
            last_elr = felr;
            /*
             * TPIDR_EL1 comes along because the wall this is chasing is
             * XNU's own "TPIDR is corrupted?" case: the SP1 vector reads
             * thread->machine.CpuDatap and deadloops when it is null.
             * Whether the pointer looks like a thread decides between a
             * thread that simply is not current and a register the two
             * guest levels are sharing.
             */
            /*
             * NV comes along because the guest kernel believes it is at EL2
             * while the hardware has it at EL1, so every exception return it
             * writes names EL2t or EL2h.  Those are only legal because NV
             * traps them here to be rewritten.  With NV off the hardware runs
             * one for real, sees a return to a higher EL, and sets PSTATE.IL
             * -- which surfaces as EC 0x0e on whatever instruction comes
             * next.  The one thing that takes NV off and waits for something
             * else to put it back is the guarded ERET, so its flag is worth
             * seeing next to it.
             */
            fprintf(stderr, "[ohv] guest fault: esr %#llx (ec %#llx) far %#llx"
                    " elr %#llx pc %#llx spsr %#llx tpidr_el1 %#llx"
                    " nv %d geret_off %d\n",
                    (unsigned long long)esr,
                    (unsigned long long)((esr >> 26) & 0x3f),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 4 * 8),
                    (unsigned long long)felr,
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 14 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_SHARED_SYSREGS + 1 * 8),
                    (ohv_rw_controls(s->ctx)->hcr_el2 & OHV_HCR_NV) ? 1 : 0,
                    s->nv_off_for_geret ? 1 : 0);
        }
    }
    uint32_t reason = e->vmexit_reason;
    hv_vcpu_exit_t *out = &s->pub_exit;
    uint64_t hpfar = e->vmexit_hpfar;

    out->exception.syndrome = e->vmexit_esr;
    out->exception.virtual_address = e->vmexit_far;
    /*
     * The third word the kernel writes about a fault is the intermediate
     * physical address itself, not HPFAR_EL2: the framework hands it straight
     * to its MMIO handler as an address.  Only the page matters in it -- the
     * offset within the page comes from FAR -- and treating it as a register
     * to shift put the fault a couple of pages away from where it happened,
     * which is a device that is not there.
     */
    out->exception.physical_address =
        (hpfar & ~0xfffull) | (e->vmexit_far & 0xfff);

    /*
     * Note a guest stopped on WFI/WFE, so the interrupt gates can let a level
     * through to it.  Only a synchronous exit carries a syndrome worth
     * reading; anything else clears the flag rather than latching a stale one.
     */
    s->in_wfx = (reason == 1) && (((e->vmexit_esr >> 26) & 0x3f) == 0x1);

    switch (reason) {
        case 0:
            /* The run came back with nothing to report. */
            out->reason = HV_EXIT_REASON_CANCELED;
            return EXIT_TO_VMM;

        case 2: case 7: case 11:
            /* The kernel dealt with it; go back in. */
            return EXIT_KEEP_RUNNING;

        case 12:
            return EXIT_KEEP_RUNNING;

        case 3: case 4: {
            if (ohv_env("OHV_TRACE_IRQ")) {
                static unsigned n;

                if (n < 10) {
                    n++;
                    fprintf(stderr, "[ohv] vcpu %llu physical %s, cntv_ctl"
                            " %#llx timer %#llx\n", (unsigned long long)s->id,
                            reason == 3 ? "IRQ" : "FIQ",
                            (unsigned long long)*ohv_rw_u64(s->ctx,
                                OHV_RW_BANKED_SYSREGS + 18 * 8),
                            (unsigned long long)*ohv_rw_u64(s->ctx,
                                OHV_RW_TIMER));
                }
            }
            /*
             * A physical interrupt.  It is the virtual timer's only when the
             * guest's own timer says it fired and the mask is not already up;
             * anything else here is not this VMM's to be told about.
             */
            uint64_t cntv_ctl = *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS +
                offsetof(ohv_banked_sysregs_t, cntv_ctl_el0));
            uint64_t *timer = (uint64_t *)ohv_rw_u64(s->ctx, OHV_RW_TIMER);

            if ((*timer & OHV_TIMER_MASK) == 0 && cntv_ctl == 5) {
                *timer |= OHV_TIMER_MASK;
                out->reason = HV_EXIT_REASON_VTIMER_ACTIVATED;
                return EXIT_TO_VMM;
            }
            out->reason = HV_EXIT_REASON_UNKNOWN;
            return EXIT_TO_VMM;
        }

        case OHV_VMEXIT_MSR_TRAP:
            /*
             * A trapped register access.  If it names one of the guest
             * hypervisor's own EL2 registers, it is only here because the
             * redirection did not happen; carry it out and go back in rather
             * than handing the VMM a register it has no way to place.
             */
            if (((e->vmexit_esr >> 26) & 0x3f) == 0x18 &&
                service_nv2_access(s, e->vmexit_esr)) {
                return EXIT_KEEP_RUNNING;
            }
            /* fall through */
        case 1:
            /*
             * WFI, spun rather than slept.  A diagnostic: the VMM turns a
             * WFI exit into a halt and waits for work it can only learn
             * about through its own interrupt line, so a guest waiting on
             * something this library delivers never restarts.  Stepping over
             * the instruction says whether the guest is waiting to be woken
             * or waiting for something that never happens.
             */
            if (ohv_env("OHV_SPIN_WFI") &&
                ((e->vmexit_esr >> 26) & 0x3f) == 0x1) {
                ohv_rw(s->ctx)->regs.pc += 4;
                return EXIT_KEEP_RUNNING;
            }
            /*
             * An HVC out of the guest the guest hypervisor is hosting.  It is
             * that hypervisor's to service, not this library's and not the
             * VMM's, so it goes to its vector table.
             */
            if (((e->vmexit_esr >> 26) & 0x3f) == 0x16 && !s->guest_at_el2 &&
                !ohv_env("OHV_NO_EL2_INJECTION")) {
                enter_guest_el2(s, e->vmexit_esr, ohv_rw(s->ctx)->regs.pc + 4);
                if (ohv_env("OHV_TRACE_ERET")) {
                    fprintf(stderr, "[ohv] vcpu %llu hvc -> guest EL2 vector"
                            " %#llx\n", (unsigned long long)s->id,
                            (unsigned long long)ohv_rw(s->ctx)->regs.pc);
                }
                return EXIT_KEEP_RUNNING;
            }
            if (ohv_env("OHV_TRACE_GL1") &&
                ((e->vmexit_esr >> 26) & 0x3f) == 0x1a) {
                static unsigned n;
                /* Only the monitors' own returns; the earlier ones are the
                 * boot ROM's and iBoot's, and they run before GXF exists. */
                bool late = ohv_rw(s->ctx)->regs.pc >= 0xfffffff000000000ull;

                if (late && n < 4) {
                    n++;
                    /* The guarded bank is filled by hardware; ask for it. */
                    ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);
                    fprintf(stderr, "[ohv] gl1 dump at pc %#llx\n",
                            (unsigned long long)ohv_rw(s->ctx)->regs.pc);
                    for (unsigned i = 40; i < 62; i++) {
                        uint64_t v = *ohv_rw_u64(s->ctx,
                                                 OHV_RW_EXTREGS + i * 8);

                        if (v) {
                            fprintf(stderr, "        ext[%u] = %#llx\n", i,
                                    (unsigned long long)v);
                        }
                    }
                }
            }
            if (ohv_env("OHV_TRACE_ERET") &&
                ((e->vmexit_esr >> 26) & 0x3f) == 0x1a) {
                fprintf(stderr, "[ohv] vcpu %llu EC 0x1a: esr %#llx"
                        " at_el2 %d spsr_gl1 %#llx elr %#llx spsr %#llx"
                        " pc %#llx\n",
                        (unsigned long long)s->id,
                        (unsigned long long)e->vmexit_esr,
                        (int)s->guest_at_el2,
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                                                        OHV_EXT_SPSR_GL1),
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 3 * 8),
                        (unsigned long long)*ohv_rw_u64(s->ctx,
                            OHV_RW_BANKED_SYSREGS + 14 * 8),
                        (unsigned long long)ohv_rw(s->ctx)->regs.pc);
            }
            /*
             * A guarded return, performed by the hardware rather than here.
             *
             * The guarded level is not something this side can restore: the
             * bit SPTM sets in the value it writes -- 0x13c0, EL0t *and
             * guarded* -- is not kept by SPSR_GL1, which reads back 0x3c0,
             * and the level being returned to lives somewhere only the
             * hardware's own ERET consults.  Completing the return by writing
             * PC and PSTATE therefore lands the monitor at plain EL0, where
             * the address it was given is not executable, and the guarded
             * prefetch-abort entry catches it: SPTM stops in its own dead
             * loop with the machine looking healthy.
             *
             * So put the pair where a hardware ERET at this level looks for
             * it -- the guest's own EL2 pair, which with the host extensions
             * on is the banked EL1 one -- take NV off so the instruction is
             * no longer trapped, and let it re-execute.  Everything the
             * guarded transition needs is then the silicon's to do.
             */
            /*
             * On by default: without it the trapped guarded ERET falls
             * through to a software completion that returns at EL0, and TXM
             * is entered there -- its first instruction fetch aborts from a
             * lower level and SPTM deadloops.  Letting the silicon do the
             * transition is the whole point, so the switch only turns it off.
             */
            if (!ohv_env("OHV_NO_HW_GERET") &&
                ((e->vmexit_esr >> 26) & 0x3f) == 0x1a &&
                (e->vmexit_esr & 1) == 0) {
                uint64_t gspsr = 0;

                if (eret_is_guarded(s, &gspsr)) {
                    uint64_t gelr =
                        *ohv_rw_u64(s->ctx, OHV_RW_EXTREGS + 51 * 8);

                    report_illegal_eret(s, "guarded, left to the hardware",
                                        gelr, gspsr, true);
                    *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 3 * 8) = gelr;
                    *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + 14 * 8) = gspsr;
                    mark_dirty(s->ctx, OHV_STATE_SYSREGS);
                    ohv_rw_controls(s->ctx)->hcr_el2 &= ~OHV_HCR_NV;
                    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
                    s->nv_off_for_geret = true;
                    if (ohv_env("OHV_TRACE_ERET")) {
                        fprintf(stderr, "[ohv] guarded ERET left to the"
                                " hardware: elr %#llx spsr %#llx pc %#llx\n",
                                (unsigned long long)gelr,
                                (unsigned long long)gspsr,
                                (unsigned long long)ohv_rw(s->ctx)->regs.pc);
                    }
                    return EXIT_KEEP_RUNNING;
                }
            }
            /*
             * A trapped ERET, which is a guest hypervisor returning into the
             * guest it hosts.  Carrying it out is not only PC and PSTATE: the
             * level the guest believes it is at changes with it, and that is
             * HCR_EL2.NV.  Left on, the guest arrives at its own EL1 and
             * every CurrentEL it reads still answers EL2 -- which the first
             * thing SEP's ROM does is check, and take the other branch on.
             */
            if (((e->vmexit_esr >> 26) & 0x3f) == 0x1a &&
                (e->vmexit_esr & 1) == 0 && s->guest_at_el2 &&
                !eret_is_guarded(s, nullptr) &&
                !ohv_env("OHV_NO_ERET_EMULATION")) {
                ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
                /*
                 * The return pair is the hardware's ELR_EL1 and SPSR_EL1, not
                 * the VNCR copies: with the host extensions on, the guest
                 * hypervisor's return registers are the ones it is actually
                 * running with.  Measured -- SEP's ROM leaves the address of
                 * its own next instruction in ELR_EL1 and 0x3c5 in SPSR_EL1
                 * before returning to its EL1.
                 *
                 * A guarded return is not this.  It comes out of the guarded
                 * bank, only the VMM can lower the guarded level, and it is
                 * left for the VMM to complete.
                 */
                uint64_t elr = *ohv_rw_u64(s->ctx,
                                           OHV_RW_BANKED_SYSREGS + 3 * 8);
                uint64_t spsr = *ohv_rw_u64(s->ctx,
                                            OHV_RW_BANKED_SYSREGS + 14 * 8);
                uint32_t mode = (uint32_t)spsr & 0xf;

                /*
                 * Which level the guest is returning to decides whether it
                 * stops being a hypervisor.
                 *
                 * To its own EL1: yes, and HCR_EL2.NV comes off, or every
                 * CurrentEL it reads there still answers EL2.
                 *
                 * To EL0: no.  The guest hypervisor's EL0 tasks come back to
                 * it with an SVC, and that is an exception from EL0 to EL1 --
                 * hardware handles it end to end and nothing here sees it.
                 * Clearing NV on the way down would leave the guest running
                 * its own EL2 code without it, and the first Apple register
                 * only EL2 may touch becomes an undefined instruction: the
                 * payload takes a synchronous exception it has no business
                 * taking and panics.  NV means nothing at EL0 anyway, so it
                 * costs nothing to leave it on.
                 *
                 * To EL2 (EL2t or EL2h in the saved PSTATE): the guest
                 * hypervisor is returning to itself, and the hardware level
                 * stays EL1 underneath it.
                 */
                report_illegal_eret(s, "completed here", elr, spsr, false);
                rw->regs.pc = elr;
                rw->regs.cpsr = (uint32_t)spsr;
                if (mode == 0x4 || mode == 0x5) {
                    s->guest_at_el2 = false;
                    ohv_rw_controls(s->ctx)->hcr_el2 &= ~OHV_HCR_NV;
                    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
                    if (ohv_env("OHV_TRACE_ERET")) {
                        fprintf(stderr, "[ohv] vcpu %llu eret -> %#llx spsr"
                                " %#llx, NV cleared, hcr now %#llx\n",
                                (unsigned long long)s->id,
                                (unsigned long long)elr,
                                (unsigned long long)spsr,
                                (unsigned long long)ohv_rw_controls(s->ctx)->hcr_el2);
                    }
                } else if (mode == 0x8 || mode == 0x9) {
                    /* Staying at EL2 means staying at hardware EL1. */
                    rw->regs.cpsr = (uint32_t)((spsr & ~0xfu) | (mode - 4));
                    if (ohv_env("OHV_TRACE_ERET")) {
                        fprintf(stderr, "[ohv] vcpu %llu eret stays at EL2"
                                " -> %#llx spsr %#llx\n",
                                (unsigned long long)s->id,
                                (unsigned long long)elr,
                                (unsigned long long)spsr);
                    }
                }
                return EXIT_KEEP_RUNNING;
            }
            /* fall through */
        case 6:
            /*
             * A guest exception, which is the VMM's to service.  Two of the
             * three arrive without a syndrome the VMM could act on, so it is
             * built here: a trapped system register access out of the
             * instruction, and a guarded exit out of the exception class the
             * framework gives it.
             */
            /*
             * Only when the kernel left the syndrome empty.  It rarely does
             * now that it is read from the right place, and overwriting a
             * syndrome the kernel did supply with one decoded from an
             * instruction fetched at a guest virtual address -- read as if it
             * were physical -- is how a trapped register access turns into a
             * different register entirely once the guest MMU is on.
             */
            if (e->vmexit_esr == 0) {
                if (reason == OHV_VMEXIT_MSR_TRAP) {
                    synth_msr_trap_syndrome(s, out);
                } else if (reason == 1) {
                    synth_guarded_exit_syndrome(s, out);
                }
            }
            out->reason = HV_EXIT_REASON_EXCEPTION;
            return EXIT_TO_VMM;

        default:
            if (ohv_env("OHV_TRACE_EXIT")) {
                fprintf(stderr, "openhyp: kernel vmexit reason %u, esr %#llx"
                        " far %#llx hpfar %#llx pc %#llx -- not classified\n",
                        reason, (unsigned long long)e->vmexit_esr,
                        (unsigned long long)e->vmexit_far,
                        (unsigned long long)hpfar,
                        (unsigned long long)ohv_rw(s->ctx)->regs.pc);
            }
            out->reason = HV_EXIT_REASON_UNKNOWN;
            return EXIT_TO_VMM;
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
    if (ohv::g_vm_el2) {
        /*
         * Turn the guest into a guest hypervisor: NV, NV1 and NV2 in its
         * HCR_EL2, plus E2H when it is to see the host-extension form.  These
         * are what make an EL1 guest believe it is at EL2; without them the
         * kernel has an ordinary EL1 guest and refuses anything else.
         */
        volatile uint64_t *controls = (volatile uint64_t *)ohv_rw_controls(s.ctx);
        const char *mask = ohv_env("OHV_HCR_NESTED_MASK");
        uint64_t hcr = controls[0] |
            (mask ? strtoull(mask, nullptr, 0) : OHV_HCR_NESTED);

        if (ohv::g_vm_vhe) {
            hcr |= OHV_HCR_E2H;
        } else {
            hcr &= ~OHV_HCR_E2H;
        }
        /*
         * What the framework leaves on a vCPU of its own, on top of what the
         * kernel hands out: two more traps in HCR_EL2 and one bit in
         * HACR_EL2, which is Apple's own hypervisor control and has no
         * architected meaning to go by.  Measured against a framework vCPU
         * side by side with one of ours, where these were the only controls
         * that differed.
         */
        hcr |= OHV_HCR_TID3 | OHV_HCR_TSC;
        /*
         * Route the physical exceptions to EL2, which is what makes HCR_EL2.VI
         * and VF -- the only way anything here has of handing the guest an
         * interrupt -- mean anything at all.  The kernel forces all three on
         * anyway (they are in its fixed HCR set), so this changes no bit on
         * the way in; it is here so the value we ask for says what we rely on
         * rather than leaving it to a default we do not own.
         */
        hcr |= OHV_HCR_AMO | OHV_HCR_IMO | OHV_HCR_FMO;
        /*
         * OHV_HCR_TIDCP=0 hands the Apple registers back to the kernel.
         *
         * The kernel's own default for a new vCPU is HCR_TIDCP | HCR_API |
         * HCR_APK, and this library inherits it.  With TIDCP standing, every
         * implementation-defined MSR is punted to the VMM before the kernel's
         * handler ever looks at it -- including APCTL_EL1, JCTL and the
         * JITBox keys, which are the ones it knows how to carry into the
         * silicon.  What the VMM does with them instead is keep them in a
         * register file of its own, and the state block they belong to is
         * never marked used, so the hardware keeps whatever the last thing on
         * that physical core left.  With one core that is invisible; with six
         * it is "PAC failure from kernel with IB key" on a thread that signed
         * its return address on one core and returned on another.
         *
         * Cleared, the kernel services the registers it recognises and marks
         * ARM_GUEST_STATE_PTRAUTH_APPLE, JITBOX, SPRR, GXF and CTRR used, so
         * they are saved and restored per vCPU; everything it does not
         * recognise still falls through to the VMM.
         */
        if (const char *t = ohv_env("OHV_HCR_TIDCP")) {
            if (strtoull(t, nullptr, 0) == 0) {
                hcr &= ~OHV_HCR_TIDCP;
            } else {
                hcr |= OHV_HCR_TIDCP;
            }
        }
        if (const char *o = ohv_env("OHV_HCR_XMO")) {
            uint64_t m = OHV_HCR_AMO | OHV_HCR_IMO | OHV_HCR_FMO;

            hcr = (hcr & ~m) | (strtoull(o, nullptr, 0) & m);
        }
        controls[0] = hcr;
        controls[1] = OHV_HACR_APPLE_DEFAULT;
        if (ohv_env("OHV_HACR_TACTL0")) {
            /*
             * The trap that decides whether the kernel or this library owns
             * APCTL_EL1 -- and with it whether the Apple pointer-auth
             * registers are ever carried into the silicon.  The kernel marks
             * ARM_GUEST_STATE_PTRAUTH_APPLE used, and so loads APCTL and
             * KERNKEY, only when it services that trap itself; it declines to
             * whenever this bit is standing.  Settable so the two can be
             * measured against each other.
             */
            controls[1] |= 1ull;
        }
        /*
         * This cannot be used to catch a GENTER, which is what it was tried
         * for.  HACR_EL2.TGXF is already fixed to one in the kernel's own
         * capability set, and what it traps is a GXF *register* access, not
         * the instruction: measured with it asked for explicitly, the boot ran
         * to the same place with the same console and not one exit of an
         * unfamiliar shape appeared.  The GENTER boundary is not observable
         * from this side.
         */
        if (ohv_env("OHV_TRAP_GXF")) {
            controls[1] |= OHV_HACR_TGXF;
        }
        /*
         * The fine-grained traps the framework leaves on its own vCPUs, on
         * top of what the kernel hands out.  Measured side by side: these
         * three are the only controls it sets that were still zero here.
         */
        ohv_rw_controls(s.ctx)->hfgrtr_el2 |= 0x2600ull;
        ohv_rw_controls(s.ctx)->hfgwtr_el2 |= 0x2000ull;
        ohv_rw_controls(s.ctx)->hfgitr_el2 |= 0x3f000ull;
        /*
         * The physical timer has to trap.  The guest arms CNTP_CTL_EL0 and
         * waits; left open by CNTHCTL_EL2 that write reaches the real timer,
         * whose interrupt is the host's -- taken by the kernel, with no way
         * back to a guest that is only ever offered virtual ones.  Trapping
         * the timer registers (EL1PTEN, bit 11) puts the compare value in the
         * banked slots where ohv_expire_timers can see it come due and raise
         * HCR_EL2.VI; the counter (EL1PCTEN, bit 10) stays open, since
         * reading it costs nothing and firmware reads it in loops.
         */
        {
            volatile uint64_t *ch = &ohv_rw_controls(s.ctx)->cnthctl_el2;
            /*
             * EL1NVPCT/EL1NVVCT are the ones that matter here.  Clearing
             * EL1PTEN alone is not enough for a guest running under NV: the
             * hardware does not trap its CNTP_* accesses, it redirects them
             * to the EL2 timers, whose interrupt belongs to the host.  These
             * two bits (FEAT_ECV) say trap instead of redirect, which is what
             * puts the compare value somewhere ohv_expire_timers can see it.
             * EL1TVT/EL1TVCT do the same for the virtual pair, and leaving
             * EL1PCTEN clear keeps the counter reads coming here too, so the
             * guest stays on the VMM's clock rather than the host's raw one.
             */
            uint64_t v = (*ch & ~0x1fc00ull) | 0x1e000ull;

            if (const char *o = ohv_env("OHV_CNTHCTL")) {
                v = strtoull(o, nullptr, 0);
            }
            if (ohv_env("OHV_TRACE_TIMER")) {
                fprintf(stderr, "[ohv] cnthctl_el2 %#llx -> %#llx\n",
                        (unsigned long long)*ch, (unsigned long long)v);
            }
            *ch = v;
        }
        mark_dirty(s.ctx, OHV_STATE_CONTROLS);
        if (ohv_env("OHV_TRACE_HACR")) {
            fprintf(stderr, "[ohv] hacr asked %#llx\n",
                    (unsigned long long)controls[1]);
        }
        if (ohv_env("OHV_TRACE_USED")) {
            /*
             * What the kernel says it is carrying.  The Apple pointer-auth
             * registers reach the silicon only from
             * _hv_load_guest_apple_ptrauth_regs, and that runs only when
             * ARM_GUEST_STATE_PTRAUTH_APPLE -- bit sixty -- is in this word.
             */
            fprintf(stderr, "[ohv] state_used %#llx  ptrauth_apple %s"
                    "  gxf %s sprr %s\n",
                    (unsigned long long)*ohv_ro_u64(s.ctx, OHV_RO_STATE_USED),
                    (*ohv_ro_u64(s.ctx, OHV_RO_STATE_USED) & (1ull << 60)) ? "yes" : "no",
                    (*ohv_ro_u64(s.ctx, OHV_RO_STATE_USED) & (1ull << 58)) ? "yes" : "no",
                    (*ohv_ro_u64(s.ctx, OHV_RO_STATE_USED) & (1ull << 59)) ? "yes" : "no");
        }

        /*
         * The guest hypervisor's own HCR_EL2 -- the one it reads and writes,
         * which is not the one above -- starts with RW set, because the guest
         * it will run is AArch64.  The framework leaves this word at
         * 0x80000000 on a fresh vCPU; zero says the guest's EL1 is AArch32,
         * and a VMM that reads the register back and puts it into its own CPU
         * model refuses the value rather than building a 32-bit guest.
         */
        *ohv_rw_u64(s.ctx, OHV_NESTED_HCR_EL2) = 0x80000000ull;
        if (const char *w = ohv_env("OHV_TRY_14C8")) {
            *ohv_rw_u64(s.ctx, 0x14c8) = strtoull(w, nullptr, 0);
        }
    }
    /*
     * With an interrupt controller in the machine the guest talks to it
     * through system registers, and those only answer while the virtual CPU
     * interface is enabled.  Left off, every ICC_* access the guest makes
     * leaves the guest instead -- and a VMM told its controller is in the
     * kernel has no case for being asked.
     */
    s.guest_at_el2 = ohv::g_vm_el2;
    if (ohv::g_gic && s.ctx) {
        ohv_rw_controls(s.ctx)->ich_hcr_el2 |= 1ull;
        mark_dirty(s.ctx, OHV_STATE_GIC | OHV_STATE_CONTROLS);
    }
    tl_current_vcpu = &s;

    /*
     * A vCPU that will run a guest hypervisor has to be in one of the nested
     * address spaces the VM stood up; the framework assigns one through the
     * same trap.  Without it the kernel has nowhere to put guest EL2 and
     * refuses the first run as illegal guest state.
     */
    if (ohv::g_vm_el2 && !ohv_env("OHV_NO_NESTED_SPACE")) {
        /*
         * One space per vCPU, not one space for all of them.  Guest EL2 state
         * lives in the space -- that is where the kernel puts the page the
         * guest's EL2 registers are redirected to -- so vCPUs sharing a space
         * share those registers.  With every core in space zero the second
         * one to start finds the first one's guest EL2, and its own writes to
         * VBAR_EL2 and the rest go somewhere it never reads back: the boot
         * ROM sets a vector table, takes an exception, and lands at zero.
         *
         * This is why the VM stands up as many spaces as it does.
         */
        uint64_t asid = ohv_nested_asid((unsigned)s.id);

        if (!asid) {
            fprintf(stderr, "[ohv] no nested space left for vcpu %llu;"
                            " guest EL2 will share another core's\n",
                    (unsigned long long)s.id);
            asid = ohv_nested_asid(0);
        }
        if (ohv_env("OHV_TRACE_NESTED")) {
            fprintf(stderr, "[ohv] vcpu %llu joining nested space %#llx\n",
                    (unsigned long long)s.id, (unsigned long long)asid);
        }
        if (asid) {
            hv_return_t sr = ohv_raw_trap(OHV_TRAP_VCPU_SET_ADDRESS_SPACE,
                                          (void *)(uintptr_t)asid);

            if (ohv_env("OHV_TRACE_NESTED")) {
                fprintf(stderr, "[ohv] join rc %#x\n", sr);
            }
            if (sr != HV_SUCCESS) {
                fprintf(stderr, "[ohv] vcpu could not join nested space"
                                " %#llx (%#x)\n",
                        (unsigned long long)asid, sr);
            }
        }
    }
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
    /* Rebuilt in place: a slot holds an atomic and cannot be assigned. */
    g_vcpus[id].~VcpuSlot();
    new (&g_vcpus[id]) VcpuSlot();
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
        case HV_REG_CPSR: {
            /*
             * A guest hypervisor does not run at hardware EL2, so PSTATE must
             * not say EL2.  A caller describes its guest the way the guest
             * sees itself and hands us EL2h; writing that through asks the
             * kernel to return into a level the guest does not have, which it
             * refuses as illegal guest state.  Translate the level and leave
             * the rest alone -- the framework does the same, visibly: give it
             * EL2h and its context reads back EL1h.
             */
            uint32_t pstate = (uint32_t)value;

            if (ohv::g_vm_el2) {
                uint32_t mode = pstate & 0xf;

                if (mode == 0x8 || mode == 0x9) {   /* EL2t, EL2h */
                    pstate = (pstate & ~0xfu) | (mode - 4);
                }
            }
            rw->regs.cpsr = pstate;
            break;
        }
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
        /*
         * Kind 0 is the shared block and kind 1 the banked one, which is what
         * the generator emits and what the declaration in ohv_internal.h
         * says.  These two were the other way round here, so every register
         * in either block was read and written eight-and-a-bit registers away
         * from itself: VBAR_EL1 landed on a shared slot, came back as 2, and
         * a VMM that checks its own register file against the accelerator
         * stopped the machine over it.
         */
        case 0: region = OHV_RW_SHARED_SYSREGS; break;
        case 1: region = OHV_RW_BANKED_SYSREGS; break;
        case 2: region = OHV_RW_EXTREGS; break;
        case 3: region = OHV_RW_DBGREGS; break;
        case 5: {   /* the ID registers: host's to describe, VMM's to set */
            int slot = ohv_id_slot(enc);

            if (slot < 0) {
                if (write) return HV_SUCCESS;
                *value = 0;
                return HV_SUCCESS;
            }
            if (!s->id_regs_loaded) {
                static ohv_capabilities_t caps{};
                static bool have = false;

                if (!have) {
                    ohv_raw_trap(OHV_TRAP_CAPABILITIES, &caps);
                    have = true;
                }
                for (unsigned i = 0; i < 18; i++) {
                    s->id_regs[i] = ohv_id_reg_for(&caps, i, false);
                }
                s->id_regs[OHV_ID_SLOT_MIDR] = ohv_caps_field(&caps, HV_SYS_REG_MIDR_EL1);
                s->id_regs[OHV_ID_SLOT_MPIDR] = 0;
                s->id_regs_loaded = true;
            }
            if (write) s->id_regs[slot] = *value;
            else *value = s->id_regs[slot];
            return HV_SUCCESS;
        }
        case 6: region = 0; break;  /* index is the context offset itself */
        case 7: {                   /* no context slot; see VcpuSlot::el2_shadow */
            if (d->index >= sizeof(s->el2_shadow) / sizeof(s->el2_shadow[0]))
                return HV_UNSUPPORTED;
            if (!write) { *value = s->el2_shadow[d->index]; return HV_SUCCESS; }
            s->el2_shadow[d->index] = *value;
            return HV_SUCCESS;
        }
        default: return HV_UNSUPPORTED;
    }
    volatile uint64_t *p = (volatile uint64_t *)(base + region + (uint64_t)d->index * 8);
    /*
     * Sync when the kernel says this part of the shared copy is not current.
     * The per-entry sync_before below is the stronger statement -- always
     * sync, whatever the mask says -- and is kept for the guarded bank, which
     * the hardware fills without going through here at all.
     */
    if (!d->sync_before) {
        uint64_t need = 0;

        switch (d->kind) {
            case 0: case 1: need = 1ull << 0;  break;   /* system registers */
            case 3: need = OHV_STATE_DEBUG;    break;
            case 2: need = 1ull << 61;         break;   /* extended registers */
            case 6: need = OHV_STATE_CONTROLS; break;
            default: break;
        }
        if (need && (*ohv_ro_u64(s->ctx, OHV_RO_STATE_VALID) & need) == 0) {
            hv_return_t r = ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);

            if (r != HV_SUCCESS) return r;
        }
    }
    if (d->sync_before) {
        /*
         * Both directions, not just writes.  A read is where a stale mirror
         * shows: the guarded bank is filled by the hardware on GENTER, and a
         * read that skips the sync answers whatever the context happened to
         * hold -- zero, where the return address should be.  Before a write
         * it keeps the rest of the block from being carried back stale.
         */
        hv_return_t r = ohv_raw_trap(OHV_TRAP_VCPU_SYSREGS_SYNC, nullptr);
        if (r != HV_SUCCESS) return r;
    }
    if (write) {
        uint64_t out = *value;

        /*
         * CPTR_EL2 has bits the kernel put there for its own reasons, and a
         * VMM writing the register from outside does not know about them: it
         * writes what its own CPU model holds, which is zero, and the trap
         * configuration the hypervisor needs goes with it.  The framework
         * filters this write; keeping the bits is the same answer.
         */
        if (enc == 0xe08a) {
            out |= 0x300000ull;
        }
        *p = out;
        /*
         * The key the guest's pointer authentication actually uses.
         *
         * A guest under this hypervisor does not get the part's kernel key.
         * The host keeps a key per virtual machine -- VMKEYLO/HI_EL2, loaded
         * from ro.controls and copied there out of rw.controls, which is the
         * VMM's to write and starts at zero -- and that is what the guest's
         * signing and authenticating are diversified with.  KERNKEYLO/HI_EL1
         * are carried into the silicon and change nothing, which is what a
         * probe measured: changing the architected APIAKey under a signed
         * pointer breaks its authentication, and changing KERNKEY does not,
         * with the signature coming out byte-identical every time.
         *
         * SPTM installs a kernel key and hands TXM images whose pointers are
         * signed under it; TXM authenticates them and takes an FPAC with the
         * value intact and the signature wrong.  So the guest's kernel key is
         * put where the guest's keys live.
         */
        /*
         * The mirror is off; OHV_VMKEY_MIRROR=1 puts it back.
         *
         * It was added because TXM authenticated a pointer out of its own
         * image and took an FPAC, and it is what made the boot core and the
         * cores released later diversify differently.  The boot core writes
         * APIBKey while the VM key is still zero and writes KERNKEY after it;
         * a core released at fifty seconds writes its APIBKey with the mirror
         * already standing, so it signs with a different key.  Read back per
         * vCPU the split is exact and repeats every round -- the boot core
         * keeps 0xaf39aabf231a2fa2, every secondary lands on
         * 0x7544742432da937f -- and a thread that signs its return address on
         * one and returns on the other dies in thread_invoke's RETAB:
         *
         *     panic(cpu N ...): PAC failure from kernel with IB key
         *
         * Without the mirror all six cores hold one key, the panic is gone,
         * and TXM comes up regardless: whatever it needed is being met by
         * something else now.
         */
        if ((enc == 0xe788 || enc == 0xe789) &&
            ohv_env("OHV_VMKEY_MIRROR")) {
            volatile ohv_controls_t *c = ohv_rw_controls(s->ctx);

            if (enc == 0xe788) {
                c->vmkeylo_el2 = out;
            } else {
                c->vmkeyhi_el2 = out;
            }
            mark_dirty(s->ctx, OHV_STATE_CONTROLS);
            if (ohv_env("OHV_TRACE_PAC")) {
                fprintf(stderr, "[ohv] vcpu %llu vmkey %s <- %#llx\n",
                        (unsigned long long)s->id,
                        enc == 0xe788 ? "lo" : "hi",
                        (unsigned long long)out);
            }
        }
        if (d->dirty_bit) mark_dirty(s->ctx, d->dirty_bit);
    } else {
        *value = *p;
    }
    if (ohv_env("OHV_TRACE_ONE")) {
        unsigned want = (unsigned)strtoul(ohv_env("OHV_TRACE_ONE"), nullptr, 0);

        if (enc == want) {
            fprintf(stderr, "openhyp: %s %#06x kind %u index %u at +%#llx"
                    " value %#llx valid %#llx runs %llu\n",
                    write ? "write" : "read", enc, d->kind, d->index,
                    (unsigned long long)(region + (uint64_t)d->index * 8),
                    (unsigned long long)*value,
                    (unsigned long long)*ohv_ro_u64(s->ctx, OHV_RO_STATE_VALID),
                    (unsigned long long)s->run_count);
        }
    }
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_get_sys_reg_traced(hv_vcpu_t, hv_sys_reg_t, uint64_t *);

extern "C" hv_return_t hv_vcpu_get_sys_reg(hv_vcpu_t id, hv_sys_reg_t reg, uint64_t *value) {
    return sysreg_access(id, (uint16_t)reg, value, false);
}
/*
 * A refused system register is otherwise invisible from the outside: the
 * caller sees only the return code, and qemu turns that straight into an
 * assertion naming its own source line.  OHV_TRACE_SYSREG says which register
 * it was.
 */
static void trace_sysreg_refusal(const char *what, uint16_t reg,
                                 uint64_t value, hv_return_t r) {
    if (r == HV_SUCCESS || !ohv_env("OHV_TRACE_SYSREG")) {
        return;
    }
    fprintf(stderr, "openhyp: %s of sysreg %#06x (op0=%u op1=%u crn=%u crm=%u"
            " op2=%u) value %#llx refused: %#x\n", what, reg,
            (reg >> 14) & 3, (reg >> 11) & 7, (reg >> 7) & 0xf,
            (reg >> 3) & 0xf, reg & 7, (unsigned long long)value, r);
}

extern "C" hv_return_t hv_vcpu_set_sys_reg(hv_vcpu_t id, hv_sys_reg_t reg, uint64_t value) {
    hv_return_t r = sysreg_access(id, (uint16_t)reg, &value, true);

    trace_sysreg_refusal("write", (uint16_t)reg, value, r);
    /*
     * OHV_SURVEY_SYSREG is for finding out what a caller needs in one run
     * rather than one register per run: the write is dropped instead of
     * refused, so the caller carries on and every gap gets named.  It leaves
     * the guest missing that state, so it is a survey and nothing else.
     */
    if (r == HV_UNSUPPORTED && ohv_env("OHV_SURVEY_SYSREG")) {
        return HV_SUCCESS;
    }
    return r;
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

/*
 * A pending interrupt is a bit in HCR_EL2: VI for an IRQ and VF for an FIQ,
 * the two the architecture provides for a hypervisor to raise an interrupt in
 * its guest.  The framework keeps them beside its vCPU and pushes them into
 * HCR_EL2 on the way into the guest, comparing the pair each time; writing
 * them straight into the controls and marking them dirty is the same thing
 * one step earlier.
 *
 * This used to answer "unsupported", which is not a distinction a caller
 * makes: qemu raises every interrupt this board produces through here, so
 * nothing the guest waits on ever arrived -- it got as far as loading the
 * next stage over USB and then waited forever for the transfer to complete.
 */
/* Defined beside the timers, and used by the injection call below. */
static void ohv_set_fiq_level(VcpuSlot *s);
static void ohv_set_irq_level(VcpuSlot *s);

#define OHV_HCR_VF (1ull << 6)
#define OHV_HCR_VI (1ull << 7)

static uint64_t pending_bit(hv_interrupt_type_t type) {
    return type == HV_INTERRUPT_TYPE_FIQ ? OHV_HCR_VF : OHV_HCR_VI;
}

extern "C" hv_return_t hv_vcpu_get_pending_interrupt(hv_vcpu_t id, hv_interrupt_type_t type, bool *pending) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s || !pending) return HV_BAD_ARGUMENT;
    *pending = (ohv_rw_controls(s->ctx)->hcr_el2 & pending_bit(type)) != 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_pending_interrupt(hv_vcpu_t id, hv_interrupt_type_t type, bool pending) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s) return HV_BAD_ARGUMENT;
    uint64_t bit = pending_bit(type);
    uint64_t hcr = ohv_rw_controls(s->ctx)->hcr_el2;

    /*
     * The first interrupts the kernel is handed, and only the transitions:
     * the VMM re-asserts the same level on every entry, so logging each call
     * buries the one that matters.  What this is for is XNU's sleh_irq
     * dereferencing a null global -- the object it dispatches an interrupt
     * through is published by IOKit, and anything delivered before that is
     * fatal.  Whether the first one is an IRQ or an FIQ says whether it came
     * from the interrupt controller or from the CPU timer this side drives.
     */
    if (ohv_env("OHV_TRACE_KIRQ")) {
        static bool was[64][2];
        static unsigned n;
        unsigned slot = (unsigned)s->id < 64 ? (unsigned)s->id : 0;
        unsigned kind = type == HV_INTERRUPT_TYPE_FIQ ? 0u : 1u;
        uint64_t pc = ohv_rw(s->ctx)->regs.pc;

        if (pending && !was[slot][kind] && g_kernel_running && n < 40) {
            n++;
            fprintf(stderr, "[ohv] kernel irq %u: vcpu %llu %s raised at"
                    " pc %#llx\n", n, (unsigned long long)s->id,
                    kind == 0 ? "FIQ" : "IRQ", (unsigned long long)pc);
            if (n == 40) {
                fprintf(stderr, "[ohv] kernel irq: 40 shown, no more\n");
            }
        }
        was[slot][kind] = pending;
    }
    (void)bit;
    if (type == HV_INTERRUPT_TYPE_FIQ) {
        s->vmm_fiq = pending;
        ohv_set_fiq_level(s);
    } else {
        s->vmm_irq = pending;
        ohv_set_irq_level(s);
    }
    (void)hcr;
    /*
     * Every transition, uncapped, both ways.  The VMM re-asserts the same
     * level on most entries, so logging each call buries the edge; and a
     * budget hides the one at the end, which is the only one a stall is
     * about.
     */
    if (ohv_env("OHV_TRACE_IRQ")) {
        static bool was[64][2];
        unsigned slot = (unsigned)s->id < 64 ? (unsigned)s->id : 0;
        unsigned kind = type == HV_INTERRUPT_TYPE_FIQ ? 0u : 1u;

        if (was[slot][kind] != pending) {
            was[slot][kind] = pending;
            fprintf(stderr, "[ohv] vcpu %llu %s %s pc %#llx hcr %#llx\n",
                    (unsigned long long)s->id,
                    kind == 0 ? "FIQ" : "IRQ", pending ? "on " : "off",
                    (unsigned long long)ohv_rw(s->ctx)->regs.pc,
                    (unsigned long long)ohv_rw_controls(s->ctx)->hcr_el2);
        }
    }
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_get_trap_debug_exceptions(hv_vcpu_t id, bool *v) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    *v = (ohv_rw_controls(s->ctx)->mdcr_el2 >> 14) & 1; // MDCR_EL2.TDE
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_trap_debug_exceptions(hv_vcpu_t id, bool on) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    volatile uint64_t *mdcr = &ohv_rw_controls(s->ctx)->mdcr_el2;
    *mdcr = on ? (*mdcr | (1ull << 14)) : (*mdcr & ~(1ull << 14));
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_trap_debug_reg_accesses(hv_vcpu_t id, bool *v) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    *v = (ohv_rw_controls(s->ctx)->mdcr_el2 >> 9) & 1; // TDA|TDOSA|TDRA collapsed
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_trap_debug_reg_accesses(hv_vcpu_t id, bool on) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    volatile uint64_t *mdcr = &ohv_rw_controls(s->ctx)->mdcr_el2;
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
    *masked = (ohv_rw_controls(s->ctx)->timer & OHV_TIMER_MASK) != 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_vtimer_mask(hv_vcpu_t id, bool masked) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    volatile uint64_t *t = &ohv_rw_controls(s->ctx)->timer;
    *t = masked ? (*t | OHV_TIMER_MASK) : (*t & ~OHV_TIMER_MASK);
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_get_vtimer_offset(hv_vcpu_t id, uint64_t *off) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !off) return HV_BAD_ARGUMENT;
    *off = ohv_rw_controls(s->ctx)->virtual_timer_offset;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vcpu_set_vtimer_offset(hv_vcpu_t id, uint64_t off) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    ohv_rw_controls(s->ctx)->virtual_timer_offset = off;
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    return HV_SUCCESS;
}

extern "C" hv_return_t hv_vcpu_invalidate_tlb(hv_vcpu_t id, hv_tlbi_op_t op, uint64_t param) {
    VcpuSlot *s = owned_vcpu(id); if (!s) return HV_BAD_ARGUMENT;
    ohv_vm_stage1_tlb_op_t a{ 0, (uint64_t)op, param };
    return ohv_raw_trap(OHV_TRAP_VM_STAGE1_TLB_OP, &a);
}

// -------------------------------------------------------------------- run --
/*
 * The guest's own timers, counted here.
 *
 * A timer that expires is a PPI, and a PPI needs an interrupt controller to
 * arrive at.  This one is modelled in the library, so nothing in the kernel
 * has anywhere to deliver it: the guest arms CNTP_CTL_EL0 with the enable bit
 * and waits, and the hypervisor never hears about it.  Comparing the compare
 * value against the clock the guest was given, and raising the virtual
 * interrupt when it has passed, is the delivery -- the same thing the
 * controller would have done.
 */
#define OHV_CNT_ENABLE  (1ull << 0)
#define OHV_CNT_IMASK   (1ull << 1)
#define OHV_CNT_ISTATUS (1ull << 2)

/*
 * Whether a timer that has come due may be delivered right now.
 *
 * OHV_TIMER_GATE picks the rule, so the three can be compared without a
 * rebuild between them:
 *
 *   0  VM_TMR_FIQ_ENA_EL2.  What this used to do, and wrong -- see the long
 *      note in ohv_expire_timers.  The guest's timed sleeps never wake.
 *   1  no gate at all: any due, unmasked timer is delivered.
 *   2  no gate, except while the guest is running as its own hypervisor.
 *      Useless: guest_at_el2 is never cleared once the guarded-ERET path has
 *      run, so this reads the same as mode 0.
 *   3  no gate, except while the guest's PC is in a monitor image.  Does not
 *      work either, and why is the whole story -- see case 4.
 *   4  the default: raised only when the guest will take it at once.
 *
 * Measured end to end, kernel console bytes to the wedge or panic:
 * mode 0 -> 13613 and a hang, mode 1 -> 625, mode 3 -> 33, mode 4 -> 25515
 * and a named driver panic, with the whole kext list loaded on the way.
 */
static unsigned ohv_timer_gate_mode(void) {
    const char *m = ohv_env("OHV_TIMER_GATE");
    return m ? (unsigned)strtoul(m, nullptr, 0) : 4;
}

/*
 * Whether the guest is running one of the monitor images rather than the
 * kernel.  See VcpuSlot::sptm_base for why one address settles all three.
 */
/*
 * Learn where the monitor images are.  Called on every exit, because the
 * first kernel-VA the guest is seen at has to be SPTM's -- it runs for a long
 * time before XNU exists -- and sampling only when a timer happens to be due
 * gets XNU's instead and inverts the whole test.  Measured: with the capture
 * inside the timer path the one raise of a round landed at
 * 0xfffffff04bbd0118, which is SPTM, in a mode that exists to avoid exactly
 * that.
 *
 * The three images are linked 0x10000000 apart and share one slide, so the
 * lowest 0x10000000 band the guest has ever executed in is SPTM's, the next
 * TXM's, and the one after XNU's.
 */
/*
 * Where the monitor images are is a property of the machine, not of a core.
 *
 * Kept per vCPU it was right for the boot core and wrong for every other one:
 * a core released at fifty seconds runs its whole SPTM entry at physical
 * addresses, which this declines to learn from, so its band stayed zero -- and
 * a band of zero is what makes the FIQ gate answer "raise it" unconditionally.
 * Every secondary therefore had no gate at all across the part of its life
 * that is entirely inside the monitor.
 */
static std::atomic<uint64_t> g_monitor_band{0};

static void ohv_note_image_band(VcpuSlot *s) {
    uint64_t pc = ohv_rw(s->ctx)->regs.pc;
    uint64_t band;

    if (s->sptm_base == 0) {
        s->sptm_base = g_monitor_band.load(std::memory_order_relaxed);
    }
    if (pc < 0xfffffff000000000ull) {
        return;
    }
    band = pc & ~0xfffffffull;
    if (s->sptm_base == 0 || band < s->sptm_base) {
        if (ohv_env("OHV_TRACE_BAND")) {
            fprintf(stderr, "[ohv] vcpu %llu monitor band %#llx"
                    " (from pc %#llx)\n", (unsigned long long)s->id,
                    (unsigned long long)band, (unsigned long long)pc);
        }
        s->sptm_base = band;
        for (;;) {
            uint64_t seen = g_monitor_band.load(std::memory_order_relaxed);

            if (seen != 0 && seen <= band) {
                break;
            }
            if (g_monitor_band.compare_exchange_weak(seen, band,
                                                     std::memory_order_relaxed)) {
                break;
            }
        }
    }
}

static bool ohv_guest_in_monitor(VcpuSlot *s) {
    uint64_t pc = ohv_rw(s->ctx)->regs.pc;

    if (s->sptm_base == 0) {
        return false;
    }
    return pc >= s->sptm_base && pc < s->sptm_base + 0x20000000ull;
}

/*
 * Whether the guest would take a virtual FIQ the moment it is let back in.
 *
 * In the kernel, with PSTATE.F clear.  Anything else leaves the level
 * standing, and a level that is still standing when the guest does a GENTER
 * is taken *inside* SPTM through VBAR_EL1 -- XNU's vector table, not the
 * guarded one -- after which XNU returns the only way it knows and SPTM's
 * entry assertion
 *
 *     mrs x8, TPIDR_GL2 ; ldr x9, [x8, #0x30] ; cmp x9, #0 ; b.ne .
 *
 * spins forever at link 0xfffffff007060770 with the nesting depth at one.
 */
static bool ohv_fiq_takeable(VcpuSlot *s) {
    return s->in_wfx ||
           (!ohv_guest_in_monitor(s) &&
            (ohv_rw(s->ctx)->regs.cpsr & (1u << 6)) == 0);
}

/*
 * The same question for the IRQ, which is masked by a different bit.
 *
 * PSTATE.F (bit 6) is the FIQ mask and PSTATE.I (bit 7) is the IRQ mask, and
 * asking about the wrong one is not a near miss: the kernel masks FIQ on its
 * own around plenty of work with IRQ still open, so a gate that looked at F
 * refused to raise VI at exactly the moments the guest was most able to take
 * it.  A core sitting in the idle loop
 *
 *     mrs x13, ISR_EL1 ; cbnz x13, out ; wfe ; cmn x1, #1 ; b.eq loop
 *
 * wakes only when ISR_EL1 goes non-zero, which is to say only when the level
 * is up; withheld, it never wakes at all.
 */
static bool ohv_irq_takeable(VcpuSlot *s) {
    return s->in_wfx ||
           (!ohv_guest_in_monitor(s) &&
            (ohv_rw(s->ctx)->regs.cpsr & (1u << 7)) == 0);
}

static bool ohv_timer_fiq_wanted(VcpuSlot *s) {
    bool v = s->timer_fired_v, p = s->timer_fired_p;

    switch (ohv_timer_gate_mode()) {
    case 1:
        return v || p;
    case 2:
        return (v || p) && !s->guest_at_el2 && !s->nv_off_for_geret;
    case 3:
        /*
         * Held while the monitor is running.  Measured with mode 1: of the
         * timer FIQs raised once the kernel is up, all but two were taken at
         * a kernelcache PC and serviced; the two that were not landed in
         * SPTM at 0xfffffff0070604ec and _4d4, inside the dispatch that runs
         * a call on XNU's behalf with XNU's own DAIF restored --
         *
         *     ldr x9, [x8, #0x30]      ; the nesting depth
         *     cmp x9, #0x1
         *     b.ne .+0x18              ; depth != 1 -> mask everything
         *     ...  and x8, x8, #0x3c0  ; else take the caller's DAIF
         *     msr DAIF, x8
         *     blraaz x11
         *
         * and the boot then stops in the guarded entry SPTM reaches from
         * there, whose first act is to assert that depth is zero:
         *
         *     mrs x8, S3_6_C15_C11_1 ; ldr x9, [x8, #0x30]
         *     cmp x9, #0x0 ; b.ne .
         *
         * So the FIQ is not being refused by SPTM, it is arriving down a
         * path SPTM only uses for exceptions from below.  Holding it until
         * the guest is back in the kernel is what this mode tests.
         */
        return (v || p) && !ohv_guest_in_monitor(s);
    case 0:
        /* bit 0 is the physical timer, bit 1 the virtual one */
        return (p && (s->vm_tmr_fiq_ena & 1u)) ||
               (v && (s->vm_tmr_fiq_ena & 2u));
    default:
        /*
         * Raised only at a moment the guest will take it at once: in the
         * kernel, with FIQ unmasked.
         *
         * Mode 3 -- hold it while the monitor runs -- does not work, and why
         * it does not is the whole answer.  HCR_EL2.VF is a level, and the
         * guest crosses into SPTM with GENTER, which the hardware carries out
         * without an exit; there is no moment in between at which this side
         * could take the level down.  Measured: 469 edges and the boot ends
         * in the same place as with no gate at all.
         *
         * What happens there, from the wedged stack:
         *
         *     PC  return_from_sptm_exception   (spinning; depth == 1)
         *     #0  _sptm_dispatch_asm_wrapper+0x58
         *     #2  XNU
         *
         * XNU masks FIQ and GENTERs into SPTM, which takes depth 0 -> 1 and
         * then deliberately restores the caller's DAIF around the call it is
         * making on XNU's behalf:
         *
         *     and x8, x8, #0x3c0 ; msr DAIF, x8 ; blraaz x11
         *
         * A pending virtual FIQ is taken right there -- and at hardware EL1
         * it is delivered through VBAR_EL1, which is XNU's vector table, not
         * _GuardedExceptionVectorBase where an exception inside SPTM belongs.
         * XNU services it, returns the only way it knows, GENTER 1
         * (return_from_sptm_exception), and that asserts the nesting depth is
         * zero while SPTM's dispatch frame is still open.
         *
         * So the level must never be left standing across a GENTER.  Raising
         * it only when the guest is in the kernel with F clear means the
         * guest takes it on the next entry, before it can reach one.
         */
        if (!(v || p)) {
            return false;
        }
        /*
         * Before the monitor exists there is no GENTER to leave a level
         * standing across, and the firmware needs the level standing: its
         * main loop at 0x1fc146094 finds its timer by reading ISR_EL1.F,
         * which reports a pending virtual FIQ whether or not PSTATE.F would
         * let it be taken.  Withholding the level while the firmware has FIQ
         * masked leaves that bit reading zero and the firmware never services
         * its timer at all.
         *
         * sptm_base is zero until the guest has executed a kernel VA, which
         * is exactly that boundary.
         */
        if (s->sptm_base == 0) {
            return true;
        }
        return ohv_fiq_takeable(s);
    }
}

/*
 * The guest's virtual IRQ, under the same rule as its FIQ.
 *
 * HCR_EL2.VI is a level too, and a level standing at a GENTER is taken inside
 * SPTM through VBAR_EL1 -- XNU's vector table -- exactly as a standing VF is,
 * with the same end: XNU services it, returns by GENTER 1, and SPTM's entry
 * assertion that the nesting depth is zero spins forever.  The FIQ has been
 * gated for this since the timer work; the IRQ never was, because with one
 * core and the interrupt controller pointed at it the two were never separate
 * enough to notice.
 *
 * Deferring costs nothing: the AIC keeps its own pending set until the guest
 * takes the event, so an interrupt held back until the guest is out of the
 * monitor is delivered late, not lost.  OHV_IRQ_UNGATED=1 puts it back.
 */
static void ohv_set_irq_level(VcpuSlot *s) {
    uint64_t hcr = ohv_rw_controls(s->ctx)->hcr_el2;
    bool want_irq = s->vmm_irq &&
                    (s->sptm_base == 0 || ohv_env("OHV_IRQ_UNGATED") ||
                     ohv_irq_takeable(s));
    uint64_t want = want_irq ? (1ull << 7) : 0;

    if ((hcr & (1ull << 7)) == want) {
        return;
    }
    ohv_rw_controls(s->ctx)->hcr_el2 = (hcr & ~(1ull << 7)) | want;
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
}

/* The guest's virtual FIQ is whichever of its two halves is asserted. */
static void ohv_set_fiq_level(VcpuSlot *s) {
    uint64_t hcr = ohv_rw_controls(s->ctx)->hcr_el2;

    s->timer_fiq = ohv_timer_fiq_wanted(s);

    /*
     * The VMM's own FIQ gets the same gate the timer's does.
     *
     * It did not, and with one core that never showed: nothing else was
     * signalling it.  With six, cpu0's fast IPIs land on cores that are
     * inside SPTM, and each one that arrives there wedges that core's nesting
     * depth for good -- one core is lost on every boot, and the panic path
     * can then never stop it, which is why no stackshot has ever come out.
     *
     * Holding the level costs nothing: the IPI itself is latched in the
     * VMM's IPI_SR until the guest acknowledges it, so a delivery deferred
     * until the guest is back in the kernel is a delivery, not a loss.
     * OHV_IPI_UNGATED=1 puts the old behaviour back.
     */
    bool vmm = s->vmm_fiq &&
               (s->sptm_base == 0 || ohv_env("OHV_IPI_UNGATED") ||
                ohv_fiq_takeable(s));
    uint64_t want = (vmm || s->timer_fiq) ? (1ull << 6) : 0;

    if ((hcr & (1ull << 6)) == want) {
        return;
    }
    /*
     * OHV_TRACE_VF_K narrows this to the kernel era.  The firmware works its
     * timer thousands of times before the kernel exists, and one line per
     * edge there is both the wrong question and slow enough to change the
     * answer.  el2/ger say where the guest is, which is what decides whether
     * gate mode 2 could have held this edge back.
     */
    if (ohv_env("OHV_TRACE_VF") &&
        (!ohv_env("OHV_TRACE_VF_K") || g_kernel_running)) {
        fprintf(stderr, "[ohv] VF %s: pc %#llx mon %#llx cpsr %#x vmm %d"
                " timer %d el2 %d ger %d p_ctl %#llx p_cval %#llx now %#llx\n",
                want ? "up  " : "down",
                (unsigned long long)ohv_rw(s->ctx)->regs.pc,
                (unsigned long long)s->sptm_base,
                (unsigned)ohv_rw(s->ctx)->regs.cpsr,
                (int)s->vmm_fiq, (int)s->timer_fiq,
                (int)s->guest_at_el2, (int)s->nv_off_for_geret,
                (unsigned long long)s->el2_shadow[OHV_SHADOW_CNTP_CTL],
                (unsigned long long)*ohv_rw_u64(s->ctx,
                    OHV_RW_BANKED_SYSREGS + 10 * 8),
                (unsigned long long)mach_absolute_time());
    }
    ohv_rw_controls(s->ctx)->hcr_el2 = (hcr & ~(1ull << 6)) | want;
    mark_dirty(s->ctx, OHV_STATE_CONTROLS);
}

static void ohv_expire_timers(VcpuSlot *s) {
    /* The compare values in the banked slots are in the host's counter. */
    uint64_t now = mach_absolute_time();
    bool raise = false;

    ohv_note_image_band(s);

    for (unsigned which = 0; which < 2; which++) {
        unsigned ctl_index = which ? 19 : 18;   /* CNTP_CTL, CNTV_CTL */
        unsigned cval_index = which ? 10 : 9;
        volatile uint64_t *ctl = which
            ? (volatile uint64_t *)&s->el2_shadow[OHV_SHADOW_CNTP_CTL]
            : ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + ctl_index * 8);
        uint64_t cval =
            *ohv_rw_u64(s->ctx, OHV_RW_BANKED_SYSREGS + cval_index * 8);

        bool fired = false;

        if ((*ctl & OHV_CNT_ENABLE) != 0) {
            if ((int64_t)(now - cval) >= 0) {
                if ((*ctl & OHV_CNT_ISTATUS) == 0) {
                    *ctl |= OHV_CNT_ISTATUS;
                    mark_dirty(s->ctx, OHV_STATE_SYSREGS);
                }
                /*
                 * ISTATUS is maintained whatever the delivery rule decides:
                 * the firmware polls it to find out its timer expired, and
                 * skipping the whole timer when delivery is off takes that
                 * away -- measured, the round stops partway through the
                 * upload with the firmware waiting on a bit that never sets.
                 *
                 * IMASK is the timer's own mask and belongs here.  Whether a
                 * timer that is due and unmasked may actually be delivered is
                 * a question about the guest, not the timer, and is answered
                 * in ohv_timer_fiq_wanted().
                 */
                fired = (*ctl & OHV_CNT_IMASK) == 0;
            } else if ((*ctl & OHV_CNT_ISTATUS) != 0) {
                *ctl &= ~OHV_CNT_ISTATUS;
                mark_dirty(s->ctx, OHV_STATE_SYSREGS);
            }
        }
        if (which) {
            s->timer_fired_p = fired;
        } else {
            s->timer_fired_v = fired;
        }
        raise = raise || fired;
    }
    /*
     * Only once the kernel is up, and then without a budget.  The firmware
     * arms these timers thousands of times before the kernel exists, and a
     * trace that spends itself there answers about the wrong era -- while
     * still being heavy enough, at one write to stderr per exit, to slow the
     * upload it is printing through to a crawl.
     */
    if (ohv_env("OHV_TRACE_TIMER") && g_kernel_running) {
        uint64_t ctl = s->el2_shadow[OHV_SHADOW_CNTP_CTL];

        {
            (void)ctl;
            fprintf(stderr, "[ohv] vcpu %llu timers now %#llx cntp_ctl %#llx"
                    " cntp_cval %#llx cntv_ctl %#llx raise %d\n",
                    (unsigned long long)s->id, (unsigned long long)now,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 19 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 10 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 18 * 8),
                    (int)raise);
        }
    }
    /*
     * Delivered on the virtual FIQ, as a level, because that is where both
     * the firmware and the kernel look for it.
     *
     * iBoot's main loop at 0x1fc146094 reads ISR_EL1 and dispatches on two
     * bits: bit 6, F, goes to the routine that reads CNTP_CTL and services
     * the timer; bit 7, I, goes to the interrupt controller.  Linux's
     * irq-apple-aic agrees from the other side -- the CPU timers are read in
     * aic_handle_fiq, never as an AIC source.  Raised on the virtual IRQ, as
     * this was, the timer arrives with F clear and I set, so the firmware
     * skips its own service routine and goes looking for a controller
     * interrupt that is not there.
     *
     * This needed the VMM's side of the same line sorted out first: three
     * places there wrote CPU_INTERRUPT_FIQ from their own partial view of it,
     * and one of them raised without ever lowering, so the line was latched
     * high before this side could contribute anything.
     *
     * The gate on VM_TMR_FIQ_ENA_EL2 above is kept, and it is wrong.  Linux's
     * irq-apple-aic handles cntp_ctl_el0 and cntv_ctl_el0 -- which is what
     * the banked pair here is -- with no gate at all, and consults that
     * register only for the _EL02 pair, the guest timers a VHE host sees; its
     * aic_init_cpu() clears both bits on every core exactly as SPTM's
     * common_start does.  Obeying it silences the guest's own timers for the
     * whole boot: twenty million passes through this function once the kernel
     * is up and not one raise, so every timed sleep in the kernel blocks
     * forever.
     *
     * It is kept anyway because removing it, with the line and the edges both
     * already right, breaks the boot earlier still -- the kernel reaches
     * AppleImage4 and SPTM then spins in return_from_sptm_exception:
     *
     *     mrs x8, TPIDR_GL2 ; ldr x9, [x8, #0x30] ; cbnz x9, .
     *
     * which is its "still nested inside an exception" assertion.  So the
     * timer FIQ is arriving at a moment SPTM cannot take one.  The
     * architectural shape of that is the thing to fix: HCR_EL2.VF raises a
     * virtual FIQ *for EL1*, and SPTM believes it is at EL2 -- but its EL2 is
     * emulated on the hardware's EL1 here, so it receives one anyway.  The
     * timer must not be raised while the guest is in its own EL2, and this
     * side already knows when that is.
     */
    (void)raise;
    ohv_set_fiq_level(s);
    /*
     * And the IRQ, on the same beat: a level held back because the guest was
     * in the monitor has to be offered again once it is not, and this is the
     * only place that runs on every turn of the vCPU.
     */
    ohv_set_irq_level(s);
}


extern "C" hv_return_t hv_vcpu_run(hv_vcpu_t id) {
    VcpuSlot *s = owned_vcpu(id);
    if (!s) return HV_BAD_ARGUMENT;
    /*
     * One call from the caller is as many entries into the guest as the
     * kernel asks for: it returns for things that are its own business, and
     * the framework goes straight back in rather than passing them up.
     */
    for (;;) {
        uint64_t t0 = mach_absolute_time();
        hv_return_t r;

        ohv_expire_timers(s);
        r = ohv_raw_trap(OHV_TRAP_VCPU_RUN, nullptr);
        uint64_t t1 = mach_absolute_time();

        s->wfi_ticks += (t1 - t0);
        s->run_count++;
        /*
         * The first handful of exits of a vcpu other than the first one.
         *
         * A core released into the kernel parks on a load that fetches a zero
         * from memory which is demonstrably not zero, and every layer above
         * this one has been ruled out by measurement.  What has never been
         * watched is the vcpu itself as it starts: whether it enters at the
         * address it was given, what it faults on, and whether its early exits
         * look like the first one's.
         */
        if (ohv_env("OHV_TRACE_FIRSTRUN") &&
            (ohv_ro(s->ctx)->exit.vmexit_reason == OHV_VMEXIT_HANDLED_FAULT ||
             ohv_ro(s->ctx)->exit.vmexit_reason == OHV_VMEXIT_UNHANDLED_FAULT) &&
            s->kernel_exits++ < 60) {
            ohv_rw_page_head_t *rw = ohv_rw(s->ctx);
            ohv_vmexit_info_t *e = &ohv_ro(s->ctx)->exit;

            fprintf(stderr, "[ohv] vcpu %llu run #%llu -> %d, pc %#llx,"
                    " reason %u, esr %#llx, far %#llx,"
                    " ttbr1 %#llx, tcr %#llx, sctlr %#llx\n",
                    (unsigned long long)s->id,
                    (unsigned long long)s->run_count, (int)r,
                    (unsigned long long)rw->regs.pc,
                    (unsigned)e->vmexit_reason,
                    (unsigned long long)e->vmexit_esr,
                    (unsigned long long)e->vmexit_far,
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 1 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 2 * 8),
                    (unsigned long long)*ohv_rw_u64(s->ctx,
                        OHV_RW_BANKED_SYSREGS + 12 * 8));
        }
        if (r != HV_SUCCESS) {
            return r;
        }
        /*
         * A cancel that was asked for while the guest was running, and that
         * the run trap did not report.  It is still standing here, and this
         * is the first chance to act on it -- waiting for the turnaround
         * check below would carry it past exit_from_ro, which may decide the
         * exit was its own business and re-enter without ever telling the VMM.
         */
        if (s->exit_requested.exchange(false, std::memory_order_acq_rel)) {
            if (ohv_env("OHV_TRACE_KICK")) {
                fprintf(stderr, "[ohv] vcpu %llu cancel after run at pc %#llx"
                        "\n", (unsigned long long)s->id,
                        (unsigned long long)ohv_rw(s->ctx)->regs.pc);
            }
            s->pub_exit.reason = HV_EXIT_REASON_CANCELED;
            return HV_SUCCESS;
        }
        if (exit_from_ro(s) == EXIT_TO_VMM) {
            /*
             * The VMM re-reads everything it cares about on the way back in,
             * so a cancel still standing here has already been served.
             */
            s->exit_requested.store(false, std::memory_order_release);
            return HV_SUCCESS;
        }
        /*
         * About to re-enter without telling the VMM.  This is the turnaround a
         * cancel gets lost in: the run trap can only deliver one to a vCPU
         * that is inside it, and this one is not.  Honour it here instead --
         * anything the VMM wanted to inject is injected on the way back.
         */
        if (s->exit_requested.exchange(false, std::memory_order_acq_rel)) {
            if (ohv_env("OHV_TRACE_KICK")) {
                fprintf(stderr, "[ohv] vcpu %llu cancel honoured at pc %#llx"
                        " hcr %#llx\n", (unsigned long long)s->id,
                        (unsigned long long)ohv_rw(s->ctx)->regs.pc,
                        (unsigned long long)ohv_rw_controls(s->ctx)->hcr_el2);
            }
            s->pub_exit.reason = HV_EXIT_REASON_CANCELED;
            return HV_SUCCESS;
        }
    }
}

extern "C" hv_return_t hv_vcpus_exit(hv_vcpu_t *vcpus, uint32_t count) {
    if (!vcpus && count) return HV_BAD_ARGUMENT;
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (vcpus[i] >= kMaxVcpuIds) return HV_BAD_ARGUMENT;
        mask |= 1ull << vcpus[i];
    }
    /*
     * Latched before the trap, so a cancel that lands while the vCPU is
     * between entries -- which the trap alone cannot deliver -- is still
     * seen.  See VcpuSlot::exit_requested.
     */
    for (uint32_t i = 0; i < count; i++) {
        g_vcpus[vcpus[i]].exit_requested.store(true, std::memory_order_release);
    }
    return ohv_raw_trap(OHV_TRAP_VCPU_RUN_CANCEL, (void *)mask);
}

// -------------------------------------------------- private control/ext API --
/*
 * A control field is not the n-th word of the controls block.  The framework
 * numbers them through a table, and the order is nothing like the layout:
 * field 1 is CPTR_EL2 where the second word is HACR_EL2, field 6 is HACR_EL2,
 * field 9 is APSTS_EL1 where the tenth word is HFGRTR_EL2.  Treating the
 * number as an index puts every write on a different register than the caller
 * named -- a VMM asking for the guarded-operation trap sets a bit in HACR
 * instead of CPTR, and one raising the guarded level sets a fine-grained trap
 * instead, which is enough to take a core somewhere it never returns from.
 *
 * Offsets are from the start of the controls block, read out of the
 * framework's own table.  Two of the eighteen are not fields at all.
 */
#define OHV_CONTROL_FIELD_NONE 0xffff
static const uint16_t kControlFieldOffset[] = {
    0x00,                   /*  0  HCR_EL2      */
    0x10,                   /*  1  CPTR_EL2     */
    0x18,                   /*  2  MDCR_EL2     */
    0x20,                   /*  3  VMPIDR_EL2   */
    OHV_CONTROL_FIELD_NONE, /*  4               */
    0x28,                   /*  5  VPIDR_EL2    */
    0x08,                   /*  6  HACR_EL2     */
    0x70,                   /*  7  VMKEYHI_EL2  */
    0x78,                   /*  8  VMKEYLO_EL2  */
    0x80,                   /*  9  APSTS_EL1    */
    OHV_CONTROL_FIELD_NONE, /* 10               */
    0x38,                   /* 11  HFGRTR_EL2   */
    0x40,                   /* 12  HFGWTR_EL2   */
    0x48,                   /* 13  HFGITR_EL2   */
    0x50,                   /* 14  HDFGRTR_EL2  */
    0x58,                   /* 15  HDFGWTR_EL2  */
    0x60,                   /* 16  CNTHCTL_EL2  */
    0xD0,                   /* 17  HCRX_EL2     */
};

static hv_return_t control_field_access(hv_vcpu_t id, _hv_control_field_t f, uint64_t *v, bool w) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !v) return HV_BAD_ARGUMENT;
    if ((unsigned)f >= sizeof(kControlFieldOffset) / sizeof(kControlFieldOffset[0]))
        return HV_BAD_ARGUMENT;
    uint16_t off = kControlFieldOffset[(unsigned)f];
    if (off == OHV_CONTROL_FIELD_NONE) return HV_BAD_ARGUMENT;

    volatile uint64_t *c =
        (volatile uint64_t *)((uint8_t *)ohv_rw_controls(s->ctx) + off);
    if (w) {
        /*
         * Field 9 is APSTS_EL1, the guarded level.  A caller that raises it
         * from out here is describing a state the guest is not in, and the
         * framework does not let the write stand: ask it to set field 9 and
         * it still reads back zero.  Doing what it was told instead leaves
         * every later reader -- including the code that decides whether a
         * trapped ERET is a guarded return -- believing the guest is guarded
         * before it has ever executed a GENTER.
         */
        if ((unsigned)f == 9 && !ohv_env("OHV_ALLOW_APSTS_WRITE")) {
            return HV_SUCCESS;
        }
        *c = *v;
        mark_dirty(s->ctx, OHV_STATE_CONTROLS);
    }
    else *v = *c;
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
/*
 * The same private surface under the names Apple actually exports.
 *
 * Every consumer of this surface reaches it with dlsym("_hv_..."), because
 * the symbols are not in any SDK header -- QEMU's HVF backend does exactly
 * that.  Exporting only the two-underscore spelling means those lookups
 * return null and the caller carries on with the feature quietly disabled:
 * control fields are never written, so the traps they enable never arrive,
 * and the context is never read.  Nothing reports an error, which is the
 * worst shape a missing symbol can take.
 *
 * _hv_vcpu_get_context also has a different shape there: it answers with the
 * pointer rather than writing it through an out-parameter.
 */
extern "C" void *_hv_vcpu_get_context(hv_vcpu_t id) {
    VcpuSlot *s = owned_vcpu(id);
    return s ? s->ctx : nullptr;
}

extern "C" hv_return_t _hv_vcpu_get_control_field(hv_vcpu_t id,
                                                 _hv_control_field_t f,
                                                 uint64_t *v) {
    return control_field_access(id, f, v, false);
}

extern "C" hv_return_t _hv_vcpu_set_control_field(hv_vcpu_t id,
                                                 _hv_control_field_t f,
                                                 uint64_t v) {
    return control_field_access(id, f, &v, true);
}

extern "C" hv_return_t __hv_vcpu_get_ext_reg(hv_vcpu_t id, _hv_ext_reg_t reg, uint64_t *value) {
    VcpuSlot *s = owned_vcpu(id); if (!s || !value) return HV_BAD_ARGUMENT;
    if ((unsigned)reg >= sizeof(ohv_extregs_t) / 8) return HV_BAD_ARGUMENT;
    *value = ((volatile uint64_t *)(s ? (uint8_t *)s->ctx + OHV_RW_EXTREGS : nullptr))[(unsigned)reg];
    return HV_SUCCESS;
}