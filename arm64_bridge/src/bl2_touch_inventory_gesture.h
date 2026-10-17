#pragma once

#include "bl2_touch_gfx_values.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include <array>
#include <optional>

namespace bl2::touch {

enum class InventoryDragFamily { None, Bank, Stash, Vending, Trading, StatusInventory };

// Only the sink's freshly verified native focused inventory movie may supply
// this identity. A whitelist entry alone is not a draggable item admission.
struct InventoryDragOwner {
    InventoryDragFamily family = InventoryDragFamily::None;
    std::uint64_t epoch = 0, scene = 0, generation = 0;
    std::uint64_t owner = 0, controller = 0, world = 0;
    std::uint32_t viewport = 0, width = 0, height = 0;
    explicit operator bool() const;
    bool operator==(const InventoryDragOwner&) const = default;
};

// Real visible native panel/cell identities and the exact item/data identity,
// rechecked at the captured origin, not a new cell under the moving finger.
// Native bCanDrag/item validity and the original cancel route are prerequisites
// for supplying a source. No session handle is kept by this state helper.
struct InventoryDragSource {
    GfxIdentity panel{}, cell{};
    std::uint64_t content = 0;
    explicit operator bool() const;
    bool operator==(const InventoryDragSource&) const = default;
};

enum class InventoryDragCommand { Click, StartMove, Move, Drop, Cancel };
struct InventoryDragUpdate {
    InventoryDragCommand command = InventoryDragCommand::Cancel;
    InventoryDragOwner owner{};
    InventoryDragSource source{};
    float startX = 0, startY = 0, x = 0, y = 0;
};

// Short UP retains the existing complete native click. StartMove first sends
// native DOWN at startX/startY, then MOVE at x/y. Drop sends the original UP.
// Cancel must call the original scoped DragManager.cancelDrag before clearing
// native mouse state, and only while its captured native target is still safe.
// A replaced/dead scene gets no stale callbacks from this helper.
class UiInventoryGesture {
public:
    std::optional<InventoryDragUpdate> begin(const InventoryDragOwner&, bool inputAllowed,
                                           bool reset, std::uint64_t now);
    std::optional<InventoryDragUpdate> pointer(const PointerEvent&, const InventoryDragSource& freshSource);
    std::optional<InventoryDragUpdate> cancel();
    bool captured() const { return primary_ >= 0; }
    bool held() const { return held_; }
    bool ownsPointer(std::int32_t id) const;
    const InventoryDragSource& source() const { return source_; }
    float startX() const { return startX_; }
    float startY() const { return startY_; }
private:
    std::optional<InventoryDragUpdate> invalidate();
    InventoryDragUpdate update(InventoryDragCommand) const;
    void clear();
    int find(std::int32_t) const;
    InventoryDragOwner owner_{};
    InventoryDragSource source_{};
    std::array<std::int32_t, kMaxPointers> ids_{};
    std::int32_t primary_ = -1;
    std::uint64_t now_ = 0, sequence_ = 0, timestamp_ = 0;
    float startX_ = 0, startY_ = 0, x_ = 0, y_ = 0;
    bool valid_ = false, held_ = false, cancelled_ = false, moved_ = false;
};

} // namespace bl2::touch
