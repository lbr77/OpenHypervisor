// ohv_vm.cpp - VM lifecycle, guest memory, config objects.
#include <new>
#include <stdio.h>
#include "openhyp/ohv_object.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include "ohv_internal.h"
#include <cstring>
#include <cstdlib>
#include "openhyp/ohv_trap.h"

namespace {
const unsigned kNestedSpaces = 8;   /* what the framework asks for */
uint64_t g_nested_asid[kNestedSpaces];
unsigned g_nested_count;
}  // namespace


using namespace ohv;

// ---------------------------------------------------------------- config --
// hv_vm_config_s is a pure userspace object; the kernel only sees the flat
// ohv_vm_create_t we build from it. Framework-observed offsets are pinned:
// granule@0x14, el2@24, vhe@25, isa@28 (PROTOCOL.md section 3).
struct hv_vm_config_s {
    Class __isa;            /* os_release() reads this; see ohv_object.h */
    uint32_t refcnt;
    uint32_t ipa_bits;
    uint8_t __pad_to_granule[0x14 - 8];
    uint32_t granule;       // +0x14 behind the isa
    uint8_t el2_enabled;    // +24
    uint8_t vhe_enabled;    // +25
    uint8_t pad[2];
    uint32_t isa;           // +28
};
/*
 * The offsets are the ones the framework's own accessors use, measured from
 * behind the object header rather than from the start of the allocation.
 */
#define OHV_VM_CONFIG_AT(f) (offsetof(hv_vm_config_s, f) - sizeof(Class))
static_assert(OHV_VM_CONFIG_AT(granule) == 0x14, "granule@0x14");
static_assert(OHV_VM_CONFIG_AT(el2_enabled) == 24, "el2@24");
static_assert(OHV_VM_CONFIG_AT(vhe_enabled) == 25, "vhe@25");
static_assert(OHV_VM_CONFIG_AT(isa) == 28, "isa@28");

extern "C" hv_vm_config_t hv_vm_config_create(void) {
    hv_vm_config_s *c = (hv_vm_config_s *)ohv_object_alloc(sizeof(*c));
    if (!c) return nullptr;
    c->refcnt = 1;
    c->ipa_bits = 0; // 0 = machine default
    c->granule = 0;  // 0 = default granule
    c->isa = OHV_VM_ISA_APPLE;
    return c;
}

extern "C" uint32_t hv_vm_get_isa(void) { return g_vm_alive ? g_vm_isa : OHV_VM_ISA_NONE; }

/*
 * The framework spells this one with the underscore in the C name, and hands
 * the level back through the argument rather than the return value.  A caller
 * that reads the granted level back after hv_vm_create -- qemu does, and
 * refuses to run when it cannot -- looks it up by that name through dlsym, so
 * the name and the shape both have to match to be a drop-in.
 */
extern "C" hv_return_t _hv_vm_get_isa(uint32_t *isa) {
    if (!isa) return HV_BAD_ARGUMENT;
    if (!g_vm_alive) return HV_BAD_ARGUMENT;
    *isa = g_vm_isa;
    return HV_SUCCESS;
}

static uint32_t machine_ipa_bits() {
    static uint32_t cached = 0;
    static bool have = false;
    if (!have) {
        ohv_capabilities_t caps{};
        if (ohv_raw_trap(OHV_TRAP_CAPABILITIES, &caps) == HV_SUCCESS &&
            caps.ipa_bits_4k >= 24 && caps.ipa_bits_4k <= 52)
            cached = (uint32_t)caps.ipa_bits_4k;
        else
            cached = 40; /* Apple Silicon physical address width */
        have = true;
    }
    return cached;
}

