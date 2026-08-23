
import re
P="/Users/libr/Desktop/life/vphone-qemu/.scratch/hvf-ida/OpenHypervisor"

def rw(path, fn):
    t=open(path).read(); t2=fn(t)
    assert t2 is not None, path
    open(path,"w").write(t2)

# ---- internal header: shared vcpu accessor + gic model sizes
rw(P+"/src/ohv_internal.h", lambda t:
   t.replace("struct VcpuSlot {", """// Userspace GIC model geometry (our own; kernel sees only context writes).
#define GIC_DISTRIBUTOR_REG_COUNT    32
#define GIC_REDISTRIBUTOR_REG_COUNT  16
#define GIC_ICC_REG_COUNT            24
#define GIC_ICH_REG_COUNT            20
#define GIC_ICV_REG_COUNT            24
#define GIC_MSI_REG_COUNT            8

struct VcpuSlot {""")
     .replace("hv_return_t require_vm();", "VcpuSlot *owned_vcpu(hv_vcpu_t id);\nhv_return_t require_vm();"))

# ---- vcpu.cpp: share ownership helper
rw(P+"/src/ohv_vcpu.cpp", lambda t:
   t.replace("// ------------------------------------------------------------- ownership --\nstatic VcpuSlot *owned_vcpu(hv_vcpu_t id) {",
             "// ------------------------------------------------------------- ownership --\nVcpuSlot *owned_vcpu(hv_vcpu_t id) {"))

# ---- misc.cpp: namespace-qualify helper + granule signatures
def fix_misc(t):
    t=t.replace("__hv_vcpu_get_context","ohv::owned_vcpu_marker_noop")  # placeholder guard
    return t
rw(P+"/src/ohv_misc.cpp", lambda t:
   t.replace("VcpuSlot *s = owned_vcpu(id);", "VcpuSlot *s = ohv::owned_vcpu(id);")
    .replace('extern "C" hv_return_t hv_vcpu_config_get_ccsidr_el1_sys_reg_values', 'extern "C" hv_return_t hv_vcpu_config_get_ccsidr_el1_sys_reg_values'))

# ---- vm.cpp: granule uses hv_ipa_granule_t (uint32) like SDK
rw(P+"/src/ohv_vm.cpp", lambda t:
   t.replace("hv_return_t hv_vm_config_get_default_ipa_granule(uint64_t *granule)", "hv_return_t hv_vm_config_get_default_ipa_granule(hv_ipa_granule_t *granule)")
    .replace("hv_return_t hv_vm_config_get_ipa_granule(hv_vm_config_t c, uint64_t *g)", "hv_return_t hv_vm_config_get_ipa_granule(hv_vm_config_t c, hv_ipa_granule_t *g)")
    .replace("*g = c->granule ? c->granule : 0x4000;", "*g = c->granule ? c->granule : 0x4000;")
    .replace("hv_return_t hv_vm_config_set_ipa_granule(hv_vm_config_t c, uint64_t g)", "hv_return_t hv_vm_config_set_ipa_granule(hv_vm_config_t c, hv_ipa_granule_t g)")
    .replace("c->granule = (uint32_t)g;", "c->granule = g;"))

# ---- generator: sme_state struct + vcpuid typedef
rw(P+"/tools/gen_compat.py", lambda t:
   t.replace("typedef uint32_t hv_sme_p_reg_t;\n", "typedef uint32_t hv_sme_p_reg_t;\ntypedef uint32_t hv_vcpuid_t;\n")
    .replace('''typedef uint32_t hv_gic_msi_reg_t;
''', '''typedef uint32_t hv_gic_msi_reg_t;
typedef struct {
    bool streaming_sve_mode_enabled;
    bool za_storage_enabled;
} hv_vcpu_sme_state_t;
'''))

# ---- private compat surface appended to protos by generator
rw(P+"/tools/gen_compat.py", lambda t:
   t.replace('''    f.write("\\n#ifdef __cplusplus\\n}\\n#endif\\n#endif\\n")

print("blocks:", len(blocks_out), "names:", len(seen_names), "protos:", len(seen))''',
'''    f.write("""
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
hv_vm_space_config_t __hv_vm_space_config_create(void);
hv_return_t __hv_vm_space_config_set_ipa_base(hv_vm_space_config_t config, hv_ipa_t base);
hv_return_t __hv_vm_space_config_get_ipa_base(hv_vm_space_config_t config, hv_ipa_t *base);
hv_return_t __hv_vm_space_config_set_ipa_size(hv_vm_space_config_t config, hv_ipa_t size);
hv_return_t __hv_vm_space_config_get_ipa_size(hv_vm_space_config_t config, hv_ipa_t *size);
hv_return_t __hv_vm_space_config_set_ipa_granule(hv_vm_space_config_t config, hv_ipa_t granule);
hv_return_t __hv_vm_space_config_get_ipa_granule(hv_vm_space_config_t config, hv_ipa_t *granule);
typedef uint64_t hv_vm_space_t_;
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

\\n#ifdef __cplusplus\\n}\\n#endif\\n#endif\\n")

print("blocks:", len(blocks_out), "names:", len(seen_names), "protos:", len(seen))'''))
print("patched all")
