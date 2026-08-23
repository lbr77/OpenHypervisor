# OpenHypervisor

A clean-room C++ reimplementation of Hypervisor.framework for arm64 macOS.
It links nothing from Apple: the kernel protocol is spoken directly through
this library's own `svc` gate, and there is no entitlement checking in
userspace.

Drop-in for the Apple surface — all 134 exported symbols, including the
private ones (`_hv_vm_config_set_vhe_enabled`, `_hv_vcpu_get_context`, the
control-field and ext-register calls) — so existing consumers such as QEMU's
HVF backend can link against it unchanged. A modern C++ API sits alongside it
for things Apple does not offer.

Guest EL1 is exercised against the real thing and matches it exit for exit,
GXF included. Guest EL2 does not start yet; `PROTOCOL.md` records what is
known about the gap.

## Building

```sh
make          # libopenhyp.dylib + tests/smoke_bin
make smoke    # run the smoke test (needs a host that permits the hv trap)
```

No CMake. Header dependencies are tracked with `-MMD -MP`.

The smoke test checks that the API answers, not that a guest executed an
instruction — do not read it as proof that guests run.

## What the host has to allow

This library performs no SecTask or entitlement check of its own, and neither
does Apple's (its import table has none). The refusal happens in the **kernel**:
XNU's hv trap asks AMFI for `com.apple.security.hypervisor`. That is host
configuration, and it turns away any userspace library equally, Apple's
included.

- With `amfi_get_out_of_my_way=1` in effect, an unsigned process can create a
  VM outright.
- On a host where SIP's boot-arg restrictions are on, that boot-arg is ignored
  and the entitlement has to come from somewhere the kernel accepts — a
  properly provisioned signature, or whatever local arrangement already gets
  your VMM through.
- `amfidont --spoof-apple` / `--allow-all` only affect amfid's userspace
  validation. They do nothing for the kernel's hv gate.

## Using it

### The Apple-compatible surface

```c
#include "openhyp/hv_compat_protos.h"
#include "openhyp/ohv_core.h"     /* HV_SUCCESS and the other return codes */

hv_vm_config_t cfg = hv_vm_config_create();
hv_vm_config_set_el2_enabled(cfg, true);
hv_vm_create(cfg);
```

Link with `-lopenhyp` in place of `-framework Hypervisor`; nothing else in a
caller has to change. Code that resolves the private surface by name keeps
working too — those symbols carry the same single-underscore spellings Apple
exports, and `_hv_vcpu_get_context` answers with the pointer rather than an
out-parameter, as it does there.

### The C++ API

```cpp
#include "openhyp/openhyp.hpp"
using namespace openhyp;

Vm::Config c;
c.set_isa(3).set_el2(true).set_vhe(true);   // vhe is beyond Apple's public surface
Vm vm(c);

Vcpu vcpu;
vcpu.set_regs({{HV_REG_PC, entry}, {HV_REG_X0, arg}});
vcpu.run_loop([](const ExitInfo &e) {
    return e.reason == HV_EXIT_REASON_EXCEPTION ? Vcpu::Action::Stop
                                                : Vcpu::Action::Continue;
});

auto snap = vcpu.snapshot({HV_SYS_REG_SCTLR_EL1});
vcpu.restore(snap);
Vcpu::raw_trap(42, nullptr);                 // any trap, straight through
```

Beyond Apple: batched register access, context snapshot and restore, a
callback run loop, raw trap passthrough, a data-abort monitor wrapper, error
strings, and SMP broadcast helpers.

### Apple private system registers

The GXF and SPRR encodings (`s3_6_c15_*`) are served through the ordinary
`hv_vcpu_get_sys_reg` / `hv_vcpu_set_sys_reg` calls. They are described in
`src/ohv_sysreg_apple.cpp`, kept beside the generated table rather than in it,
because no SDK header describes them and the generator has nothing to read.

## Layout

```
include/openhyp/   ohv_core.h (protocol)  ohv_context.h (mapped page layout)
                   hv_compat_* (Apple surface)  openhyp.hpp (C++ API)
src/               trap, state, vm, vcpu, gic, misc, sysreg tables
tools/             gen_compat.py, gen_sysreg_table.py — SDK headers to interface
                   headers, clean-room
tests/smoke.cpp    on-hardware smoke test
reference/         kernel interface headers, framework export list, IDA output
PROTOCOL.md        the reversed protocol this is built on
API_MATRIX.md      per-symbol implementation matrix
```

## Clean room

The implementation uses only public SDK interface constants, structure layout
facts from open XNU sources, and offsets observed from binary behaviour. None
of Apple's expressive content is copied; interface constant values are
functional data required for interoperation. Kernel-side policy is neither
affected nor circumvented.
