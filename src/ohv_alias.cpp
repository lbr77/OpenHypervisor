/*
 * ohv_alias.cpp -- the private entry points under the names the framework
 * actually exports them by.
 *
 * Every one of these is spelled with a single leading underscore in the
 * framework's export list, and this library defined them with two.  Nothing
 * calling by header notices, because the header agrees with itself; what
 * notices is a caller reaching for the private surface through dlsym, which
 * is the only way to reach it, and which is how every user of these gets to
 * them.  qemu asks for _hv_vcpu_config_set_tlbi_workaround_enabled by that
 * exact string and refuses to make a vCPU without it.
 *
 * The definitions stay where they are; these are the framework's names for
 * them.  Both spellings are exported, so nothing that already built against
 * the two-underscore names has to change.
 *
 * Generated once against the framework's export list, then kept by hand.
 */
#include "ohv_internal.h"
#include "openhyp/openhyp.hpp"

extern "C" hv_return_t __hv_vcpu_get_ext_reg(hv_vcpu_t id, _hv_ext_reg_t reg, uint64_t *value);
extern "C" hv_return_t _hv_vcpu_get_ext_reg(hv_vcpu_t id, _hv_ext_reg_t reg, uint64_t *value) { return __hv_vcpu_get_ext_reg(id, reg, value); }

extern "C" hv_return_t __hv_vcpu_config_get_vmkey(hv_vcpu_config_t c, uint64_t *k);
extern "C" hv_return_t _hv_vcpu_config_get_vmkey(hv_vcpu_config_t c, uint64_t *k) { return __hv_vcpu_config_get_vmkey(c, k); }

extern "C" hv_return_t __hv_vcpu_config_set_vmkey(hv_vcpu_config_t c, uint64_t k);
extern "C" hv_return_t _hv_vcpu_config_set_vmkey(hv_vcpu_config_t c, uint64_t k) { return __hv_vcpu_config_set_vmkey(c, k); }

extern "C" hv_return_t __hv_vcpu_config_get_fgt_enabled(hv_vcpu_config_t c, bool *v);
extern "C" hv_return_t _hv_vcpu_config_get_fgt_enabled(hv_vcpu_config_t c, bool *v) { return __hv_vcpu_config_get_fgt_enabled(c, v); }

extern "C" hv_return_t __hv_vcpu_config_set_fgt_enabled(hv_vcpu_config_t c, bool v);
extern "C" hv_return_t _hv_vcpu_config_set_fgt_enabled(hv_vcpu_config_t c, bool v) { return __hv_vcpu_config_set_fgt_enabled(c, v); }

extern "C" hv_return_t __hv_vcpu_config_get_tlbi_workaround_enabled(hv_vcpu_config_t c, bool *v);
extern "C" hv_return_t _hv_vcpu_config_get_tlbi_workaround_enabled(hv_vcpu_config_t c, bool *v) { return __hv_vcpu_config_get_tlbi_workaround_enabled(c, v); }

extern "C" hv_return_t __hv_vcpu_config_set_tlbi_workaround_enabled(hv_vcpu_config_t c, bool v);
extern "C" hv_return_t _hv_vcpu_config_set_tlbi_workaround_enabled(hv_vcpu_config_t c, bool v) { return __hv_vcpu_config_set_tlbi_workaround_enabled(c, v); }

extern "C" hv_return_t __hv_vcpu_amx_prepare(hv_vcpu_t id);
extern "C" hv_return_t _hv_vcpu_amx_prepare(hv_vcpu_t id) { return __hv_vcpu_amx_prepare(id); }

extern "C" hv_return_t __hv_vcpu_amx_query_active_context(hv_vcpu_t id, bool *active);
extern "C" hv_return_t _hv_vcpu_amx_query_active_context(hv_vcpu_t id, bool *active) { return __hv_vcpu_amx_query_active_context(id, active); }

extern "C" hv_return_t __hv_vcpu_get_amx_x_space(hv_vcpu_t id, uint8_t out[8][64]);
extern "C" hv_return_t _hv_vcpu_get_amx_x_space(hv_vcpu_t id, uint8_t out[8][64]) { return __hv_vcpu_get_amx_x_space(id, out); }

extern "C" hv_return_t __hv_vcpu_set_amx_x_space(hv_vcpu_t id, const uint8_t in[8][64]);
extern "C" hv_return_t _hv_vcpu_set_amx_x_space(hv_vcpu_t id, const uint8_t in[8][64]) { return __hv_vcpu_set_amx_x_space(id, in); }

extern "C" hv_return_t __hv_vcpu_get_amx_y_space(hv_vcpu_t id, uint8_t out[8][64]);
extern "C" hv_return_t _hv_vcpu_get_amx_y_space(hv_vcpu_t id, uint8_t out[8][64]) { return __hv_vcpu_get_amx_y_space(id, out); }

extern "C" hv_return_t __hv_vcpu_set_amx_y_space(hv_vcpu_t id, const uint8_t in[8][64]);
extern "C" hv_return_t _hv_vcpu_set_amx_y_space(hv_vcpu_t id, const uint8_t in[8][64]) { return __hv_vcpu_set_amx_y_space(id, in); }

