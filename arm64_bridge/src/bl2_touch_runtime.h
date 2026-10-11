#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"

namespace bl2::touch {
enum class Bl2TouchContextKind : std::uint8_t { OnFoot, Vehicle, Cinematic };
struct Bl2TouchContextRule {
    std::string_view name;
    Bl2TouchContextKind kind;
    bool axes;
};
// Every InputContextDefinition the shipped Startup.xxx can select for a local
// player: Globals.InputContexts (7) and CharClass_*.InputContexts (9). A class
// entry replaces the global context for its skill/state (Phaselock, Scorpio,
// Gunzerking, VisionMode, Execute, Mercenary injured) or joins it: Siren OnFoot
// adds SirenOnFoot, which on its own holds neither Fire nor Jump. Execute and
// Cinematic hold no movement set, so they never receive touch axes.
inline constexpr Bl2TouchContextRule kBl2TouchContexts[] = {
    {"InputContext_OnFoot", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_Injured", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_InjuredMoving", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_SirenOnFoot", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_SirenPhaselock", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_SoldierScorpio", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_MercenaryGunzerking", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_MercenaryGunzerkingInjured", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_MercenaryInjured", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_MercenaryInjuredMoving", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_AssassinVisionMode", Bl2TouchContextKind::OnFoot, true},
    {"InputContext_AssassinExecute", Bl2TouchContextKind::OnFoot, false},
    {"InputContext_DrivingVehicle", Bl2TouchContextKind::Vehicle, true},
    {"InputContext_InVehicleTurret", Bl2TouchContextKind::Vehicle, true},
    {"InputContext_VehicleAttachedRider", Bl2TouchContextKind::Vehicle, true},
    {"InputContext_Cinematic", Bl2TouchContextKind::Cinematic, false},
};
inline const Bl2TouchContextRule* Bl2TouchContext(std::string_view name) {
    for (const auto& rule : kBl2TouchContexts) if (rule.name == name) return &rule;
    return nullptr;
}
// Mode of the live context array once no focused movie owns input: one or two
// contexts, every name listed. A cinematic wins over a vehicle, a vehicle over
// on-foot contexts. Anything else stays Unknown and publishes nothing.
inline Mode Bl2TouchContextMode(const std::vector<std::string>& names) {
    if (names.empty() || names.size() > 2) return Mode::Unknown;
    bool cinematic = false, vehicle = false;
    for (const auto& name : names) {
        const auto* rule = Bl2TouchContext(name);
        if (!rule) return Mode::Unknown;
        cinematic |= rule->kind == Bl2TouchContextKind::Cinematic;
        vehicle |= rule->kind == Bl2TouchContextKind::Vehicle;
    }
    return cinematic ? Mode::Cinematic : (vehicle ? Mode::Vehicle : Mode::Gameplay);
}
// Gameplay actions for the classified on-foot contexts. A matched name cannot
// authorize a focused movie or a different mode.
inline bool Bl2TouchGameplayActions(Mode mode, std::uint32_t movie, const std::vector<std::string>& names) {
    return mode == Mode::Gameplay && !movie && Bl2TouchContextMode(names) == Mode::Gameplay;
}
// Cinematic Skip/Pause only: the live array is a listed cinematic and no
// focused movie owns input. Never axes.
inline bool Bl2TouchCinematicActions(Mode mode, std::uint32_t movie, const std::vector<std::string>& names) {
    return mode == Mode::Cinematic && !movie && Bl2TouchContextMode(names) == Mode::Cinematic;
}
// Vehicle seats (DrivingVehicle, InVehicleTurret, VehicleAttachedRider): the
// same resolver and IsActionAvailable over the live seat context while no
// focused movie owns input. Never mixed with on-foot or cinematic routing.
inline bool Bl2TouchVehicleActions(Mode mode, std::uint32_t movie, const std::vector<std::string>& names) {
    return mode == Mode::Vehicle && !movie && Bl2TouchContextMode(names) == Mode::Vehicle;
}
// Seat axes: PlayerDriving.PlayerMove drives with RawJoyUp/RawJoyRight, copied
// from aBaseY/aStrafe before PIProcessInput; look uses the on-foot aTurn seam.
inline bool Bl2TouchVehicleAxes(Mode mode, std::uint32_t movie, const std::vector<std::string>& names) {
    if (!Bl2TouchVehicleActions(mode, movie, names)) return false;
    for (const auto& name : names) if (!Bl2TouchContext(name)->axes) return false;
    return true;
}
// This admits axes, not actions. The original injured Pawn state still owns
// crawl/immobile GroundSpeed and the injured view sensitivity/pitch clamp.
inline bool Bl2TouchGameplayAxes(Mode mode, std::uint32_t movie, const std::vector<std::string>& names) {
    if (!Bl2TouchGameplayActions(mode, movie, names)) return false;
    for (const auto& name : names) if (!Bl2TouchContext(name)->axes) return false;
    return true;
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
