# API_MATRIX.md — 134 导出符号实现矩阵

图例：
- ✅ 原生实现并编译进 dylib
- 🧪 真机验证通过（trap 层往返）
- 🔶 实现但语义待真机回归（需有 hv 权限的宿主）
- ⛔ 返回 HV_UNSUPPORTED（依赖未具备的内核状态/文档缺口，见备注）

机制缩写：T#=trap 号；CTX=用户态映射 context 页直读直写；USR=纯用户态模型。

## 能力 / 全局
| 符号 | 机制 | 状态 |
|---|---|---|
| __hv_capability | T0 | ✅🧪 |
| hv_vm_get_max_vcpu_count | 常量64 | ✅ |
| hv_vm_get_isa | 全局态 | ✅ |

## VM 配置
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_vm_config_create | USR 对象 | ✅ |
| get/set_ipa_size, get_max/default_ipa_size | USR+T0缓存 | ✅ |
| get/set_ipa_granule, get_default | USR | ✅ |
| get_el2_supported/get/set_el2_enabled | USR(偏移24) | ✅ |
| _hv_vm_config_set/get_vhe_enabled | USR(偏移25,依赖el2) | ✅ |
| _hv_vm_config_set/get_isa | USR(偏移28) | ✅ |

## VM 生命周期 / 内存
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_vm_create | T1 | ✅🔶（本机内核策略返回 DENIED） |
| hv_vm_destroy | T2 | ✅🔶 |
| hv_vm_map/unmap/protect | T3/T4/T5 | ✅🔶 |
| hv_vm_allocate/deallocate | mmap 包装 | ✅ |

## 地址空间（私有面）
| 符号 | 机制 | 状态 |
|---|---|---|
| __hv_vm_space_config_create + set/get ×3字段 | USR | ✅ |
| __hv_vm_space_create | T12 | ✅🔶 |
| __hv_vm_space_destroy | 无 trap（随 VM 释放） | ✅ |
| __hv_vm_map_space/unmap_space/protect_space | T3/T4/T5(asid) | ✅🔶 |
| __hv_vm_stage1_tlb_op | T14 | ✅🔶 |

## vCPU 生命周期 / 运行
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_vcpu_create | T6{ id,out } + 魔数校验 | ✅🔶 |
| hv_vcpu_destroy | T7 | ✅🔶 |
| hv_vcpu_run | T9(arg=NULL,线程绑定) | ✅🔶 |
| hv_vcpus_exit | T10(位图按值) | ✅🔶 |
| hv_vcpu_invalidate_tlb | T14 | ✅🔶 |

## GPR / SIMD
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_vcpu_get/set_reg | CTX regs(x[i]=8i,fp=0xe8,lr=0xf0,sp=0xf8,pc=0x100,cpsr=0x108) | ✅🔶 |
| hv_vcpu_get/set_simd_fp_reg | CTX neon(q@0x140,fpsr=0x340,fpcr=0x344) | ✅🔶 |

## 系统寄存器（112 编码全覆盖）
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_vcpu_get/set_sys_reg | CTX 分派表(shared@0x350/banked@0x3b8/dbg@0x478/extreg@0xa10/caps只读) + T8 同步 | ✅🔶 |

## 中断 / 定时器 / 调试
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_vcpu_get_serror | RO 页退出原因 | ✅ |
| hv_vcpu_set_serror | — | ⛔ 待 VGIC 注入路径 |
| hv_vcpu_get/set_pending_interrupt | get=RO 页;set→GIC 模型 | ⛔ set 待接 |
| hv_vcpu_get/set_trap_debug_exceptions | MDCR_EL2.TDE(bit14)+dirty | ✅🔶 |
| hv_vcpu_get/set_trap_debug_reg_accesses | MDCR_EL2.TDA/TDOSA/TDRA | ✅🔶 |
| hv_vcpu_get/set_vtimer_mask | controls.timer bit0+dirty | ✅🔶 |
| hv_vcpu_get/set_vtimer_offset | controls.virtual_timer_offset | ✅🔶 |
| hv_vcpu_get_exec_time | CTX 0xa08 guest_tick_count | ✅🔶 |
| hv_vcpu_get_wait_for_interrupt_time | 库内累计 | ✅（口径与 Apple 不同） |

## 私有控制面
| 符号 | 机制 | 状态 |
|---|---|---|
| __hv_vcpu_get/set_control_field | CTX controls[索引] | ✅🔶 |
| __hv_vcpu_get_context | 返回映射指针 | ✅ |
| __hv_vcpu_get_ext_reg | CTX extregs@0xa10 | ✅🔶 |
| __hv_vcpu_config_get/set_vmkey、get/set_fgt_enabled、get/set_tlbi_workaround_enabled | USR 配置对象 | ✅ |
| hv_vcpu_config_create / get_feature_reg / get_ccsidr_el1_sys_reg_values | USR+T0 缓存 | ✅ |

## SME
| 符号 | 机制 | 状态 |
|---|---|---|
| hv_sme_config_get_max_svl_bytes | RO svl_b | ✅🔶 |
| hv_vcpu_get/set_sme_state | T15(svcr 按值) | ✅🔶 |
| hv_vcpu_get/set_sme_z_reg | sme 指针区+SVL | ✅🔶 |
| hv_vcpu_get/set_sme_za_reg | 同上(ZA 区) | ✅🔶 |
| hv_vcpu_get/set_sme_zt0_reg | 同上(头部64B) | ✅🔶 |
| hv_vcpu_get/set_sme_p_reg | — | ⛔ 布局待定 |

## AMX（私有）
| 符号 | 机制 | 状态 |
|---|---|---|
| __hv_vcpu_amx_prepare | T16 | ✅🔶 |
| __hv_vcpu_amx_query_active_context | RO amx 指针 | ✅🔶 |
| __hv_vcpu_get/set_amx_x/y/z_space、state_t_el1 | amx 结构(x8·y8·z64·t) | ✅🔶 |

## GIC（50 符号，全部纯用户态——与原框架一致，无 trap）
| 族 | 状态 |
|---|---|
| hv_gic_config_create/set_distributor_base/set_redistributor_base/set_msi_region_base/set_msi_interrupt_range | ✅ |
| hv_gic_create（锁序+vcpus 检查+create_gic 语义） | ✅🔶 |
| hv_gic_get_distributor/redistributor/msi_region 尺寸与对齐族（GICv3 架构常量） | ✅ |
| hv_gic_get/set_distributor_reg、redistributor_reg、icc_reg、ich_reg、icv_reg、msi_reg | ✅🔶（写后置 GIC dirty 位） |
| hv_gic_set_spi / send_msi / reset | ✅🔶 |
| hv_gic_state_create/state_get_size/state_get_data/set_state | ✅🔶 |
| hv_gic_get_intid / get_spi_interrupt_range | ✅ |

## 监控（私有）
| 符号 | 机制 | 状态 |
|---|---|---|
| ohv_monitor_data_abort（封装 T17） | mach port 通知 | ✅🔶 |

## 扩展 API（Apple 没有）
openhyp.hpp：Vm/Vcpu RAII、set_regs/get_regs 批量、snapshot/restore、run_loop 回调、
raw_trap 直通、monitor_data_abort 封装、exit_vcpus 广播、Error 类型。
