# gen_compat.py - emit hv_compat_enums.h and hv_compat_protos.h from SDK headers.
import re, os

D = "/Users/libr/Desktop/life/vphone-qemu/.scratch/hvf-ida/sdk-headers"
OUT = "/Users/libr/Desktop/life/vphone-qemu/.scratch/hvf-ida/OpenHypervisor/include/openhyp"
ORDER = ["hv_base.h","hv_error.h","hv_types.h","hv_vm_types.h","hv_vm_allocate.h","hv_vm_config.h",
         "hv_vcpu_types.h","hv_vcpu_config.h","hv_intr.h","hv_sme_config.h","hv_gic_types.h",
         "hv_gic_config.h","hv_gic_parameters.h","hv_gic_state.h","hv_gic.h","hv_vm.h","hv_vcpu.h","hv.h"]

def clean(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)

X86_MARKERS = ("err_common_hypervisor","HV_VM_EXITINFO","APIC","kHV_ION","hv_ion",
               "atpic","HV_MSR_","hv_msr","MITIGATION")

enum_blocks, defines = [], []
protos = []
for fn in ORDER:
    p = os.path.join(D, fn)
    if not os.path.exists(p):
        continue
    t = clean(open(p).read())
    for m in re.finditer(r"OS_ENUM\s*\(", t):
        # balanced-paren scan: OS_ENUM(name, type, A, B, ...) has no braces
        i = t.index("(", m.end() - 1)
        depth = 0; j = i
        while j < len(t):
            if t[j] == "(": depth += 1
            elif t[j] == ")":
                depth -= 1
                if depth == 0: break
            j += 1
        inner = t[i+1:j]
        # strip leading "name, type,"
        inner = re.sub(r"^\s*\w+\s*,\s*[^,]+,", "", inner)
        enum_blocks.append(inner)
    for m in re.finditer(r"\benum\s+(?:\w+\s+)?\{", t):
        start = m.end() - 1; depth = 0; i = start
        while i < len(t):
            if t[i] == "{": depth += 1
            elif t[i] == "}":
                depth -= 1
                if depth == 0: break
            i += 1
        body = t[start+1:i]
        if "HV_" in body or "GIC_" in body or "hv_" in body:
            enum_blocks.append(body)
    for m in re.finditer(r"^#define\s+((?:HV_|GIC_)[A-Za-z0-9_]+)\s+(.+?)\s*$", t, re.M):
        defines.append((m.group(1), m.group(2)))
    for m in re.finditer(r"(?:extern\s+)?([A-Za-z_][A-Za-z0-9_ \t]*?[ \t]*\**?)\b(hv_[a-z0-9_]+)\s*\(([^;()]*)\)\s*;", t, re.S):
        ret = " ".join(m.group(1).split())
        name, args = m.group(2), " ".join(m.group(3).split())
        args = args.replace("_Nullable", "").replace("_Nonnull", "")
        protos.append((ret, name, args))

def entries(body):
    body = re.sub(r"API_AVAILABLE\s*\([^)]*\)", "", body)
    out = []
    for part in body.split(","):
        part = " ".join(part.split())
        if part and re.match(r"^[A-Za-z_][A-Za-z0-9_]*(\s*=.*)?$", part) \
           and not re.match(r"^(u?int[0-9]+_t|char|bool|void|float|double)$", part):
            out.append(part)
    return out

seen_names, blocks_out = set(), []
for body in enum_blocks:
    es = entries(body)
    if not es: continue
    joined = " ".join(es)
    if any(mk in joined for mk in X86_MARKERS): continue
    new = [e for e in es if e.split("=")[0].strip() not in seen_names]
    if not new: continue
    for e in new: seen_names.add(e.split("=")[0].strip())
    blocks_out.append(new)

