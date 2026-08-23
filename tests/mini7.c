// mini7 - decisive probe: full guest-EL2 configuration over raw traps,
// mirroring what qemu + Hypervisor.framework do for the d16ap bring-up:
//   isa=4, eight nested address spaces, vCPU bound to one of them,
//   HCR_EL2 seeded with the value the framework's guests run with,
//   CPSR = EL2h, PC into mapped RAM.
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
typedef struct __attribute__((packed)) {
  uint64_t id, interface, fut[8];
} vcpu_create_t;
static uint8_t caps[1024] __attribute__((aligned(16)));
static uint8_t ctx[2*65536] __attribute__((aligned(16384)));

#define W(off,v) (*(volatile uint64_t*)(ctx+(off))=(v))
#define R(off) (*(volatile uint64_t*)(ctx+(off)))

int main(int argc,char**argv){
  setvbuf(stdout,0,_IONBF,0);
  uint32_t isa = argc>1 ? (uint32_t)strtoul(argv[1],0,0) : 4;
  hv_return_t r;

  memset(caps,0,sizeof caps);
  r = raw_trap(0,caps);
  printf("caps=%d\n", r);

  vm_create_t vc = {0, 1ULL<<40, 0x4000, 0, isa, {0}};
  r = raw_trap(1,&vc);
  printf("vm_create(isa=%u) r=%d\n", isa, r);
  if (r) return 1;

  // eight nested address spaces
  uint64_t asid[8]={0};
  for (int i=0;i<8;i++){
    addrspace_t as = {0,0,0x1000,0,0,{0}};
    r = raw_trap(12,&as);
    printf("  space[%d] r=%d asid=%llx\n", i, r, (unsigned long long)as.out_asid);
    if (r==0) asid[i]=as.out_asid;
  }

  // map guest ram at IPA 0x40000000, code there
  size_t sz = 1<<20;
  void *ram = mmap(NULL,sz,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  uint64_t ipa = 0x40000000ull;
  typedef struct __attribute__((packed)){uint64_t uva,ipa,size,flags,asid,fut[8];} map_item_t;
  map_item_t mi = {(uint64_t)ram, ipa, sz, 7ull, 0, {0}};
  r = raw_trap(3,&mi); printf("map r=%d\n", r);

  // vcpu
  vcpu_create_t vcc = {0,0,{0}};
  r = raw_trap(6,&vcc);
  printf("vcpu_create r=%d magic=%llx\n", r, *(unsigned long long*)((uint8_t*)vcc.interface+0x4000));
  volatile uint8_t *c = (uint8_t*)vcc.interface;
  if (r) return 2;

  // bind vCPU to first nested space (trap 11, argument form A: pointer to asid)
  uint64_t space = asid[0];
  r = raw_trap(11,&space);
  printf("set_space(ptr) r=%d\n", r);
  if (r) { r = raw_trap(11,(void*)space); printf("set_space(val) r=%d\n", r); }

  // seed controls: HCR_EL2 like the framework's guests, plus CPTR/MDCR zeros
  W(0x920, 0x200300001c0000ull);            // hcr
  W(0x928, 0); W(0x930, 0); W(0x938, 0);    // hacr cptr mdcr

  // guest state: EL2h at the code, SP ready
  uint32_t *code = (uint32_t*)ram;
  code[0] = 0xd503201fu;  // nop
  code[1] = 0xd503201fu;  // nop
  code[2] = 0xd50323bf;   // wfi
  *(volatile uint64_t*)(c+0x108) = ipa;          // pc
  *(volatile uint32_t*)(c+0x110) = 0x3cd;        // cpsr: EL2h, DAIF masked
  *(volatile uint64_t*)(c+0x100) = ipa + sz;     // sp
  *(volatile uint64_t*)(c+0xa00) = 0;            // rw.state_dirty
  printf("ro.ver=%llx\n", *(unsigned long long*)(c+0x4000));

  r = raw_trap(9,NULL);
  uint32_t reason = *(volatile uint32_t*)(c+0x4008);
  uint64_t pc = *(volatile uint64_t*)(c+0x108);
  printf("run r=%d reason=%u pc=%llx far=%llx\n", r, reason,
         (unsigned long long)pc,
         (unsigned long long)*(volatile uint64_t*)(c+0x4018));
  return 0;
}
