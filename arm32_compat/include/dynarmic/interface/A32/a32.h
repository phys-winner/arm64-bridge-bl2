#pragma once
#include <array>
#include <cstdint>
#include <cstdlib>

namespace Dynarmic {
enum class HaltReason { UserDefined1, MemoryAbort };
namespace A32 {
// The shared compatibility handlers consume an AAPCS register frame. Native
// ARM32 supplies the real registers; no instruction translator is involved.
class Jit {
public:
    std::array<std::uint32_t, 16>& Regs() { return regs_; }
    const std::array<std::uint32_t, 16>& Regs() const { return regs_; }
    std::array<std::uint32_t, 64>& ExtRegs() { return ext_; }
    const std::array<std::uint32_t, 64>& ExtRegs() const { return ext_; }
    std::uint32_t Cpsr() const { return cpsr_; }
    void SetCpsr(std::uint32_t value) { cpsr_ = value; }
    std::uint32_t Fpscr() const { return fpscr_; }
    void SetFpscr(std::uint32_t value) { fpscr_ = value; }
    // Each dispatch owns its register frame, including nested callbacks. The
    // immutable token identifies the actual native engine thread across them.
    static std::uintptr_t NativeCoreIdentity() {
        static thread_local const std::uint8_t identity = 0;
        return reinterpret_cast<std::uintptr_t>(&identity);
    }
    void HaltExecution(HaltReason) { std::abort(); }
private:
    std::array<std::uint32_t, 16> regs_{};
    std::array<std::uint32_t, 64> ext_{};
    std::uint32_t cpsr_ = 0, fpscr_ = 0;
};
}
}
