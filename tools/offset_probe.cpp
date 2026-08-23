// offset_probe.cpp - runtime offset matcher against the original
// Hypervisor.framework (arm64e macOS).
//
// dlopen()s the installed framework, resolves exports with dlsym() and reads
// mapped __TEXT as raw bytes. Exported functions are thin dispatchers here,
// so a bounded BFS follows BL/B edges into internal helpers and merges every
// load/store immediate it meets. Findings are compared against the pinned
// constants in ohv_context.h / ohv_vm.cpp.
//
//   make offset_probe && ./tools/offset_probe
//   OHV_PROBE=1 ./tools/offset_probe   # also diff hv_vm_config setters live
#include <dlfcn.h>
#include <inttypes.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *name; uint64_t val; } Pin;

static const Pin pins_config[] = {
    {"granule", 0x14}, {"el2", 24}, {"vhe", 25}, {"isa", 28},
};
static const Pin pins_regs[] = {
    {"x0", 0x008}, {"fp", 0x0f0}, {"lr", 0x0f8}, {"sp", 0x100},
    {"pc", 0x108}, {"cpsr", 0x110},
};
static const Pin pins_ro[] = {
    {"ver", 0x4000}, {"exit_reason", 0x4008}, {"esr", 0x400c},
    {"instr", 0x4010}, {"far", 0x4018}, {"hpfar", 0x4020},
    {"controls_mirror", 0x4028}, {"state_valid", 0x4100},
    {"state_dirty", 0x4108}, {"state_used", 0x4110},
};
static const Pin pins_traps[] = {
    {"capabilities", 0}, {"vm_create", 1}, {"vm_destroy", 2},
    {"vm_map", 3}, {"vm_unmap", 4}, {"vm_protect", 5},
    {"vcpu_create", 6}, {"vcpu_destroy", 7},
    {"sysregs_sync", 8}, {"vcpu_run", 9},
    {"run_cancel", 10}, {"set_address_space", 11},
    {"as_create", 12}, {"invalidate_tlb", 13},
    {"stage1_tlb", 14}, {"set_svcr", 15},
    {"amx_prepare", 16}, {"monitor_data_abort", 17},
};

static int findings, mismatches, misses, indirects;

// ------------------------------------------------------------- decoder -----
typedef enum { I_UNK = 0, I_LDR, I_STR, I_LDUR, I_STUR, I_MOVZ, I_MOVK,
               I_SVC, I_RET, I_RETAB, I_BL, I_B } Kind;
typedef struct { Kind kind; uint8_t rt; int32_t imm; uint8_t size; } Insn;

static bool dec_uimm(uint32_t w, Insn *o) {
    if ((w & 0x3b000000) != 0x39000000) return false;
    uint32_t size = w >> 30, v = (w >> 26) & 1, opc = (w >> 22) & 3;
    if ((opc & 2) && !(v && size == 0)) return false;
    o->kind = (opc & 1) ? I_LDR : I_STR;
    o->rt = w & 31;
    o->size = v ? ((opc >= 2 && size == 0) ? 16 : (1u << size)) : (1u << size);
    o->imm = (int32_t)(((w >> 10) & 0xfff) * o->size);
    return true;
}
static bool dec_ur(uint32_t w, Insn *o) {
    if ((w & 0x3fe00400) != 0x38000000) return false;
    uint32_t size = w >> 30, v = (w >> 26) & 1, opc = (w >> 22) & 3;
    if (v) return false;
    o->kind = (opc & 1) ? I_LDUR : I_STUR;
    o->rt = w & 31; o->size = 1u << size;
    int32_t imm9 = (int32_t)((w >> 12) & 0x1ff);
    if (imm9 & 0x100) imm9 -= 0x200;
    o->imm = imm9;
    return true;
}
static Insn decode(uint32_t w) {
    Insn o = {};
    if (dec_uimm(w, &o)) return o;
    if (dec_ur(w, &o)) return o;
    if ((w & 0x7f800000) == 0x52800000) {
        uint32_t top = (w >> 29) & 3, hw = (w >> 21) & 3;
        o.kind = (top == 3) ? I_MOVK : (top == 2) ? I_MOVZ : I_UNK;
        o.rt = w & 31;
        o.imm = (int32_t)(((w >> 5) & 0xffff) << (16 * hw));
        return o;
    }
    if ((w & 0xfff000ff) == 0xd4000001) { o.kind = I_SVC; o.imm = (w >> 5) & 0xffff; return o; }
    if (w == 0xd65f03c0) { o.kind = I_RET; return o; }
    if ((w & 0xfffffc1f) == 0xd65f0c00) { o.kind = I_RETAB; return o; }
    if ((w & 0xfc000000) == 0x94000000) { o.kind = I_BL; return o; }
    if ((w & 0xfc000000) == 0x14000000) { o.kind = I_B; return o; }
    return o;
}
static int64_t branch_off(uint32_t w) {
    return ((int32_t)((w & 0x03ffffff) << 6) >> 6) * 4;
}

