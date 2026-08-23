# Guest EL2

Guest EL1 works and matches Apple's framework exit for exit. Guest EL2 does
not start. This is what is known about the gap, so that the next attempt does
not re-derive it.

## The failure

A vCPU whose CPSR selects EL2h is refused:

```
hv_vcpu_run -> 0xfae94004   (HV_ILLEGAL_GUEST_STATE)
PC unchanged at the entry point, no traps serviced
```

`HV_ILLEGAL_GUEST_STATE` has three sources in the kernel's `hv_trap_vcpu_run`,
and only one of them fits:

```c
/* refused before running */
if (!(extregs->sprr_config_el1 & SPRR_CONFIG_EL1_ENABLE) &&
     (extregs->gxf_config_el1  & GXF_CONFIG_EL1_ENABLE)) return HV_ILLEGAL_GUEST_STATE;
if (amx_active && sme_active)                             return HV_ILLEGAL_GUEST_STATE;
...
/* refused after running */
case ARM_VMEXIT_REASON_ILLEGAL_ERET: ret = HV_ILLEGAL_GUEST_STATE;
```

Neither of the first two applies here — GXF is off at entry and no AMX or SME
context is active — so this is the third: the kernel *did* enter, and the ERET
into the guest was itself illegal. That is what an ERET to EL2h looks like on
a vCPU whose guest has no EL2. The error code is shared with those other two
causes, so on its own it says less than it appears to.

## What the framework does that this does not

From the framework binary (`Hv::Vm::Vm`), guest EL2 is not part of any request
the library sends. `hv_vm_create` transmits `flags = 0` there too. Instead:

```c
*((_BYTE *)this + 92) = el2_enabled;
*((_BYTE *)this + 93) = vhe_enabled;
...
if (el2_enabled)
    GuestHypervisorSpaceManager::create(&out, this, 4096, 64, 8);
```

The manager creates **eight nested address spaces** — the `8` is the loop
count, the `64` the table width — each through `Hv::Vm::create_nested_space`,
which issues the address-space trap with the constant block the framework
keeps at `0x21b3f4948`:

```
min_ipa = 0, ipa_size = 0, granule = 0x1000, flags = 0, ASID read back at +24
```

and hands their ASIDs to a `NestedGuestMemoryMap`. A vCPU is placed in a space
through the same trap `Hv::Vcpu::set_space` and `set_vm_space` use, both of
which do nothing but issue it.

## What is implemented here

Both of the above, in `src/ohv_vm.cpp` and `src/ohv_vcpu.cpp`:

- a VM with `el2_enabled` creates the eight nested spaces on the same terms,
  and says so if one is refused;
- a vCPU on such a VM is placed in the first of them.

Both succeed. Entering EL2h is still refused, so something further is
required.

## Ruled out, with the evidence

| Candidate | Why not |
|---|---|
| A flag in the VM-create request | The framework sends `flags = 0`. Every bit of `flags`, and every byte of the whole request, was tried; none changes the outcome. The kernel's `hv_vm_create_t` is `{min_ipa, ipa_size, granule, flags, isa}` and has no field for it. |
| The vCPU-create request | The framework sends `{id, interface}`, the same as this library. |
| Control fields | The framework's are **bit-identical** across EL1, EL2 and EL2+VHE before the run: `[0]200300001c0000 [1]300000 [3]80000000 [5]610f0000 [6]100000000000000 [9]1 [11]c0000000002600 [12]c0000000002000`. Enabling EL2 changes nothing here. Seeding `HCR_EL2` with the framework's value changes nothing either. |
| Creating the nested spaces | Done, all eight succeed, still refused. |
| Placing the vCPU in one | Done, succeeds, still refused. |
| The identification registers | The framework reports the same `ID_AA64PFR0_EL1 = 0x1101000010110011` (EL2 field 0) whether or not EL2 was requested, and a VMM overwrites that register for its guest in any case. They cannot answer whether nested is available. |
| The public kernel sources | `osfmk/arm64/hv/` has no notion of guest EL2 at all — no `el2_enabled`, no nested state. The support is private to the shipping kernel; the sources can confirm trap shapes and the `ILLEGAL_ERET` path above, and nothing more. |

## Leads left

