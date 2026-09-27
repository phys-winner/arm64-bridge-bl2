#include "bl2_touch_widget_gestures.h"
#include <algorithm>
#include <cmath>

namespace bl2::touch {
namespace {
bool sameRect(const GfxRect& a, const GfxRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
bool samePoint(WidgetPoint a, WidgetPoint b) { return a.x == b.x && a.y == b.y; }
}
void WidgetGestures::clear() {
    pointers_.fill(-1); primary_ = -1; cancelled_ = false; target_ = {};
    drag_ = Drag::Pending; sliderIntent_ = false; lastScroll_ = -1; lastFraction_ = -1;
}
void WidgetGestures::cancel() { cancelled_ = true; }
int WidgetGestures::find(std::int32_t id) const {
    for (std::size_t i = 0; i < pointers_.size(); ++i) if (pointers_[i] == id) return static_cast<int>(i);
    return -1;
}
void WidgetGestures::begin(std::uint64_t epoch, std::uint64_t scene, bool admitted, bool reset,
                           const WidgetRegistry& registry) {
    if (reset || !admitted || !registry.valid || epoch != epoch_ || scene != scene_ ||
        width_ != registry.surfaceWidth || height_ != registry.surfaceHeight) clear();
    else if (primary_ >= 0 && registry.route != route_) cancel();
    epoch_ = epoch; scene_ = scene; admitted_ = admitted && registry.valid;
    width_ = registry.surfaceWidth; height_ = registry.surfaceHeight;
}
bool WidgetGestures::retained(const WidgetRegistry& r) const {
    if (!r.valid || r.route != route_) return false;
    if (drag_ == Drag::Scroll) return true;
    if (!target_.id) return r.content == content_;
    for (const auto& row : r.rows) if (row.id == target_.id) {
        return row.enabled && row.visible && row.kind == target_.kind && row.caption == target_.caption &&
            row.renderer == target_.renderer && row.data == target_.data &&
            sameRect(row.bounds,target_.bounds) && row.slider == target_.slider && row.inner == target_.inner &&
            row.minimum == target_.minimum && row.maximum == target_.maximum && row.increment == target_.increment &&
            samePoint(row.sliderStart,target_.sliderStart) && samePoint(row.sliderEnd,target_.sliderEnd);
    }
    return false;
}
std::optional<WidgetCommand> WidgetGestures::command(WidgetCommandKind kind, const WidgetRegistry& r, WidgetPoint p) {
    WidgetCommand out;
    out.kind = kind; out.route = route_; out.content = content_; out.epoch = epoch_; out.target = target_;
    if (kind == WidgetCommandKind::Scroll) {
        if (pitch_ <= 0 || r.rowCount <= r.totalRenderers) return {};
        const auto delta = (p.y - start_.y) * height_ / pitch_;
        out.scrollPosition = static_cast<std::int32_t>(std::clamp(std::round(startScroll_ - delta),
            0.0,static_cast<double>(r.rowCount - r.totalRenderers)));
        if (out.scrollPosition == lastScroll_) return {};
        lastScroll_ = out.scrollPosition;
    } else if (kind == WidgetCommandKind::Slider) {
        if (!ProjectWidgetSlider(target_,p,width_,height_,out.sliderFraction)) return {};
        if (out.sliderFraction == lastFraction_) return {};
        lastFraction_ = out.sliderFraction;
    }
    return out;
}
std::optional<WidgetCommand> WidgetGestures::pointer(const PointerEvent& e, const WidgetRegistry& r) {
    if (!admitted_ || e.sourceEpoch != epoch_ || e.pointerId < 0 || !width_ || !height_) return {};
    const WidgetPoint point{e.surfaceX,e.surfaceY};
    const bool numeric = std::isfinite(point.x) && std::isfinite(point.y) &&
        point.x >= 0 && point.x <= 1 && point.y >= 0 && point.y <= 1;
    int slot = find(e.pointerId);
    if (e.phase == PointerPhase::Down) {
        if (slot >= 0) { cancel(); return {}; }
        slot = find(-1); if (slot < 0) { cancel(); return {}; }
        const bool empty = std::all_of(pointers_.begin(),pointers_.end(),[](int id) { return id < 0; });
        pointers_[static_cast<std::size_t>(slot)] = e.pointerId;
        if (!empty) { cancel(); return {}; }
        primary_ = e.pointerId; cancelled_ = !numeric || !r.valid;
        target_ = {}; route_ = r.route; content_ = r.content; start_ = point;
        startScroll_ = lastScroll_ = r.scrollPosition; lastFraction_ = -1;
        drag_ = Drag::Pending; sliderIntent_ = false; pitch_ = 0;
        // Ambiguous overlapping enabled rows fail closed instead of guessing z-order.
        unsigned hits = 0;
        for (const auto& row : r.rows) if (row.enabled && row.visible && WidgetContains(row.bounds,point)) {
            target_ = row; ++hits;
        }
        if (hits > 1 || !WidgetContains(r.listBounds,point)) cancel();
        sliderIntent_ = hits == 1 && target_.kind == WidgetKind::Slider && WidgetContains(target_.sliderBounds,point);
        // Actual row pitch from two adjacent live renderers, not menu row numbers
        // or guessed text baselines. A one-row scrolling layout stays unsupported.
        for (std::size_t i = 1; i < r.rows.size(); ++i) {
            const auto& a = r.rows[i - 1]; const auto& b = r.rows[i];
            if (a.visible && b.visible && b.index == a.index + 1) {
                const double pitch = (b.bounds.top - a.bounds.top) * height_;
                if (pitch > 1 && (!pitch_ || std::abs(pitch - pitch_) < pitch_ * .1)) pitch_ = pitch;
                else { pitch_ = 0; break; }
            }
        }
        return {};
    }
    if (slot < 0) return {};
    if (!numeric || static_cast<unsigned>(e.phase) > static_cast<unsigned>(PointerPhase::Cancel) ||
        e.phase == PointerPhase::Cancel || !retained(r)) cancel();
    std::optional<WidgetCommand> result;
    if (e.pointerId == primary_ && !cancelled_) {
        const double dx = (point.x - start_.x) * width_, dy = (point.y - start_.y) * height_;
        const double slop = .015 * std::min(width_,height_);
        if (drag_ == Drag::Pending && std::hypot(dx,dy) > slop) {
            if (sliderIntent_) drag_ = Drag::Slider;
            else if (pitch_ > 0 && r.rowCount > r.totalRenderers && std::abs(dy) > std::abs(dx) * 1.2) drag_ = Drag::Scroll;
            else cancel();
        }
        if (!cancelled_ && (e.phase == PointerPhase::Move || e.phase == PointerPhase::Up)) {
            if (drag_ == Drag::Slider) result = command(WidgetCommandKind::Slider,r,point);
            else if (drag_ == Drag::Scroll) result = command(WidgetCommandKind::Scroll,r,point);
            else if (e.phase == PointerPhase::Up) {
                if (sliderIntent_) result = command(WidgetCommandKind::Slider,r,start_);
                else if (target_.id && target_.kind == WidgetKind::Button) result = command(WidgetCommandKind::Click,r,start_);
            }
        }
    }
    if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
        pointers_[static_cast<std::size_t>(slot)] = -1;
        if (std::all_of(pointers_.begin(),pointers_.end(),[](int id) { return id < 0; })) clear();
    }
    return result;
}
}
