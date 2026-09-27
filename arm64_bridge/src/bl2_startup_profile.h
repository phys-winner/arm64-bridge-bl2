#pragma once

#include <cstdint>

namespace Dynarmic::A32 { class Jit; }

// Opt-in diagnostic for the black startup interval. Only wait, input-file and
// decompression thunks are timed; rendering/allocation calls are never timed.
// debug.bl2.startup_profile is cached at first dispatch, defaults to disabled.
class Bl2StartupThunkScope {
public:
    Bl2StartupThunkScope(uint32_t id, const char* name,
                         Dynarmic::A32::Jit& jit);
    ~Bl2StartupThunkScope();

    Bl2StartupThunkScope(const Bl2StartupThunkScope&) = delete;
    Bl2StartupThunkScope& operator=(const Bl2StartupThunkScope&) = delete;

private:
    Dynarmic::A32::Jit* jit_ = nullptr;
    void* activity_slot_ = nullptr;
    uint64_t previous_activity_;
    uint64_t previous_activity_call_;
    char name_[48];
    uint64_t wall_start_;
    uint64_t cpu_start_;
    uint32_t id_;
    uint32_t lr_;
    uint32_t arg0_;
    uint32_t arg2_;
};
