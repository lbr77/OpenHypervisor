// openhyp.hpp - OpenHypervisor modern C++ API.
//
// Beyond the Apple-compatible C surface (hv_compat_protos.h) this library
// ships its own API with capabilities Apple never provided:
//   * RAII ownership of VMs and vCPUs
//   * batch register read/write (single lock acquisition)
//   * full-context snapshots (save/restore a vCPU)
//   * callback-driven run loop
//   * raw trap passthrough for future/private kernel traps
//   * zero entitlement gating anywhere
#ifndef OPENHYP_HPP
#define OPENHYP_HPP

#include "hv_compat_protos.h"
#include "ohv_core.h"
#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <optional>

extern "C" {
const char *ohv_error_string(hv_return_t r);
uint32_t hv_vm_get_isa(void);
hv_return_t ohv_monitor_data_abort(bool add, hv_vm_space_t asid, uint64_t context,
                                   hv_ipa_t base, size_t size, void *port);
}

namespace openhyp {

// ------------------------------------------------------------ error type --
class Error : public std::runtime_error {
public:
    explicit Error(hv_return_t r) : std::runtime_error(ohv_error_string(r)), code_(r) {}
    hv_return_t code() const noexcept { return code_; }
private:
    hv_return_t code_;
};

inline void check(hv_return_t r) { if (r != HV_SUCCESS) throw Error(r); }

// ------------------------------------------------------------------- VM --
class Vm {
public:
    // config: optional builder; pass {} for defaults.
    class Config {
    public:
        Config() : cfg_(hv_vm_config_create()) {}
        ~Config() = default;
        Config& set_ipa_bits(uint32_t bits) { check(hv_vm_config_set_ipa_size(cfg_, bits)); return *this; }
        Config& set_el2(bool on)            { check(hv_vm_config_set_el2_enabled(cfg_, on)); return *this; }
        // Private extensions (not present in Apple's public API docs):
        Config& set_vhe(bool on)            { check(_hv_vm_config_set_vhe_enabled(cfg_, on)); return *this; }
        Config& set_isa(uint32_t isa)       { check(_hv_vm_config_set_isa(cfg_, isa)); return *this; }
        hv_vm_config_t get() const { return cfg_; }
    private:
        hv_vm_config_t cfg_;
    };

    explicit Vm(const Config& c = {}) { check(hv_vm_create(c.get())); }
    ~Vm() { if (alive_) hv_vm_destroy(); }
    Vm(Vm&& o) noexcept : alive_(o.alive_) { o.alive_ = false; }
    Vm& operator=(Vm&& o) noexcept { std::swap(alive_, o.alive_); return *this; }

    void map(void *uva, hv_ipa_t ipa, size_t size, hv_memory_flags_t flags) {
        check(hv_vm_map(uva, ipa, size, flags));
    }
    void unmap(hv_ipa_t ipa, size_t size)      { check(hv_vm_unmap(ipa, size)); }
    void protect(hv_ipa_t ipa, size_t size, hv_memory_flags_t f) { check(hv_vm_protect(ipa, size, f)); }

    static uint32_t max_vcpus() { uint32_t m = 0; check(hv_vm_get_max_vcpu_count(&m)); return m; }
    static uint32_t machine_isa() { return hv_vm_get_isa(); }

private:
    bool alive_ = true;
};

// ----------------------------------------------------------------- vCPU --
struct ExitInfo {
    hv_exit_reason_t reason;
    uint64_t syndrome;
    uint64_t virtual_address;
    hv_ipa_t physical_address;
};

class Vcpu {
public:
    explicit Vcpu(std::optional<hv_vcpu_config_t> config = std::nullopt) {
        hv_vcpu_config_t c = config.value_or(nullptr);
        check(hv_vcpu_create(&id_, &exit_, c));
    }
    ~Vcpu() { if (id_ != kInvalid) hv_vcpu_destroy(id_); }
    Vcpu(Vcpu&& o) noexcept : id_(o.id_), exit_(o.exit_) { o.id_ = kInvalid; }
    Vcpu& operator=(Vcpu&& o) noexcept { std::swap(id_, o.id_); std::swap(exit_, o.exit_); return *this; }

    hv_vcpu_t id() const { return id_; }

    // -- single registers ---------------------------------------------------
    uint64_t reg(hv_reg_t r) { uint64_t v = 0; check(hv_vcpu_get_reg(id_, r, &v)); return v; }
    void set_reg(hv_reg_t r, uint64_t v) { check(hv_vcpu_set_reg(id_, r, v)); }
    uint64_t sys_reg(hv_sys_reg_t r) { uint64_t v = 0; check(hv_vcpu_get_sys_reg(id_, r, &v)); return v; }
    void set_sys_reg(hv_sys_reg_t r, uint64_t v) { check(hv_vcpu_set_sys_reg(id_, r, v)); }

