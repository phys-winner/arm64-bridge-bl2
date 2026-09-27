#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <set>
#include <utility>

namespace bl2::touch {

// Host input ownership only. These methods never invoke guest functions.
// Existing JNI arguments are controller, Android action, key code for Key;
// controller + eight normalized axes for Motion; controller, left/right
// transitions (0 unchanged, 1 down, 2 up), wheel for Mouse.
class PhysicalOwnership {
public:
    static constexpr std::uint64_t kNeutralDelayMs = 800;
    void key(int controller, int androidAction, int keyCode, std::uint64_t now);
    void motion(int controller, const std::array<float, 8>& axes, std::uint64_t now);
    void mouse(int controller, int left, int right, int wheel, std::uint64_t now);
    void disconnected(int controller, std::uint64_t now);
    void reset(std::uint64_t now);
    bool ownsInput(std::uint64_t now) const;
private:
    struct Controller { int id = 0; bool used = false; bool axes = false; unsigned mouse = 0; };
    Controller* controllerLocked(int id);
    void activityLocked(std::uint64_t now);
    mutable std::mutex mutex_;
    std::array<Controller, 8> controllers_{};
    std::set<std::pair<int, int>> keys_;
    std::uint64_t lastActivity_ = 0;
    bool seen_ = false;
    bool unknownController_ = false;
};

PhysicalOwnership& physicalOwnership();

} // namespace bl2::touch

// Parent bridge integration: call immediately BEFORE the existing guest JNI
// queue callback. Never call these for the new NativeTouchControls mailbox.
void NotifyBl2TouchPhysicalKey(int controller, int action, int keyCode);
void NotifyBl2TouchPhysicalMotion(int controller, const std::array<float, 8>& axes);
void NotifyBl2TouchPhysicalMouse(int controller, int left, int right, int wheel);
void NotifyBl2TouchPhysicalDevice(int controller, bool connected);
void ResetBl2TouchPhysicalOwnership();
// Mark calls emitted by the temporary LaunchControls fallback. Calls are
// balanced on the same Android UI thread; they are not physical device input.
void BeginBl2TouchLegacyPacket();
void EndBl2TouchLegacyPacket();
