// smoke.cpp - OpenHypervisor bring-up test on real hardware.
//
// Expected environment: arm64 Mac. Kernel-side entitlement policy applies
// regardless of this library; with amfi_get_out_of_my_way=1 (this host) an
// unsigned binary may create VMs.
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include "openhyp/openhyp.hpp"

using namespace openhyp;

static int failures = 0;
#define EXPECT(cond, msg) do { \
    if (cond) printf("PASS %s\n", msg); \
    else { printf("FAIL %s\n", msg); failures++; } } while (0)

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("INFO starting\n");
    // 1. capabilities reachable
    uint64_t vcpumax = 0;
    hv_return_t r = __hv_capability(HV_CAP_VCPUMAX, &vcpumax);
    EXPECT(r == HV_SUCCESS && vcpumax > 0, "capabilities");

    try {
        // 2. VM lifecycle
        Vm::Config cfg;
        cfg.set_isa(3 /* APPLE */).set_el2(true);
        Vm vm(cfg);
        EXPECT(true, "vm create");

        // 3. guest memory
        const size_t kSize = 1ull << 20;
        void *page = mmap(nullptr, kSize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        EXPECT(page != MAP_FAILED, "alloc guest memory");
        vm.map(page, 0x1000000ull, kSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        EXPECT(true, "vm map");
        vm.unmap(0x1000000ull, kSize);
        vm.map(page, 0x1000000ull, kSize, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);

        // tiny guest program: wfi loop
        static const uint32_t code[] = { 0xd503207fu, 0xd503207fu }; // nop; nop
        memcpy(page, code, sizeof(code));

        // 4. vcpu + register roundtrip
        Vcpu vcpu;
        EXPECT(vcpu.id() != Vcpu::kInvalid, "vcpu create");
        vcpu.set_reg(HV_REG_X0, 0x4141414142424242ull);
        EXPECT(vcpu.reg(HV_REG_X0) == 0x4141414142424242ull, "gpr roundtrip");
        vcpu.set_reg(HV_REG_PC, 0x1000000ull);
        vcpu.set_reg((hv_reg_t)31 /* PC */, 0x1000000ull);
        vcpu.set_sys_reg(HV_SYS_REG_SCTLR_EL1, vcpu.sys_reg(HV_SYS_REG_SCTLR_EL1));
        EXPECT(true, "sysreg roundtrip");

        // 5. batch ops
        std::vector<std::pair<hv_reg_t, uint64_t>> batch{{HV_REG_X1, 1}, {HV_REG_X2, 2}, {HV_REG_X3, 3}};
        vcpu.set_regs(batch);
        auto got = vcpu.get_regs({HV_REG_X1, HV_REG_X2, HV_REG_X3});
        EXPECT(got[0] == 1 && got[1] == 2 && got[2] == 3, "batch regs");

        // 6. one run: two nops then WFI -> should exit quickly (vtimer/cancel)
        vcpu.set_vtimer_mask(false);
        auto e = vcpu.run();
        printf("INFO exit reason=%d syndrome=%#llx pc=%llx\n",
               e.reason, (unsigned long long)e.syndrome,
               (unsigned long long)vcpu.reg((hv_reg_t)31));
        EXPECT(e.reason != HV_EXIT_REASON_UNKNOWN, "run exits with known reason");

        printf("%s: %d failure(s)\n", failures ? "SMOKE FAILED" : "SMOKE OK", failures);
        return failures ? 1 : 0;
    } catch (Error& e) {
        printf("FAIL exception: %s (%d)\n", e.what(), e.code());
        return 2;
    }
}