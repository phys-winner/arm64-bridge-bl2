#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"

namespace bl2::touch {
// This admits axes, not actions. The original injured Pawn state still owns
// crawl/immobile GroundSpeed and the injured view sensitivity/pitch clamp.
inline bool Bl2TouchGameplayAxes(Mode mode, std::uint32_t movie,
                                std::size_t contextCount, std::string_view name) {
    if (mode != Mode::Gameplay || movie || contextCount != 1) return false;
    if (name == "InputContext_OnFoot") return true;
    return name == "InputContext_Injured" || name == "InputContext_InjuredMoving";
}
} // namespace bl2::touch

class DynarmicHost;

// Loader thread only. Verify the immutable file bytes before ELF relocation.
bool VerifyBl2TouchInput(const std::uint8_t* bytes, std::size_t size);

std::uint64_t Bl2TouchLifetimeGeneration();

// Before guest JNI_OnLoad or worker creation. Default is off. The explicit
// debug.bl2.native_touch=observe mode installs an input-boundary context probe.
// This first integration stage publishes NO input capabilities and must not
// replace LaunchControls. It never writes UObject fields or invokes actions.
bool InstallBl2TouchRuntime(DynarmicHost& host);
