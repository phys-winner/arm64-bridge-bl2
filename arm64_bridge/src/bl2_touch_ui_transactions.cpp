#include "bl2_touch_ui_transactions.h"
#include <algorithm>
#include <cmath>

namespace bl2::touch {
void UiTransactions::clear() {
    held_ = 0; pointers_.fill(-1); primary_ = -1; cancelled_ = keysCancelled_ = false;
}
void UiTransactions::begin(std::uint64_t epoch, std::uint64_t scene, bool valid,
                           bool reset, std::uint32_t width, std::uint32_t height) {
    if (reset || !valid || epoch != epoch_ || scene != scene_ ||
        width != width_ || height != height_) clear();
    epoch_ = epoch; scene_ = scene; valid_ = valid;
    width_ = width; height_ = height;
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
            cancelled_ = held_ != 0 || !std::isfinite(startX_) || !std::isfinite(startY_) ||
                startX_ < 0 || startX_ > 1 || startY_ < 0 || startY_ > 1;
        } else cancelled_ = true; // No second-finger click or ownership transfer.
        if (held_) keysCancelled_ = true;
        return {};
    }
    if (slot < 0) return {};
    if (static_cast<unsigned>(event.phase) > static_cast<unsigned>(PointerPhase::Cancel)) cancelled_ = true;
    if (event.phase == PointerPhase::Cancel) cancelled_ = true;
    if (event.pointerId == primary_) {
        const float dx = (event.surfaceX - startX_) * static_cast<float>(width_);
        const float dy = (event.surfaceY - startY_) * static_cast<float>(height_);
        const float slop = .015f * static_cast<float>(std::min(width_, height_));
        if (!std::isfinite(dx) || !std::isfinite(dy) || std::hypot(dx, dy) > slop) cancelled_ = true;
    }
    if (event.phase != PointerPhase::Up && event.phase != PointerPhase::Cancel) return {};
    const bool commit = event.phase == PointerPhase::Up && event.pointerId == primary_ && !cancelled_;
    pointers_[static_cast<std::size_t>(slot)] = -1;
    // Commit at the original coordinate. A small finger slip must not select an
    // adjacent row. Widget identity during same-movie mutations is a later gate.
    const UiTap result{startX_, startY_};
    if (std::all_of(pointers_.begin(), pointers_.end(), [](int id) { return id < 0; })) {
        primary_ = -1; cancelled_ = false;
    }
    return commit ? std::optional<UiTap>(result) : std::nullopt;
}
} // namespace bl2::touch
