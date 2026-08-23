# Hypervisor.framework (macOS 27.0 / 26A5368g, arm64e) — 逆向协议规格

本文档是 OpenHypervisor 干净室重写的依据。所有事实来自：

1. XNU 内核源码（`xnu-full/osfmk/arm64/hv/hv_interface.h`、`guest_thread_state.h`）
2. 框架二进制全量反汇编（2428 函数 / 135768 指令，objdump + IDA 9.4 交叉验证）
3. IDA 反编译关键包装函数（`reference/`、`.scratch/hvf-ida/decomp/`）
4. 前期研究 `hvf-research-26A5368g/FINDINGS.md` 与 HANDOFF-2026-08-22.md

## 1. 用户态↔内核传输协议

整个框架 2428 个函数中**只有一个函数含 `svc` 指令**：`_hv_trap`：

```asm
_hv_trap:
    mov  x16, #-0x5        ; mach trap 号 -5
    svc  #0x80
    ret
```

调用约定：`x0` = trap 号（`hv_trap_t`），`x1` = 参数结构指针（可为 NULL）。
返回 `hv_return_t`（w0）。

### 1.1 trap 号表（来自 hv_interface.h，反汇编 100% 吻合）

| # | 名称 | 参数结构 | 框架内调用者 |
|---|------|----------|--------------|
| 0 | TRAP_HV_CAPABILITIES | `hv_capabilities_t`（输出，约 400B） | Arm::HypervisorCapabilities::get_capabilities（call_once 缓存） |
| 1 | TRAP_HV_VM_CREATE | `hv_vm_create_t{min_ipa,ipa_size,granule:u32,flags:u32,isa:u32}` | Hv::Vm::create |
| 2 | TRAP_HV_VM_DESTROY | NULL | Hv::Vm::destroy |
| 3 | TRAP_HV_VM_MAP | `hv_vm_map_item_t{uva,ipa,size,flags,asid}` | Hv::Vm::map_space |
| 4 | TRAP_HV_VM_UNMAP | 同上 | Hv::Vm::unmap_space 等（5 处） |
| 5 | TRAP_HV_VM_PROTECT | 同上（flags=新权限） | Hv::Vm::protect_space（2 处） |
| 6 | TRAP_HV_VCPU_CREATE | `hv_vcpu_create_t{id, interface:u64 出参}` | Hv::Vcpu::create |
| 7 | TRAP_HV_VCPU_DESTROY | NULL | Hv::Vcpu::create 失败回滚 / destroy |
| 8 | TRAP_HV_VCPU_SYSREGS_SYNC | 见 §4 | 227 个调用者（全部私有寄存器族 + 公开 get/set_sys_reg） |
| 9 | TRAP_HV_VCPU_RUN | 见 §5 | Hv::Vcpu::run_once |
| 10 | TRAP_HV_VCPU_RUN_CANCEL | `uint64_t vcpu 位图`（id≤63） | hv_vcpus_exit |
| 11 | TRAP_HV_VCPU_SET_ADDRESS_SPACE | `{asid}`? | Hv::Vcpu::set_vm_space / set_space |
| 12 | TRAP_HV_VM_ADDRESS_SPACE_CREATE | `hv_vm_addrspace_create_t{min_ipa,ipa_size,granule:u32,flags:u32,out_asid}` | Vm::create_space / create_host_space / create_nested_space |
| 13 | TRAP_HV_VM_INVALIDATE_TLB | `hv_vm_tlbi_item_t{asid,ipa,nbytes}` | （经包装） |
| 14 | TRAP_HV_VM_STAGE1_TLB_OP | `hv_vm_stage1_tlb_op_t{asid,op,param}` | Vm::stage1_tlb_operation / invalidate_stage1_tlb_for_space |
| 15 | TRAP_HV_VCPU_SET_SVCR | `{svcr}`? | Vcpu::set_sme_state |
| 16 | TRAP_HV_VCPU_AMX_PREPARE | `{x,y,z 指针}`? | Hv::Vcpu::amx_prepare |
| 17 | TRAP_HV_VM_MONITOR_DATA_ABORT | `hv_vm_monitor_data_abort_t` | 框架内无直接调用者（内核能力，经私有导出） |

**GIC 族（~50 个导出）不经过任何 trap**：`hv_gic_create` 是纯用户态实现
（vm 锁 + vcpus 锁 + `Hv::Vm::create_gic`），内核侧通过 context 页里的
`ich_*` 寄存器 + state_dirty 位在下次 run 时感知。

### 1.2 错误码（hv_error.h 语义，二进制中确认）