// ---------------------------------------------------------- call graph -----
#define MAXOFFS 8192
typedef struct { uint64_t offs[MAXOFFS]; uint8_t widths[MAXOFFS]; unsigned n; } Offsets;
static void offs_add(Offsets *t, uint64_t off, uint8_t width) {
    for (unsigned k = 0; k < t->n; k++) if (t->offs[k] == off) return;
    if (t->n < MAXOFFS) { t->offs[t->n] = off; t->widths[t->n] = width; t->n++; }
}

#define VIS_BITS 14
static uint64_t vis_tab[1u << VIS_BITS];
static unsigned visited_nodes;
static bool vis_seen(uint64_t a) {
    uint64_t h = (a >> 2) * 0x9e3779b97f4a7c15ull;
    unsigned i = (unsigned)((h >> 50) & ((1u << VIS_BITS) - 1));
    while (vis_tab[i]) {
        if (vis_tab[i] == a) return true;
        i = (i + 1) & ((1u << VIS_BITS) - 1);
    }
    if (visited_nodes < (1u << VIS_BITS) / 2) { vis_tab[i] = a; visited_nodes++; }
    return false;
}
static void vis_reset(void) { memset(vis_tab, 0, sizeof(vis_tab)); visited_nodes = 0; }

static uint64_t g_text_lo, g_text_hi;      // runtime __TEXT bounds
static uint64_t g_hvtrap;                  // libsystem_kernel hv_trap
#define NODE_MAX 6144                      // bytes scanned per body
#define MAX_DEPTH 6
#define MAX_NODES 12000

// BFS over call edges; merges all ldr/str immediates into out.
#define MAXHITS 256
typedef struct { uint64_t val; } NumHit;
static NumHit g_traps[MAXHITS]; static unsigned g_ntraps;
static void push_trap(uint64_t t) {
    for (unsigned i = 0; i < g_ntraps; i++) if (g_traps[i].val == t) return;
    if (g_ntraps < MAXHITS) g_traps[g_ntraps++].val = t;
}
static NumHit g_strb[MAXHITS]; static unsigned g_nstrb;
static void push_strb(uint64_t off) {
    for (unsigned i = 0; i < g_nstrb; i++) if (g_strb[i].val == off) return;
    if (g_nstrb < MAXHITS) g_strb[g_nstrb++].val = off;
}
// Every small movz w0 immediate in reachable code: the kernel-gate command
// ids are prepared this way by callers (the gate itself is reached through a
// function pointer, so there is no direct call edge to follow).
static NumHit g_w0[MAXHITS]; static unsigned g_nw0;
static void push_w0(uint64_t v) {
    for (unsigned i = 0; i < g_nw0; i++) if (g_w0[i].val == v) return;
    if (g_nw0 < MAXHITS) g_w0[g_nw0++].val = v;
}

typedef struct { uint64_t addr; int depth; } QueueEnt;
#define STACK_MAX 4096
static QueueEnt q[STACK_MAX]; static unsigned qn;   // LIFO stack

static void enqueue(uint64_t tgt, int depth) {
    if (depth > MAX_DEPTH) return;
    if (tgt < g_text_lo || tgt >= g_text_hi) return;
    if (qn >= STACK_MAX) return;
    if (vis_seen(tgt)) return;
    q[qn] = (QueueEnt){tgt, depth}; qn++;
}

