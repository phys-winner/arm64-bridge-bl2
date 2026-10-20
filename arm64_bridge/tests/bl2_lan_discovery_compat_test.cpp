#include "bl2_lan_discovery_compat.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>

struct Loader {
    std::uint32_t base = 0x40000000, size = bl2::lan::kImageSize;
    bool symbols = true, readonly = true;
    std::uint32_t GetBaseAddress() const { return base; }
    std::uint32_t GetImageSize() const { return size; }
    bool IsReadOnlyRange(std::uint32_t, std::size_t) const { return readonly; }
    std::uint32_t GetSymbolAddress(const char* name) const {
        for (const auto& [symbol, rva] : bl2::lan::kSymbols)
            if (symbols && std::strcmp(name, symbol) == 0) return base + rva;
        return 0;
    }
};
struct Host {
    Loader loader;
    std::map<std::uint32_t, std::uint32_t> memory;
    unsigned writes = 0, invalidations = 0;
    bool corruptWrite = false;
    Host() {
        for (const auto& [rva, word] : bl2::lan::kContext) memory[loader.base + rva] = word;
        for (const auto& patch : bl2::lan::kPatches) memory[loader.base + patch.rva] = patch.original;
        memory[loader.base + bl2::lan::kPlatformGotRva] = loader.base + bl2::lan::kPlatformFunctionRva;
    }
    Loader& GetElfLoader() { return loader; }
    std::uint32_t MemoryRead32(std::uint32_t address) { return memory.at(address); }
    void MemoryWrite32(std::uint32_t address, std::uint32_t value) { ++writes; memory[address] = value ^ (corruptWrite ? 1 : 0); }
    void InvalidateGuestCode(std::uint32_t, std::size_t bytes) { assert(bytes == 4); ++invalidations; }
};

int main() {
    Host valid;
    const auto before = valid.memory;
    assert(InstallBl2LanDiscoveryCompat(valid) && valid.writes == 5 && valid.invalidations == 5);
    unsigned changed = 0;
    for (const auto& [address, word] : valid.memory) changed += word != before.at(address);
    assert(changed == 5);
    assert(valid.memory.at(valid.loader.base + bl2::lan::kPlatformFunctionRva) == 0xE3A00C02);
    assert(InstallBl2LanDiscoveryCompat(valid));
    for (const auto& patch : bl2::lan::kPatches) {
        Host rejected;
        rejected.memory[rejected.loader.base + patch.rva] ^= 1;
        assert(!InstallBl2LanDiscoveryCompat(rejected) && rejected.writes == 0);
    }
    for (const auto& [rva, word] : bl2::lan::kContext) {
        Host rejected;
        rejected.memory[rejected.loader.base + rva] = word ^ 1;
        assert(!InstallBl2LanDiscoveryCompat(rejected) && rejected.writes == 0);
    }
    for (unsigned failure = 0; failure < 5; ++failure) {
        Host rejected;
        if (failure == 0) rejected.loader.size -= 4096;
        if (failure == 1) rejected.loader.symbols = false;
        if (failure == 2) rejected.loader.readonly = false;
        if (failure == 3) rejected.memory[rejected.loader.base + bl2::lan::kPlatformGotRva] += 4;
        if (failure == 4) rejected.loader.base = UINT32_MAX - 4095;
        assert(!InstallBl2LanDiscoveryCompat(rejected) && rejected.writes == 0);
    }
    Host corrupt;
    corrupt.corruptWrite = true;
    assert(!InstallBl2LanDiscoveryCompat(corrupt));
    std::puts("PASS: exactly five LAN words, idempotence, all patch/context rejects before mutation, symbol/image/range/GOT guards, write verification; global platform function unchanged");
}
