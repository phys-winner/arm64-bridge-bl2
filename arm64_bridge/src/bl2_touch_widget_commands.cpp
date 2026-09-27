#include "bl2_touch_widget_commands.h"
#include <algorithm>
#include <cmath>

namespace bl2::touch {
namespace {
struct Value {
    GfxSession& s; GfxHandle h;
    ~Value() { s.release(h); }
    operator GfxHandle() const { return h; }
    explicit operator bool() const { return bool(h); }
};
bool sameIdentity(GfxSession& s, GfxHandle v, const GfxIdentity& expected) {
    GfxIdentity actual; return s.identity(v,actual) && actual == expected;
}
bool number(GfxSession& s, GfxHandle object, const char* name, double& result) {
    const Value v{s,s.member(object,name)}; return v && s.number(v,result);
}
bool boolean(GfxSession& s, GfxHandle object, const char* name, bool& result) {
    const Value v{s,s.member(object,name)}; return v && s.boolean(v,result);
}
bool rectEqual(const GfxRect& a, const GfxRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
bool sameTarget(const WidgetTarget& a, const WidgetTarget& b) {
    return a.id && a.id == b.id && a.index == b.index && a.eventId == b.eventId && a.kind == b.kind &&
        a.enabled && b.enabled && a.visible && b.visible && a.caption == b.caption &&
        a.rendererType == b.rendererType && a.renderer == b.renderer && a.data == b.data &&
        a.inner == b.inner && a.slider == b.slider && rectEqual(a.bounds,b.bounds) &&
        rectEqual(a.sliderBounds,b.sliderBounds) && a.minimum == b.minimum && a.maximum == b.maximum &&
        a.increment == b.increment && a.sliderStart.x == b.sliderStart.x && a.sliderStart.y == b.sliderStart.y &&
        a.sliderEnd.x == b.sliderEnd.x && a.sliderEnd.y == b.sliderEnd.y;
}
WidgetDelivery invoke(GfxSession& s, GfxHandle object, const char* name,
                      std::initializer_list<GfxHandle> arguments) {
    const auto before = s.calls();
    const Value output{s,s.invoke(object,name,arguments,true)};
    if (output && s.good()) return {WidgetDeliveryState::Dispatched,"semantic-callback-dispatched",0};
    if (!s.good() && s.calls() != before) return {WidgetDeliveryState::SceneChanged,"context-changed-in-semantic-callback",0};
    return {WidgetDeliveryState::Failed,"semantic-call-failed",0};
}
}
GfxHandle WidgetBinding::keep(GfxHandle h) { if (h) retained_.push_back(h); return h; }
WidgetBinding::WidgetBinding(GfxSession& session, const UiModelSnapshot& model,
                             const WidgetRegistry& widgets, const WidgetTarget* row)
    : s_(session), model_(model.structure), owner_(model.owner), nativeList_(model.list),
      provider_(model.provider), route_(widgets.route) {
    if (!session.good() || !model.valid || !widgets.valid || !owner_ || !nativeList_ || !provider_ ||
        model.structure != widgets.model || (model.movieClass != "FrontendGFxMovie" &&
        model.movieClass != "PauseGFxMovie" && model.movieClass != "OptionsGFxMovie")) return;
    list_ = keep(s_.pin(static_cast<std::uint32_t>(nativeList_ >> 32) + 0x3C));
    data_ = keep(s_.member(list_,"data"));
    if (!sameIdentity(s_,list_,widgets.list) || !sameIdentity(s_,data_,widgets.data)) { reason_ = "binding-list-data"; return; }
    const Value provider{s_,s_.member(list_,"dataProvider")};
    if (!sameIdentity(s_,provider,widgets.data)) { reason_ = "binding-uncommitted-provider"; return; }
    if (row) {
        target_ = *row;
        const auto match = std::find_if(widgets.rows.begin(),widgets.rows.end(),[&](const WidgetTarget& r) { return sameTarget(r,*row); });
        if (match == widgets.rows.end() || row->index < widgets.scrollPosition || row->index >= widgets.rowCount ||
            row->index >= widgets.scrollPosition + widgets.totalRenderers || row->index < 0 ||
            static_cast<std::size_t>(row->index) >= model.eventIds.size() || model.eventIds[row->index] != row->eventId) {
            reason_ = "binding-row-mismatch"; return;
        }
        const Value renderers{s_,s_.member(list_,"renderers")};
        renderer_ = keep(s_.element(renderers,static_cast<std::uint32_t>(row->index - widgets.scrollPosition)));
        rowData_ = keep(s_.element(data_,static_cast<std::uint32_t>(row->index)));
        const Value actualData{s_,s_.member(renderer_,"data")}, actualOwner{s_,s_.member(renderer_,"owner")};
        double index = -1; bool disabled = true, unselectable = true;
        if (!sameIdentity(s_,renderer_,row->renderer) || !sameIdentity(s_,rowData_,row->data) ||
            !sameIdentity(s_,actualData,row->data) || !sameIdentity(s_,actualOwner,widgets.list) ||
            !number(s_,renderer_,"index",index) || index != row->index ||
            !boolean(s_,renderer_,"disabled",disabled) || disabled ||
            !boolean(s_,rowData_,"unselectable",unselectable) || unselectable) { reason_ = "binding-live-row"; return; }
        if (row->kind == WidgetKind::Slider) {
            inner_ = keep(s_.member(renderer_,"inner"));
            slider_ = keep(s_.member(inner_,"slider"));
            const Value innerData{s_,s_.member(inner_,"data")};
            if (!sameIdentity(s_,inner_,row->inner) || !sameIdentity(s_,slider_,row->slider) ||
                !sameIdentity(s_,innerData,row->data) || !boolean(s_,slider_,"disabled",disabled) || disabled) {
                reason_ = "binding-live-slider"; return;
            }
        }
    }
    if (s_.good()) reason_ = nullptr;
}
WidgetBinding::~WidgetBinding() { for (auto i = retained_.rbegin(); i != retained_.rend(); ++i) s_.release(*i); }
bool WidgetBinding::valid() const { return !reason_ && s_.good(); }
bool WidgetBinding::matches(const UiModelSnapshot& model, const WidgetRegistry& widgets, bool requireRow) const {
    if (!valid() || !model.valid || !widgets.valid || model.structure != model_ || widgets.model != model_ ||
        model.owner != owner_ || model.list != nativeList_ || model.provider != provider_ || widgets.route != route_) return false;
    if (!requireRow) return true;
    for (const auto& row : widgets.rows) if (sameTarget(row,target_)) return true;
    return false;
}
WidgetDelivery ApplyBl2WidgetCommand(GfxSession& s, const WidgetBinding& retained,
    const UiModelSnapshot& model, const WidgetRegistry& widgets, const WidgetCommand& command,
    std::uint32_t controllerId, const std::function<bool()>& exactModelStillCurrent) {
    const bool rowRequired = command.kind != WidgetCommandKind::Scroll;
    if (!s.good() || !exactModelStillCurrent || controllerId > 3 || command.route != widgets.route ||
        !retained.matches(model,widgets,rowRequired) || !exactModelStillCurrent() ||
        (rowRequired && !sameTarget(retained.target(),command.target))) return {};
    // A second session reads/pins the currently attached values. The old retained
    // values prevent allocator ABA but are never blindly treated as still attached.
    WidgetBinding live(s,model,widgets,rowRequired ? &command.target : nullptr);
    if (!live.valid()) return {WidgetDeliveryState::Rejected,live.reason() ? live.reason() : "binding-stale",0};
    if (command.kind == WidgetCommandKind::Click) {
        if (command.target.kind != WidgetKind::Button || !exactModelStillCurrent()) return {};
        const Value controller{s,s.number(controllerId)}, button{s,s.number(0)};
        if (!controller || !button) return {};
        return invoke(s,live.renderer(),"handleClick",{controller,button});
    }
    if (command.kind == WidgetCommandKind::Scroll) {
        if (command.scrollPosition < 0 || widgets.rowCount < widgets.totalRenderers ||
            command.scrollPosition > widgets.rowCount - widgets.totalRenderers) return {};
        double current = -1;
        if (!number(s,live.list(),"scrollPosition",current)) return {};
        if (current == command.scrollPosition) return {WidgetDeliveryState::Unchanged,"scroll-unchanged",current};
        const Value value{s,s.number(command.scrollPosition)};
        if (!value || !exactModelStillCurrent()) return {};
        return invoke(s,live.list(),"__set__scrollPosition",{value});
    }
    if (command.kind != WidgetCommandKind::Slider || command.target.kind != WidgetKind::Slider ||
        !std::isfinite(command.sliderFraction) || command.sliderFraction < 0 || command.sliderFraction > 1) return {};
    const auto& target = command.target;
    double min = 0,max = 0,increment = 0;
    if (!number(s,live.slider(),"minimum",min) || !number(s,live.slider(),"maximum",max) ||
        !number(s,live.slider(),"snapInterval",increment) || min != target.minimum || max != target.maximum ||
        increment != target.increment || max <= min || increment <= 0 || !exactModelStillCurrent()) return {};
    const Value value{s,s.number(min + command.sliderFraction * (max - min))};
    if (!value) return {};
    auto delivery = invoke(s,live.slider(),"__set__value",{value});
    if (delivery.state != WidgetDeliveryState::Dispatched) return delivery;
    // The shipped setter clamps/snaps and redraws but emits no change event.
    // Read its actual value; never emulate its rounding with a host approximation.
    double actual = 0, modelValue = 0;
    if (!number(s,live.slider(),"value",actual) || !number(s,live.rowData(),"currentValue",modelValue) ||
        actual < min || actual > max || !exactModelStillCurrent())
        return {WidgetDeliveryState::Failed,"slider-set-before-validation-loss",actual};
    if (actual == modelValue) return {WidgetDeliveryState::Unchanged,"slider-unchanged",actual};
    // This handler ignores its event argument and reads the real slider. It
    // updates row.currentValue and invokes owner.onSliderValueChange(index,...).
    // No mouse capture, no release callback and no preview for unchanged values.
    delivery = invoke(s,live.inner(),"onSliderChange",{});
    delivery.actualValue = actual;
    return delivery;
}
}