static void walk_from(uint64_t start, Offsets *out) {
    vis_reset(); qn = 0;
    enqueue(start, 0);
    while (qn > 0 && visited_nodes <= MAX_NODES) {
        QueueEnt e = q[--qn];              // depth-first: pop latest
        const uint8_t *code = (const uint8_t *)e.addr;
        int last_movz_w0 = -1;
        unsigned junk = 0;
        for (size_t i = 0; i + 4 <= NODE_MAX && junk < 320; i += 4) {
            uint32_t w = *(const uint32_t *)(code + i);
            Insn in = decode(w);
            switch (in.kind) {
            case I_LDR: case I_STR: case I_LDUR: case I_STUR:
                if (in.imm >= 0 && in.imm <= 0x8000 && (in.imm % in.size) == 0)
                    offs_add(out, (uint64_t)in.imm, in.size);
                if (in.kind == I_STR && in.size == 1) push_strb((uint64_t)in.imm);
                junk = 0;
                break;
            case I_MOVZ:
                if (in.rt == 0 && ((w >> 21) & 3) == 0) {
                    last_movz_w0 = (int)i;
                    if (in.imm >= 0 && in.imm <= 255) push_w0((uint64_t)in.imm);
                }
                junk = 0;
                break;
            case I_MOVK: case I_UNK: junk++; break;
            case I_SVC:
                if (last_movz_w0 >= 0) {
                    Insn mz = decode(*(const uint32_t *)(code + last_movz_w0));
                    push_trap((uint64_t)mz.imm);
                }
                junk = 0;
                break;
            case I_BL: {
                uint64_t tgt = e.addr + i + branch_off(w);
                if (tgt == g_hvtrap && last_movz_w0 >= 0) {
                    Insn mz = decode(*(const uint32_t *)(code + last_movz_w0));
                    push_trap((uint64_t)mz.imm);
                }
                enqueue(tgt, e.depth + 1);
                junk = 0;
                break;
            }
            case I_B: {
                uint64_t tgt = e.addr + i + branch_off(w);
                enqueue(tgt, e.depth + 1);
                goto next_node;
            }
            case I_RET: case I_RETAB: goto next_node;
            default: break;
            }
        }
    next_node:;
    }
}

// ---------------------------------------------------------------- report ---
typedef struct { void *h; uint64_t slide; } FW;
static void *fw_sym(FW *fw, const char *n) { (void)fw; return dlsym(fw->h, n); }