extern "C" hv_return_t hv_vm_config_get_max_ipa_size(uint32_t *out) {
    if (!out) return HV_BAD_ARGUMENT;
    *out = machine_ipa_bits();
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_get_default_ipa_size(uint32_t *out) {
    return hv_vm_config_get_max_ipa_size(out);
}
extern "C" hv_return_t hv_vm_config_set_ipa_size(hv_vm_config_t c, uint32_t bits) {
    if (!c) return HV_BAD_ARGUMENT;
    c->ipa_bits = bits;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_get_ipa_size(hv_vm_config_t c, uint32_t *bits) {
    if (!c || !bits) return HV_BAD_ARGUMENT;
    *bits = c->ipa_bits;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_get_el2_supported(bool *supported) {
    if (!supported) return HV_BAD_ARGUMENT;
    *supported = true;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_get_el2_enabled(hv_vm_config_t c, bool *en) {
    if (!c || !en) return HV_BAD_ARGUMENT;
    *en = c->el2_enabled != 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_set_el2_enabled(hv_vm_config_t c, bool en) {
    if (!c) return HV_BAD_ARGUMENT;
    c->el2_enabled = en ? 1 : 0;
    return HV_SUCCESS;
}
// private VHE selectors (FINDINGS.md: vhe byte requires el2 byte == 1)
extern "C" hv_return_t _hv_vm_config_set_vhe_enabled(hv_vm_config_t c, bool en) {
    if (!c) return HV_BAD_ARGUMENT;
    if (en && !c->el2_enabled) return HV_BAD_ARGUMENT;
    c->vhe_enabled = en ? 1 : 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t _hv_vm_config_get_vhe_enabled(hv_vm_config_t c, bool *en) {
    if (!c || !en) return HV_BAD_ARGUMENT;
    *en = c->vhe_enabled != 0;
    return HV_SUCCESS;
}
extern "C" hv_return_t _hv_vm_config_set_isa(hv_vm_config_t c, uint32_t isa) {
    if (!c || isa > OHV_VM_ISA_INTERNAL) return HV_BAD_ARGUMENT;
    c->isa = isa;
    return HV_SUCCESS;
}
extern "C" hv_return_t _hv_vm_config_get_isa(hv_vm_config_t c, uint32_t *isa) {
    if (!c || !isa) return HV_BAD_ARGUMENT;
    *isa = c->isa;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_get_default_ipa_granule(hv_ipa_granule_t *granule) {
    if (!granule) return HV_BAD_ARGUMENT;
    *granule = 0x4000; // 16 KiB on arm64 Apple platforms
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_get_ipa_granule(hv_vm_config_t c, hv_ipa_granule_t *g) {
    if (!c || !g) return HV_BAD_ARGUMENT;
    *g = c->granule ? c->granule : 0x4000;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_config_set_ipa_granule(hv_vm_config_t c, hv_ipa_granule_t g) {
    if (!c || (g != 0x1000 && g != 0x4000 && g != 0x10000)) return HV_BAD_ARGUMENT;
    c->granule = g;
    return HV_SUCCESS;
}

// -------------------------------------------------------------- lifecycle --

/*
 * The address spaces a guest hypervisor needs before it can exist.
 *
 * Guest EL2 is not something the VM-create request carries -- the framework
 * sends flags = 0 there too.  What it does instead, in Hv::Vm::Vm, is notice
 * el2_enabled and stand up a GuestHypervisorSpaceManager, which creates eight
 * nested address spaces through the address-space trap and hands their ASIDs
 * to a NestedGuestMemoryMap.  Without them the kernel has nothing to run a
 * guest hypervisor in, and refuses a vCPU entering EL2h as illegal guest
 * state.
 *
 * The request it sends is the constant pair at 0x21b3f4948: min_ipa 0,
 * ipa_size 0, granule 0x1000, flags 0, with the ASID read back out.
 */
uint64_t ohv_nested_asid(unsigned index) {
    if (getenv("OHV_TRACE_NESTED")) {
        fprintf(stderr, "[ohv] nested spaces available: %u (asking for %u)\n",
                g_nested_count, index);
    }
    return index < g_nested_count ? g_nested_asid[index] : 0;
}

static void nested_spaces_create(void) {
    for (unsigned i = 0; i < kNestedSpaces; i++) {
        ohv_vm_addrspace_create_t a{};

        a.granule = 0x1000;
        hv_return_t r = ohv_raw_trap(OHV_TRAP_VM_ADDRESS_SPACE_CREATE, &a);

        if (r != HV_SUCCESS) {
            /*
             * Say so.  A guest hypervisor with no address spaces is refused
             * later, at the first run, as illegal guest state -- which says
             * nothing about the space that could not be made here.
             */
            fprintf(stderr, "[ohv] nested address space %u refused (%#x)\n", i, r);
            break;
        }
        if (g_nested_count < kNestedSpaces) {
            g_nested_asid[g_nested_count++] = a.out_asid;
        }
    }
}

extern "C" hv_return_t hv_vm_create(hv_vm_config_t config) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = HV_SUCCESS;
    do {
        if (g_vm_alive) { r = HV_BUSY; break; }
        ohv_vm_create_t args{};
        uint32_t bits = (config && config->ipa_bits) ? config->ipa_bits : machine_ipa_bits();
        args.min_ipa = 0;
        args.ipa_size = bits ? (1ull << bits) : 0;
        args.granule = config ? config->granule : 0;
        /*
         * Flags are zero, which is what the framework sends too: Hv::Vm::create
         * builds {min_ipa, ipa_size, granule@16, flags@20 = 0, isa@24} and
         * leaves it at that.  Guest EL2 and VHE are not part of this request --
         * it keeps them in its own Vm object and carries them further down --
         * so there is nothing to look for here.
         */
        args.flags = 0;
        args.isa = config ? config->isa : OHV_VM_ISA_APPLE;
        const char *dbg = getenv("OHV_DEBUG");
        if (dbg && *dbg) {
          fprintf(stderr, "[ohv] create argsdump:");
          for (unsigned i = 0; i < sizeof(ohv_vm_create_t); i++)
            fprintf(stderr, "%02x", ((unsigned char*)&args)[i]);
          fprintf(stderr, "\n");
        }
        r = ohv_raw_trap(OHV_TRAP_VM_CREATE, &args);
        if (r == HV_SUCCESS) {
            g_vm_alive = true;
            g_vm_isa = args.isa;
            g_vm_el2 = config && config->el2_enabled;
            g_vm_vhe = config && config->vhe_enabled;
            if (g_vm_el2) {
                nested_spaces_create();
            }
        }
    } while (0);
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}

extern "C" hv_return_t hv_vm_destroy(void) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r;
    if (!g_vm_alive) {
        pthread_mutex_unlock(&g_vm_mutex);
        return HV_NO_DEVICE;
    }
    r = ohv_raw_trap(OHV_TRAP_VM_DESTROY, nullptr);
    if (r == HV_SUCCESS) {
        g_vm_alive = false;
        g_gic = nullptr;
        /*
         * Rebuilt in place rather than copy-assigned: a slot now holds an
         * atomic, and those cannot be assigned.
         */
        for (auto &s : g_vcpus) {
            s.~VcpuSlot();
            new (&s) VcpuSlot();
        }
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}

extern "C" hv_return_t hv_vm_get_max_vcpu_count(uint32_t *max) {
    if (!max) return HV_BAD_ARGUMENT;
    *max = g_max_vcpus;
    return HV_SUCCESS;
}

// ---------------------------------------------------------------- memory --

static hv_return_t map_op(int trap, void *uva, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) {
    pthread_mutex_lock(&g_vm_mutex);
    hv_return_t r = require_vm();
    if (r == HV_SUCCESS) {
        const char *mdbg = getenv("OHV_DEBUG");
        if (mdbg && *mdbg)
          fprintf(stderr, "[ohv] map trap=%d uva=%p ipa=%llx size=%zx flags=%llx\n",
                  trap, uva, (unsigned long long)ipa, size, (unsigned long long)flags);
        if (!size || (size & 0x3fffull) || (ipa & 0x3ffull)) {
            r = HV_BAD_ARGUMENT;
        } else {
            ohv_vm_map_item_t item{ (uint64_t)(uintptr_t)uva, ipa, size, flags, 0 };
            r = ohv_raw_trap((unsigned)trap, &item);
            if (mdbg && *mdbg) fprintf(stderr, "[ohv]   -> %d\n", r);
            /*
             * A guest hypervisor runs in one of the nested address spaces, and
             * a space with nothing in it cannot fetch an instruction.  Put the
             * same memory in each of them, so a vCPU is not left choosing
             * between an address space that describes its guest and one the
             * kernel will accept.
             */
            if (r == HV_SUCCESS && g_vm_el2) {
                for (unsigned i = 0; i < g_nested_count; i++) {
                    ohv_vm_map_item_t in_space{ (uint64_t)(uintptr_t)uva, ipa,
                                                size, flags, g_nested_asid[i] };
                    hv_return_t sr = ohv_raw_trap((unsigned)trap, &in_space);

                    if (sr != HV_SUCCESS && mdbg && *mdbg) {
                        fprintf(stderr, "[ohv]   space %llx -> %d\n",
                                (unsigned long long)g_nested_asid[i], sr);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_vm_mutex);
    return r;
}

/*
 * What is mapped where, which the kernel does not keep on our behalf.
 *
 * A trapped system register access arrives as a reason and nothing else: no
 * syndrome, no opcode.  Answering it means reading the instruction, and the
 * instruction is in guest memory, so the guest physical address has to be
 * turned back into something this process can read.  That needs a record of
 * the mappings, kept here as they are made.
 */
namespace {
struct MapRecord { uint64_t uva; uint64_t ipa; uint64_t size; };
MapRecord g_maps[64];
unsigned g_map_count;

void map_remember(void *uva, uint64_t ipa, uint64_t size) {
    for (unsigned i = 0; i < g_map_count; i++) {
        if (g_maps[i].ipa == ipa) { g_maps[i] = { (uint64_t)(uintptr_t)uva, ipa, size }; return; }
    }
    if (g_map_count < 64) g_maps[g_map_count++] = { (uint64_t)(uintptr_t)uva, ipa, size };
}

void map_forget(uint64_t ipa) {
    for (unsigned i = 0; i < g_map_count; i++) {
        if (g_maps[i].ipa == ipa) { g_maps[i] = g_maps[--g_map_count]; return; }
    }
}
}  // namespace

void *ohv_guest_ptr(uint64_t ipa, uint64_t len) {
    for (unsigned i = 0; i < g_map_count; i++) {
        const MapRecord &m = g_maps[i];
        if (ipa >= m.ipa && ipa + len <= m.ipa + m.size) {
            return (void *)(uintptr_t)(m.uva + (ipa - m.ipa));
        }
    }
    return nullptr;
}

extern "C" hv_return_t hv_vm_map(void *addr, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) {
    if (!addr) return HV_BAD_ARGUMENT;
    hv_return_t r = map_op(OHV_TRAP_VM_MAP, addr, ipa, size, flags);
    if (r == HV_SUCCESS) map_remember(addr, ipa, size);
    return r;
}
extern "C" hv_return_t hv_vm_unmap(hv_ipa_t ipa, size_t size) {
    map_forget(ipa);
    return map_op(OHV_TRAP_VM_UNMAP, nullptr, ipa, size, 0);
}
extern "C" hv_return_t hv_vm_protect(hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) {
    return map_op(OHV_TRAP_VM_PROTECT, nullptr, ipa, size, flags);
}

// legacy allocate-style API
extern "C" hv_return_t hv_vm_allocate(void **uvap, size_t size, hv_allocate_flags_t flags) {
    if (!uvap || !size) return HV_BAD_ARGUMENT;
    void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) return HV_NO_RESOURCES;
    *uvap = p;
    return HV_SUCCESS;
}
extern "C" hv_return_t hv_vm_deallocate(void *uva, size_t size) {
    if (!uva || !size) return HV_BAD_ARGUMENT;
    return munmap(uva, size) == 0 ? HV_SUCCESS : (errno == EINVAL ? HV_BAD_ARGUMENT : HV_ERROR);
}