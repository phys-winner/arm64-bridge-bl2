#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include <array>
#include <cstdint>
#include <optional>

namespace bl2::touch {
// UiMapGesture carries2*log2(newDistance/oldDistance). Original AnalogScale
// instead takes a rate multiplied by ZoomSpeedFactor*DeltaSeconds*60.
std::optional<float> Bl2MapZoomDelta(float logarithmicRatio,float speed,float deltaSeconds);

// Stage-3 additive capability bits. DirectUi remains reserved for the complete
// press/drag/cancel adapter; a tap probe must not claim that capability.
constexpr std::uint32_t kUiNavigation = 1u << 5;
constexpr std::uint32_t kUiTapTest = 1u << 6;
constexpr std::uint64_t kUiActions = actionBit(Action::Back) | actionBit(Action::Confirm) |
    actionBit(Action::UiUp) | actionBit(Action::UiDown) |
    actionBit(Action::UiLeft) | actionBit(Action::UiRight) |
    actionBit(Action::UiAction) | actionBit(Action::UiOther) |
    actionBit(Action::UiPreviousTab) | actionBit(Action::UiNextTab);
// A vertical menu drag sends one list step per this share of the Surface height.
constexpr float kUiScrollPitch = .06f;

// One menu command from a pointer sequence, always at the DOWN point: a tap;
// whole list steps while a vertical drag moves (+ = next row: the finger moved
// up and the content follows it); or one tab when a horizontal swipe ends
// (+ = next: the finger moved left).
struct UiTap { float x = 0, y = 0; int scrollDirection = 0; int tabDirection = 0; };

struct UiPauseResumeIdentity {
    std::uint64_t engine=0,local=0,controller=0,pawn=0,world=0,owner=0,list=0;
    std::uint32_t viewport=0,movie=0,interface=0,nativeWorld=0,width=0,height=0;
    explicit operator bool() const {
        return engine && local && controller && pawn && world && owner && list &&
            viewport && movie && interface && nativeWorld && width && height;
    }
    bool operator==(const UiPauseResumeIdentity&) const = default;
    bool sameGame(const UiPauseResumeIdentity& other) const {
        return engine==other.engine && local==other.local && controller==other.controller && pawn==other.pawn &&
            world==other.world && viewport==other.viewport && nativeWorld==other.nativeWorld &&
            width==other.width && height==other.height;
    }
};
// Original Options Cancel closes a GFx scene and changes its generation. Keep
// only this two-step intent. Options may be the only registered movie; the
// newly published Pause may have entirely new owner/list/movie identities.
// Each step validates its actual focused native scene and the same live game.
class UiEditResume {
public:
    bool request(const UiPauseResumeIdentity&,std::uint64_t now);
    bool next(const UiPauseResumeIdentity&,bool options,bool pause,bool inputAllowed,std::uint64_t now);
    void clear() { identity_={};stage_=0; }
    unsigned stage() const { return stage_; }
private:
    UiPauseResumeIdentity identity_;
    unsigned stage_=0;
    std::uint64_t started_=0;
};

// MouseMove is queued by native Scaleform. One uncommitted waypoint may wait
// for its next input boundary, but never survives another owner/contact/action
// or turns an old mouse position into a second command.
class UiMapDeferredPoint {
public:
    void store(UiTap,std::uint64_t epoch,std::uint64_t scene,std::uint64_t now);
    std::optional<UiTap> take(std::uint64_t epoch,std::uint64_t scene,std::uint64_t now,bool sameOwner);
    void clear() { point_.reset(); }
    bool pending() const { return point_.has_value(); }
private:
    std::optional<UiTap> point_;
    std::uint64_t epoch_=0,scene_=0,started_=0;
};

struct UiMapUpdate {
    bool start = false, stop = false, waypoint = false;
    float x = 0, y = 0, zoom = 0;
    std::int32_t dx = 0, dy = 0;
};
// Map keeps two physical IDs, not two synthetic clicks. A third contact or
// changed owner cancels the original drag. Pinching never becomes a tap/UP.
class UiMapGesture {
public:
    void begin(std::uint64_t epoch,std::uint64_t scene,bool valid,bool reset,std::uint32_t width,std::uint32_t height);
    std::optional<UiMapUpdate> pointer(const PointerEvent&,bool insideMap);
    void clear();
    bool captured() const { return ids_[0] >= 0 || ids_[1] >= 0; }
private:
    std::array<std::int32_t,3> ids_{-1,-1,-1};
    std::array<float,2> x_{},y_{};
    std::uint64_t epoch_ = 0,scene_ = 0,started_ = 0;
    std::uint32_t width_ = 0,height_ = 0;
    float startX_ = 0,startY_ = 0,distance_ = 0;
    bool valid_ = false,dragging_ = false,cancelled_ = false,moved_ = false,pinched_ = false;
};

// Touch DOWN is a local intent, not a guest key/mouse DOWN. Only a matching UP
// commits a complete short UI command. Reset, cancellation, source takeover or
// scene replacement therefore cannot accidentally activate release-driven UI.
class UiTransactions {
public:
    void begin(std::uint64_t epoch, std::uint64_t scene, bool valid, bool reset,
               std::uint32_t width, std::uint32_t height, bool swipeNavigation = false);
    std::optional<Action> action(const ActionEdge& edge);
    std::optional<UiTap> pointer(const PointerEvent& event);
    bool ownsPointer(std::int32_t id) const { return valid_ && primary_ == id && !cancelled_ && !held_; }
    // Past the slop a swipe-navigation drag scrolls or changes tabs; it no
    // longer points at a widget under the finger.
    bool dragging() const { return axis_ != 0; }
    void clear();
private:
    int find(std::int32_t id) const;
    std::array<std::int32_t, kMaxPointers> pointers_ = [] {
        std::array<std::int32_t, kMaxPointers> result{}; result.fill(-1); return result;
    }();
    std::uint64_t epoch_ = 0, scene_ = 0, held_ = 0;
    std::uint32_t width_ = 0, height_ = 0;
    std::int32_t primary_ = -1;
    float startX_ = 0, startY_ = 0, anchorY_ = 0;
    int axis_ = 0; // 0 undecided, 1 horizontal, 2 vertical
    bool valid_ = false, cancelled_ = false, keysCancelled_ = false;
    bool moved_ = false, swipeNavigation_ = false;
};

} // namespace bl2::touch
