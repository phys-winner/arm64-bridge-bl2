#pragma once

#include <cstdint>

class DynarmicHost;

// Opt-in pinned A32 ASIMD replacements. Startup debug.bl2.convex_fastpath:
// bit1 = plain box/outcode, bit2 = contained box, bit4 = plain sphere.
// Missing property uses startupMask; explicit0..7 overrides it. Mask0 installs
// nothing. Every installed body must pass real guest/native/
// patched differential checks before game execution; default stays disabled.
bool InstallBl2ConvexFastpath(DynarmicHost& host, uint32_t startupMask = 0);

// Changes only already installed bits; uninstalled bits remain original.
void SetBl2ConvexMask(uint32_t mask);