extern "C" hv_return_t __hv_vcpu_get_amx_z_space(hv_vcpu_t id, uint8_t out[64][64]);
extern "C" hv_return_t _hv_vcpu_get_amx_z_space(hv_vcpu_t id, uint8_t out[64][64]) { return __hv_vcpu_get_amx_z_space(id, out); }

extern "C" hv_return_t __hv_vcpu_set_amx_z_space(hv_vcpu_t id, const uint8_t in[64][64]);
extern "C" hv_return_t _hv_vcpu_set_amx_z_space(hv_vcpu_t id, const uint8_t in[64][64]) { return __hv_vcpu_set_amx_z_space(id, in); }

extern "C" hv_return_t __hv_vcpu_get_amx_state_t_el1(hv_vcpu_t id, uint64_t *v);
extern "C" hv_return_t _hv_vcpu_get_amx_state_t_el1(hv_vcpu_t id, uint64_t *v) { return __hv_vcpu_get_amx_state_t_el1(id, v); }

extern "C" hv_return_t __hv_vcpu_set_amx_state_t_el1(hv_vcpu_t id, uint64_t v);
extern "C" hv_return_t _hv_vcpu_set_amx_state_t_el1(hv_vcpu_t id, uint64_t v) { return __hv_vcpu_set_amx_state_t_el1(id, v); }

extern "C" hv_vm_space_config_t __hv_vm_space_config_create(void);
extern "C" hv_vm_space_config_t _hv_vm_space_config_create(void) { return __hv_vm_space_config_create(); }

extern "C" hv_return_t __hv_vm_space_config_set_ipa_base(hv_vm_space_config_t c, hv_ipa_t v);
extern "C" hv_return_t _hv_vm_space_config_set_ipa_base(hv_vm_space_config_t c, hv_ipa_t v) { return __hv_vm_space_config_set_ipa_base(c, v); }

extern "C" hv_return_t __hv_vm_space_config_get_ipa_base(hv_vm_space_config_t c, hv_ipa_t *v);
extern "C" hv_return_t _hv_vm_space_config_get_ipa_base(hv_vm_space_config_t c, hv_ipa_t *v) { return __hv_vm_space_config_get_ipa_base(c, v); }

extern "C" hv_return_t __hv_vm_space_config_set_ipa_size(hv_vm_space_config_t c, hv_ipa_t v);
extern "C" hv_return_t _hv_vm_space_config_set_ipa_size(hv_vm_space_config_t c, hv_ipa_t v) { return __hv_vm_space_config_set_ipa_size(c, v); }

extern "C" hv_return_t __hv_vm_space_config_get_ipa_size(hv_vm_space_config_t c, hv_ipa_t *v);
extern "C" hv_return_t _hv_vm_space_config_get_ipa_size(hv_vm_space_config_t c, hv_ipa_t *v) { return __hv_vm_space_config_get_ipa_size(c, v); }

extern "C" hv_return_t __hv_vm_space_config_set_ipa_granule(hv_vm_space_config_t c, hv_ipa_t v);
extern "C" hv_return_t _hv_vm_space_config_set_ipa_granule(hv_vm_space_config_t c, hv_ipa_t v) { return __hv_vm_space_config_set_ipa_granule(c, v); }

extern "C" hv_return_t __hv_vm_space_config_get_ipa_granule(hv_vm_space_config_t c, hv_ipa_t *v);
extern "C" hv_return_t _hv_vm_space_config_get_ipa_granule(hv_vm_space_config_t c, hv_ipa_t *v) { return __hv_vm_space_config_get_ipa_granule(c, v); }

extern "C" hv_return_t __hv_vm_space_create(hv_vm_space_config_t c, hv_vm_space_t *out);
extern "C" hv_return_t _hv_vm_space_create(hv_vm_space_config_t c, hv_vm_space_t *out) { return __hv_vm_space_create(c, out); }

extern "C" hv_return_t __hv_vm_space_destroy(hv_vm_space_t space);
extern "C" hv_return_t _hv_vm_space_destroy(hv_vm_space_t space) { return __hv_vm_space_destroy(space); }

extern "C" hv_return_t __hv_vm_map_space(hv_vm_space_t space, void *addr, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags);
extern "C" hv_return_t _hv_vm_map_space(hv_vm_space_t space, void *addr, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) { return __hv_vm_map_space(space, addr, ipa, size, flags); }

extern "C" hv_return_t __hv_vm_unmap_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size);
extern "C" hv_return_t _hv_vm_unmap_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size) { return __hv_vm_unmap_space(space, ipa, size); }

extern "C" hv_return_t __hv_vm_protect_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags);
extern "C" hv_return_t _hv_vm_protect_space(hv_vm_space_t space, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) { return __hv_vm_protect_space(space, ipa, size, flags); }

extern "C" hv_return_t __hv_vm_stage1_tlb_op(hv_vm_space_t space, uint64_t op, uint64_t param);
extern "C" hv_return_t _hv_vm_stage1_tlb_op(hv_vm_space_t space, uint64_t op, uint64_t param) { return __hv_vm_stage1_tlb_op(space, op, param); }

extern "C" hv_return_t __hv_capability(hv_capability_t cap, uint64_t *value);
extern "C" hv_return_t _hv_capability(hv_capability_t cap, uint64_t *value) { return __hv_capability(cap, value); }
