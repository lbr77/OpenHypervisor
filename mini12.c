// mini12 - probe the kernel's actual ISA acceptance boundary
#include <stdio.h>
#include <stdint.h>
#include <Hypervisor/Hypervisor.h>
extern int _hv_vm_config_set_isa(hv_vm_config_t, uint32_t);
int main(void){
  setvbuf(stdout,0,_IONBF,0);
  for (uint32_t isa=1; isa<=12; isa++){
    hv_vm_config_t cfg=hv_vm_config_create();
    int sr=_hv_vm_config_set_isa(cfg,isa);
    int cr=(int)hv_vm_create(cfg);
    printf("isa=%2u set=%d create=%d\n",isa,sr,cr);
    if(!cr) hv_vm_destroy();
  }
  return 0;
}
