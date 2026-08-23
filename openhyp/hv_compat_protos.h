// hv_compat_protos.h - OpenHypervisor generated C interface.
#ifndef OHV_HV_COMPAT_PROTOS_H
#define OHV_HV_COMPAT_PROTOS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hv_compat_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hv_vm_config_s *hv_vm_config_t;
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
typedef uint32_t hv_vm_space_t;
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

typedef struct {
    hv_exception_syndrome_t syndrome;
    hv_exception_address_t virtual_address;
    hv_ipa_t physical_address;
} hv_vcpu_exit_exception_t;
typedef struct {
    hv_exit_reason_t reason;
    hv_vcpu_exit_exception_t exception;
} hv_vcpu_exit_t;

hv_gic_config_t hv_gic_config_create(void);
hv_return_t hv_gic_config_set_distributor_base(hv_gic_config_t config, hv_ipa_t distributor_base_address);
hv_return_t hv_gic_config_set_msi_interrupt_range(hv_gic_config_t config, uint32_t msi_intid_base, uint32_t msi_intid_count);
hv_return_t hv_gic_config_set_msi_region_base(hv_gic_config_t config, hv_ipa_t msi_region_base_address);
hv_return_t hv_gic_config_set_redistributor_base(hv_gic_config_t config, hv_ipa_t redistributor_base_address);
hv_return_t hv_gic_create(hv_gic_config_t gic_config);
hv_return_t hv_gic_get_distributor_base_alignment(size_t *distributor_base_alignment);
hv_return_t hv_gic_get_distributor_reg(hv_gic_distributor_reg_t reg, uint64_t *value);
hv_return_t hv_gic_get_distributor_size(size_t *distributor_size);
hv_return_t hv_gic_get_icc_reg(hv_vcpu_t vcpu, hv_gic_icc_reg_t reg, uint64_t *value);
hv_return_t hv_gic_get_ich_reg(hv_vcpu_t vcpu, hv_gic_ich_reg_t reg, uint64_t* value);
hv_return_t hv_gic_get_icv_reg(hv_vcpu_t vcpu, hv_gic_icv_reg_t reg, uint64_t* value);
hv_return_t hv_gic_get_intid(hv_gic_intid_t interrupt, uint32_t *intid);
hv_return_t hv_gic_get_msi_reg(hv_gic_msi_reg_t reg, uint64_t *value);
hv_return_t hv_gic_get_msi_region_base_alignment(size_t *msi_region_base_alignment);
hv_return_t hv_gic_get_msi_region_size(size_t *msi_region_size);
hv_return_t hv_gic_get_redistributor_base(hv_vcpu_t vcpu, hv_ipa_t *redistributor_base_address);
hv_return_t hv_gic_get_redistributor_base_alignment(size_t *redistributor_base_alignment);
hv_return_t hv_gic_get_redistributor_reg(hv_vcpu_t vcpu, hv_gic_redistributor_reg_t reg, uint64_t *value);
hv_return_t hv_gic_get_redistributor_region_size(size_t *redistributor_region_size);
hv_return_t hv_gic_get_redistributor_size(size_t *redistributor_size);
hv_return_t hv_gic_get_spi_interrupt_range(uint32_t *spi_intid_base, uint32_t *spi_intid_count);
hv_return_t hv_gic_reset();
hv_return_t hv_gic_send_msi(hv_ipa_t address, uint32_t intid);
hv_return_t hv_gic_set_distributor_reg(hv_gic_distributor_reg_t reg, uint64_t value);
hv_return_t hv_gic_set_icc_reg(hv_vcpu_t vcpu, hv_gic_icc_reg_t reg, uint64_t value);
hv_return_t hv_gic_set_ich_reg(hv_vcpu_t vcpu, hv_gic_ich_reg_t reg, uint64_t value);
hv_return_t hv_gic_set_icv_reg(hv_vcpu_t vcpu, hv_gic_icv_reg_t reg, uint64_t value);
hv_return_t hv_gic_set_msi_reg(hv_gic_msi_reg_t reg, uint64_t value);
hv_return_t hv_gic_set_redistributor_reg(hv_vcpu_t vcpu, hv_gic_redistributor_reg_t reg, uint64_t value);
hv_return_t hv_gic_set_spi(uint32_t intid, bool level);
hv_return_t hv_gic_set_state(const void *gic_state_data, size_t gic_state_size);
hv_gic_state_t hv_gic_state_create(void);
hv_return_t hv_gic_state_get_data(hv_gic_state_t state, void *gic_state_data);
hv_return_t hv_gic_state_get_size(hv_gic_state_t state, size_t *gic_state_size);
hv_return_t hv_sme_config_get_max_svl_bytes(size_t *value);
hv_vcpu_config_t hv_vcpu_config_create(void);
hv_return_t hv_vcpu_config_get_ccsidr_el1_sys_reg_values(hv_vcpu_config_t config, hv_cache_type_t cache_type, uint64_t values[ 8]);
hv_return_t hv_vcpu_config_get_feature_reg(hv_vcpu_config_t config, hv_feature_reg_t feature_reg, uint64_t *value);
hv_return_t hv_vcpu_create(hv_vcpu_t *vcpu, hv_vcpu_exit_t *  *  exit, hv_vcpu_config_t  config);
hv_return_t hv_vcpu_destroy(hv_vcpu_t vcpu);
hv_return_t hv_vcpu_get_exec_time(hv_vcpu_t vcpu, uint64_t *time);
hv_return_t hv_vcpu_get_pending_interrupt(hv_vcpu_t vcpu, hv_interrupt_type_t type, bool *pending);
hv_return_t hv_vcpu_get_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t *value);
hv_return_t hv_vcpu_get_serror(hv_vcpu_t vcpu, bool *pending);
hv_return_t hv_vcpu_get_simd_fp_reg(hv_vcpu_t vcpu, hv_simd_fp_reg_t reg, hv_simd_fp_uchar16_t *value);
hv_return_t hv_vcpu_get_sme_p_reg(hv_vcpu_t vcpu, hv_sme_p_reg_t reg, uint8_t *value, size_t length);
hv_return_t hv_vcpu_get_sme_state(hv_vcpu_t vcpu, hv_vcpu_sme_state_t *sme_state);
hv_return_t hv_vcpu_get_sme_z_reg(hv_vcpu_t vcpu, hv_sme_z_reg_t reg, uint8_t *value, size_t length);
hv_return_t hv_vcpu_get_sme_za_reg(hv_vcpu_t vcpu, uint8_t *value, size_t length);
hv_return_t hv_vcpu_get_sme_zt0_reg(hv_vcpu_t vcpu, hv_sme_zt0_uchar64_t *value);
hv_return_t hv_vcpu_get_sys_reg(hv_vcpu_t vcpu, hv_sys_reg_t reg, uint64_t *value);
hv_return_t hv_vcpu_get_trap_debug_exceptions(hv_vcpu_t vcpu, bool *value);
hv_return_t hv_vcpu_get_trap_debug_reg_accesses(hv_vcpu_t vcpu, bool *value);
hv_return_t hv_vcpu_get_vtimer_mask(hv_vcpu_t vcpu, bool *vtimer_is_masked);
hv_return_t hv_vcpu_get_vtimer_offset(hv_vcpu_t vcpu, uint64_t *vtimer_offset);
hv_return_t hv_vcpu_get_wait_for_interrupt_time(hv_vcpu_t vcpu, uint64_t *time);
hv_return_t hv_vcpu_invalidate_tlb(hv_vcpu_t vcpu, hv_tlbi_op_t op, uint64_t param);
hv_return_t hv_vcpu_run(hv_vcpu_t vcpu);
hv_return_t hv_vcpu_set_pending_interrupt(hv_vcpu_t vcpu, hv_interrupt_type_t type, bool pending);
hv_return_t hv_vcpu_set_reg(hv_vcpu_t vcpu, hv_reg_t reg, uint64_t value);
hv_return_t hv_vcpu_set_serror(hv_vcpu_t vcpu, bool pending);
hv_return_t hv_vcpu_set_simd_fp_reg(hv_vcpu_t vcpu, hv_simd_fp_reg_t reg, hv_simd_fp_uchar16_t value);
hv_return_t hv_vcpu_set_sme_p_reg(hv_vcpu_t vcpu, hv_sme_p_reg_t reg, const uint8_t *value, size_t length);
hv_return_t hv_vcpu_set_sme_state(hv_vcpu_t vcpu, const hv_vcpu_sme_state_t *sme_state);
hv_return_t hv_vcpu_set_sme_z_reg(hv_vcpu_t vcpu, hv_sme_z_reg_t reg, const uint8_t *value, size_t length);
hv_return_t hv_vcpu_set_sme_za_reg(hv_vcpu_t vcpu, const uint8_t *value, size_t length);
hv_return_t hv_vcpu_set_sme_zt0_reg(hv_vcpu_t vcpu, const hv_sme_zt0_uchar64_t *value);
hv_return_t hv_vcpu_set_sys_reg(hv_vcpu_t vcpu, hv_sys_reg_t reg, uint64_t value);
hv_return_t hv_vcpu_set_trap_debug_exceptions(hv_vcpu_t vcpu, bool value);
hv_return_t hv_vcpu_set_trap_debug_reg_accesses(hv_vcpu_t vcpu, bool value);
hv_return_t hv_vcpu_set_vtimer_mask(hv_vcpu_t vcpu, bool vtimer_is_masked);
hv_return_t hv_vcpu_set_vtimer_offset(hv_vcpu_t vcpu, uint64_t vtimer_offset);
hv_return_t hv_vcpus_exit(hv_vcpu_t *vcpus, uint32_t vcpu_count);
hv_return_t hv_vm_allocate(void *  *  uvap, size_t size, hv_allocate_flags_t flags);
hv_return_t hv_vm_atpic_port_read(int port, uint8_t *valuep);
hv_return_t hv_vm_atpic_port_write(int port, uint8_t value);
hv_vm_config_t hv_vm_config_create(void);
hv_return_t hv_vm_config_get_default_ipa_granule(hv_ipa_granule_t *granule);
hv_return_t hv_vm_config_get_default_ipa_size(uint32_t *ipa_bit_length);
hv_return_t hv_vm_config_get_el2_enabled(hv_vm_config_t config, bool *el2_enabled);
hv_return_t hv_vm_config_get_el2_supported(bool *el2_supported);
hv_return_t hv_vm_config_get_ipa_granule(hv_vm_config_t config, hv_ipa_granule_t *granule);
hv_return_t hv_vm_config_get_ipa_size(hv_vm_config_t config, uint32_t *ipa_bit_length);
hv_return_t hv_vm_config_get_max_ipa_size(uint32_t *ipa_bit_length);
hv_return_t hv_vm_config_set_el2_enabled(hv_vm_config_t config, bool el2_enabled);
hv_return_t hv_vm_config_set_ipa_granule(hv_vm_config_t config, hv_ipa_granule_t granule);
hv_return_t hv_vm_config_set_ipa_size(hv_vm_config_t config, uint32_t ipa_bit_length);
hv_return_t hv_vm_create(hv_vm_config_t  config);
hv_return_t hv_vm_deallocate(void *uva, size_t size);
hv_return_t hv_vm_destroy(void);
hv_return_t hv_vm_get_max_vcpu_count(uint32_t *max_vcpu_count);
hv_return_t hv_vm_map(void *addr, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags);
hv_return_t hv_vm_protect(hv_ipa_t ipa, size_t size, hv_memory_flags_t flags);
hv_return_t hv_vm_unmap(hv_ipa_t ipa, size_t size);

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

#ifdef __cplusplus
}
#endif
#endif
