#include "bl2_performance_settings.h"
#include "bl2_touch_settings.h"
#include "bl2_convex_fastpath.h"
#include "bl2_vertex_element_fastpath.h"
#include "bl2_program_locals_fastpath.h"
#include "bl2_limiter_peak_fastpath.h"
#include "bl2_audio_decode_fastpath.h"
#include <android/log.h>
#include <sys/system_properties.h>
#include <chrono>
#include <cstring>
#include <cstdlib>
// Settings 32 onward are these replacements, then frame pacing and Reload.
static_assert(bl2::touch::kTouchFramePacingSetting == 32+kBl2PerformanceSettingBits.size() &&
              bl2::touch::kTouchReloadButtonSetting == bl2::touch::kTouchFramePacingSetting+1 &&
              bl2::touch::kTouchSettingCount == bl2::touch::kTouchReloadButtonSetting+1);

void SyncBl2PerformanceSettings() {
    // Saved choices and an optional QA override at an owned input boundary.
    static auto next=std::chrono::steady_clock::time_point{};
    static std::uint32_t previous=UINT32_MAX;
    const auto now=std::chrono::steady_clock::now();
    if(now<next)return;
    next=now+std::chrono::milliseconds(500);
    std::uint32_t mask=kBl2PerformanceDefaults;
    {
        auto& settings=bl2::touch::touchSettingsState;
        std::lock_guard<std::mutex> lock(settings.mutex);
        if(settings.initialized) {
            mask=0;
            for(std::size_t i=0;i<kBl2PerformanceSettingBits.size();++i)
                if(settings.values[32+i])mask|=kBl2PerformanceSettingBits[i];
        }
    }
    char property[PROP_VALUE_MAX]{};
    if(__system_property_get("debug.bl2.perf_mask",property) && property[0]>='0' && property[0]<='9') {
        char* end=nullptr;const auto parsed=std::strtoul(property,&end,10);
        if(end && !*end && parsed<=255)mask=static_cast<std::uint32_t>(parsed);
    }
    if(mask==previous)return;
    previous=mask;
    SetBl2ConvexMask(mask&7);
    SetBl2SetupVertexArraysEnabled((mask&8)!=0);
    SetBl2ProgramLocalsEnabled((mask&16)!=0);
    SetBl2LimiterPeakEnabled((mask&32)!=0);
    SetBl2AudioDecodeMask((mask>>6)&3);
    if(__system_property_get("debug.bl2.perf_tests",property)==1 && property[0]=='1')
        __android_log_print(ANDROID_LOG_INFO,"BL2PerfCheck","active performance mask=%u",mask);
}