```
HV_SUCCESS            0x00000000
HV_ERROR              0xFAE94001
HV_BUSY               0xFAE94002   （thread_local vcpu 已存在时 vcpu_create 返回它）
HV_BAD_ARGUMENT       0xFAE94003
HV_ILLEGAL_GUEST_STATE0xFAE94004
HV_NO_RESOURCES       0xFAE94005
HV_NO_DEVICE          0xFAE94006   （gic_create 无 VM 时）
HV_DENIED             0xFAE94007
HV_UNSUPPORTED        0xFAE94008
内核内部 _HV_VM_QUOTA 0xFAE94FFF   （gic/vm 配额检查常量）
```

### 1.3 ISA 等级（hv_vm_isa_t，内核枚举）

```
0 _HV_VM_ISA_NONE               不允许建 VM
1 _HV_VM_ISA_GENERIC            ARM 标准特性
2 _HV_VM_ISA_APPLE_COPROCESSOR  Apple 协处理器子集
3 _HV_VM_ISA_APPLE              macOS/iOS guest 扩展（ISA3/ISA4 由此起）
4 _HV_VM_ISA_INTERNAL           几乎全部宿主特性
```

## 2. vCPU 用户态接口映射（arm_guest_context_t，2×16K 页）

trap 6 返回的 `interface` 指向 2 个 16K 页：

### 2.1 RW 页（+0x0000，arm_guest_rw_context_t）

顺序布局（union 视图之一，全部顺序排列）：

```
0x000  res1 u64
0x008  x[29]（x0..x28）        hv_vcpu_get/set_reg(HV_REG_*)
0x098  fp  0x0A0 lr  0x0A8 sp  0x0B0 pc  0x0B8 cpsr(u32)+pad
0x0C0  res2[4]
0x0E0  neon.q[32]（__uint128_t）0x2E0 fpsr 0x2E4 fpcr   → simd_fp_reg API
0x2E8  arm_guest_shared_sysregs_t（13×u64: mdscr_el1,tpidr_el1,tpidr_el0,tpidrro_el0,
       sp_el0,sp_el1,par_el1,csselr_el1,apstate,afpcr_el0,scxtnum_el0,tpidr2_el0,smpri_el1）
0x350  arm_guest_banked_sysregs_t（24×u64: ttbr0_el1..smcr_el1，含 ich_vmcr_el2）
0x410  arm_guest_dbgregs_t（bp[16]{bvr,bcr}, wp[16]{wvr,wcr}, mdccint,osdtrrx,osdtrtx,dbgclaim:u8）
0x630  arm_vgic_sysregs_t{ich_ap0r0_el2, ich_ap1r0_el2}
0x640  volatile arm_guest_controls_t（见 §2.3）
       volatile arm_guest_frozen_t{actlr_el1}
       volatile u64 state_dirty   ← VMM 写 ARM_GUEST_STATE_* 位
       u64 guest_tick_count
       arm_guest_extregs_t（Apple 私有 SPR，~80×u64，含 GXF/SPRR/CTRR/PPL keys/GL1 bank）
0x1000 arm_vncr_context_t（对齐 0x1000，VNCR_EL2 映射布局）
0x2000 apple_vncr_context_t（AVNCR，Apple 私有）
```

（精确偏移以我们头文件里的 offsetof/static_assert 为准，与内核 static_assert 对齐。）

### 2.2 RO 页（+0x4000，arm_guest_ro_context_t）

```
+0x4000 u64 ver  ← 魔数 ARM_HV_STATE_VER：magic " hyp"(0x20687970)<<32 | major<<24 | su<<16 | minor
       本机实测框架检查值 0x2068797003000003（major=3, minor=3）
+0x4008 arm_guest_vmexit_t:
        u32 vmexit_reason（见 §5）
        u32 vmexit_esr
        u32 vmexit_instr
        u64 vmexit_far
        u64 vmexit_hpfar
+0x4020 controls 镜像（内核退出时刷新）
+0x4120? u64 state_valid / state_dirty / state_used
        state_used 位：SYSREGS=0,DEBUG=1,CONTROLS=2,GIC=3,SME=4,
        APPLE=63,CTRR=62,PTRAUTH_ARM=61,PTRAUTH_APPLE=60,SPRR=59,GXF=58,AMX=57,
        JITBOX=56,APPLE_GENERIC_TIMER=55,AMX_CONTEXT=54
        u32 ich_vtr_el2 / u32 ich_misr_el2 / u32 ich_elrsr_el2
        u64 svcr; u16 svl_b; pad[3]; *sme; *amx（用户态 AMX 保存区指针）
```

（HANDOFF 实测：GXF_ENTRY→ctx+0xb68、SP_GL1→0xb78、TPIDR_GL1→0xb80、
ASPSR_GL1→0xb88、ESR_GL1→0xba0、ELR_GL1→0xba8、FAR_GL1→0xb98、VBAR_GL1→0xb90、
SPSR_GL1→0xbb0 —— 全部落在 extregs 尾部 GL1 bank，与上述布局一致。）

