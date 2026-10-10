#pragma once

#include <cstdint>

class DynarmicHost;

// Startup opt-in: debug.bl2.audio_decode, bit 0=codebook, bit 1=floor.
// Zero leaves both original functions untouched. Runtime selection is limited
// to the functions installed at startup, so enabling a new bit needs restart.
bool InstallBl2AudioDecodeFastpaths(DynarmicHost& host, std::uint32_t startup_mask = 0);
void SetBl2AudioDecodeMask(std::uint32_t mask);
