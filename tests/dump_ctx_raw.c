
// dump_ctx_raw.c - raw trap path: identical dump.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef int hv_return_t;
__attribute__((naked)) hv_return_t raw_trap(unsigned t, void *a){
  __asm__ volatile("mov x16,#-5\nsvc #0x80\nret");
}
typedef struct __attribute__((packed)) {
  uint64_t min_ipa, ipa_size; uint32_t granule, flags; uint32_t isa; uint64_t fut[8];
} vm_create_t;
typedef struct __attribute__((packed)) {
  uint64_t min_ipa, ipa_size; uint32_t granule, flags; uint64_t out_asid, fut[8];
} addrspace_t;
typedef struct __attribute__((packed)) { uint64_t id, interface, fut[8]; } vcpu_create_t;
typedef struct __attribute__((packed)) { uint64_t uva,ipa,size,flags,asid,fut[8]; } map_item_t;
static const uint64_t IPA = 0x40000000ull;
static const size_t SZ = 1ull << 20;
int main(int argc, char **argv) {
  int el2 = argc > 1 ? atoi(argv[1]) : 1;
  const char *out = argc > 2 ? argv[2] : "/tmp/ctxraw.bin";
  setvbuf(stdout, 0, _IONBF, 0);
  vm_create_t vc = {0, 1ULL<<40, 0x4000, 0, 4, {0}};
  int r = raw_trap(1, &vc);
  printf("vm=%d\n", r); if (r) return 1;
  for (int i = 0; i < 8 && el2; i++) {
    addrspace_t as = {0,0,0x1000,0,0,{0}};
    raw_trap(12, &as);
  }
  void *ram = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  map_item_t mi = {(uint64_t)ram, IPA, SZ, 7ull, 0, {0}};
  raw_trap(3, &mi);
  vcpu_create_t vcc = {0,0,{0}};
  r = raw_trap(6, &vcc);
  printf("vcpu=%d\n", r); if (r) return 2;
  FILE *f = fopen(out, "wb");
  fwrite((void*)vcc.interface, 1, 32768, f);
  fclose(f);
  printf("dumped %s\n", out);
  return 0;
}
