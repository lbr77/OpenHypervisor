// mini8 - sweep ro.state_used claims looking for the bit that lets a
// guest ERET to EL2h through.  One VM per attempt (destroy/recreate).
#include <stdio.h>
#include <stdint.h>
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

static uint8_t ctx[131072] __attribute__((aligned(16384)));
static void *ram; static const uint64_t IPA=0x40000000ull, SZ=1ull<<20;

static int setup(uint32_t isa){
  vm_create_t vc={0,1ULL<<40,0x4000,0,isa,{0}};
  int r=raw_trap(1,&vc); if(r) return r;
  for(int i=0;i<8;i++){addrspace_t as={0,0,0x1000,0,0,{0}}; raw_trap(12,&as);}
  map_item_t mi={(uint64_t)ram,IPA,SZ,7,0,{0}};
  raw_trap(3,&mi);
  vcpu_create_t vcc={0,0,{0}};
  r=raw_trap(6,&vcc); if(r) return r;
  memset(ctx,0,sizeof ctx);
  memcpy((void*)vcc.interface, ctx, 4096); // keep ver? actually kernel wrote it; do not clobber
  // restore: never mind, we do not touch first pages except targeted slots below
  return 0;
}

int main(void){
  setvbuf(stdout,0,_IONBF,0);
  ram=mmap(NULL,SZ,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  uint32_t *code=ram; code[0]=0xd503201f;code[1]=0xd503201f;code[2]=0xd50323bf;
  static const uint64_t bits[]={
    1ull<<63,1ull<<62,1ull<<61,1ull<<60,1ull<<59,1ull<<58,
    1ull<<57,1ull<<56,1ull<<55,1ull<<54,1ull<<53,1ull<<52 };
  for (unsigned bi=0; bi<sizeof bits/sizeof *bits; bi++){
    hv_return_t r=setup(4);
    if (r){ printf("setup fail %d\n",r); return 1; }
    volatile uint8_t *c=ctx;
    *(volatile uint64_t*)(c+0x4110) |= bits[bi];          // ro.state_used claim
    *(volatile uint32_t*)(c+0x110)=0x3cd;                  // cpsr EL2h
    *(volatile uint64_t*)(c+0x108)=IPA;                    // pc
    *(volatile uint64_t*)(c+0x100)=IPA+SZ;                 // sp
    *(volatile uint64_t*)(c+0x920)=0x200300001c0000ull;    // hcr
    r=raw_trap(9,NULL);
    uint32_t reason=*(volatile uint32_t*)(c+0x4008);
    printf("bit=%2d used -> run=%d reason=%u\n", 63-bi, r, reason);
    if (!(r==0 || (unsigned)r==0xfae94004u)){ printf("  ^ new behaviour!\n"); }
    raw_trap(2,NULL);
  }
  return 0;
}
