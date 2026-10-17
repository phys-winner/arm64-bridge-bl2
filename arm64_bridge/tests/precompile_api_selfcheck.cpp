#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <unistd.h>

#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/exclusive_monitor.h"

// Keep production diagnostic writers disabled in this standalone check.
extern "C" int __wrap___system_property_get(const char*, char* value) {
    value[0] = '\0';
    return 0;
}

namespace {

constexpr std::uint32_t kEntry = 0x1000;
constexpr std::uint32_t kOtherEntry = kEntry + 16;
unsigned checks = 0;
unsigned failures = 0;

void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}

struct Snapshot {
    std::array<std::uint32_t, 16> regs;
    std::array<std::uint32_t, 64> ext_regs;
    std::uint32_t cpsr;
    std::uint32_t fpscr;
    bool executing;

    bool operator==(const Snapshot&) const = default;
};

Snapshot ReadState(const Dynarmic::A32::Jit& jit) {
    return {jit.Regs(), jit.ExtRegs(), jit.Cpsr(), jit.Fpscr(), jit.IsExecuting()};
}

std::uint64_t Descriptor(const Dynarmic::A32::Jit& jit, std::uint32_t pc) {
    return Dynarmic::A32::LocationDescriptor{
        pc, Dynarmic::A32::PSR{jit.Cpsr()}, Dynarmic::A32::FPSCR{jit.Fpscr()}}.UniqueHash();
}

struct Callbacks final : Dynarmic::A32::UserCallbacks {
    std::array<std::uint32_t, 8> code{
        0xE3A00011, 0xEF000001, 0xEAFFFFFE, 0xEAFFFFFE,
        0xE3A00037, 0xEF000002, 0xEAFFFFFE, 0xEAFFFFFE};
    Dynarmic::A32::Jit* jit = nullptr;
    unsigned code_reads = 0;
    unsigned data_writes = 0;
    unsigned svc_calls = 0;
    unsigned runtime_errors = 0;
    unsigned tick_callbacks = 0;
    bool attempt_reentry = false;
    bool reentry_attempted = false;
    bool reentry_rejected = false;
    bool reentry_preserved_state = false;
    bool reentry_preserved_reads = false;

    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t address) override {
        ++code_reads;
        if (address >= kEntry && address - kEntry <= sizeof(code) - 4) {
            std::uint32_t value = 0;
            std::memcpy(&value, reinterpret_cast<const unsigned char*>(code.data()) +
                                   address - kEntry, sizeof(value));
            return value;
        }
        return std::nullopt;
    }
    std::uint8_t MemoryRead8(std::uint32_t) override { return 0; }
    std::uint16_t MemoryRead16(std::uint32_t) override { return 0; }
    std::uint32_t MemoryRead32(std::uint32_t) override { return 0; }
    std::uint64_t MemoryRead64(std::uint32_t) override { return 0; }
    void MemoryWrite8(std::uint32_t, std::uint8_t) override { ++data_writes; }
    void MemoryWrite16(std::uint32_t, std::uint16_t) override { ++data_writes; }
    void MemoryWrite32(std::uint32_t, std::uint32_t) override { ++data_writes; }
    void MemoryWrite64(std::uint32_t, std::uint64_t) override { ++data_writes; }

    void InterpreterFallback(std::uint32_t, std::size_t) override {
        ++runtime_errors;
        jit->HaltExecution();
    }
    void ExceptionRaised(std::uint32_t, Dynarmic::A32::Exception) override {
        ++runtime_errors;
        jit->HaltExecution();
    }
    void CallSVC(std::uint32_t) override {
        ++svc_calls;
        if (attempt_reentry) {
            const Snapshot before = ReadState(*jit);
            const unsigned reads_before = code_reads;
            reentry_attempted = true;
            reentry_rejected = !jit->Precompile(Descriptor(*jit, kOtherEntry));
            reentry_preserved_state = ReadState(*jit) == before;
            reentry_preserved_reads = code_reads == reads_before;
        }
        jit->HaltExecution();
    }
    void AddTicks(std::uint64_t) override { ++tick_callbacks; }
    std::uint64_t GetTicksRemaining() override {
        ++tick_callbacks;
        return 100000;
    }
};

}  // namespace

