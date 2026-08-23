# OpenHypervisor

A clean-room C++ rewrite of Hypervisor.framework (arm64 macOS). It links no
Apple framework; every kernel protocol is implemented through this library's
own `svc` gate. **Zero entitlement checks in user space.**

## About entitlements

- This library performs no SecTask/entitlement checks in user space (neither
  does the original framework — verified from its import table).
- Rejection happens **in the kernel**: XNU's hv trap → AMFI checks
  `com.apple.security.hypervisor`. That policy is host configuration; any
  user-space library (Apple's own included) is rejected the same way.
- On machines where `amfi_get_out_of_my_way=1` is honored (and SIP lets the
  boot-arg take effect), unsigned processes can create VMs directly. On this
  machine (macOS 27, Mac16,10) the boot-arg was observed to be blocked by
  Boot-arg Restrictions; resolve it through your existing process (the amfidont
  handling used for qemu / properly signed provisioning).
- `amfidont --spoof-apple/--allow-all` only affects amfid's user-side checks;
  it does nothing to the kernel hv gate (verified live).

## Todo

- [x] Finish first system version
- [x] Enable GFX/SPRR usage with vhe & EL2. 
- [x] Runtime offset matcher (`make offset_probe`) re-derives every pinned
  constant from the installed Hypervisor.framework on the running OS

## Build

```bash
make            # produces libopenhyp.dylib + tests/smoke_bin
```

No CMake dependency; header dependencies are tracked automatically via `-MMD -MP`.

## Usage

### Apple compatibility layer

```c
#include "openhyp/hv_compat_protos.h"
hv_vm_config_t cfg = hv_vm_config_create();
hv_vm_config_set_el2_enabled(cfg, true);
hv_vm_create(cfg);
```

All 134 exported symbols — including private-surface ones such as
`_hv_vm_config_set_vhe_enabled` and `__hv_vcpu_get_context` — are provided by
this dylib, so existing code such as QEMU's HVF backend can relink against it
as a drop-in replacement.

### Modern C++ extension API

```cpp
#include "openhyp/openhyp.hpp"
using namespace openhyp;

Vm::Config c; c.set_isa(3).set_el2(true).set_vhe(true); // vhe goes beyond Apple's public surface
Vm vm(c);
Vcpu vcpu;
vcpu.set_regs({{HV_REG_PC, entry}, {HV_REG_X0, arg}});
vcpu.run_loop([](const ExitInfo& e) {
    if (e.reason == HV_EXIT_REASON_EXCEPTION) return Vcpu::Action::Stop;
    return Vcpu::Action::Continue;
});
auto snap = vcpu.snapshot({HV_SYS_REG_SCTLR_EL1});   // snapshot/restore
vcpu.restore(snap);
Vcpu::raw_trap(42, nullptr);                          // raw trap passthrough
```

Capabilities Apple does not have: batched register access, context
snapshot/restore, a callback-driven run loop, raw trap passthrough, a
data-abort monitoring wrapper, error strings, SMP broadcast helpers.
### Runtime offset matching

The pinned offsets (config fields, vCPU context layout, kernel trap ids) are
only observations of one framework build. `tools/offset_probe.cpp` re-derives
them live: it dlopen()s the installed Hypervisor.framework, treats the mapped
`__TEXT` as bytes, follows call edges from the public entry points through a
small AArch64 decoder, and compares every immediate it finds against the
pinned constants.

```bash
make offset_probe && ./tools/offset_probe
```

Verdicts per pin: MATCH (direct site reachable from an exported function),
INDIR (offset referenced somewhere in `__TEXT`, path went through a function
pointer), MISS / DRIFT. Exit status is non-zero unless every pin matched, so
it can gate CI. On macOS 27.0 (26A5388g, arm64) all config fields, the whole
register block, and 14 of 18 trap ids verify directly; nothing drifted.
