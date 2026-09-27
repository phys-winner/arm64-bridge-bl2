#pragma once
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include <array>
#include <cstdint>
#include <optional>

namespace bl2::touch {

// Stage-3 additive capability bits. DirectUi remains reserved for the complete
// press/drag/cancel adapter; a tap probe must not claim that capability.
constexpr std::uint32_t kUiNavigation = 1u << 5;
constexpr std::uint32_t kUiTapTest = 1u << 6;
constexpr std::uint64_t kUiActions = actionBit(Action::Back) | actionBit(Action::Confirm) |
    actionBit(Action::UiUp) | actionBit(Action::UiDown) |
    actionBit(Action::UiLeft) | actionBit(Action::UiRight);

struct UiTap { float x = 0, y = 0; };

// Touch DOWN is a local intent, not a guest key/mouse DOWN. Only a matching UP
// commits a complete short UI command. Reset, cancellation, source takeover or
// scene replacement therefore cannot accidentally activate release-driven UI.
class UiTransactions {
public:
    void begin(std::uint64_t epoch, std::uint64_t scene, bool valid, bool reset,
               std::uint32_t width, std::uint32_t height);
    std::optional<Action> action(const ActionEdge& edge);
    std::optional<UiTap> pointer(const PointerEvent& event);
    void clear();
private:
    int find(std::int32_t id) const;
    std::array<std::int32_t, kMaxPointers> pointers_ = [] {
        std::array<std::int32_t, kMaxPointers> result{}; result.fill(-1); return result;
    }();
    std::uint64_t epoch_ = 0, scene_ = 0, held_ = 0;
    std::uint32_t width_ = 0, height_ = 0;
    std::int32_t primary_ = -1;
    float startX_ = 0, startY_ = 0;
    bool valid_ = false, cancelled_ = false, keysCancelled_ = false;
};

} // namespace bl2::touch
