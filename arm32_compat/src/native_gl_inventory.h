#pragma once
#include <string_view>

namespace bl2 {
constexpr bool IsGraphicsOperation(std::string_view name) {
    const auto prefix = name.starts_with("egl") ? 3u : name.starts_with("gl") ? 2u : 0u;
    return prefix && name.size() > prefix && name[prefix] >= 'A' && name[prefix] <= 'Z';
}
}

#if BL2_NATIVE_PERF_PROBE
#include <android/log.h>
#include <sys/system_properties.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

inline void NativeGlInventory(unsigned id, const char* name, const std::uint32_t* registers) {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        return __system_property_get("debug.bl2.gl_inventory", value) == 1 && value[0] == '1';
    }();
    if (!enabled || !bl2::IsGraphicsOperation(name)) return;
    static std::array<std::atomic<unsigned>, 2048> observed{};
    if (id >= observed.size()) std::abort();
    const auto count = observed[id].fetch_add(1, std::memory_order_relaxed);
    if (count >= 4) return;
    __android_log_print(ANDROID_LOG_INFO, "BL2GlInventory",
        "op=%s occurrence=%u r0=%08x r1=%08x r2=%08x r3=%08x",
        name, count + 1, registers[0], registers[1], registers[2], registers[3]);
}
#else
inline void NativeGlInventory(unsigned, const char*, const std::uint32_t*) {}
#endif