- `state_used` and the `ARM_GUEST_STATE_*` bits. The kernel consumes
  `ro.state_used` in `_hv_vcpu_entry`, and the framework may be declaring the
  guest's EL2 by claiming a state bit rather than by any request field.
- `HvCore::Hypervisor::VcpuStateManager` and the
  `VheNestedRegisterCollection` / `NoVheNestedRegisterCollection` pair. The
  framework decides there whether it is running a VHE guest hypervisor.
- What the eight nested spaces are supposed to *contain*. `Hv::Vcpu` has
  `map_nested_space` and `unmap_nested_space`; the spaces created here are
  empty, and an empty one may not be a usable one.

## A separate problem found along the way

Three regions of the mapped context page do not line up with what the kernel
uses. One is fixed; two are open.

- **Register block — fixed.** X0 is at `+0x008`, PC at `+0x108`, PSTATE at
  `+0x110`, one word later than the header had them. While that was wrong,
  `hv_vcpu_set_reg` wrote PC where the kernel does not read it and no guest
  ever executed an instruction.
- **Control block — fixed.** The controls base is `+0x920`, not the header's
  guess, and Apple's public field index does not follow the kernel's member
  order: it goes through a table at `0x21B3F4858` (`Hv::Vcpu::get/set_control_field`,
  validity mask `0x3FBEF`). Two anchors confirm the base: public field 5 lands
  on vpidr at `+0x28` (the MIDR anchor above) and field 17 on hcrx at `+0xd0` —
  both match the kernel's sequential order from that base. The vtimer words sit
  inside at their kernel slots (+0x30 offset, +0x68 mask word).
  `control_field_access` now translates through the same table.
- **Capabilities — fixed.** The framework serves these from its own
  `hypervisor_capabilities` buffer through one-line getters, and each getter's
  offset is the layout: `ipa_bits_4k` at `+0x1a1` (417 — matching the earlier
  live probe), `ipa_bits_16k` at `+0x1a9`, and the identification registers in
  the `+0x148..+0x198` range (dfr0/dfr1/isar0/isar1/mmfr0/mmfr1/mmfr2/pfr0/
  pfr1/smfr0/zfr0). `kCapsMap` uses these; selector-served ID reads no longer
  answer zero.

Pinned from the framework binary rather than guessed: both were anchor sweeps,
just done against the disassembly instead of live values.

## Reproducing

The probe is `svc/hvprobe/glbank.c` in the vphone tree; it builds against
either backend (`build.sh` for Apple's framework, `build-openhyp.sh` for this
library) and takes `--el1` / `--el2 --vhe --gic`. The guest-EL1 comparison
that both backends pass is:

```
traps f788w f7c1w f7c2w f7d2w f78aw f7c0r! f7d6r f7c0r! f7d6w f7d3w f7d4r f7d4w f7d6r f7c0r!
LANDED  elr_gl1 read back 0x40001800  pc 0x40001808  (14 serviced)
guarded handler entry ELR_GL1 = GENTER+4
```

## 结论（2026-08-23 确认）

Hypervisor.framework 在这个 build 上运行的是 **NVHE 模式**：所有 guest 一律跑在
EL1，不真正进入硬件 EL2。guest 的"EL2"是通过以下机制实现的：

1. 内核为每个 VM/vCPU 配置 HCR_EL2，按 ISA 等级开启特定的 trap 位
   （包括 TIDCP 使 impdef 寄存器访问产生 trap）
2. 被trap 的 EL2 寄存器访问以 `ARM_VMEXIT_REASON_MSR_TRAP` 或
   `ILLEGAL_ERET` 退出到 VMM
3. 框架的 `handle_get/set_vhe_el2_system_register` 和
   `handle_get/set_nested_el1_system_register` 应答这些访问，
   或者通过 hook（`hvf-el2-sysreg-exits.c`）转发给 VMM
4. guest 软件看到的是一致的 EL2 环境——但硬件 PSTATE 始终是 EL1

因此：
- 入口 CPSR 必须设为 **EL1h**（`0x3c5`），不能是 EL2h（会被降级或拒绝）
- 控制字段 idx0（HCR_EL2）的 NV/NV1/NV2 位不影响入口合法性——它们只控制
  特定 trap 行为，不是"让 guest 进 EL2"的开关
- 我们库的 sysreg 分派表 + Apple 私有寄存器表已经能正确应答这些 trap，
  不需要框架的 hook
