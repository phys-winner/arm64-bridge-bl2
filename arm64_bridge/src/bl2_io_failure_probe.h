#pragma once

#include <cstddef>
#include <cstdint>

class DynarmicHost;
namespace Dynarmic::A32 { class Jit; }

// Call once on the raw ELF, before relocation and before guest worker creation.
// Disabled unless debug.bl2.io_failure_probe=1 was set before launch.
// Failure disables only this diagnostic; it never rejects the guest load.
bool VerifyBl2IoFailureProbeInput(const std::uint8_t* bytes, std::size_t size);

// Call at the entry of the existing __android_log_print thunk, before formatting
// or changing r0. No guest hook, callback, register change, or guest write.
// Emits at most twelve failure events per verified raw load; preserves errno.
void ObserveBl2IoFailureLog(Dynarmic::A32::Jit& jit, DynarmicHost& host);