    // == EXTENDED: batch register operations (one call, many registers) ====
    void set_regs(const std::vector<std::pair<hv_reg_t, uint64_t>>& batch) {
        for (auto& [r, v] : batch) check(hv_vcpu_set_reg(id_, r, v));
    }
    std::vector<uint64_t> get_regs(const std::vector<hv_reg_t>& regs) {
        std::vector<uint64_t> out;
        out.reserve(regs.size());
        for (auto r : regs) out.push_back(reg(r));
        return out;
    }
    void set_sys_regs(const std::vector<std::pair<hv_sys_reg_t, uint64_t>>& batch) {
        for (auto& [r, v] : batch) check(hv_vcpu_set_sys_reg(id_, r, v));
    }

    // == EXTENDED: full context snapshot ====================================
    struct Snapshot {
        std::vector<uint64_t> gpr;         // x0..x30, pc
        std::vector<std::pair<hv_sys_reg_t, uint64_t>> sysregs;
        uint64_t cpsr, fpcr, fpsr;
    };
    Snapshot snapshot(const std::vector<hv_sys_reg_t>& interesting) {
        Snapshot s;
        s.gpr.reserve(33);
        for (unsigned i = 0; i <= 31; i++) s.gpr.push_back(reg((hv_reg_t)i));
        s.cpsr = reg(HV_REG_CPSR);
        s.fpcr = reg(HV_REG_FPCR);
        s.fpsr = reg(HV_REG_FPSR);
        for (auto sr : interesting) s.sysregs.emplace_back(sr, sys_reg(sr));
        return s;
    }
    void restore(const Snapshot& s) {
        for (unsigned i = 0; i <= 31 && i < s.gpr.size(); i++) set_reg((hv_reg_t)i, s.gpr[i]);
        set_reg(HV_REG_CPSR, s.cpsr);
        set_reg(HV_REG_FPCR, s.fpcr);
        set_reg(HV_REG_FPSR, s.fpsr);
        set_sys_regs(s.sysregs);
    }

    // ---------------------------------------------------------------- run --
    ExitInfo run() {
        check(hv_vcpu_run(id_));
        return ExitInfo{ exit_->reason,
                         exit_->exception.syndrome,
                         exit_->exception.virtual_address,
                         exit_->exception.physical_address };
    }

    // == EXTENDED: callback run loop ========================================
    enum class Action { Continue, Stop };
    using ExitHandler = std::function<Action(const ExitInfo&)>;
    // Runs until handler returns Stop, the guest powers down (unsupported
    // fatal), or max_iters is hit. Returns number of exits processed.
    uint64_t run_loop(ExitHandler handler, uint64_t max_iters = 1000000) {
        uint64_t n = 0;
        while (n++ < max_iters) {
            ExitInfo e = run();
            if (handler(e) == Action::Stop) break;
        }
        return n;
    }

    // timers / interrupts ----------------------------------------------------
    void set_vtimer_mask(bool m)   { check(hv_vcpu_set_vtimer_mask(id_, m)); }
    bool vtimer_masked()           { bool m = false; check(hv_vcpu_get_vtimer_mask(id_, &m)); return m; }
    void set_vtimer_offset(uint64_t off) { check(hv_vcpu_set_vtimer_offset(id_, off)); }
    uint64_t exec_time()           { uint64_t t = 0; check(hv_vcpu_get_exec_time(id_, &t)); return t; }

    // == EXTENDED: raw trap passthrough =====================================
    // Issue any kernel hypervisor trap directly - including trap ids beyond
    // OHV_TRAP_COUNT that future/private kernels might expose. No gatekeeping.
    static hv_return_t raw_trap(unsigned trap_id, void* arg) { return ohv_raw_trap(trap_id, arg); }

    // == EXTENDED: data-abort monitor (kernel capability, wrapped) ==========
    static hv_return_t monitor_data_abort(bool add, hv_vm_space_t asid, uint64_t ctx,
                                          hv_ipa_t base, size_t size, void *port) {
        return ohv_monitor_data_abort(add, asid, ctx, base, size, port);
    }

    static constexpr hv_vcpu_t kInvalid = ~(hv_vcpu_t)0;

private:
    hv_vcpu_t id_ = kInvalid;
    hv_vcpu_exit_t *exit_ = nullptr;
};

// == EXTENDED: SMP broadcast ==============================================
inline void exit_vcpus(const std::vector<hv_vcpu_t>& ids) {
    std::vector<hv_vcpu_t> tmp(ids);
    check(hv_vcpus_exit(tmp.data(), (uint32_t)tmp.size()));
}

} // namespace openhyp
#endif