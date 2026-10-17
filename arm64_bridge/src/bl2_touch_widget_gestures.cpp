#include "bl2_touch_widget_gestures.h"
#include <algorithm>
#include <cmath>

namespace bl2::touch {
namespace {
bool samePoint(WidgetPoint a, WidgetPoint b) { return a.x == b.x && a.y == b.y; }
}
void WidgetGestures::clear() {
    pointers_.fill(-1); primary_ = -1; cancelled_ = false; target_ = {};
    drag_ = Drag::Pending; sliderIntent_ = numericIntent_ = selectedAtDown_ = false; lastScroll_ = -1; lastFraction_ = -1;
}
void WidgetGestures::cancel() { cancelled_ = true; }
bool WidgetGestures::rebindSelection(const WidgetRegistry& r) {
    if (!selectOnDown_ || !captured() || !r.valid || r.route != route_ || drag_ != Drag::Pending) return false;
    for (const auto& row : r.rows) if (row.id == target_.id && row.enabled && row.visible &&
        row.kind == target_.kind && row.caption == target_.caption && row.rendererType == target_.rendererType &&
        row.renderer == target_.renderer && row.data == target_.data && row.inner == target_.inner &&
        row.slider == target_.slider && row.minimum == target_.minimum && row.maximum == target_.maximum &&
        row.increment == target_.increment && row.choices == target_.choices && row.choiceLabels == target_.choiceLabels) {
        target_ = row; content_ = r.content; return true;
    }
    return false;
}
bool WidgetGestures::rebindSpinner(const WidgetRegistry& r) {
    if(!captured() || !r.valid || target_.kind!=WidgetKind::Spinner || drag_!=Drag::Numeric) return false;
    for(const auto& row:r.rows) if(row.enabled && row.visible && row.kind==target_.kind &&
        row.index==target_.index && row.eventId==target_.eventId && row.caption==target_.caption &&
        row.rendererType==target_.rendererType && row.minimum==target_.minimum && row.maximum==target_.maximum &&
        row.increment==target_.increment && row.choiceLabels==target_.choiceLabels) {
        target_=row;route_=r.route;content_=r.content;return true;
    }
    return false;
}
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
            row.slider == target_.slider && row.inner == target_.inner &&
            row.minimum == target_.minimum && row.maximum == target_.maximum && row.increment == target_.increment &&
            row.choices==target_.choices && row.choiceLabels==target_.choiceLabels &&
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
        if(drag_==Drag::Numeric) {
            const double steps=std::round((p.x-start_.x)*width_/(.03*std::min(width_,height_)));
            const double value=std::clamp(startValue_+steps*target_.increment,target_.minimum,target_.maximum);
            if(target_.maximum<=target_.minimum) return {};
            out.sliderFraction=(value-target_.minimum)/(target_.maximum-target_.minimum);
        } else if (!ProjectWidgetSlider(target_,p,width_,height_,out.sliderFraction)) return {};
        if (out.sliderFraction == lastFraction_) return {};
        lastFraction_ = out.sliderFraction;
    } else if(kind==WidgetCommandKind::Spinner) {
        const double steps=std::round((p.x-start_.x)*width_/(.03*std::min(width_,height_)));
        out.spinnerIndex=static_cast<std::int32_t>(std::clamp(startValue_+steps,target_.minimum,target_.maximum));
        if(out.spinnerIndex==lastFraction_) return {};
        lastFraction_=out.spinnerIndex;
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
        numericIntent_=false;startedMs_=e.timestampMs;
        // Selection artwork can overlap another row or end above its own
        // thumb. Prefer the captured native track over that artwork; captions
        // still use vertical row centres when outside the slider finger area.
        unsigned hits = 0;
        double nearest = 0;
        bool ambiguous = false, nearestSlider = false;
        const double trackPadding = .025 * std::min(width_,height_);
        for (const auto& row : r.rows) if (row.enabled && row.visible) {
            const bool bodyHit = WidgetContains(row.bounds,point);
            const double dy=(point.y-(row.bounds.top+row.bounds.bottom)*.5)*height_;
            double distance=dy*dy, fraction=0;
            bool sliderHit=false;
            if(row.kind==WidgetKind::Slider && ProjectWidgetSlider(row,point,width_,height_,fraction)) {
                const double x=row.sliderStart.x+(row.sliderEnd.x-row.sliderStart.x)*fraction;
                const double y=row.sliderStart.y+(row.sliderEnd.y-row.sliderStart.y)*fraction;
                const double dx=(point.x-x)*width_, trackDy=(point.y-y)*height_;
                const double trackDistance=dx*dx+trackDy*trackDy;
                sliderHit=trackDistance<=trackPadding*trackPadding;
                if(sliderHit) distance=trackDistance;
            }
            if(!bodyHit && !sliderHit) continue;
            if(!hits || (sliderHit && !nearestSlider) || (sliderHit==nearestSlider && distance<nearest)) {
                target_=row;nearest=distance;nearestSlider=sliderHit;ambiguous=false;
            } else if(sliderHit==nearestSlider && distance==nearest) ambiguous=true;
            ++hits;
        }
        if (ambiguous || !WidgetContains(r.listBounds,point)) cancel();
        if(hits) hits=1;
        selectedAtDown_ = hits == 1 && r.selectedIndex == target_.index;
        sliderIntent_ = hits == 1 && target_.kind == WidgetKind::Slider &&
            (nearestSlider || WidgetContains(target_.sliderBounds,point));
        if(hits==1 && (target_.kind==WidgetKind::Slider || target_.kind==WidgetKind::Spinner)) {
            auto bounds=target_.valueBounds;
            const double padding=.015*std::min(width_,height_);
            bounds.left-=padding/width_;bounds.right+=padding/width_;
            bounds.top-=padding/height_;bounds.bottom+=padding/height_;
            numericIntent_=WidgetContains(target_.valueBounds,point) ||
                (WidgetContains(bounds,point) && !sliderIntent_);
            if(numericIntent_) sliderIntent_=false;
            startValue_=target_.value;
        }
        // Actual row pitch from two adjacent live renderers, not menu row numbers
        // or guessed text baselines. A one-row scrolling layout stays unsupported.
        for (std::size_t i = 1; i < r.rows.size(); ++i) {
            const auto& a = r.rows[i - 1]; const auto& b = r.rows[i];
            if (a.visible && b.visible && b.index == a.index + 1) {
                const double pitch = (b.origin.y - a.origin.y) * height_;
                if (pitch > 1 && (!pitch_ || std::abs(pitch - pitch_) < pitch_ * .1)) pitch_ = pitch;
                else { pitch_ = 0; break; }
            }
        }
        if (selectOnDown_ && !cancelled_ && hits == 1 && target_.kind != WidgetKind::Unsupported)
            return command(WidgetCommandKind::Select,r,point);
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
            if(numericIntent_ && std::abs(dx)>std::abs(dy)*1.2 && e.timestampMs>=startedMs_ && e.timestampMs-startedMs_>=250)
                drag_=Drag::Numeric;
            else if (pitch_ > 0 && r.rowCount > r.totalRenderers && std::abs(dy) > std::abs(dx) * 1.2) drag_ = Drag::Scroll;
            else if (sliderIntent_ && std::abs(dx) > std::abs(dy) * 1.2) drag_ = Drag::Slider;
            else cancel();
        }
        if (!cancelled_ && (e.phase == PointerPhase::Move || e.phase == PointerPhase::Up)) {
            if(drag_==Drag::Numeric) result=command(target_.kind==WidgetKind::Spinner?WidgetCommandKind::Spinner:WidgetCommandKind::Slider,r,point);
            else if (drag_ == Drag::Slider) result = command(WidgetCommandKind::Slider,r,point);
            else if (drag_ == Drag::Scroll) result = command(WidgetCommandKind::Scroll,r,point);
            else if (e.phase == PointerPhase::Up) {
                // The DOWN setter has already highlighted this row. Preserve
                // its earlier selection for setting taps; a deliberate drag
                // still changes the value on its first gesture.
                if (selectOnDown_ && !selectedAtDown_ &&
                    (target_.kind == WidgetKind::Slider || target_.kind == WidgetKind::Spinner)) {}
                else if (sliderIntent_ && r.selectedIndex == target_.index) result = command(WidgetCommandKind::Slider,r,start_);
                else if (target_.id && target_.kind != WidgetKind::Unsupported) result = command(WidgetCommandKind::Click,r,start_);
            }
        }
    }
    if (e.phase == PointerPhase::Up || e.phase == PointerPhase::Cancel) {
        const bool cancelled = cancelled_;
        pointers_[static_cast<std::size_t>(slot)] = -1;
        if (std::all_of(pointers_.begin(),pointers_.end(),[](int id) { return id < 0; })) {
            clear(); cancelled_ = cancelled;
        }
    }
    return result;
}
}