### 2.3 arm_guest_controls_t（RW 页 controls + RO 页镜像，字段序）

```
hcr_el2, hacr_el2, cptr_el2, mdcr_el2, vmpidr_el2, vpidr_el2,
virtual_timer_offset,          ← hv_vcpu_get/set_vtimer_offset
hfgrtr_el2, hfgwtr_el2, hfgitr_el2, hdfgrtr_el2, hdfgwtr_el2, cnthctl_el2,
timer,                         ← bit0=HV_TIMER_MASK → hv_vcpu_get/set_vtimer_mask
vmkeyhi_el2, vmkeylo_el2,      ← __hv_vcpu_config_set_vmkey 相关
apsts_el1,
ich_hcr_el2, ich_lr_el2[8],    ← LR bit32=ARM_GUEST_ICH_LR_DIRTY
hcrx_el2
```

`_hv_control_field_t`（公开 API 的 control field 枚举）即此结构成员索引。

## 3. VM 配置对象（hv_vm_config_s）

```
+0x00 u64 min_ipa?（getter 族）
+0x14 u32 granule
+0x18 u32 ipa_size_bits（ipa_bit_length）
+0x18? …
+0x18/0x1C u64 对（Vcpu::create 从 config+16/+24 拷贝两 u64，+32 拷贝 u16 → 属 vcpu config）
+24 (byte) el2_enabled
+25 (byte) vhe_enabled（要求 el2=1）
+28 u32 isa（_hv_vm_config_get_isa）
```

Vm::create 组装 trap1 参数：`{min_ipa=0, ipa_size=1<<ipa_bit_length,
granule=config->granule(dword@0x14), flags=0, isa=get_isa(config)}`。

## 4. TRAP 8（SYSREGS_SYNC）参数

`Hv::Vcpu::get/set_system_register` → trap 8。参数结构待精确转储
（`reference/` 后续补充）；语义：把指定寄存器在内核 bank 与用户 context 间同步。
公开 API 流程：`get_if_owning_thread`（vcpu 必须被当前线程拥有，否则 HV_BAD_ARGUMENT）
→ trap8。

## 5. vCPU 运行与退出

- `Hv::Vcpu::run_once` → trap 9（参数待转储；进入前需 thread_local 绑定）。
- 退出原因（内核枚举，公开 API 的 HV_VCPU_EXIT_* 映射见 SDK 头）：

```
0 NONE 1 SYNC 2 SERROR 3 IRQ 4 FIQ 5 HANDLED_FAULT 6 UNHANDLED_FAULT
7 UNKNOWN_TRAP 8 MSR_TRAP 9 INTERRUPTED 10 ILLEGAL_ERET 11 HOST_AST
12 VGIC 13 SME_TRAP  (1<<31)|0 AMX_DISABLED（私有位）
```

- `hv_vcpus_exit(vcpus,count)`：把 id≤63 的 vcpu 编成 `1ULL<<id` 位图 → trap 10。

## 6. Entitlement

- 框架二进制**无任何 Sec*/entitlement 导入**（nm -u 验证），用户态无校验。
- 门在内核：XNU hv_trap → AMFI entitlement（`com.apple.security.hypervisor`）。
- 本机 boot-args `amfi_get_out_of_my_way=1` 已解除内核门。
- OpenHypervisor 用户态同样零校验；内核策略不受本库影响。

## 7. 重写映射表（OpenHypervisor 实现 → 协议）

| OpenHypervisor | 实现方式 |
|---|---|
| `ohv_trap()` | 内联 asm `mov x16,#-5; svc #0x80`（与 _hv_trap 等价的干净室表达） |
| hv_vm_* 生命周期/内存 | 直填 §1.1 结构 → trap |
| hv_vcpu_get/set_reg | RW 页 regs 直读直写（+state_dirty CONTROLS? 仅 regs 无需 dirty） |
| simd_fp / sys_reg / control_field / vtimer / ext_reg / amx / sme | RW/RO 页 + trap8/15/16（按 §1.1） |
| hv_vcpu_run | trap 9 |
| GIC 族 | 用户态模型：context ich_* + state_dirty GIC 位（对齐 Hv::Vm::create_gic 语义） |
| capabilities | trap 0 + call_once 缓存 |

## 8. 与 Apple 实现的已知偏差（诚实清单）

- trap 8/9/11/15/16 参数结构以二进制转储为准逐个核对后才启用对应 API。
- GIC 模型按可观测行为重写，不承诺 bit 级一致。
- 私有 GL1/GXF/SPRR 访问器按 HANDOFF 实测偏移实现。
