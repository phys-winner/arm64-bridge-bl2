#pragma once
#include "bl2_touch_input.h"

namespace bl2::touch {

// ActionName values from shipped Startup.xxx, not console commands or C++ symbols.
// Resolve each against the LIVE input context and its availability rules.
// Returning a name does not invoke an action. A real EngineSink is still required.
constexpr const char* shippedActionName(Action action) {
    switch (action) {
    case Action::Fire: return "Fire";
    case Action::Aim: return "Aim";
    case Action::Jump: return "Jump";
    case Action::Crouch: return "Crouch";
    case Action::Reload: return "Reload";
    case Action::Use: return "Use";
    case Action::Melee: return "Melee";
    case Action::Grenade: return "Grenade";
    case Action::Skill: return "ActionSkill";
    case Action::Sprint: return "Sprint";
    case Action::NextWeapon: return "NextWeapon";
    case Action::PreviousWeapon: return "PreviousWeapon";
    case Action::Inventory: return "Inventory";
    case Action::Map: return "Map";
    case Action::Pause: return "PauseMenu";
    case Action::Chat: return "Say"; // Original OnBegin opens StartTextChat.
    case Action::Skip: return "SkipCinematic";
    case Action::VehicleBrake: return "Handbrake";
    case Action::VehicleBoost: return "Afterburner";
    case Action::VehicleHorn: return "Horn";
    // InputContext_DrivingVehicle / InVehicleTurret / VehicleAttachedRider.
    case Action::ExitVehicle: return "ExitVehicle";
    case Action::SwitchSeats: return "SwitchSeats";
    case Action::DriverFireTurret: return "DriverFireTurret";
    case Action::LookBack: return "LookBack";
    // GFx navigation: derive keys/actions from the focused movie. No fabricated
    // InputActionDefinition named Confirm, Back, or UI_Up exists in this table.
    case Action::LastWeapon: // Original inventory selection, not PreviousWeapon.
    case Action::Back: case Action::Confirm:
    case Action::UiUp: case Action::UiDown: case Action::UiLeft: case Action::UiRight:
        return nullptr;
    }
    return nullptr;
}

} // namespace bl2::touch
