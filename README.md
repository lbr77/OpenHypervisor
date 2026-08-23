# OpenHypervisor

Hypervisor.framework（arm64 macOS）的干净室 C++ 重写。不链接 Apple 框架，
直接以本库自己的 `svc` 门实现全部内核协议；**用户态零 entitlement 校验**。

## 状态

| 项 | 状态 |
|---|---|
| 协议逆向 | `PROTOCOL.md`（二进制+内核源码双重验证）；上下文页与 vm-create 请求已与实测对齐 |
| 库编译 | ✅ `libopenhyp.dylib`（arm64，clang++17，无外部依赖） |
| trap 层 | ✅ 实测内核返回正确 |
| API 覆盖 | 134/134 导出符号已声明（见 `API_MATRIX.md`） |
| guest EL1 | ✅ 真机跑通，且与 Apple 框架逐条一致（见下） |
| guest EL2 | ❌ vCPU 以 EL2h 进入被内核拒为 `HV_ILLEGAL_GUEST_STATE` |
| 冒烟测试 | ✅ 全绿 —— 但它**不检查 guest 是否真的执行了指令**，别拿它当运行证据 |

### 与 Apple 框架的对照（2026-08-23，Mac16,10 / macOS 27）

同一个探针分别链接本库和 Apple 框架，在 guest EL1 下抬 SPRR、开 GXF、GENTER
进守护态、GEXIT 返回，两边输出逐条相同：

```
traps f788w f7c1w f7c2w f7d2w f78aw f7c0r! f7d6r f7c0r! f7d6w f7d3w f7d4r f7d4w f7d6r f7c0r!
LANDED  elr_gl1 read back 0x40001800  pc 0x40001808  (14 serviced)
守护处理程序入口处的 ELR_GL1 = GENTER+4
```

带 in-kernel GIC 时同样一致。

### guest EL2 还差什么

EL2 **不是**由 vm-create 请求携带的 —— 框架在那里也是发 `flags = 0`。它在
`Hv::Vm::Vm` 里看到 `el2_enabled` 之后建一个 `GuestHypervisorSpaceManager`，
经地址空间 trap 创建 **8 个 nested 地址空间**（`min_ipa 0, ipa_size 0,
granule 0x1000, flags 0`，回读 ASID）交给 `NestedGuestMemoryMap`。本库现在
同样会建，且 8 个都创建成功，但 vCPU 以 EL2h 进入仍被拒 —— 还差把 vCPU 关联
到这些空间上，`OHV_TRAP_VCPU_SET_ADDRESS_SPACE` 是下一个要看的地方。

判断 nested 是否可用**不要读 ID 寄存器**：框架不管开不开 EL2，
`ID_AA64PFR0_EL1` 都报 `0x1101000010110011`（EL2 字段为 0），而 VMM 本来就会
自己覆写它。`ohv_capabilities_t` 的布局目前也与内核 copyout 不符
（`ipa_bits_4k` 读出来是垃圾值），其 ID 字段一律答 0，同样不可信。

## 关于 entitlement

- 本库用户态不做任何 SecTask/entitlement 检查（原框架同样没有，验证过导入表）。
- 拒绝发生在**内核**：XNU 的 hv trap → AMFI 校验 `com.apple.security.hypervisor`。
  该策略属于宿主机配置，任何用户态库（包括 Apple 原版）都会被同样拒绝。
- 在 `amfi_get_out_of_my_way=1` 且 SIP 允许 boot-arg 生效的机器上，未签名进程可直接建 VM；
  本机（macOS 27, Mac16,10）实测该 boot-arg 被 Boot-arg Restrictions 拦截，需按你们现有
  流程（amfidont 对 qemu 的处理 / 正式签名的 provisioning）解决。
- `amfidont --spoof-apple/--allow-all` 只影响 amfid 用户端校验，对内核 hv 门无效（实测）。

## 构建

```bash
make            # 产出 libopenhyp.dylib + tests/smoke_bin
make smoke      # 运行冒烟测试（需宿主机允许 hv trap）
```

无 CMake 依赖；头文件依赖通过 `-MMD -MP` 自动跟踪。

## 使用

### Apple 兼容层

```c
#include "openhyp/hv_compat_protos.h"
hv_vm_config_t cfg = hv_vm_config_create();
hv_vm_config_set_el2_enabled(cfg, true);
hv_vm_create(cfg);
```

所有 134 个导出符号（含 `_hv_vm_config_set_vhe_enabled`、`__hv_vcpu_get_context`
等私有面）都由本 dylib 提供，可与 QEMU HVF 后端等现有代码直接链接替换。

### 现代 C++ 扩展 API

```cpp
#include "openhyp/openhyp.hpp"
using namespace openhyp;

Vm::Config c; c.set_isa(3).set_el2(true).set_vhe(true); // vhe 为超出 Apple 公开面的扩展
Vm vm(c);
Vcpu vcpu;
vcpu.set_regs({{HV_REG_PC, entry}, {HV_REG_X0, arg}});
vcpu.run_loop([](const ExitInfo& e) {
    if (e.reason == HV_EXIT_REASON_EXCEPTION) return Vcpu::Action::Stop;
    return Vcpu::Action::Continue;
});
auto snap = vcpu.snapshot({HV_SYS_REG_SCTLR_EL1});   // 快照/恢复
vcpu.restore(snap);
Vcpu::raw_trap(42, nullptr);                          // 私有 trap 直通
```

扩展能力（Apple 没有）：批量寄存器读写、上下文快照/恢复、回调运行循环、
原始 trap 直通、data-abort 监控封装、错误字符串、SMP 广播辅助。

## 目录

```
include/openhyp/   ohv_core.h(协议) ohv_context.h(映射布局) hv_compat_*(兼容面) openhyp.hpp(扩展)
src/               trap/state/vm/vcpu/gic/misc/sysreg_table(+生成器)
tools/             gen_compat.py gen_sysreg_table.py（SDK 头 → 接口头的干净室转换）
tests/smoke.cpp    真机冒烟
eference/          内核接口头副本、框架导出表、IDA 分析产物
PROTOCOL.md        逆向出的完整协议规格（本文档的依据）
API_MATRIX.md      逐符号实现矩阵
```

## 干净室声明

实现只使用：公开 SDK 接口常量、XNU 开源头码中的结构布局事实、以及从
二进制观测到的行为偏移。未复制 Apple 的任何表达性内容；接口常量值是
互操作所必需的功能性数据。内核侧策略不受本库影响，也未做任何规避。