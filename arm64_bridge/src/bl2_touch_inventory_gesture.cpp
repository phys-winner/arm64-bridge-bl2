#include "bl2_touch_inventory_gesture.h"
#include <algorithm>
#include <cmath>

namespace bl2::touch {
InventoryDragOwner::operator bool() const {
    return family >= InventoryDragFamily::Bank && family <= InventoryDragFamily::StatusInventory &&
        epoch && scene && generation && owner && controller && world && viewport &&
        width && height && width <= 16384 && height <= 16384;
}
InventoryDragSource::operator bool() const {
    return panel.interface && panel.data && cell.interface && cell.data && content && panel != cell;
}
int UiInventoryGesture::find(std::int32_t id) const {
    const auto slot = std::find(ids_.begin(), ids_.end(), id);
    return slot == ids_.end() ? -1 : static_cast<int>(slot - ids_.begin());
}
bool UiInventoryGesture::ownsPointer(std::int32_t id) const {
    return id >= 0 && captured() && find(id) >= 0;
}
InventoryDragUpdate UiInventoryGesture::update(InventoryDragCommand command) const {
    return {command,owner_,source_,startX_,startY_,x_,y_};
}
void UiInventoryGesture::clear() {
    ids_.fill(-1); primary_ = -1; held_ = cancelled_ = moved_ = false;
    source_ = {}; sequence_ = timestamp_ = 0;
}
std::optional<InventoryDragUpdate> UiInventoryGesture::invalidate() {
    const auto result = held_ ? std::optional(update(InventoryDragCommand::Cancel)) : std::nullopt;
    held_ = false; cancelled_ = true;
    return result;
}
std::optional<InventoryDragUpdate> UiInventoryGesture::cancel() {
    const auto result = invalidate(); clear(); return result;
}
std::optional<InventoryDragUpdate> UiInventoryGesture::begin(const InventoryDragOwner& owner,
    bool inputAllowed, bool reset, std::uint64_t now) {
    std::optional<InventoryDragUpdate> result;
    if (reset || !inputAllowed || !owner || owner != owner_ || now < now_) result = cancel();
    owner_ = owner; now_ = now; valid_ = inputAllowed && static_cast<bool>(owner);
    return result;
}
std::optional<InventoryDragUpdate> UiInventoryGesture::pointer(const PointerEvent& event,
    const InventoryDragSource& freshSource) {
    if (!valid_ || event.pointerId < 0) return {};
    // Old-epoch releases must not cancel a newly captured contact that happens
    // to reuse the Android pointer ID.
    if (event.sourceEpoch != owner_.epoch) return {};
    const bool position = std::isfinite(event.surfaceX) && std::isfinite(event.surfaceY) &&
        event.surfaceX >= 0 && event.surfaceX <= 1 && event.surfaceY >= 0 && event.surfaceY <= 1;
    const bool timely = event.sequence && event.sequence > sequence_ && event.timestampMs >= timestamp_ &&
        event.timestampMs <= now_ && now_ - event.timestampMs <= 1000;
    if (event.phase == PointerPhase::Down) {
        if (!captured()) {
            if (!position || !timely || !freshSource) return {};
            ids_.fill(-1); ids_[0] = event.pointerId; primary_ = event.pointerId;
            source_ = freshSource; startX_ = x_ = event.surfaceX; startY_ = y_ = event.surfaceY;
            sequence_ = event.sequence; timestamp_ = event.timestampMs;
            held_ = cancelled_ = moved_ = false;
            return {};
        }
        auto free = std::find(ids_.begin(),ids_.end(),-1);
        if (find(event.pointerId) < 0 && free != ids_.end()) *free = event.pointerId;
        return invalidate(); // No second-finger drop or transfer of ownership.
    }
    const auto slot = find(event.pointerId);
    if (!captured() || slot < 0) return {};
    std::optional<InventoryDragUpdate> result;
    if (!position || !timely || event.phase == PointerPhase::Cancel ||
        static_cast<unsigned>(event.phase) > static_cast<unsigned>(PointerPhase::Cancel) ||
        !freshSource || freshSource != source_) result = invalidate();
    if (timely) { sequence_ = event.sequence; timestamp_ = event.timestampMs; }
    if (!cancelled_ && event.pointerId == primary_) {
        x_ = event.surfaceX; y_ = event.surfaceY;
        const float slop = .015f * static_cast<float>(std::min(owner_.width,owner_.height));
        moved_ |= std::hypot((x_-startX_)*owner_.width,(y_-startY_)*owner_.height) > slop;
        if (event.phase == PointerPhase::Move && moved_) {
            result = update(held_ ? InventoryDragCommand::Move : InventoryDragCommand::StartMove);
            held_ = true;
        } else if (event.phase == PointerPhase::Up) {
            // A final UP slip without a preceding held MOVE neither starts a
            // drag nor becomes a click at the initial item.
            if (held_) result = update(InventoryDragCommand::Drop);
            else if (!moved_) result = update(InventoryDragCommand::Click);
            held_ = false;
        }
    }
    if (event.phase == PointerPhase::Up || event.phase == PointerPhase::Cancel) {
        ids_[static_cast<std::size_t>(slot)] = -1;
        if (std::all_of(ids_.begin(),ids_.end(),[](auto id) { return id < 0; })) clear();
    }
    return result;
}
} // namespace bl2::touch
