#include "bl2_touch_widget_router.h"
#include <algorithm>
#include <utility>

namespace bl2::touch {
WidgetSessionScope::WidgetSessionScope(std::unique_ptr<GfxTransport> vm, std::uint32_t base,
    std::uint32_t movie, std::uint32_t scratch, std::uint32_t calls, bool mutation)
    : transport_(std::move(vm)), values_(std::make_unique<GfxSession>(*transport_,base,movie,scratch,calls,mutation)) {}
WidgetSessionScope::~WidgetSessionScope() = default;
bool WidgetSessionScope::close() { values_->close(); return values_->cleanupOkay(); }
bool SameWidgetNativeModel(const UiModelSnapshot& a, const UiModelSnapshot& b) {
    return a.valid && b.valid && a.owner && a.list && a.provider && a.owner == b.owner &&
        a.list == b.list && a.provider == b.provider && a.structure == b.structure &&
        a.movieClass == b.movieClass && a.providerClass == b.providerClass && a.eventIds == b.eventIds;
}
WidgetRouter::WidgetRouter(AcquireScene acquire) : acquire_(std::move(acquire)) {}
WidgetRouter::~WidgetRouter() { closeOperation(); releaseCapture(); }
bool WidgetRouter::admitted(const Frame& frame) const {
    return !stopped_ && frame.fresh && (frame.context.capabilities & kUiWidgetsTestCapability) &&
        !(frame.context.capabilities & (GameplayInput | PhysicalOwnsInput)) &&
        (frame.context.mode == Mode::Frontend || frame.context.mode == Mode::Menu || frame.context.mode == Mode::Dialog);
}
void WidgetRouter::stop(const char* reason) { stopped_ = true; admitted_ = false; reason_ = reason; gestures_.cancel(); }
void WidgetRouter::releaseCapture() {
    binding_.reset(); // Its destructor still needs the retention session.
    if (retention_) {
        if (!retention_->close()) stop("retained-managed-cleanup-fault-disabled");
        retention_.reset();
    }
}
void WidgetRouter::closeOperation() {
    if (operation_) {
        remainingCalls_ -= std::min(remainingCalls_,operation_->values().calls());
        if (!operation_->close()) stop("operation-managed-cleanup-fault-disabled");
        operation_.reset();
    }
    registryFresh_ = false;
}
void WidgetRouter::reject(const char* reason) {
    reason_ = reason; failedFrame_ = true; gestures_.cancel();
    closeOperation(); releaseCapture();
}
void WidgetRouter::beginTouchFrame(const Frame& frame) {
    // A caller omitting endTouchFrame still cannot reuse last frame's budget.
    closeOperation();
    const bool changed = frame.resetReasons || frame.epoch != frame_.epoch ||
        frame.context.sceneIdentity != frame_.context.sceneIdentity || !admitted(frame) ||
        (scene_ && !scene_->current());
    if (changed) { releaseCapture(); gestures_.clear(); scene_.reset(); registry_ = {}; blockingActions_ = 0; }
    frame_ = frame; admitted_ = admitted(frame); failedFrame_ = false; starts_ = 0; remainingCalls_ = 1024;
}
bool WidgetRouter::exactModel() const {
    return admitted_ && !failedFrame_ && !stopped_ && scene_ && scene_->current() &&
        SameWidgetNativeModel(model_,scene_->readModel());
}
bool WidgetRouter::refresh() {
    if (!admitted_ || failedFrame_ || stopped_) return false;
    if (!scene_) {
        scene_ = acquire_ ? acquire_(frame_) : nullptr;
        if (!scene_) { reject("native-scene-not-admitted"); return false; }
    }
    const auto& info = scene_->info();
    if (!scene_->current() || info.epoch != frame_.epoch || info.scene != frame_.context.sceneIdentity ||
        !info.width || !info.height || info.width > 16384 || info.height > 16384 || info.controllerId > 3) {
        reject("native-scene-expired"); return false;
    }
    if (!operation_ && remainingCalls_) operation_ = scene_->open(remainingCalls_,true);
    if (!operation_ || !operation_->values().good()) { reject("operation-session-unavailable"); return false; }
    if (!registryFresh_) {
        model_ = scene_->readModel();
        registry_ = CaptureBl2Widgets(operation_->values(),model_,info.width,info.height);
        if (!registry_.valid) { const auto failure = registry_.reason; reject(failure.c_str()); return false; }
        if (!exactModel()) { reject("native-model-changed-during-capture"); return false; }
        registryFresh_ = true;
    }
    return true;
}
void WidgetRouter::action(const ActionEdge& edge) {
    if (edge.sourceEpoch != frame_.epoch || !admitted_) return;
    const auto bit = actionBit(edge.action);
    if (edge.down) blockingActions_ |= bit;
    else blockingActions_ &= ~bit;
    // Cancel before the navigation callback. Never let Back and pointer UP
    // synthesize a click in a newly selected row or newly opened movie.
    gestures_.cancel(); closeOperation(); releaseCapture(); registryFresh_ = false;
    reason_ = "competing-ui-action";
}
void WidgetRouter::pointer(const PointerEvent& event) {
    if (!admitted_ || failedFrame_ || stopped_ || event.sourceEpoch != frame_.epoch) return;
    if (blockingActions_) {
        // Keep any known pointer IDs until their matching UP while a navigation
        // action owns input. A pointer that starts here cannot become a DOWN
        // retrospectively when the action is released.
        gestures_.cancel(); (void)gestures_.pointer(event,registry_); gestures_.cancel();
        releaseCapture(); return;
    }
    if (!refresh()) return;
    gestures_.begin(frame_.epoch,frame_.context.sceneIdentity,true,false,registry_);
    const bool wasCaptured = captured();
    const auto command = gestures_.pointer(event,registry_);
    if (event.phase == PointerPhase::Down && !wasCaptured && gestures_.captured()) {
        // Bounded adversarial packet stream. Four complete starts fit an input
        // frame; excess gestures fail closed, with no delayed synthetic click.
        if (++starts_ > 4) { reject("gesture-start-budget"); return; }
        retention_ = scene_->open(128,false);
        if (!retention_ || !retention_->values().good()) { reject("retention-session-unavailable"); return; }
        binding_ = std::make_unique<WidgetBinding>(retention_->values(),model_,registry_,gestures_.target());
        if (!binding_->valid() || !exactModel()) { reject("retained-target-changed"); return; }
    }
    if (command) {
        if (!binding_ || command->epoch != frame_.epoch || !exactModel()) { reject("command-without-live-capture"); return; }
        const auto result = ApplyBl2WidgetCommand(operation_->values(),*binding_,model_,registry_,*command,
            scene_->info().controllerId,[this] { return exactModel(); });
        reason_ = result.reason;
        registryFresh_ = false; // A setter/click may change attached AS data.
        if (result.state == WidgetDeliveryState::Dispatched) ++dispatched_;
        if (result.state == WidgetDeliveryState::Rejected || result.state == WidgetDeliveryState::Failed ||
            result.state == WidgetDeliveryState::SceneChanged) { reject(result.reason); return; }
    }
    if (!gestures_.captured()) releaseCapture();
    if (!scene_->current()) reject("native-scene-changed-after-pointer");
}
void WidgetRouter::endTouchFrame(const Frame&) {
    closeOperation();
    if (!admitted_ || stopped_ || !scene_ || !scene_->current() || !gestures_.captured()) {
        releaseCapture();
        if (scene_ && !scene_->current()) gestures_.cancel();
        // A canceled multi-finger sequence must keep its IDs until UP/cancel,
        // so the second finger never becomes a new primary owner.
        if (!gestures_.captured()) scene_.reset();
    }
}
}
