#include "../src/bl2_weapon_ammo.h"
#include <initializer_list>

constexpr bool CheckAmmoSources() {
    for (unsigned flags = 0; flags != 8; ++flags) {
        const bool clip = flags & 1, resource = flags & 2, alternate = flags & 4;
        if (Bl2WeaponAmmoCount(clip, 17, resource, 31, alternate, 53) !=
            (clip ? 17u : resource ? 31u : alternate ? 53u : 0u)) return false;
    }
    for (const auto count : {0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
        if (Bl2WeaponAmmoCount(true, count, true, 31, true, 53) != count ||
            Bl2WeaponAmmoCount(false, 17, true, count, true, 53) != count ||
            Bl2WeaponAmmoCount(false, 17, false, 31, true, count) != count ||
            Bl2WeaponAmmoCount(false, count, false, count, false, count) != 0) return false;
    }
    return true;
}
static_assert(CheckAmmoSources());

int main() { return 0; }
