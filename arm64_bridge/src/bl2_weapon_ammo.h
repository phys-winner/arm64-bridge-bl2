#pragma once

#include <cstdint>

// GetAmmoCount supplies InvManager.bInfiniteAmmo and ClipSize before the
// resource/overheat counts; the former TSEC replacement discarded that pair.
constexpr std::uint32_t Bl2WeaponAmmoCount(bool infiniteAmmo, std::uint32_t clipSize,
                                         bool resource, std::uint32_t resourceCount,
                                         bool alternate, std::uint32_t alternateCount) {
    return infiniteAmmo ? clipSize : resource ? resourceCount : alternate ? alternateCount : 0;
}
