#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
typedef int hv_return_t;
__attribute__((naked)) hv_return_t raw_trap(unsigned t, void *a){
  __asm__ volatile("mov x16,#-5\nsvc #0x80\nret");
}
static uint8_t caps[4096] __attribute__((aligned(16)));
typedef struct __attribute__((packed)) {
  uint64_t min_ipa; uint64_t ipa_size; uint32_t granule; uint32_t flags; uint32_t isa;
  uint64_t fut[8];
} vm_create_t;
typedef struct __attribute__((packed)) {
  uint64_t uva; uint64_t ipa; uint64_t size; uint64_t flags; uint64_t asid;
  uint64_t fut[8];
} map_item_t;
int main(int argc, char**argv){
  setvbuf(stdout,0,_IONBF,0);
  memset(caps,0,sizeof caps);
  raw_trap(0,caps);
  uint64_t ipabits = *(uint64_t*)(caps+417);
  uint32_t granule = argc>1 ? (uint32_t)strtoul(argv[1],0,0) : 0;
  vm_create_t cr = {0, 1ULL<<ipabits, granule, 0, 3, {0}};
  printf("createdump:");
  for (unsigned i=0;i<sizeof cr;i++) printf("%02x", ((unsigned char*)&cr)[i]);
  printf("\n");
  hv_return_t r = raw_trap(1,&cr);
  printf("create(granule=%u,bits=%llu) r=%d\n", granule,(unsigned long long)ipabits,r);
  if (r) return 1;
  size_t sz = 1<<20;
  void *page = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE,-1,0);
  map_item_t it = { (uint64_t)page, 0x1000000ull, sz, 7ull, 0, {0} };
  r = raw_trap(3,&it);
  printf("map r=%d\n", r);
  return 0;
}