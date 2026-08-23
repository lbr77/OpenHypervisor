
// dump_ctx_fw.c - framework path: dump full vCPU context for byte-diff.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <Hypervisor/Hypervisor.h>
extern int _hv_vm_config_set_isa(hv_vm_config_t, uint32_t);
extern int _hv_vm_config_set_vhe_enabled(hv_vm_config_t, int);
static const uint64_t IPA = 0x40000000ull;
static const size_t SZ = 1ull << 20;
int main(int argc, char **argv) {
  int el2 = argc > 1 ? atoi(argv[1]) : 1;
  const char *out = argc > 2 ? argv[2] : "/tmp/ctxfw.bin";
  setvbuf(stdout, 0, _IONBF, 0);
  hv_vm_config_t cfg = hv_vm_config_create();
  _hv_vm_config_set_isa(cfg, 4);
  if (el2) {
    hv_vm_config_set_el2_enabled(cfg, true);
    _hv_vm_config_set_vhe_enabled(cfg, true);
  }
  int r = hv_vm_create(cfg);
  printf("vm=%d\\n", r); if (r) return 1;
  hv_gic_config_t gc = hv_gic_config_create();
  hv_gic_config_set_distributor_base(gc, 0x2000000ull);
  hv_gic_config_set_redistributor_base(gc, 0x2100000ull);
  printf("gic=%d\\n", hv_gic_create(gc));
  void *ram = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  hv_vm_map(ram, IPA, SZ, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
  hv_vcpu_config_t vcc = hv_vcpu_config_create();
  hv_vcpu_t vcpu; hv_vcpu_exit_t *ex;
  r = hv_vcpu_create(&vcpu, &ex, vcc);
  printf("vcpu=%d\\n", r); if (r) return 2;
  void *ctxp = NULL;
  extern void *_hv_vcpu_get_context(hv_vcpu_t);
  ctxp = _hv_vcpu_get_context(vcpu);
  /* dump the SPRR/GXF config words from extregs */
  {
    extern void *dummy(void);
    uint8_t *rw = (uint8_t *)ctxp;
    uint64_t sprr = *(volatile uint64_t *)(rw + 0xBC8);  /* sprr_config_el1 */
    uint64_t gxf  = *(volatile uint64_t *)(rw + 0xB60);  /* gxf_config_el1 */
    printf("sprr_config=%llx gxf_config=%llx\n",
           (unsigned long long)sprr, (unsigned long long)gxf);
  }
  FILE *f = fopen(out, "wb");
  fwrite(ctxp, 1, 32768, f);
  fclose(f);
  printf("dumped %s\\n", out);
  return 0;
}