#pragma once
#include "bl2_touch_ui_model.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bl2::touch {
#ifndef BL2_ENABLE_DEBUG_MENU_DEFAULT
#define BL2_ENABLE_DEBUG_MENU_DEFAULT 0
#endif
constexpr std::uint32_t kTouchSettingsVersion = 8;
constexpr std::size_t kTouchSettingCount = 37;
// Keep the v8 wire slot for older preference files; Motion Blur stays disabled.
constexpr std::size_t kTouchMotionBlurSetting = 22;
// Present pacing (bl2_frame_pacer). Its Video row is commented out by user
// request and the pacer stays off; the wire field is kept unchanged.
constexpr std::size_t kTouchFramePacingSetting = 34;
// Java shows Reload while a reload is possible; ON by default.
constexpr std::size_t kTouchReloadButtonSetting = 35;
// A short tap on More keeps the radial open for a second tap; OFF by default.
constexpr std::size_t kTouchRadialTapSetting = 36;
constexpr std::int32_t kTouchSettingsOpen = 0x6A100000;
constexpr std::int32_t kTouchSettingsGraphicsOpen = 0x6A100004;
constexpr std::int32_t kTouchSettingsValue = 0x6A100100;
constexpr std::int32_t kTouchSettingsBack = 0x6A100200;
constexpr std::int32_t kTouchSettingsEdit = 0x6A100201;
constexpr std::int32_t kTouchSettingsReset = 0x6A100202;
constexpr std::int32_t kTouchSettingsCalibrate = 0x6A100204;
constexpr std::int32_t kTouchSettingsNativeBack = 0x6A100206;
struct TouchSettingSpec { const char* caption; double minimum, maximum, step; bool toggle; };
inline constexpr TouchSettingSpec touchSettingSpecs[] = {
    {"RENDER RESOLUTION (%)",20,150,10,false}, {"BUTTON OPACITY",.15,.9,.05,false},
    {"CONTROL SIZE",.85,1.4,.05,false}, {"LOOK SENSITIVITY",.04,1,.02,false},
    {"AIM SENSITIVITY",.1,1,.05,false}, {"LEFT HANDED",0,1,1,true},
    {"INVERT LOOK Y",0,1,1,true}, {"AUTORUN LATCH",0,1,1,true},
    {"MICROPHONE",0,1,1,true}, {"TILT AIMING",0,1,1,true},
    {"TILT ONLY IN AIM",0,1,1,true}, {"TILT SENSITIVITY",.25,3,.05,false},
    {"TILT DEAD ZONE",0,3,.1,false}, {"INVERT TILT Y",0,1,1,true},
    {"HAPTIC FEEDBACK",0,1,1,true}, {"TAP TO AIM",0,1,1,true},
    {"SHOW DEBUG MENU",0,1,1,true}, {"INTERFACE SCALE (%)",100,200,10,false},
    {"SHOW FPS",0,1,1,true}, {"FPS LIMIT",0,90,30,false}, {"POTATO GRAPHICS",0,1,1,true},
    {"BLOOM",0,1,1,true}, {"MOTION BLUR",0,1,1,true}, {"LENS FLARES",0,1,1,true},
    {"DYNAMIC SHADOWS",0,1,1,true}, {"LIGHT ENVIRONMENT SHADOWS",0,1,1,true},
    {"STATIC DECALS",0,1,1,true}, {"UNBATCHED DECALS",0,1,1,true}, {"LIGHT SHAFTS",0,1,1,true},
    {"DISTORTION",0,1,1,true}, {"RADIAL BLUR",0,1,1,true}, {"ANISOTROPIC FILTERING",0,4,1,false},
    {"FASTER VISIBILITY TESTS",0,1,1,true}, {"FASTER AUDIO LIMITER",0,1,1,true},
    {"FRAME PACING",0,1,1,true}, {"RELOAD BUTTON",0,1,1,true}, {"RADIAL BY TAP",0,1,1,true}
};
static_assert(std::size(touchSettingSpecs) == kTouchSettingCount);
inline const char* TouchSettingCaption(std::size_t id, bool russian) {
    static constexpr const char* captions[] = {
        "РАЗРЕШЕНИЕ РЕНДЕРА (%)", "НЕПРОЗРАЧНОСТЬ КНОПОК", "РАЗМЕР КНОПОК", "СКОРОСТЬ ОБЗОРА",
        "СКОРОСТЬ ПРИЦЕЛА", "РЕЖИМ ДЛЯ ЛЕВШЕЙ", "ИНВЕРСИЯ ОБЗОРА Y", "ФИКСАЦИЯ АВТОБЕГА",
        "МИКРОФОН", "ПРИЦЕЛИВАНИЕ НАКЛОНОМ", "ТОЛЬКО В ПРИЦЕЛЕ", "СКОРОСТЬ НАКЛОНА",
        "МЁРТВАЯ ЗОНА", "ИНВЕРСИЯ НАКЛОНА Y", "ВИБРООТКЛИК", "ПРИЦЕЛ ПО НАЖАТИЮ",
        "ПУНКТ DEBUG В ПАУЗЕ", "МАСШТАБ ИНТЕРФЕЙСА (%)", "ПОКАЗЫВАТЬ FPS", "ЛИМИТ FPS",
        "КАРТОФЕЛЬНАЯ ГРАФИКА", "СВЕЧЕНИЕ", "РАЗМЫТИЕ ДВИЖЕНИЯ", "БЛИКИ ЛИНЗ",
        "ДИНАМИЧЕСКИЕ ТЕНИ", "ТЕНИ ОСВЕЩЕНИЯ", "СТАТИЧЕСКИЕ ДЕКАЛИ", "ОТДЕЛЬНЫЕ ДЕКАЛИ",
        "СВЕТОВЫЕ ЛУЧИ", "ИСКАЖЕНИЯ", "РАДИАЛЬНОЕ РАЗМЫТИЕ", "АНИЗОТРОПНАЯ ФИЛЬТРАЦИЯ",
        "УСКОРЕНИЕ ОТСЕЧЕНИЯ ОБЪЕКТОВ", "УСКОРЕНИЕ АУДИОЛИМИТЕРА", "РОВНЫЙ ТЕМП КАДРОВ",
        "КНОПКА ПЕРЕЗАРЯДКИ", "РАДИАЛ ПО ТАПУ"
    };
    static_assert(std::size(captions) == kTouchSettingCount);
    return russian ? captions[id] : touchSettingSpecs[id].caption;
}
inline const char* TouchSettingHint(std::size_t id, bool russian) {
    if (id == 32) return russian ?
        "Ускоряет штатные проверки видимости. Дальность прорисовки и качество изображения сохраняются." :
        "Speeds up the original visibility tests, preserving draw distance and image quality.";
    if (id == 33) return russian ?
        "Ускоряет анализ пиков звука и снижает загрузку CPU, сохраняя качество звука." :
        "Speeds up audio peak analysis, reducing CPU use and preserving sound quality.";
    if (id == 20) return russian ?
        "Упрощает графику ниже штатного минимума, сохраняя разрешение рендера. Выключение возвращает выбранные настройки." :
        "Reduces graphics below the normal minimum, preserving render resolution. Switching off restores your selected settings.";
    if (id == kTouchFramePacingSetting) return russian ?
        "Каждый кадр на экране одинаковое время: без рывков, но FPS округляется вниз (на 120 Гц: 60, 40, 30, 24, 20)." :
        "Every frame stays on screen equally long: no judder, but FPS rounds down (at 120 Hz: 60, 40, 30, 24, 20).";
    if (id == kTouchReloadButtonSetting) return russian ?
        "Кнопка «Зарядить» появляется, когда магазин неполный и перезарядка ещё не идёт. Тап по полосе патронов перезаряжает и без неё." :
        "Shows Reload while the magazine is not full and no reload is running. Tapping the ammo bar reloads either way.";
    if (id == kTouchRadialTapSetting) return russian ?
        "Короткий тап по «Ещё» оставляет радиал открытым до следующего тапа. Удержание с протяжкой работает как раньше." :
        "A short tap on More keeps the radial open until the next tap. Hold and drag work as before.";
    if (id >= 21 && id < 32) return russian ?
        "Сохраняется отдельно. Картофельная графика временно перекрывает выбранное значение." :
        "Saved separately. Potato graphics temporarily overrides the selected value.";
    if (id == 0) return russian ?
        "Больше — чётче изображение; меньше — быстрее игра. Применяется после перезапуска игры." :
        "Higher values sharpen the image; lower values improve performance. Applied after the game restarts.";
    if (id == 8) return russian ?
        "Микрофон для голосового чата. При первом включении требуется разрешение Android." :
        "Microphone for voice chat. Android permission is required the first time it is enabled.";
    if (id == 7) return russian ?
        "Продолжать бег после отпускания стика. Далеко вверх — включить; вернуть стик или коснуться его — остановить." :
        "Keep running after releasing the stick. Push far upward to latch; pull back or tap the stick to stop.";
    if (id == 16) return russian ? "Показывать пункт Debug в меню паузы." : "Show the Debug entry in the pause menu.";
    if (id == 17) return russian ?
        "Размер меню, HUD и Debug. Разрешение рендера задаётся отдельно." :
        "Size of menus, HUD and Debug. Render resolution is set separately.";
    if (id == 18) return russian ? "Штатный счётчик кадров игры." : "The game's original frame-rate counter.";
    if (id == 19) return russian ?
        "Предел кадров в секунду. ЭКРАН — частота текущего дисплея; фактическая скорость зависит от нагрузки." :
        "Frame-rate limit. DISPLAY uses the current display rate; actual performance depends on workload.";
    return nullptr;
}
inline std::uint32_t TouchFloatBits(float value) { std::uint32_t bits; std::memcpy(&bits,&value,4); return bits; }
inline double TouchSettingNumber(std::size_t id, std::uint32_t bits) {
    if (id == 0 || id == 17 || id == 19 || id >= 20 || touchSettingSpecs[id].toggle) return bits;
    float value; std::memcpy(&value,&bits,4); return value;
}
inline std::string TouchSettingValueText(std::size_t id, double value) {
    if (id >= kTouchSettingCount || !std::isfinite(value)) return {};
    char text[32];
    if (id == 31) {
        if (value < 0 || value > 4 || std::floor(value) != value) return {};
        std::snprintf(text,sizeof(text),"%ux",1u << static_cast<unsigned>(value));
    }
    else if (id == 19) { if (value == 0) return "DISPLAY"; std::snprintf(text,sizeof(text),"%.0f",value); }
    else if (id <= 2 || id == 17) std::snprintf(text,sizeof(text),"%.0f%%",id == 0 || id == 17 ? value : value * 100);
    else std::snprintf(text,sizeof(text),"%.2f",value);
    return text;
}
struct TouchSettingsState {
    std::mutex mutex;
    bool initialized = false, resetPending = false;
    bool videoOptions = false;  // The shown options list is the video page.
    unsigned page = 0;
    std::uint32_t revision = 0, commands = 0, uiRequest = 0;
    std::array<std::uint32_t,kTouchSettingCount> values{};
    std::uint64_t owner = 0, list = 0, provider = 0, nativeStructure = 0;
    std::vector<std::int32_t> nativeEvents, events;
};
inline TouchSettingsState touchSettingsState;
// Host input [version 8, revision acknowledgement, 37 values]; output
// [version 8, revision, command bits, 37 values]. Bits 1-8 are one-shot
// commands; 16 is set while the video options page is shown, so Java can offer
// a restart for a new render resolution once that page closes. Unacknowledged
// native edits win over an older Java snapshot, including the final menu-close poll.
inline std::vector<std::uint32_t> ExchangeBl2TouchSettings(const std::vector<std::uint32_t>& host) {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    bool valid = host.size() == kTouchSettingCount + 2 && host[0] == kTouchSettingsVersion;
    if (valid) for (std::size_t i = 0; i < kTouchSettingCount; ++i) {
        if (i == 16 && host[i + 2] == 0xFFFFFFFFu) continue; // absent preference: build default
        if (i >= 21 && i < 32 && host[i + 2] == 0xFFFFFFFFu) continue; // absent graphics preference: original runtime default
        const auto value = TouchSettingNumber(i,host[i + 2]); const auto& spec = touchSettingSpecs[i];
        if (!std::isfinite(value) || value < spec.minimum - .00001 || value > spec.maximum + .00001 ||
            (spec.toggle && value != 0 && value != 1) || (i == 0 && std::fmod(value,10) != 0) ||
            (i == 17 && value != 125 && std::fmod(value,10) != 0) ||
            (i == 19 && std::fmod(value,30) != 0)) { valid = false; break; }
    }
    if (valid && (!state.initialized || host[1] == state.revision)) {
        const auto previous = state.values;
        const bool initialized = state.initialized;
        if (!state.initialized) state.revision = 1;
        std::copy_n(host.begin() + 2,kTouchSettingCount,state.values.begin()); state.initialized = true;
        state.values[kTouchMotionBlurSetting] = 0;
        if (state.values[16] == 0xFFFFFFFFu) state.values[16] = BL2_ENABLE_DEBUG_MENU_DEFAULT ? 1u : 0u;
        if (initialized && state.values != previous) state.uiRequest |= 4u;
        if (state.resetPending) { state.uiRequest |= 4u; state.resetPending = false; }
    }
    std::vector<std::uint32_t> out{kTouchSettingsVersion,state.revision,
        state.commands | (state.owner && state.videoOptions ? 16u : 0u)};
    state.commands = 0;
    out.insert(out.end(),state.values.begin(),state.values.end()); return out;
}
inline bool Bl2DebugMenuVisible() {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    return state.initialized ? state.values[16] != 0 : BL2_ENABLE_DEBUG_MENU_DEFAULT != 0;
}
inline float Bl2InterfaceScale() {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    return state.initialized ? static_cast<float>(state.values[17]) / 100.f : 1.25f;
}
inline bool Bl2ShowFps() {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    return state.initialized && state.values[18] != 0;
}
inline std::uint32_t Bl2FrameRateLimit() {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    return state.initialized ? state.values[19] : 0u;
}
inline bool Bl2FramePacingEnabled() {
    // FRAME PACING is commented out by user request: always unpaced, whatever
    // value an earlier build saved. Restore the two lines below with the row.
    // auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    // return !state.initialized || state.values[kTouchFramePacingSetting] != 0;
    return false;
}
inline bool SetBl2TouchSetting(std::size_t id, double value) {
    if (id >= kTouchSettingCount || !std::isfinite(value) || (id == kTouchMotionBlurSetting && value != 0)) return false;
    const auto& spec = touchSettingSpecs[id];
    if (value < spec.minimum - .00001 || value > spec.maximum + .00001 || (spec.toggle && value != 0 && value != 1) ||
        (id == 0 && std::fmod(value,10) != 0) || (id == 17 && value != 125 && std::fmod(value,10) != 0) ||
        (id == 19 && std::fmod(value,30) != 0) || (id == 31 && std::floor(value) != value)) return false;
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized) return false;
    if (id == 16) state.commands |= 8u; // explicit edit: preserve intent across dev/release defaults
    const auto bits = (id == 0 || id == 17 || id == 19 || id >= 20 || spec.toggle) ? static_cast<std::uint32_t>(std::round(value)) : TouchFloatBits(static_cast<float>(value));
    if (state.values[id] != bits) { state.values[id] = bits; ++state.revision; if (!state.revision) ++state.revision; }
    return true;
}
inline bool DecorateBl2TouchSettingsModel(UiModelSnapshot& model) {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    if (!model.valid || !state.owner || model.owner != state.owner || model.list != state.list ||
        model.provider != state.provider || model.structure != state.nativeStructure || model.eventIds != state.nativeEvents) return false;
    model.eventIds = state.events;
    model.structure ^= 0x6A105E771A600001ull + state.page;
    return true;
}
inline void RequestBl2TouchSettingsUi(std::uint32_t request) {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex); state.uiRequest |= request;
}
inline void CommandBl2TouchSettings(std::uint32_t commands) {
    auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
    state.commands |= commands; state.resetPending |= (commands & 2u) != 0; ++state.revision;
}
} // namespace bl2::touch