static void verdict(const Pin *pr, bool found, int64_t got) {
    findings++;
    if (!found) {
        misses++;
        printf("  MISS  %-18s pinned %-#llx\n", pr->name, (unsigned long long)pr->val);
    } else if (got != (int64_t)pr->val) {
        mismatches++;
        printf("  DRIFT %-18s pinned %-#llx framework %#llx\n", pr->name,
               (unsigned long long)pr->val, (unsigned long long)got);
    } else {
        printf("  MATCH %-18s %-#llx\n", pr->name, (unsigned long long)pr->val);
    }
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
// Whole-__TEXT presence scan: catches offsets referenced through indirect
// calls (function pointers / vtables) the call-graph walk cannot follow.
static Offsets g_global; static bool g_global_built;
static NumHit g_movz[MAXHITS]; static unsigned g_nmovz;
static void ensure_global(void) {
    if (g_global_built) return;
    g_global_built = true;
    const uint8_t *code = (const uint8_t *)g_text_lo;
    size_t words = (g_text_hi - g_text_lo) / 4;
    for (size_t i = 0; i < words; i++) {
        uint32_t w = *(const uint32_t *)(code + i * 4);
        Insn in = decode(w);
        if ((in.kind == I_LDR || in.kind == I_STR || in.kind == I_LDUR || in.kind == I_STUR) &&
            in.imm >= 0 && in.imm <= 0x8000 && (in.imm % in.size) == 0)
            offs_add(&g_global, (uint64_t)in.imm, in.size);
        if ((w & 0x7f800000) == 0x52800000 && ((w >> 29) & 3) == 2 &&   // MOVZ
            ((w >> 21) & 3) == 0) {                                     // hw=0
            uint64_t v = (w >> 5) & 0xffff;
            unsigned k;
            for (k = 0; k < g_nmovz; k++) if (g_movz[k].val == v) break;
            if (k == g_nmovz && g_nmovz < MAXHITS) g_movz[g_nmovz++].val = v;
        }
    }
}

static void rule_offsets(FW *fw, const char *sym, const Pin *pins, unsigned npins,
                         const char *tag, bool dump_all,
                         const char *const *extra = 0, unsigned nextra = 0) {
    void *a = fw_sym(fw, sym);
    printf("[%s] %s%s\n", tag, sym, a ? "" : "  (missing on this OS)");
    Offsets t = {};
    unsigned reached = 0;
    if (a) { walk_from((uint64_t)a, &t); reached++; }
    for (unsigned s = 0; s < nextra; s++) {
        void *xa = fw_sym(fw, extra[s]);
        if (xa) { walk_from((uint64_t)xa, &t); reached++; }
    }
    printf("  (%u seed functions reached)\n", reached);
    for (unsigned i = 0; i < npins; i++) {
        bool f = false;
        for (unsigned k = 0; k < t.n; k++) if (t.offs[k] == pins[i].val) { f = true; break; }
        bool fg = f;
        if (!f) {
            ensure_global();
            for (unsigned k = 0; k < g_global.n; k++)
                if (g_global.offs[k] == pins[i].val) { fg = true; break; }
            for (unsigned k = 0; k < g_nmovz && !fg; k++)
                if (g_movz[k].val == pins[i].val) { fg = true; break; }
        }
        if (f) verdict(&pins[i], true, (int64_t)pins[i].val);
        else if (fg) {
            findings++;
            indirects++;
            printf("  INDIR %-18s %-#llx (indirect reference)\n",
                   pins[i].name, (unsigned long long)pins[i].val);
        } else verdict(&pins[i], false, 0);
    }
    if (dump_all && t.n) {
        qsort(t.offs, t.n, sizeof(uint64_t), cmp_u64);
        printf("  sites(%u):", t.n);
        for (unsigned k = 0; k < t.n; k++) printf(" %#llx", (unsigned long long)t.offs[k]);
        printf("\n");
    }
    printf("\n");
}

// ------------------------------------------------------------------ main ---
int main(void) {
    FW fw = {};
    const char *paths[] = {
        "/System/Library/Frameworks/Hypervisor.framework/Hypervisor",
        "/System/Library/Frameworks/Hypervisor.framework/Versions/A/Hypervisor",
    };
    for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        fw.h = dlopen(paths[i], RTLD_LAZY | RTLD_LOCAL);
        if (fw.h) break;
    }
    if (!fw.h) { fprintf(stderr, "dlopen Hypervisor.framework: %s\n", dlerror()); return 2; }
    void *any = fw_sym(&fw, "hv_vm_create");
    Dl_info info;
    if (!any || !dladdr(any, &info)) { fprintf(stderr, "dladdr failed\n"); return 2; }
    uint64_t fbase = (uint64_t)info.dli_fbase;
    struct mach_header_64 *hdr = (struct mach_header_64 *)fbase;
    uint8_t *cmds = (uint8_t *)(fbase + sizeof(*hdr));
    uint64_t text_vm = 0, text_sz = 0;
    for (uint32_t i = 0, o = 0; i < hdr->ncmds; i++) {
        struct load_command *lc = (struct load_command *)(cmds + o);
        if (lc->cmd == LC_SEGMENT_64) {
            struct segment_command_64 *sg = (struct segment_command_64 *)lc;
            if (!strcmp(sg->segname, "__TEXT")) { text_vm = sg->vmaddr; text_sz = sg->vmsize; }
        }
        o += lc->cmdsize;
    }
    fw.slide = fbase - text_vm;
    g_text_lo = text_vm + fw.slide;
    g_text_hi = g_text_lo + text_sz;
    g_hvtrap = (uint64_t)dlsym(RTLD_DEFAULT, "hv_trap");
    printf("offset_probe: Hypervisor.framework at %#llx slide %#llx __TEXT [%#llx..%#llx) hv_trap@%#llx\n\n",
           (unsigned long long)fbase, (unsigned long long)fw.slide,
           (unsigned long long)g_text_lo, (unsigned long long)g_text_hi,
           (unsigned long long)g_hvtrap);

    printf("== hv_vm_config layout (stores inside setters, BFS depth %d) ==\n", MAX_DEPTH);
    {
        const char *setters[] = { "_hv_vm_config_set_vhe_enabled",
            "hv_vm_config_set_el2_enabled", "_hv_vm_config_set_isa",
            "hv_vm_config_set_ipa_granule", "hv_vm_config_set_ipa_size" };
        g_nstrb = 0;
        Offsets t = {};
        for (unsigned s = 0; s < sizeof(setters)/sizeof(setters[0]); s++) {
            void *a = fw_sym(&fw, setters[s]);
            if (a) walk_from((uint64_t)a, &t);
        }
        printf("  byte stores seen:");
        for (unsigned k = 0; k < g_nstrb; k++) printf(" %#llx", (unsigned long long)g_strb[k].val);
        printf("\n");
        for (unsigned i = 0; i < sizeof(pins_config)/sizeof(pins_config[0]); i++) {
            bool f = false;
            for (unsigned k = 0; k < t.n; k++) if (t.offs[k] == pins_config[i].val) { f = true; break; }
            verdict(&pins_config[i], f, (int64_t)pins_config[i].val);
        }
    }
    static const char *seeds_ro[] = {
        "__hv_vcpu_get_context", "hv_vcpu_get_exec_time", "hv_vcpus_exit",
        "hv_vcpu_get_pending_interrupt", "hv_vcpu_set_pending_interrupt",
    };
    static const char *seeds_traps[] = {
        "hv_vcpus_exit", "hv_vcpu_invalidate_tlb", "__hv_vm_stage1_tlb_op",
        "__hv_vcpu_set_control_field", "__hv_vcpu_get_control_field",
        "__hv_vcpu_amx_prepare", "__hv_vm_space_create", "__hv_vm_map_space",
        "hv_vcpu_set_sme_state", "hv_vcpu_get_sme_z_reg",
    };
    printf("\n== register block (hv_vcpu_get_reg / set_reg) ==\n");
    rule_offsets(&fw, "hv_vcpu_get_reg", pins_regs, sizeof(pins_regs)/sizeof(pins_regs[0]), "regs", false);
    rule_offsets(&fw, "hv_vcpu_set_reg", pins_regs, sizeof(pins_regs)/sizeof(pins_regs[0]), "regs", false);
    printf("== RO page anchors (hv_vcpu_run path) ==\n");
    rule_offsets(&fw, "hv_vcpu_run", pins_ro, sizeof(pins_ro)/sizeof(pins_ro[0]), "ro", false,
                 seeds_ro, sizeof(seeds_ro)/sizeof(seeds_ro[0]));
    printf("== context surface (hv_vcpu_get_sys_reg) ==\n");
    rule_offsets(&fw, "hv_vcpu_get_sys_reg", pins_ro, 0, "ctx", true);
    printf("== kernel trap numbers (movz w0,N before hv_trap/svc) ==\n");
    {
        const char *entry[] = { "hv_vm_create","hv_vm_map","hv_vm_unmap","hv_vm_protect",
            "hv_vcpu_create","hv_vcpu_destroy","hv_vcpu_run","__hv_vcpu_get_context",
            "hv_vm_get_max_vcpu_count","hv_vm_config_get_max_ipa_size","hv_vm_destroy" };
        Offsets sink = {};
        g_nw0 = 0; g_ntraps = 0;
        for (unsigned e = 0; e < sizeof(entry)/sizeof(entry[0]); e++) {
            void *a = fw_sym(&fw, entry[e]);
            if (a) walk_from((uint64_t)a, &sink);
        }
        for (unsigned s = 0; s < sizeof(seeds_traps)/sizeof(seeds_traps[0]); s++) {
            void *a = fw_sym(&fw, seeds_traps[s]);
            if (a) walk_from((uint64_t)a, &sink);
        }
        printf("  direct svc sites: %u; small movz w0 immediates:", g_ntraps);
        for (unsigned k = 0; k < g_nw0 && k < 64; k++)
            printf(" %llu", (unsigned long long)g_w0[k].val);
        printf("\n");
        for (unsigned t = 0; t < sizeof(pins_traps)/sizeof(pins_traps[0]); t++) {
            bool f = false;
            for (unsigned k = 0; k < g_nw0; k++) if (g_w0[k].val == pins_traps[t].val) { f = true; break; }
            for (unsigned k = 0; k < g_ntraps && !f; k++) if (g_traps[k].val == pins_traps[t].val) { f = true; break; }
            verdict(&pins_traps[t], f, (int64_t)pins_traps[t].val);
        }
    }
    printf("\n%d checks: %d direct match, %d indirect, %d drift, %d missing\n",
           findings, findings - mismatches - misses - indirects,
           indirects, mismatches, misses);
    return (mismatches || misses) ? 1 : 0;
}