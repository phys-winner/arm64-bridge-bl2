#pragma once
#include "bl2_touch_widget_commands.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
#include <functional>
#include <memory>

namespace bl2::touch {
// Experimental protocol bit. The canonical protocol must accept this only with
// the reviewed stage5 mixed FIFO patch; it does not assert production DirectUi.
constexpr std::uint32_t kUiWidgetsTestCapability = 1u << 7;

// Values must be destroyed before their exclusive transport/scratch. close()
// reports ownership failure while both remain available for required cleanup.
class WidgetSessionScope final {
public:
    WidgetSessionScope(std::unique_ptr<GfxTransport>, std::uint32_t base,
        std::uint32_t movie, std::uint32_t scratch, std::uint32_t calls, bool mutation);
    ~WidgetSessionScope();
    GfxSession& values() { return *values_; }
    bool close();
private:
    std::unique_ptr<GfxTransport> transport_;
    std::unique_ptr<GfxSession> values_;
};

struct WidgetSceneInfo {
    std::uint64_t epoch = 0, scene = 0;
    std::uint32_t width = 0, height = 0, controllerId = 0;
};
// A lease captures IMMUTABLE native owner/controller/list identity, generation,
// viewport and epoch. current() must check focus, physical ownership and those
// captured identities without guest callbacks. It must never follow a mutable
// "latest prepared target" when a gesture survives to another input frame.
class WidgetSceneLease {
public:
    virtual ~WidgetSceneLease() = default;
    virtual const WidgetSceneInfo& info() const = 0;
    virtual bool current() const = 0;
    virtual UiModelSnapshot readModel() const = 0;
    virtual std::unique_ptr<WidgetSessionScope> open(std::uint32_t calls, bool mutation) = 0;
};
bool SameWidgetNativeModel(const UiModelSnapshot&, const UiModelSnapshot&);

// This is the pointer half of a UI sink. Its wrapper calls action() BEFORE the
// existing navigation sink to retire capture before a key can change the scene.
// Only the existing single mailbox consumer dispatches into this object.
class WidgetRouter final : public EngineSink {
public:
    using AcquireScene = std::function<std::shared_ptr<WidgetSceneLease>(const Frame&)>;
    explicit WidgetRouter(AcquireScene);
    ~WidgetRouter() override;
    void beginTouchFrame(const Frame&) override;
    void action(const ActionEdge&) override;
    void pointer(const PointerEvent&) override;
    void movement(float, float) override {}
    void relativeLook(float, float) override {}
    void endTouchFrame(const Frame&) override;
    bool captured() const { return gestures_.captured() && bool(binding_); }
    bool stopped() const { return stopped_; }
    const std::string& reason() const { return reason_; }
    std::uint64_t dispatched() const { return dispatched_; }
private:
    AcquireScene acquire_;
    Frame frame_{};
    bool admitted_ = false, stopped_ = false, failedFrame_ = false, registryFresh_ = false;
    std::uint32_t starts_ = 0, remainingCalls_ = 1024;
    std::uint64_t dispatched_ = 0, blockingActions_ = 0;
    std::string reason_ = "idle";
    WidgetGestures gestures_;
    WidgetRegistry registry_;
    UiModelSnapshot model_;
    std::shared_ptr<WidgetSceneLease> scene_;
    std::unique_ptr<WidgetSessionScope> operation_, retention_;
    std::unique_ptr<WidgetBinding> binding_;
    bool admitted(const Frame&) const;
    bool refresh();
    bool exactModel() const;
    void releaseCapture();
    void closeOperation();
    void reject(const char*);
    void stop(const char*);
};
}
