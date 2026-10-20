#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace bl2::lan {
constexpr std::uint32_t kImageSize = 0x03089000;
constexpr std::uint32_t kPlatformFunctionRva = 0x00FE4258;
constexpr std::uint32_t kPlatformGotRva = 0x02F12BAC;
constexpr std::uint32_t kCompatiblePlatformMask = 0x241;

struct Patch { std::uint32_t rva, original, replacement; };
// Advertise and accept PC bits 0x41 plus Android 0x200 only in LAN packets.
constexpr std::array<Patch, 5> kPatches{{
    {0x01C8525C, 0xE12FFF33, 0xE3000241},
    {0x01C85B78, 0xE12FFF33, 0xE3000241},
    {0x01F90B08, 0xE12FFF33, 0xE3000241},
    {0x01C840EC, 0xE59520E0, 0xE3002241},
    {0x01C84250, 0xE59530E0, 0xE3003241},
}};
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 7> kContext{{
    {0x00FE4258, 0xE3A00C02}, {0x00FE425C, 0xE12FFF1E},
    {0x01C85258, 0xE7983002}, {0x01C85B74, 0xE7963002},
    {0x01F90B04, 0xE7953002},
    {0x01C840FC, 0xE1110002}, {0x01C84260, 0xE1120003},
}};
constexpr std::array<std::pair<const char*, std::uint32_t>, 6> kSymbols{{
    {"_Z18appGetPlatformTypev", 0x00FE4258},
    {"_ZN24UOnlineGameInterfaceImpl21IsValidLanQueryPacketEPKhmRy", 0x01C84084},
    {"_ZN24UOnlineGameInterfaceImpl24IsValidLanResponsePacketEPKhm", 0x01C841F0},
    {"_ZN24UOnlineGameInterfaceImpl12FindLanGamesEv", 0x01C85164},
    {"_ZN24UOnlineGameInterfaceImpl16ProcessLanPacketEPhi", 0x01C85AF0},
    {"_ZN30UOnlineGameInterfaceSteamworks16ProcessLanPacketEPhi", 0x01F90748},
}};
} // namespace bl2::lan

template<class Host>
bool InstallBl2LanDiscoveryCompat(Host& host) {
    using namespace bl2::lan;
    auto& loader = host.GetElfLoader();
    const auto base = loader.GetBaseAddress();
    if (loader.GetImageSize() != kImageSize || base > UINT32_MAX - kImageSize) return false;
    for (const auto& [symbol, rva] : kSymbols)
        if (loader.GetSymbolAddress(symbol) != base + rva) return false;
    for (const auto& [rva, word] : kContext)
        if (!loader.IsReadOnlyRange(base + rva, 4) || host.MemoryRead32(base + rva) != word) return false;
    if (host.MemoryRead32(base + kPlatformGotRva) != base + kPlatformFunctionRva) return false;
    for (const auto& patch : kPatches) {
        if (!loader.IsReadOnlyRange(base + patch.rva, 4)) return false;
        const auto word = host.MemoryRead32(base + patch.rva);
        if (word != patch.original && word != patch.replacement) return false;
    }
    for (const auto& patch : kPatches) {
        host.MemoryWrite32(base + patch.rva, patch.replacement);
        host.InvalidateGuestCode(base + patch.rva, 4);
        if (host.MemoryRead32(base + patch.rva) != patch.replacement) return false;
    }
    return true;
}
