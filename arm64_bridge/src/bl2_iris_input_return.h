#pragma once
#include <cstdint>

namespace bl2::iris::activation {
// The hook runs before ProcessInput's PUSH/VPUSH. NativeThunkEntry consumes
// an interworking PC; the bridge/AOT dispatcher consumes a separate T bit.
template<bool Native, class Cpu> inline void ReturnBeforePrologue(Cpu& cpu) {
    const std::uint32_t lr=cpu.Regs()[14];
    if constexpr(Native) cpu.Regs()[15]=lr;
    else {
        cpu.SetCpsr((cpu.Cpsr()&~0x20u)|((lr&1u)<<5));
        cpu.Regs()[15]=lr&~1u;
    }
}
struct WaitBudget {
    std::uint64_t First=0;
    bool Started=false;
    bool Within(std::uint64_t now) {
        if(!Started){First=now;Started=true;}
        return now>=First&&now-First<30000;
    }
};
} // namespace bl2::iris::activation
