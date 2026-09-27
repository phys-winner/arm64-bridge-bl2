#pragma once
#include "bl2_touch_widget_gestures.h"
#include <functional>
#include <string>
#include <vector>

namespace bl2::touch {
// Keep one of these on a separate, read-only GfxSession from DOWN until UP or
// cancellation. Pinned list/data/renderer/row/slider values prevent address reuse
// while the input capture survives across engine frames. The retention session
// is not reused for per-frame queries; each operation has a fresh bounded session
// and exclusive scratch. Both sessions pin the same Movie independently.
class WidgetBinding final {
public:
    WidgetBinding(GfxSession& retention, const UiModelSnapshot&, const WidgetRegistry&,
                  const WidgetTarget* target);
    ~WidgetBinding();
    WidgetBinding(const WidgetBinding&) = delete;
    WidgetBinding& operator=(const WidgetBinding&) = delete;
    bool valid() const;
    const char* reason() const { return reason_; }
    bool matches(const UiModelSnapshot&, const WidgetRegistry&, bool requireRow) const;
    const WidgetTarget& target() const { return target_; }
    GfxHandle list() const { return list_; }
    GfxHandle renderer() const { return renderer_; }
    GfxHandle rowData() const { return rowData_; }
    GfxHandle inner() const { return inner_; }
    GfxHandle slider() const { return slider_; }
private:
    GfxSession& s_;
    std::vector<GfxHandle> retained_;
    std::uint64_t model_ = 0, owner_ = 0, nativeList_ = 0, provider_ = 0, route_ = 0;
    WidgetTarget target_;
    GfxHandle list_, data_, renderer_, rowData_, inner_, slider_;
    const char* reason_ = "not-bound";
    GfxHandle keep(GfxHandle);
};

enum class WidgetDeliveryState { Rejected, Unchanged, Dispatched, SceneChanged, Failed };
struct WidgetDelivery {
    WidgetDeliveryState state = WidgetDeliveryState::Rejected;
    const char* reason = "unvalidated";
    double actualValue = 0;
};

// exactModelStillCurrent must re-read the native list/provider structure and
// compare the owner/list/provider tokens/model hash from the supplied model,
// plus the sink's controller/focus/epoch/source guards. It is called immediately
// before each UI mutation, outside the mailbox lock, on the input thread.
// Result says a native semantic callback was dispatched, not hardware acceptance.
// Never retry an operation automatically after SceneChanged/Failed.
WidgetDelivery ApplyBl2WidgetCommand(GfxSession& operation, const WidgetBinding& retained,
    const UiModelSnapshot& freshModel, const WidgetRegistry& freshWidgets,
    const WidgetCommand&, std::uint32_t actualControllerId,
    const std::function<bool()>& exactModelStillCurrent);
}
