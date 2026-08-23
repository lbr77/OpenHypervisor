// fw_el2.c - pure Apple-framework probe: isa=4 + el2 + vhe, enter guest EL2h.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <Hypervisor/Hypervisor.h>

// Private SPIs live in the dylib but not in SDK headers; declare them here.
extern int _hv_vm_config_set_isa(hv_vm_config_t, uint32_t);
extern int _hv_vm_config_set_vhe_enabled(hv_vm_config_t, int);
// GIC 配置结构体在 SDK 头文件中是透明的
#include <Hypervisor/hv_gic_types.h>

static const uint64_t IPA = 0x40000000ull;
static const size_t  SZ  = 1ull << 20;

int main(int argc, char **argv) {
  setvbuf(stdout, 0, _IONBF, 0);
  uint32_t isa = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 0) : 4;

  hv_vm_config_t cfg = hv_vm_config_create();
  int rr = _hv_vm_config_set_isa(cfg, isa);
  printf("set_isa(%u)=%d\n", isa, rr);
  rr = hv_vm_config_set_el2_enabled(cfg, true);
  printf("set_el2=%d\n", rr);
  rr = _hv_vm_config_set_vhe_enabled(cfg, true);
  printf("set_vhe=%d\n", rr);

  hv_return_t r = hv_vm_create(cfg);
  printf("vm_create=%d\n", r);
  if (r) return 1;

  void *ram = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  r = hv_vm_map(ram, IPA, SZ, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
  printf("map=%d\n", r);

  uint32_t *code = ram;
  code[0] = 0xd503201fu;  // nop
  code[1] = 0xd503201fu;  // nop
  code[2] = 0xd50323bfu;  // wfi

  // GIC 必须在任何 vCPU 之前创建（框架强制顺序；无 GIC 时框架无法处理 EL2 进入）
  {
    hv_gic_config_t gc = hv_gic_config_create();
    hv_gic_config_set_distributor_base(gc, 0x2000000ull);
    hv_gic_config_set_redistributor_base(gc, 0x2100000ull);
    int gr = hv_gic_create(gc);
    printf("gic_create=%d\n", gr);
  }

  hv_vcpu_config_t vc = hv_vcpu_config_create();
  printf("config=%p\n", (void*)vc);
  hv_vcpu_t vcpu; hv_vcpu_exit_t *exit;
  r = hv_vcpu_create(&vcpu, &exit, vc);
  printf("vcpu_create=%d\n", r);
  if (r) return 2;

  // 显式打开 HCR_EL2 的 NV/NV1/NV2（bit52/53/54）——嵌套的硬件开关
  extern int _hv_vcpu_set_control_field(hv_vcpu_t, int, uint64_t);
  uint64_t hcr = 0;
  {
    extern int _hv_vcpu_get_control_field(hv_vcpu_t, int, uint64_t *);
    uint64_t cur = 0;
    if (_hv_vcpu_get_control_field(vcpu, 0, &cur) == 0)
      hcr = cur;
  }
  hcr |= (1ull << 54) | (1ull << 53) | (1ull << 52); /* NV2 | NV1 | NV */
  printf("set_ctrl_hcr=%d hcr=%llx\n",
         _hv_vcpu_set_control_field(vcpu, 0, hcr), (unsigned long long)hcr);

  // 完整控制字段写入（与 qemu probe 值一致）
  extern int _hv_vcpu_set_control_field(hv_vcpu_t, int, uint64_t);
  _hv_vcpu_set_control_field(vcpu, 0,  0x200300001c0000ull | (1ull<<54) | (1ull<<52)); // hcr + NV2 + NV
  _hv_vcpu_set_control_field(vcpu, 1,  0x300000ull);                    // hacr
  _hv_vcpu_set_control_field(vcpu, 3,  0x80000000ull);                  // mdcr
  _hv_vcpu_set_control_field(vcpu, 5,  0x610f0000ull);                  // vmpidr
  _hv_vcpu_set_control_field(vcpu, 6,  0x100000000000000ull);           // virtual_timer_offset
  _hv_vcpu_set_control_field(vcpu, 9,  1ull);                           // apsts
  _hv_vcpu_set_control_field(vcpu, 11, 0xc0000000002600ull);            // hdfgwtr
  _hv_vcpu_set_control_field(vcpu, 12, 0xc0000000002000ull);            // cnthctl

  hv_vcpu_set_reg(vcpu, HV_REG_PC, IPA);
  uint32_t entry_cpsr = (argc > 2) ? (uint32_t)strtoul(argv[2],0,0) : 0x3cd;
  printf("entry_cpsr=%x\n", entry_cpsr);
  hv_vcpu_set_reg(vcpu, HV_REG_CPSR, entry_cpsr);
  {
    uint64_t chk = 0;
    hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &chk);
    printf("cpsr after set (pre-run)=%llx\n", (unsigned long long)chk);
  }

  r = hv_vcpu_run(vcpu);
  uint64_t pc = 0, cpsr = 0;
  hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc);
  hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr);
  printf("run=%d reason=%d esr=%#llx pc=%llx cpsr=%llx\n",
         r, exit->reason,
         (unsigned long long)exit->exception.syndrome,
         (unsigned long long)pc, (unsigned long long)cpsr);
  return 0;
}