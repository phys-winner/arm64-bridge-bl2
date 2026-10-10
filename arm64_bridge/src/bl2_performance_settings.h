#pragma once
#include <cstdint>
#include <array>
// Candidate bits: box, containment, sphere, vertex arrays, program constants,
// limiter peak, audio codebook, audio floor. Only measured gains get UI rows.
inline constexpr std::array<std::uint32_t,2> kBl2PerformanceSettingBits{1,32};
inline constexpr std::uint32_t kBl2PerformanceDefaults = 33;
#if defined(BL2_NATIVE_ARM32)
inline void SyncBl2PerformanceSettings() {}
#else
void SyncBl2PerformanceSettings();
#endif
