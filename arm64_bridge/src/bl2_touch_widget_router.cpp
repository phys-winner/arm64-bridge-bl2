#include "bl2_touch_widget_router.h"
#include <algorithm>
#include <utility>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace bl2::touch {
namespace {
bool diagnosticsEnabled() {
#if defined(__ANDROID__)
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX] = {};
        __system_property_get("debug.bl2bridge.touch_diagnostics",value);
        return value[0] == '1' && !value[1];
    }();
    return enabled;
#endif
    return false;
}
bool pointerDiagnostic(const PointerEvent& event) {
    static unsigned remaining = 12;
    if (diagnosticsEnabled() && event.phase == PointerPhase::Down && remaining) { --remaining; return true; }
    return false;
}
}
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
WidgetRouter::WidgetRouter(AcquireScene acquire, bool selectOnDown)
    : acquire_(std::move(acquire)), gestures_(selectOnDown) {}
WidgetRouter::~WidgetRouter() { closeOperation(); releaseCapture(); }
bool WidgetRouter::admitted(const Frame& frame) const {
    return !stopped_ && frame.fresh && (frame.context.capabilities & kUiWidgetsTestCapability) &&
        !(frame.context.capabilities & (GameplayInput | PhysicalOwnsInput)) &&
        (frame.context.mode == Mode::Frontend || frame.context.mode == Mode::Menu || frame.context.mode == Mode::Dialog);
}
void WidgetRouter::stop(const char* reason) { pendingCommand_.reset(); stopped_ = true; admitted_ = false; reason_ = reason; gestures_.cancel(); }
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
    pendingCommand_.reset();
    reason_ = reason; failedFrame_ = true; gestures_.cancel();
    closeOperation(); releaseCapture();
}
void WidgetRouter::beginTouchFrame(const Frame& frame) {
    // A caller omitting endTouchFrame still cannot reuse last frame's budget.
    closeOperation();
    pendingCommand_.reset();
    const bool changed = frame.resetReasons || frame.epoch != frame_.epoch ||
        frame.context.sceneIdentity != frame_.context.sceneIdentity || !admitted(frame) ||
        (scene_ && !scene_->current());
    if (changed) { releaseCapture(); gestures_.clear(); scene_.reset(); registry_ = {}; blockingActions_ = 0;spinnerRefresh_=selectionRefresh_=false; }
    frame_ = frame; admitted_ = admitted(frame); failedFrame_ = false; starts_ = 0; remainingCalls_ = 1024;
    framePointers_ = coalescedCommands_ = 0;
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
    pendingCommand_.reset();
    const auto bit = actionBit(edge.action);
    if (edge.down) blockingActions_ |= bit;
    else blockingActions_ &= ~bit;
    // Cancel before the navigation callback. Never let Back and pointer UP
    // synthesize a click in a newly selected row or newly opened movie.
    gestures_.cancel(); closeOperation(); releaseCapture(); registryFresh_ = false;
    reason_ = "competing-ui-action";
}
void WidgetRouter::deliver(const WidgetCommand& command) {
    if (!operation_ || !binding_ || command.epoch != frame_.epoch || !exactModel()) {
        reject("command-without-live-capture"); return;
    }
    const auto result = ApplyBl2WidgetCommand(operation_->values(),*binding_,model_,registry_,command,
        scene_->info().controllerId,[this] { return exactModel(); });
#if defined(__ANDROID__)
    static unsigned diagnosticCommands = 0;
    if (diagnosticsEnabled() && diagnosticCommands < 24) {
        ++diagnosticCommands;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "widgets delivery kind=%u state=%u calls=%u remaining=%u pointers=%u coalesced=%u frame=%llu actual=%.6g reason=%s",
            static_cast<unsigned>(command.kind),static_cast<unsigned>(result.state),operation_->values().calls(),
            remainingCalls_ - std::min(remainingCalls_,operation_->values().calls()),framePointers_,coalescedCommands_,
            static_cast<unsigned long long>(frame_.frameSerial),result.actualValue,result.reason);
    }
