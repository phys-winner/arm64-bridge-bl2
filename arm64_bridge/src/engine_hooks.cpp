#include "engine_hooks.h"

#include "dynarmic_host.h"
#include "elf_loader.h"
#include "../../arm32_compat/src/native_render_nvsi.h"

bool InstallBl2RenderWithoutNvsi(DynarmicHost& host) {
    using namespace bl2::native_render;
    const auto& loader = host.GetElfLoader();
    const auto base = loader.GetBaseAddress();
    if (!host.GetGuestMemoryBase() || base < 0x10000 || (base & 3) ||
        loader.GetImageSize() != kImageBytes || base > UINT32_MAX - kImageBytes ||
        std::uint64_t(base) + kImageBytes > host.GetGuestMemorySize() ||
        host.FindSymbol(kSymbol) != base + kFunctionRva ||
        !loader.IsCodeRange(base + kFunctionRva, kFunctionBytes) ||
        !loader.IsReadOnlyRange(base + kFunctionRva, kFunctionBytes)) return false;
    for (const auto& binding : kBindings)
        if (host.FindSymbol(binding.symbol) != base + binding.targetRva) return false;
    // The same original 43 instructions/literals and three resolved GOT words
    // used by native ARM32 are all checked before any guest code is changed.
    return Apply(base, [&](std::uint32_t address) { return host.MemoryRead32(address); },
        [&](std::uint32_t address, std::uint32_t word) { host.MemoryWrite32(address, word); },
        [&](std::uint32_t address, std::size_t bytes) { host.InvalidateGuestCode(address, bytes); });
}
