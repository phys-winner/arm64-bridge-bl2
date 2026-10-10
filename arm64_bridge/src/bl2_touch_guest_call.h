#pragma once
#include "dynarmic_host.h"

namespace bl2::touch {

// A real zero return is successful. A64 queries must finish on an owned core;
// the legacy callback's deferred zero cannot stand in for an engine result.
inline bool Bl2TouchCallGuest(DynarmicHost& host, std::uint32_t pc,
                             const std::vector<std::uint32_t>& args, std::uint32_t& result) {
#if defined(BL2_NATIVE_ARM32)
    result = host.RunGuestCallback(pc, args);
    return true;
#else
    return host.CallGuest(pc, args, result);
#endif
}

inline bool Bl2TouchCallGuest(DynarmicHost& host, std::uint32_t pc,
                             const std::vector<std::uint32_t>& args) {
    std::uint32_t ignored = 0;
    return Bl2TouchCallGuest(host, pc, args, ignored);
}

} // namespace bl2::touch