#endif
    reason_ = result.reason;
    // The reviewed selectedIndex setter only updates selection/its Button
    // state for this already-visible target. Keep this frame's validated rows
    // so a DOWN+UP packet does not recapture the full list twice. The next
    // engine boundary captures again before adopting animated axis geometry.
    registryFresh_ = command.kind == WidgetCommandKind::Select &&
        (result.state == WidgetDeliveryState::Dispatched || result.state == WidgetDeliveryState::Unchanged);
    if (registryFresh_) registry_.selectedIndex = command.target.index;
    if (result.state == WidgetDeliveryState::Dispatched) {
        ++dispatched_;
        spinnerRefresh_ = command.kind == WidgetCommandKind::Spinner;
        selectionRefresh_ = command.kind == WidgetCommandKind::Select;
    }
    if (result.state == WidgetDeliveryState::Rejected || result.state == WidgetDeliveryState::Failed ||
        result.state == WidgetDeliveryState::SceneChanged) reject(result.reason);
}
void WidgetRouter::flushCommand() {
    if (!pendingCommand_) return;
    const auto command = *pendingCommand_; pendingCommand_.reset();
    deliver(command);
}
void WidgetRouter::pointer(const PointerEvent& event) {
    const bool diagnostic = pointerDiagnostic(event);
#if defined(__ANDROID__)
    if (diagnostic) __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
        "widgets down=(%.6f,%.6f) admitted=%u fresh=%u failed=%u stopped=%u caps=%u mode=%u "
        "pointerEpoch=%llu frameEpoch=%llu blocked=%llu reason=%s",
        event.surfaceX,event.surfaceY,admitted_ ? 1u : 0u,frame_.fresh ? 1u : 0u,
        failedFrame_ ? 1u : 0u,stopped_ ? 1u : 0u,frame_.context.capabilities,static_cast<unsigned>(frame_.context.mode),
        static_cast<unsigned long long>(event.sourceEpoch),static_cast<unsigned long long>(frame_.epoch),
        static_cast<unsigned long long>(blockingActions_),reason_.c_str());
#else
    (void)diagnostic;
#endif
    if (!admitted_ || failedFrame_ || stopped_ || event.sourceEpoch != frame_.epoch) return;
    ++framePointers_;
    if (blockingActions_) {
        pendingCommand_.reset();
        // Keep any known pointer IDs until their matching UP while a navigation
        // action owns input. A pointer that starts here cannot become a DOWN
        // retrospectively when the action is released.
        gestures_.cancel(); (void)gestures_.pointer(event,registry_); gestures_.cancel();
        releaseCapture(); return;
    }
    if (!refresh()) {
#if defined(__ANDROID__)
        if (diagnostic) __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics","widgets capture rejected=%s",reason_.c_str());
#endif
        return;
    }