with open(OUT + "/hv_compat_enums.h", "w") as f:
    f.write("// hv_compat_enums.h - OpenHypervisor generated interface constants.\n")
    f.write("#ifndef OHV_HV_COMPAT_ENUMS_H\n#define OHV_HV_COMPAT_ENUMS_H\n\n#include <stdint.h>\n#include <stddef.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    dd = set()
    for es in blocks_out:
        f.write("enum {\n")
        for e in es:
            f.write("    %s,\n" % e)
        f.write("};\n\n")
    for d, v in defines:
        if d in dd: continue
        dd.add(d)
        f.write("#ifndef %s\n#define %s %s\n#endif\n" % (d, d, v))
    f.write("\n#ifdef __cplusplus\n}\n#endif\n#endif\n")

PRIVATE = r"""
// ---- private framework surface (leading underscores) --------------------
hv_return_t __hv_capability(hv_capability_t cap, uint64_t *value);
hv_return_t _hv_vm_config_set_isa(hv_vm_config_t config, uint32_t isa);
hv_return_t _hv_vm_config_get_isa(hv_vm_config_t config, uint32_t *isa);
hv_return_t _hv_vm_config_set_vhe_enabled(hv_vm_config_t config, bool enabled);
hv_return_t _hv_vm_config_get_vhe_enabled(hv_vm_config_t config, bool *enabled);
hv_return_t __hv_vcpu_config_get_vmkey(hv_vcpu_config_t config, uint64_t *key);
hv_return_t __hv_vcpu_config_set_vmkey(hv_vcpu_config_t config, uint64_t key);
hv_return_t __hv_vcpu_config_get_fgt_enabled(hv_vcpu_config_t config, bool *enabled);
hv_return_t __hv_vcpu_config_set_fgt_enabled(hv_vcpu_config_t config, bool enabled);
hv_return_t __hv_vcpu_config_get_tlbi_workaround_enabled(hv_vcpu_config_t config, bool *enabled);
hv_return_t __hv_vcpu_config_set_tlbi_workaround_enabled(hv_vcpu_config_t config, bool enabled);
typedef struct hv_vm_space_config_s *hv_vm_space_config_t;
hv_vm_space_config_t __hv_vm_space_config_create(void);
hv_return_t __hv_vm_space_config_set_ipa_base(hv_vm_space_config_t config, hv_ipa_t base);
hv_return_t __hv_vm_space_config_get_ipa_base(hv_vm_space_config_t config, hv_ipa_t *base);
hv_return_t __hv_vm_space_config_set_ipa_size(hv_vm_space_config_t config, hv_ipa_t size);
hv_return_t __hv_vm_space_config_get_ipa_size(hv_vm_space_config_t config, hv_ipa_t *size);
hv_return_t __hv_vm_space_config_set_ipa_granule(hv_vm_space_config_t config, hv_ipa_t granule);
hv_return_t __hv_vm_space_config_get_ipa_granule(hv_vm_space_config_t config, hv_ipa_t *granule);
hv_return_t __hv_vm_space_create(hv_vm_space_config_t config, hv_vm_space_t *space);
hv_return_t __hv_vm_space_destroy(hv_vm_space_t space);
hv_return_t __hv_vm_map_space(hv_vm_space_t space, void *addr, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags);
hv_return_t __hv_vm_unmap_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size);
hv_return_t __hv_vm_protect_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags);
hv_return_t __hv_vm_stage1_tlb_op(hv_vm_space_t space, uint64_t op, uint64_t param);
hv_return_t __hv_vcpu_get_control_field(hv_vcpu_t vcpu, _hv_control_field_t field, uint64_t *value);
hv_return_t __hv_vcpu_set_control_field(hv_vcpu_t vcpu, _hv_control_field_t field, uint64_t value);
hv_return_t __hv_vcpu_get_ext_reg(hv_vcpu_t vcpu, _hv_ext_reg_t reg, uint64_t *value);
hv_return_t __hv_vcpu_get_context(hv_vcpu_t vcpu, void **context);
hv_return_t __hv_vcpu_amx_prepare(hv_vcpu_t vcpu);
hv_return_t __hv_vcpu_amx_query_active_context(hv_vcpu_t vcpu, bool *active);
hv_return_t __hv_vcpu_get_amx_x_space(hv_vcpu_t vcpu, uint8_t out[8][64]);
hv_return_t __hv_vcpu_set_amx_x_space(hv_vcpu_t vcpu, const uint8_t in[8][64]);
hv_return_t __hv_vcpu_get_amx_y_space(hv_vcpu_t vcpu, uint8_t out[8][64]);
hv_return_t __hv_vcpu_set_amx_y_space(hv_vcpu_t vcpu, const uint8_t in[8][64]);
hv_return_t __hv_vcpu_get_amx_z_space(hv_vcpu_t vcpu, uint8_t out[64][64]);
hv_return_t __hv_vcpu_set_amx_z_space(hv_vcpu_t vcpu, const uint8_t in[64][64]);
hv_return_t __hv_vcpu_get_amx_state_t_el1(hv_vcpu_t vcpu, uint64_t *value);
hv_return_t __hv_vcpu_set_amx_state_t_el1(hv_vcpu_t vcpu, uint64_t value);
"""

EXTRA_TYPES = """typedef uint32_t hv_vm_space_t;
typedef uint64_t hv_capability_t;
typedef uint32_t hv_ipa_granule_t;
typedef uint64_t hv_allocate_flags_t;
typedef uint32_t hv_feature_reg_t;
typedef uint32_t hv_cache_type_t;
typedef uint32_t hv_interrupt_type_t;
typedef uint32_t hv_tlbi_op_t;
typedef uint32_t hv_gic_intid_t;
typedef uint32_t _hv_control_field_t;
typedef uint32_t _hv_ext_reg_t;
typedef uint32_t hv_gic_distributor_reg_t;
typedef uint32_t hv_gic_redistributor_reg_t;
typedef uint32_t hv_gic_icc_reg_t;
typedef uint32_t hv_gic_ich_reg_t;
typedef uint32_t hv_gic_icv_reg_t;
typedef uint32_t hv_gic_msi_reg_t;
typedef uint32_t hv_exit_reason_t;
typedef uint32_t hv_reg_t;
typedef uint32_t hv_simd_fp_reg_t;
typedef uint32_t hv_sys_reg_t;
typedef uint32_t hv_sme_z_reg_t;
typedef uint32_t hv_sme_p_reg_t;
typedef uint32_t hv_vcpuid_t;
typedef struct {
    bool streaming_sve_mode_enabled;
    bool za_storage_enabled;
} hv_vcpu_sme_state_t;
"""

with open(OUT + "/hv_compat_protos.h", "w") as f:
    f.write("// hv_compat_protos.h - OpenHypervisor generated C interface.\n")
    f.write("#ifndef OHV_HV_COMPAT_PROTOS_H\n#define OHV_HV_COMPAT_PROTOS_H\n\n")
    f.write("#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\n#include \"hv_compat_enums.h\"\n\n")
    f.write("#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n")
    base = """typedef struct hv_vm_config_s *hv_vm_config_t;
typedef struct hv_vcpu_config_s *hv_vcpu_config_t;
typedef struct hv_gic_config_s *hv_gic_config_t;
typedef struct hv_gic_state_s *hv_gic_state_t;
typedef uint64_t hv_vcpu_t;
typedef uint64_t hv_ipa_t;
typedef uint64_t hv_memory_flags_t;
typedef uint64_t hv_exception_syndrome_t;
typedef uint64_t hv_exception_address_t;
typedef int hv_return_t;
typedef __attribute__((ext_vector_type(16))) uint8_t hv_simd_fp_uchar16_t;
typedef __attribute__((ext_vector_type(64))) uint8_t hv_sme_zt0_uchar64_t;
"""
    f.write(base + EXTRA_TYPES)
    f.write("""
typedef struct {
    hv_exception_syndrome_t syndrome;
    hv_exception_address_t virtual_address;
    hv_ipa_t physical_address;
} hv_vcpu_exit_exception_t;
typedef struct {
    hv_exit_reason_t reason;
    hv_vcpu_exit_exception_t exception;
} hv_vcpu_exit_t;

""")
    best = {}
    for ret, name, args in protos:
        cand = (ret, name, args)
        if name not in best:
            best[name] = cand
        else:
            old = best[name]
            # prefer the arm64-flavoured overload
            score = lambda c: ("hv_vcpu_t" in c[2]) + ("hv_ipa" in c[2])
            if score(cand) > score(old):
                best[name] = cand
    for name in sorted(best):
        ret, _, args = best[name]
        f.write("%s %s(%s);\n" % (ret, name, args))
    seen = best
    f.write(PRIVATE)
    f.write("\n#ifdef __cplusplus\n}\n#endif\n#endif\n")

print("blocks:", len(blocks_out), "names:", len(seen_names), "protos:", len(seen))