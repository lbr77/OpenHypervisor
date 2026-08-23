// mini9 - clean proof: claim ARM_GUEST_STATE_APPLE, enter guest at EL2h,
// have it execute instructions and store a marker into RAM we can check.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
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

static uint8_t *ctx;
static void *ram; static const uint64_t IPA=0x40000000ull, SZ=1ull<<20;

static int setup(uint32_t isa){
  vm_create_t vc={0,1ULL<<40,0x4000,0,isa,{0}};
  int r=raw_trap(1,&vc); if(r) return r;
  for(int i=0;i<8;i++){addrspace_t as={0,0,0x1000,0,0,{0}}; raw_trap(12,&as);}
  map_item_t mi={(uint64_t)ram,IPA,SZ,7,0,{0}};
  raw_trap(3,&mi);
  vcpu_create_t vcc={0,0,{0}};
  r=raw_trap(6,&vcc); if(r) return r;
  ctx=(uint8_t*)vcc.interface;
  return 0;
}

int main(int argc,char**argv){
  setvbuf(stdout,0,_IONBF,0);
  uint32_t isa = argc>1?(uint32_t)strtoul(argv[1],0,0):4;
  int use_bit  = argc>2?atoi(argv[2]):1;
  ram=mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  memset(ram,0,SZ);
  // guest: movz w0,#0x4711 ; str w0,[x1] ; wfi
  uint32_t *code=ram;
  code[0]=0x528e2220u;   // mov w0,#0x4711
  code[1]=0xb9000060u;   // str w0,[x1]
  code[2]=0xd50323bfu;   // wfi

  hv_return_t r=setup(isa);
  if(r){printf("setup %d\n",r);return 1;}

  if (use_bit)
    *(volatile uint64_t*)(ctx+0x4110) |= 1ull<<63;   /* ARM_GUEST_STATE_APPLE */

  *(volatile uint32_t*)(ctx+0x110)=0x3cd;            /* cpsr: EL2h, DAIF mask */
  *(volatile uint64_t*)(ctx+0x108)=IPA;              /* pc */
  *(volatile uint64_t*)(ctx+0x100)=IPA+SZ;           /* sp */
  *(volatile uint64_t*)(ctx+0x010)=IPA+0x800ull;     /* x1 = marker slot (x[n] @ 8+n*8) */
  *(volatile uint64_t*)(ctx+0x920)=0x200300001c0000ull; /* hcr like framework guests */
  *(volatile uint64_t*)(ctx+0x118)=0;                /* keep res2 clear */
  printf("ver=%llx\n", *(unsigned long long*)(ctx+0x4000));

  r=raw_trap(9,NULL);
  uint32_t reason=*(volatile uint32_t*)(ctx+0x4008);
  uint32_t esr   =*(volatile uint32_t*)(ctx+0x400c);
  uint64_t pc    =*(volatile uint64_t*)(ctx+0x108);
  uint32_t marker=*(volatile uint32_t*)((uint8_t*)ram+0x800);
  printf("isa=%u bit=%d -> run=%d reason=%u esr=%#x pc=%llx marker=%#x\n",
         isa,use_bit,r,reason,esr,(unsigned long long)pc,marker);
  return 0;
}