#if defined(__ANDROID__)
    if (diagnostic) {
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "widgets viewport=(%u,%u,%u,%u,%u,%u) surface=(%u,%u) frame=(%.3f,%.3f,%.3f,%.3f) "
            "list=(%.6f,%.6f,%.6f,%.6f) selected=%d scroll=%d rows=%d renderers=%d",
            registry_.viewport.words[0],registry_.viewport.words[1],registry_.viewport.words[2],registry_.viewport.words[3],
            registry_.viewport.words[4],registry_.viewport.words[5],registry_.surfaceWidth,registry_.surfaceHeight,
            registry_.frame.left,registry_.frame.top,registry_.frame.right,registry_.frame.bottom,
            registry_.listBounds.left,registry_.listBounds.top,registry_.listBounds.right,registry_.listBounds.bottom,
            registry_.selectedIndex,registry_.scrollPosition,registry_.rowCount,registry_.totalRenderers);
        for (const auto& row : registry_.rows)
            __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics","widgets row=%d event=%d enabled=%u visible=%u "
                "kind=%u value=%.6g caption='%s' bounds=(%.6f,%.6f,%.6f,%.6f) "
                "axis=(%.6f,%.6f,%.6f,%.6f) sliderBounds=(%.6f,%.6f,%.6f,%.6f) valueBounds=(%.6f,%.6f,%.6f,%.6f)",
                row.index,row.eventId,row.enabled ? 1u : 0u,row.visible ? 1u : 0u,static_cast<unsigned>(row.kind),row.value,
                row.caption.c_str(),row.bounds.left,row.bounds.top,row.bounds.right,row.bounds.bottom,
                row.sliderStart.x,row.sliderStart.y,row.sliderEnd.x,row.sliderEnd.y,
                row.sliderBounds.left,row.sliderBounds.top,row.sliderBounds.right,row.sliderBounds.bottom,
                row.valueBounds.left,row.valueBounds.top,row.valueBounds.right,row.valueBounds.bottom);
    }
#endif
    if ((spinnerRefresh_ || selectionRefresh_) && gestures_.captured()) {
        const bool selection = selectionRefresh_;
        selectionRefresh_ = false;
        spinnerRefresh_=false;
        if (!(selection ? gestures_.rebindSelection(registry_) : gestures_.rebindSpinner(registry_))) {
            reject(selection ? "selection-refresh-changed-target" : "spinner-refresh-changed-setting"); return;
        }
        releaseCapture();retention_=scene_->open(128,false);
        if(!retention_ || !retention_->values().good()) {reject("spinner-refresh-retention-unavailable");return;}
        binding_=std::make_unique<WidgetBinding>(retention_->values(),model_,registry_,gestures_.target());
        if(!binding_->valid() || !exactModel()) {reject("spinner-refresh-binding-changed");return;}
    }
    gestures_.begin(frame_.epoch,frame_.context.sceneIdentity,true,false,registry_);
    const bool wasCaptured = captured();
    const auto command = gestures_.pointer(event,registry_);
#if defined(__ANDROID__)
    if(diagnostic) {
        const auto* target=gestures_.target();
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics","widgets target=%d event=%d kind=%u cancelled=%u",
            target ? target->index : -1,target ? target->eventId : 0,
            target ? static_cast<unsigned>(target->kind) : static_cast<unsigned>(WidgetKind::Unsupported),
            gestures_.cancelled() ? 1u : 0u);
    }
#endif
    if (event.phase == PointerPhase::Down && !wasCaptured && gestures_.captured()) {
        // Bounded adversarial packet stream. Four complete starts fit an input
        // frame; excess gestures fail closed, with no delayed synthetic click.
        if (++starts_ > 4) { reject("gesture-start-budget"); return; }
        retention_ = scene_->open(128,false);
        if (!retention_ || !retention_->values().good()) { reject("retention-session-unavailable"); return; }
        binding_ = std::make_unique<WidgetBinding>(retention_->values(),model_,registry_,gestures_.target());
        if (!binding_->valid() || !exactModel()) { reject("retained-target-changed"); return; }
    }
    if (gestures_.cancelled()) pendingCommand_.reset();
    else if (command) {
        if (command->kind == WidgetCommandKind::Click || command->kind == WidgetCommandKind::Select) deliver(*command);
        else { if (pendingCommand_) ++coalescedCommands_; pendingCommand_ = *command; }
    }
    // A real UP commits its final value before another gesture can change the
    // scene. MOVE packets otherwise share one capture and one delivery/frame.
    if (event.phase == PointerPhase::Up) flushCommand();
    if (failedFrame_) return;
    if (!gestures_.captured()) releaseCapture();
    if (!scene_->current()) reject("native-scene-changed-after-pointer");
}
void WidgetRouter::endTouchFrame(const Frame&) {
    flushCommand();
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
