#include "bl2_touch_physical.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include <algorithm>
#include <cmath>
#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace {
thread_local unsigned legacyPacketDepth = 0;
thread_local unsigned touchDispatchDepth = 0;
}

namespace bl2::touch {

PhysicalOwnership::Controller* PhysicalOwnership::controllerLocked(int id) {
    for (auto& item : controllers_) if (item.used && item.id == id) return &item;
    for (auto& item : controllers_) if (!item.used) { item.used = true; item.id = id; return &item; }
    unknownController_ = true;
    return nullptr;
}
void PhysicalOwnership::activityLocked(std::uint64_t now) { seen_ = true; lastActivity_ = now; }
bool PhysicalOwnership::keyboardKey(int controller, int code) {
    // UE3JavaApp passes InputDevice.getControllerNumber(): 0 for anything that
    // is not a game controller. KEYCODE_BUTTON_A..MODE and BUTTON_1..16 stay
    // gamepad buttons even from such a device.
    return controller == 0 && !(code >= 96 && code <= 110) && !(code >= 188 && code <= 203);
}
void PhysicalOwnership::key(int controller, int action, int code, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool keyboard = keyboardKey(controller, code);
    if (keyboard) { keyboardSeen_ = true; keyboardActivity_ = now; }
    else activityLocked(now);
    auto& held = keyboard ? keyboardKeys_ : keys_;
    const auto key = std::make_pair(controller, code);
    if (action == 0) {
        if (held.size() < 256) held.insert(key);
        else unknownController_ = true;
    } else if (action == 1) held.erase(key);
    else unknownController_ = true; // Unknown action must not admit conflicting touch.
}
void PhysicalOwnership::motion(int id, const std::array<float, 8>& axes, std::uint64_t now) {
    bool deflected = false;
    for (float axis : axes) deflected |= !std::isfinite(axis) || std::abs(axis) > 0.025f;
    std::lock_guard<std::mutex> lock(mutex_);
    auto* controller = controllerLocked(id);
    if (!controller) { activityLocked(now); return; }
    if (deflected || controller->axes) activityLocked(now);
    controller->axes = deflected;
}
void PhysicalOwnership::mouse(int id, int left, int right, int wheel, std::uint64_t now) {
    (void)wheel;
    std::lock_guard<std::mutex> lock(mutex_);
    activityLocked(now); // Cursor movement itself transfers ownership.
    auto* controller = controllerLocked(id);
    if (!controller) return;
    const auto transition = [&](int state, unsigned bit) {
        if (state == 1) controller->mouse |= bit;
        else if (state == 2) controller->mouse &= ~bit;
        else if (state != 0) unknownController_ = true;
    };
    transition(left, 1); transition(right, 2);
}
void PhysicalOwnership::reset(std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    controllers_ = {}; keys_.clear(); keyboardKeys_.clear(); unknownController_ = false;
    // Preserve a short neutral interval across interruption; never synthesize
    // key-up into the guest or release physical engine state.
    activityLocked(now);
}
void PhysicalOwnership::disconnected(int id, std::uint64_t now) {
    std::lock_guard<std::mutex> lock(mutex_);
    // UE3JavaApp reports every device change (pointer capture reconfigures a
    // mouse) as disconnect + connect. Only a device that still held a key, an
    // axis or a button owned input; releasing it keeps the neutral interval.
    bool held = false, typed = false;
    for (auto& controller : controllers_) if (controller.used && controller.id == id) {
        held |= controller.axes || controller.mouse;
        controller = {};
    }
    for (auto key = keys_.begin(); key != keys_.end();) {
        if (key->first == id) { key = keys_.erase(key); held = true; } else ++key;
    }
    for (auto key = keyboardKeys_.begin(); key != keyboardKeys_.end();) {
        if (key->first == id) { key = keyboardKeys_.erase(key); typed = true; } else ++key;
    }
    if (held) activityLocked(now);
    if (typed) keyboardActivity_ = now;
}
bool PhysicalOwnership::ownsLocked(std::uint64_t now) const {
    if (unknownController_ || !keys_.empty()) return true;
    for (const auto& controller : controllers_) if (controller.axes || controller.mouse) return true;
    return seen_ && (now < lastActivity_ || now - lastActivity_ < kNeutralDelayMs);
}
bool PhysicalOwnership::keyboardLocked(std::uint64_t now) const {
    return !keyboardKeys_.empty() || (keyboardSeen_ &&
        (now < keyboardActivity_ || now - keyboardActivity_ < kNeutralDelayMs));
}
bool PhysicalOwnership::ownsInput(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ownsLocked(now);
}
bool PhysicalOwnership::keyboardActive(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return keyboardLocked(now);
}
bool PhysicalOwnership::anyPhysical(std::uint64_t now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ownsLocked(now) || keyboardLocked(now);
}
PhysicalOwnership& physicalOwnership() { static PhysicalOwnership state; return state; }

TouchDispatchScope::TouchDispatchScope() { ++touchDispatchDepth; }
TouchDispatchScope::~TouchDispatchScope() { --touchDispatchDepth; }
bool TouchDispatchScope::active() { return touchDispatchDepth != 0; }

} // namespace bl2::touch

void NotifyBl2TouchPhysicalKey(int controller, int action, int keyCode) {
    if (legacyPacketDepth) return;
    bl2::touch::physicalOwnership().key(controller, action, keyCode, bl2::touch::monotonicMilliseconds());
}
void NotifyBl2TouchPhysicalMotion(int controller, const std::array<float, 8>& axes) {
    if (legacyPacketDepth) return;
    bl2::touch::physicalOwnership().motion(controller, axes, bl2::touch::monotonicMilliseconds());
}
void NotifyBl2TouchPhysicalMouse(int controller, int left, int right, int wheel) {
    if (legacyPacketDepth) return;
    bl2::touch::physicalOwnership().mouse(controller, left, right, wheel, bl2::touch::monotonicMilliseconds());
}
void ResetBl2TouchPhysicalOwnership() {
    bl2::touch::physicalOwnership().reset(bl2::touch::monotonicMilliseconds());
}
void NotifyBl2TouchPhysicalDevice(int controller, bool connected) {
    if (!legacyPacketDepth && !connected)
        bl2::touch::physicalOwnership().disconnected(controller, bl2::touch::monotonicMilliseconds());
}
void BeginBl2TouchLegacyPacket() { if (legacyPacketDepth < 32) ++legacyPacketDepth; }
void EndBl2TouchLegacyPacket() { if (legacyPacketDepth) --legacyPacketDepth; }

#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchSources_nativeBeginLegacy(JNIEnv*, jclass) {
    BeginBl2TouchLegacyPacket();
}
extern "C" JNIEXPORT void JNICALL
Java_com_tt2kgames_Borderlands2_touch_TouchSources_nativeEndLegacy(JNIEnv*, jclass) {
    EndBl2TouchLegacyPacket();
}
#endif
