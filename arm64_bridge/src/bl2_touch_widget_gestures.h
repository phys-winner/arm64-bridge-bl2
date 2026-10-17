#pragma once
#include "bl2_touch_widget_registry.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include <array>
#include <optional>

namespace bl2::touch {
enum class WidgetCommandKind { Click, Scroll, Slider, Spinner, Select };
struct WidgetCommand {
    WidgetCommandKind kind = WidgetCommandKind::Click;
    std::uint64_t route = 0, content = 0, epoch = 0;
    WidgetTarget target;
    std::int32_t scrollPosition = 0;
    double sliderFraction = 0;
    std::int32_t spinnerIndex = 0;
};
// Pointer-only transaction layer. The eventual sink must pin the captured
// native target across the gesture and revalidate fresh model/widget ownership
// before executing each command. This core never calls guest code. Action and
// pointer edges must be merged by sequence before full UI integration.
class WidgetGestures final {
public:
    explicit WidgetGestures(bool selectOnDown = false) : selectOnDown_(selectOnDown) {}
    void begin(std::uint64_t epoch, std::uint64_t scene, bool admitted, bool reset,
               const WidgetRegistry&);
    std::optional<WidgetCommand> pointer(const PointerEvent&, const WidgetRegistry&);
    // Any competing UI action/source cancels, never commits a guest release.
    void cancel();
    void clear();
    bool rebindSpinner(const WidgetRegistry&);
    bool rebindSelection(const WidgetRegistry&);
    bool captured() const { return primary_ >= 0 && !cancelled_; }
    bool cancelled() const { return cancelled_; }
    bool scrolling() const { return captured() && drag_ == Drag::Scroll; }
    const WidgetTarget* target() const { return captured() && target_.id ? &target_ : nullptr; }
private:
    enum class Drag { Pending, Scroll, Slider, Numeric };
    std::array<std::int32_t,kMaxPointers> pointers_ = [] {
        std::array<std::int32_t,kMaxPointers> value{}; value.fill(-1); return value;
    }();
    std::uint64_t epoch_ = 0, scene_ = 0, route_ = 0, content_ = 0;
    std::uint32_t width_ = 0, height_ = 0;
    std::int32_t primary_ = -1, startScroll_ = 0, lastScroll_ = -1;
    bool admitted_ = false, cancelled_ = false, sliderIntent_ = false, numericIntent_ = false;
    bool selectedAtDown_ = false;
    const bool selectOnDown_;
    double lastFraction_ = -1, pitch_ = 0, startValue_ = 0;
    std::uint64_t startedMs_ = 0;
    WidgetPoint start_;
    WidgetTarget target_;
    Drag drag_ = Drag::Pending;
    int find(std::int32_t) const;
    bool retained(const WidgetRegistry&) const;
    std::optional<WidgetCommand> command(WidgetCommandKind, const WidgetRegistry&, WidgetPoint);
};
}
