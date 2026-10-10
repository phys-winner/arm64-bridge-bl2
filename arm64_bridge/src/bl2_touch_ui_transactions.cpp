#include "bl2_touch_ui_transactions.h"
#include <algorithm>
#include <cmath>

namespace bl2::touch {
bool UiEditResume::request(const UiPauseResumeIdentity& identity,std::uint64_t now) {
    clear();
    if (!identity) return false;
    identity_=identity;stage_=1;started_=now;return true;
}
bool UiEditResume::next(const UiPauseResumeIdentity& identity,bool options,bool pause,bool inputAllowed,std::uint64_t now) {
    if (!stage_) return false;
    if (!inputAllowed || !identity || !identity.sameGame(identity_) || now<started_ || now-started_>2000 ||
        (!options && !pause)) { clear();return false; }
    if (stage_==1) {
        if (!options || pause || identity!=identity_) { clear();return false; }
        stage_=2;return true;
    }
    if (options && !pause) {
        if(identity!=identity_) clear(); // Never wait on a different Options.
        return false; // Original close may publish next frame.
    }
    if (pause && !options) { clear();return true; }
    clear();return false;
}
std::optional<float> Bl2MapZoomDelta(float logarithmicRatio,float speed,float deltaSeconds) {
    if (!std::isfinite(logarithmicRatio) || std::abs(logarithmicRatio)>2 || !std::isfinite(speed) ||
        speed<=0 || speed>10 || !std::isfinite(deltaSeconds) || deltaSeconds<=0 || deltaSeconds>1) return {};
    const double ratio=std::exp2(double(logarithmicRatio)/2),k=double(speed)*deltaSeconds*60;
    const double delta=(ratio>=1 ? ratio-1 : 1-1/ratio)/k;
    return std::isfinite(delta) && std::abs(delta)<=1000000 ? std::optional<float>(static_cast<float>(delta)) : std::nullopt;
}
void UiMapDeferredPoint::store(UiTap point,std::uint64_t epoch,std::uint64_t scene,std::uint64_t now) {
    clear();
    if (!epoch || !scene || !std::isfinite(point.x) || !std::isfinite(point.y) ||
        point.x<0 || point.x>1 || point.y<0 || point.y>1 || point.scrollDirection) return;
    point_=point;epoch_=epoch;scene_=scene;started_=now;
}
std::optional<UiTap> UiMapDeferredPoint::take(std::uint64_t epoch,std::uint64_t scene,std::uint64_t now,bool sameOwner) {
    const auto point=point_;clear();
    return point && sameOwner && epoch==epoch_ && scene==scene_ && now>=started_ && now-started_<=250 ? point : std::nullopt;
}
void UiMapGesture::clear() {
    ids_.fill(-1); dragging_ = cancelled_ = moved_ = pinched_ = false; distance_ = 0;
}
void UiMapGesture::begin(std::uint64_t epoch,std::uint64_t scene,bool valid,bool reset,std::uint32_t width,std::uint32_t height) {
    if (reset || !valid || epoch != epoch_ || scene != scene_ || width != width_ || height != height_) clear();
    epoch_ = epoch; scene_ = scene; valid_ = valid; width_ = width; height_ = height;
}
std::optional<UiMapUpdate> UiMapGesture::pointer(const PointerEvent& e,bool inside) {
    if (!valid_ || e.sourceEpoch != epoch_ || e.pointerId < 0 || !width_ || !height_) return {};
    UiMapUpdate out; out.x = e.surfaceX; out.y = e.surfaceY;
    const bool positionValid = std::isfinite(e.surfaceX) && std::isfinite(e.surfaceY) &&
        e.surfaceX >= 0 && e.surfaceX <= 1 && e.surfaceY >= 0 && e.surfaceY <= 1;
    if (!positionValid) cancelled_ = true;
    auto found = std::find(ids_.begin(),ids_.end(),e.pointerId);
    if (e.phase == PointerPhase::Down) {
        if (found != ids_.end()) { cancelled_ = true; out.stop = dragging_; dragging_ = false; return out; }
        if (!captured() && !inside) return {};
        found = std::find(ids_.begin(),ids_.end(),-1);
        if (found == ids_.end()) { cancelled_ = true; out.stop = dragging_; dragging_ = false; return out; }
        const auto slot = static_cast<unsigned>(found - ids_.begin()); *found = e.pointerId;
        if (slot == 0) { startX_ = e.surfaceX; startY_ = e.surfaceY; started_ = e.timestampMs; }
        if (slot >= 2 || !inside) cancelled_ = true;
        else if (positionValid) { x_[slot] = e.surfaceX; y_[slot] = e.surfaceY; }
        if (slot == 1) {
            pinched_ = moved_ = true; out.stop = dragging_; dragging_ = false;
            distance_ = positionValid ? std::hypot((x_[1]-x_[0])*width_,(y_[1]-y_[0])*height_) : 0;
            if (distance_ < .04f * std::min(width_,height_)) cancelled_ = true;
        } else if (slot == 0 && !cancelled_) out.start = dragging_ = true;
    } else {
        if (found == ids_.end()) return {};
        const auto slot = static_cast<unsigned>(found - ids_.begin());
        if (e.phase == PointerPhase::Cancel || static_cast<unsigned>(e.phase) > static_cast<unsigned>(PointerPhase::Cancel)) cancelled_ = true;
        if (slot < 2 && !cancelled_) {
            out.dx = static_cast<std::int32_t>(std::round((e.surfaceX-x_[slot])*width_));
            out.dy = static_cast<std::int32_t>(std::round((e.surfaceY-y_[slot])*height_));
            x_[slot] = e.surfaceX; y_[slot] = e.surfaceY;
            if (std::hypot((e.surfaceX-startX_)*width_,(e.surfaceY-startY_)*height_) > .015f * std::min(width_,height_)) moved_ = true;
            if (ids_[0] >= 0 && ids_[1] >= 0 && e.phase == PointerPhase::Move) {
                const auto next = std::hypot((x_[1]-x_[0])*width_,(y_[1]-y_[0])*height_);
                if (distance_ > 0 && next >= .04f * std::min(width_,height_)) {
                    out.zoom = std::clamp(2.f * std::log2(next/distance_),-2.f,2.f); distance_ = next;
                } else cancelled_ = true;
                out.dx = out.dy = 0;
            } else if (!dragging_ || e.phase != PointerPhase::Move) out.dx = out.dy = 0;
        }
        if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
            out.stop = dragging_; dragging_ = false;
            out.waypoint = e.phase == PointerPhase::Up && !cancelled_ && !moved_ && !pinched_ &&
                inside && e.timestampMs >= started_ && e.timestampMs-started_ >= 500;
            *found = -1;
            // The survivor of a pinch cannot acquire a new drag or waypoint.
            if (captured()) cancelled_ = true;
        }
    }
    if (!std::isfinite(e.surfaceX) || !std::isfinite(e.surfaceY) || e.surfaceX < 0 || e.surfaceX > 1 || e.surfaceY < 0 || e.surfaceY > 1)
        cancelled_ = true;
    if (cancelled_) { out.stop = out.stop || dragging_; dragging_ = false; out.start = out.waypoint = false; out.dx = out.dy = 0; out.zoom = 0; }
    if (std::all_of(ids_.begin(),ids_.end(),[](int id) { return id < 0; })) clear();
    return out;
}
void UiTransactions::clear() {
    held_ = 0; pointers_.fill(-1); primary_ = -1; cancelled_ = keysCancelled_ = moved_ = false;
}
void UiTransactions::begin(std::uint64_t epoch, std::uint64_t scene, bool valid,
                           bool reset, std::uint32_t width, std::uint32_t height, bool swipeNavigation) {
    if (reset || !valid || epoch != epoch_ || scene != scene_ ||
        width != width_ || height != height_ || swipeNavigation != swipeNavigation_) clear();
    epoch_ = epoch; scene_ = scene; valid_ = valid;
    width_ = width; height_ = height; swipeNavigation_ = swipeNavigation;
}
std::optional<Action> UiTransactions::action(const ActionEdge& edge) {
    const auto index = static_cast<unsigned>(edge.action);
    if (!valid_ || edge.sourceEpoch != epoch_ || index >= kActionCount) return {};
    const auto bit = actionBit(edge.action);
    if (!(kUiActions & bit)) return {};
    if (edge.down) {
        if ((held_ && !(held_ & bit)) || primary_ >= 0) keysCancelled_ = cancelled_ = true;
        held_ |= bit; return {};
    }
    const bool owned = (held_ & bit) != 0 && !keysCancelled_;
    held_ &= ~bit;
    if (!held_) keysCancelled_ = false;
    return owned ? std::optional<Action>(edge.action) : std::nullopt;
}
int UiTransactions::find(std::int32_t id) const {
    for (std::size_t i = 0; i < pointers_.size(); ++i)
        if (pointers_[i] == id) return static_cast<int>(i);
    return -1;
}
std::optional<UiTap> UiTransactions::pointer(const PointerEvent& event) {
    if (!valid_ || event.sourceEpoch != epoch_ || !width_ || !height_ || event.pointerId < 0) return {};
    if (!std::isfinite(event.surfaceX) || !std::isfinite(event.surfaceY) ||
        event.surfaceX < 0 || event.surfaceX > 1 || event.surfaceY < 0 || event.surfaceY > 1) {
        cancelled_ = true;
    }
    int slot = find(event.pointerId);
    if (event.phase == PointerPhase::Down) {
        if (slot >= 0) { cancelled_ = true; return {}; }
        slot = find(-1);
        if (slot < 0) { cancelled_ = true; return {}; }
        const bool empty = std::all_of(pointers_.begin(), pointers_.end(), [](int id) { return id < 0; });
        pointers_[static_cast<std::size_t>(slot)] = event.pointerId;
        if (empty) {
            primary_ = event.pointerId; startX_ = event.surfaceX; startY_ = event.surfaceY;
            moved_ = false;
            cancelled_ = held_ != 0 || !std::isfinite(startX_) || !std::isfinite(startY_) ||
                startX_ < 0 || startX_ > 1 || startY_ < 0 || startY_ > 1;
        } else cancelled_ = true; // No second-finger click or ownership transfer.
        if (held_) keysCancelled_ = true;
        return {};
    }
    if (slot < 0) return {};
    if (static_cast<unsigned>(event.phase) > static_cast<unsigned>(PointerPhase::Cancel)) cancelled_ = true;
    if (event.phase == PointerPhase::Cancel) cancelled_ = true;
    const float dx = (event.surfaceX - startX_) * static_cast<float>(width_);
    const float dy = (event.surfaceY - startY_) * static_cast<float>(height_);
    if (event.pointerId == primary_) {
        const float slop = .015f * static_cast<float>(std::min(width_, height_));
        if (!std::isfinite(dx) || !std::isfinite(dy)) cancelled_ = true;
        else if (std::hypot(dx, dy) > slop) moved_ = true;
    }
    if (event.phase != PointerPhase::Up && event.phase != PointerPhase::Cancel) return {};
    const bool swipe = swipeNavigation_ && moved_ &&
        std::abs(dy) >= .08f * static_cast<float>(std::min(width_,height_)) && std::abs(dy) > std::abs(dx) * 1.5f;
    const bool commit = event.phase == PointerPhase::Up && event.pointerId == primary_ && !cancelled_ && (!moved_ || swipe);
    pointers_[static_cast<std::size_t>(slot)] = -1;
    // Commit at the original coordinate. A small finger slip must not select an
    // adjacent row. Widget identity during same-movie mutations is a later gate.
    const UiTap result{startX_, startY_,swipe ? (dy < 0 ? 1 : -1) : 0};
    if (std::all_of(pointers_.begin(), pointers_.end(), [](int id) { return id < 0; })) {
        primary_ = -1; cancelled_ = moved_ = false;
    }
    return commit ? std::optional<UiTap>(result) : std::nullopt;
}
} // namespace bl2::touch