int main() {
    alarm(30);
    Callbacks callbacks;
    Dynarmic::ExclusiveMonitor monitor(1);
    Dynarmic::A32::UserConfig config;
    config.callbacks = &callbacks;
    config.global_monitor = &monitor;
    config.processor_id = 0;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.always_little_endian = true;
    config.enable_cycle_counting = false;
    config.code_cache_size = 2 * 1024 * 1024;
    Dynarmic::A32::Jit jit(config);
    callbacks.jit = &jit;

    for (unsigned i = 0; i < jit.Regs().size(); ++i) {
        jit.Regs()[i] = 0x11220000u + i * 0x101u;
    }
    for (unsigned i = 0; i < jit.ExtRegs().size(); ++i) {
        jit.ExtRegs()[i] = 0x77880000u + i * 0x103u;
    }
    jit.Regs()[15] = kEntry;
    jit.SetCpsr(0xA0000000);
    jit.SetFpscr(0x03000011);
    const auto descriptor = Descriptor(jit, kEntry);
    const Snapshot original_state = ReadState(jit);

    Check(jit.Precompile(descriptor), "idle precompile accepted");
    Check(ReadState(jit) == original_state, "all public CPU state preserved");
    Check(callbacks.code_reads > 0, "real IR generation read guest code");
    Check(callbacks.svc_calls == 0 && callbacks.data_writes == 0 &&
              callbacks.runtime_errors == 0 && callbacks.tick_callbacks == 0,
          "precompile executes no guest instruction or tick callback");
    unsigned reads_before = callbacks.code_reads;
    Check(jit.Precompile(descriptor), "repeat descriptor accepted");
    Check(callbacks.code_reads == reads_before, "repeat descriptor reuses actual cache");
    Check(jit.Precompile(descriptor ^ (std::uint64_t{1} << 56)), "different FPSCR key accepted");
    Check(callbacks.code_reads > reads_before, "full raw FPSCR descriptor selects separate block");
    Check(ReadState(jit) == original_state, "alternate descriptor preserves CPU modes");

    jit.HaltExecution(Dynarmic::HaltReason::UserDefined3);
    Check(jit.Precompile(descriptor), "precompile with pending user halt accepted");
    Check(jit.Run() == Dynarmic::HaltReason::UserDefined3 && callbacks.svc_calls == 0,
          "precompile preserves unrelated halt bits");

    callbacks.code[0] = 0xE3A0002A;
    jit.InvalidateCacheRange(kEntry, sizeof(std::uint32_t));
    reads_before = callbacks.code_reads;
    const Snapshot before_invalidation = ReadState(jit);
    Check(jit.Precompile(descriptor), "precompile processes pending range invalidation");
    Check(callbacks.code_reads > reads_before, "modified code emitted after invalidation");
    Check(ReadState(jit) == before_invalidation, "invalidation precompile preserves CPU state");
    Check(jit.Run() == Dynarmic::HaltReason::UserDefined1 && jit.Regs()[0] == 42 &&
              callbacks.svc_calls == 1 && callbacks.runtime_errors == 0,
          "subsequent Run executes modified instruction");

    jit.ClearCache();
    reads_before = callbacks.code_reads;
    Check(jit.Precompile(descriptor), "precompile processes pending full invalidation");
    Check(callbacks.code_reads > reads_before, "full clear recompiles block");

    callbacks.attempt_reentry = true;
    jit.Regs()[15] = kEntry;
    Check(jit.Run() == Dynarmic::HaltReason::UserDefined1, "Run completes after rejected reentry");
    Check(callbacks.reentry_attempted && callbacks.reentry_rejected,
          "active Run rejects public precompile");
    Check(callbacks.reentry_preserved_state && callbacks.reentry_preserved_reads,
          "reentry changes neither state nor cache");
    Check(!jit.IsExecuting() && callbacks.runtime_errors == 0,
          "JIT returns idle without runtime errors");

    std::printf("Precompile API self-check %s: %u checks, %u failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    alarm(0);
    return failures ? 1 : 0;
}
