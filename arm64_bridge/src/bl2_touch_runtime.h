#pragma once

#include <cstddef>
#include <cstdint>

class DynarmicHost;

// Loader thread only. Verify the immutable file bytes before ELF relocation.
bool VerifyBl2TouchInput(const std::uint8_t* bytes, std::size_t size);

// Before guest JNI_OnLoad or worker creation. Default is off. The explicit
// debug.bl2.native_touch=observe mode installs an input-boundary context probe.
// This first integration stage publishes NO input capabilities and must not
// replace LaunchControls. It never writes UObject fields or invokes actions.
bool InstallBl2TouchRuntime(DynarmicHost& host);
