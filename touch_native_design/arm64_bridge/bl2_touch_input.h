#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace bl2::touch {

constexpr std::uint64_t kProtocolVersion = 1;
constexpr std::size_t kActionCount = 25;
constexpr std::size_t kTransitionsPerAction = 16;
constexpr std::size_t kMaxPointers = 10;
constexpr std::size_t kPointerQueueCapacity = 64;
constexpr std::uint64_t kHeartbeatTimeoutMs = 350;
constexpr float kMaxLookDegreesPerPacket = 90.0f;
constexpr float kMaxPendingLookDegrees = 180.0f;
constexpr std::uint64_t kKnownActions = (std::uint64_t{1} << kActionCount) - 1;

enum class Mode : std::uint32_t {
    Unknown = 0, Loading = 1, Cinematic = 2, Frontend = 3, Gameplay = 4,
    Vehicle = 5, Menu = 6, Dialog = 7, TextInput = 8, Dead = 9
};
enum Capability : std::uint32_t {
    GameplayInput = 1u << 0, RelativeLook = 1u << 1, DirectUi = 1u << 2,
    PhysicalOwnsInput = 1u << 3, CanSkip = 1u << 4, UiNavigation = 1u << 5, UiTapTest = 1u << 6
};
enum class Action : std::uint8_t {
    Fire = 0, Aim, Jump, Crouch, Reload, Use, Melee, Grenade, Skill, Sprint,
    NextWeapon, PreviousWeapon, Inventory, Map, Pause, Back, Confirm, Skip,
    VehicleBrake, VehicleBoost, VehicleHorn, UiUp, UiDown, UiLeft, UiRight
};
constexpr std::uint64_t actionBit(Action action) {
    return std::uint64_t{1} << static_cast<unsigned>(action);
}
enum class PointerPhase : std::uint8_t { Down = 0, Move = 1, Up = 2, Cancel = 3 };
enum ResetReason : std::uint32_t {
    NoReset = 0, ContextChanged = 1u << 0, ManualReset = 1u << 1,
    QueueOverflow = 1u << 2, HeartbeatLost = 1u << 3,
    InvalidNumeric = 1u << 4, InvalidPointer = 1u << 5
};

struct Context {
    Mode mode = Mode::Unknown;
    std::uint32_t capabilities = 0;
    std::uint64_t availableActions = 0;
    // Engine-owned lifetime token; change when the scene/controller object is
    // replaced, even if mode, address and capabilities happen to be identical.
    std::uint64_t sceneIdentity = 0;
};

struct Snapshot {
    std::uint64_t epoch = 1;
    Context context{};
    std::uint64_t frameSerial = 0;
    std::array<std::uint64_t, 8> wire() const;
};

struct ActionEdge {
    Action action = Action::Fire;
    bool down = false;
    std::uint64_t sourceEpoch = 0;
    std::uint64_t sequence = 0;
};

struct PointerEvent {
    std::int32_t pointerId = -1;
    PointerPhase phase = PointerPhase::Cancel;
    // Full engine Surface, not screen, safe-area or letterboxed-UI coordinates.
    float surfaceX = 0;
    float surfaceY = 0;
    std::uint64_t sourceEpoch = 0;
    std::uint64_t sequence = 0;
};

struct Frame {
    // False for duplicate/out-of-order engineTick; dispatch nothing in that case.
    bool fresh = false;
    std::uint64_t frameSerial = 0;
    std::uint64_t epoch = 1;
    Context context{};
    std::uint32_t resetReasons = NoReset;
    float moveX = 0;
    float moveY = 0;
    float yawDegrees = 0;
    float pitchDegrees = 0;
    std::uint64_t heldActions = 0;
    std::array<ActionEdge, kActionCount> actionEdges{};
    std::size_t actionEdgeCount = 0;
    std::array<PointerEvent, kPointerQueueCapacity> pointerEvents{};
    std::size_t pointerEventCount = 0;
};

// All producer operations are thread safe. Exactly one engine input consumer is
// allowed. Guest calls must happen AFTER consumeFrame() releases this mutex.
class Mailbox {
public:
    Mailbox() = default;
    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;

    // Engine thread, before consumeFrame. Mode/capability/scene changes invalidate
    // the epoch; an action-mask-only change selectively releases revoked actions.
    // forceNewScene also invalidates an unchanged Context, e.g. address reuse.
    std::uint64_t publishContext(Context context, bool forceNewScene = false);
    Snapshot snapshot() const;
    bool submit(std::uint64_t epoch, float moveX, float moveY,
                float yawDegrees, float pitchDegrees,
                std::uint64_t heldActions, std::uint64_t nowMs);
    bool uiPointer(std::uint64_t epoch, std::int32_t pointerId,
                   PointerPhase phase, float surfaceX, float surfaceY,
                   std::uint64_t nowMs);
    void reset(std::uint32_t reason = ManualReset);
    Frame consumeFrame(std::uint64_t engineTick, std::uint64_t nowMs);

private:
    template<class T, std::size_t N> struct Ring {
        std::array<T, N> entries{};
        std::size_t head = 0;
        std::size_t count = 0;
        bool full() const { return count == N; }
        bool empty() const { return count == 0; }
        void clear() { head = 0; count = 0; }
        T& back() { return entries[(head + count - 1) % N]; }
        void push(const T& value) { entries[(head + count++) % N] = value; }
        T pop() { const T value = entries[head]; head = (head + 1) % N; --count; return value; }
    };
    struct PointerState {
        std::int32_t id = -1;
        float x = 0;
        float y = 0;
        std::uint64_t epoch = 0;
    };

    static int pointerSlot(const std::array<PointerState, kMaxPointers>& states,
                           std::int32_t id);
    static int freePointerSlot(const std::array<PointerState, kMaxPointers>& states);
    bool inputAllowedLocked() const;
    bool gameplayAllowedLocked() const;
    void invalidateLocked(std::uint32_t reason);
    void appendReleasesLocked(Frame& frame);
    void rememberPointerLocked(const PointerEvent& event);

    mutable std::mutex mutex_;
    Context context_{};
    std::uint64_t epoch_ = 1;
    std::uint64_t sequence_ = 0;
    std::uint64_t frameSerial_ = 0;
    std::uint64_t lastEngineTick_ = 0;
    bool hasEngineTick_ = false;
    bool armed_ = false;
    std::uint64_t heartbeatMs_ = 0;
    std::uint32_t pendingResetReasons_ = NoReset;
    float moveX_ = 0;
    float moveY_ = 0;
    float yawDegrees_ = 0;
    float pitchDegrees_ = 0;
    std::uint64_t desiredActions_ = 0;
    std::uint64_t guestHeldActions_ = 0;
    std::array<std::uint64_t, kActionCount> guestActionEpochs_{};
    std::array<Ring<ActionEdge, kTransitionsPerAction>, kActionCount> transitions_{};
    Ring<PointerEvent, kPointerQueueCapacity> pointers_{};
    std::array<PointerState, kMaxPointers> submittedPointers_{};
    std::array<PointerState, kMaxPointers> guestPointers_{};
};

Mailbox& sharedMailbox();
std::uint64_t monotonicMilliseconds();

} // namespace bl2::touch
