#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <array>
#include <algorithm>
#include <cmath>
#include <string_view>
#include "../../touch_native_design/arm64_bridge/bl2_touch_engine_adapter.h"
class DynarmicHost;

bool VerifyBl2PauseDebugMenuInput(const std::uint8_t* bytes, std::size_t size);
bool InstallBl2PauseDebugMenu(DynarmicHost& host);
void RefreshBl2PauseDebugMenu(DynarmicHost& host);

namespace bl2::touch {
constexpr std::uint64_t kDebugMenuActions = actionBit(Action::Back) | actionBit(Action::Confirm) |
    actionBit(Action::UiUp) | actionBit(Action::UiDown) | actionBit(Action::UiLeft) | actionBit(Action::UiRight) |
    actionBit(Action::Reload) | actionBit(Action::NextWeapon) | actionBit(Action::Skill) | actionBit(Action::Sprint) |
    actionBit(Action::PreviousWeapon) | actionBit(Action::Inventory);
constexpr bool debugCategoryNavigation(Action action) {
    return action==Action::UiLeft || action==Action::UiRight ||
        action==Action::PreviousWeapon || action==Action::Inventory;
}

struct DebugTextExtent { float text=0,fixed=0; };
// Scene open/close advances the input epoch while this registered viewport UI
// and its column allocation survive. Restore owned display metrics by their
// actual owner chain; input validation still requires the current scene/epoch.
struct DebugColumnCacheOwner {
    std::uint64_t ui=0,engine=0,client=0;
    std::uint32_t world=0;
    bool operator==(const DebugColumnCacheOwner&) const = default;
};
// Original RefreshCurrentMenu reallocates its arrays every half second. A
// pending row intent survives only an identical semantic row/window/owner;
// allocation addresses deliberately remain in the ordinary input content key.
struct DebugRowCaptureKey {
    DebugColumnCacheOwner owner;
    std::uint64_t controller=0,schema=0,input=0,scene=0,generation=0,epoch=0,menu=0,commands=0,footer=0;
    std::uint32_t viewport=0,controllerId=0,width=0,height=0,kind=0,index=0,selected=0,
        count=0,top=0,ipp=0,main=0,sub=0;
    float requested=0;
    bool operator==(const DebugRowCaptureKey&) const = default;
};
inline bool debugPreserveRowCapture(DebugRowCaptureKey before,const DebugRowCaptureKey& after,
                                    bool ownSelection=false) {
    if(!before.owner.ui || !before.owner.engine || !before.owner.client || !before.owner.world ||
        !before.controller || !before.schema || !before.input || !before.scene || !before.generation ||
        !before.epoch || !before.menu || !before.commands || !before.viewport || !before.width || !before.height ||
        (before.kind!=0 && before.kind!=3) || !before.count || before.count>512 || before.index>=before.count ||
        before.selected>=before.count || before.top>=before.count || !before.ipp || before.ipp>512 ||
        !std::isfinite(before.requested) || before.requested<1 || before.requested>2 ||
        (before.kind==3 && !before.footer))return false;
    if(ownSelection) {
        if(before.kind!=0 || after.selected!=before.index)return false;
        before.selected=after.selected;
    }
    return before==after;
}
// Array addresses can survive an in-place RefreshCurrentMenu. Validate the
// captured command independently of the ordinary allocation content key.
inline bool debugCapturedRowCurrent(bool captured,std::uint32_t kind,
                                    const DebugRowCaptureKey& before,const DebugRowCaptureKey& after) {
    return !captured || (kind!=0 && kind!=3) || debugPreserveRowCapture(before,after);
}
struct DebugStrLenFrameAbi {
    static constexpr std::uint32_t saveSite=0x018511D4,measureSite=0x01851238;
    static constexpr std::uint32_t saveWord0=0xE5945040,saveWord1=0xE5843050;
    static constexpr std::uint32_t localBytes=0x34,frameSlot=0x2C;
};
// Text grows; authored gaps/icons keep their original size. Use one factor
// for both native measurement and drawing throughout an original RenderMenu.
inline float debugFontScale(float available,const DebugTextExtent* extents,std::size_t count,float requested) {
    if(!std::isfinite(available) || available<=0 || !std::isfinite(requested) ||
        requested<1 || requested>2 || (count && !extents))return 1;
    float scale=requested;
    for(std::size_t i=0;i<count;++i) {
        const auto& e=extents[i];
        if(!std::isfinite(e.text) || !std::isfinite(e.fixed) || e.text<0 || e.fixed<0)return 1;
        if(e.fixed>available)return 1;
        if(e.text>0)scale=std::min(scale,(available-e.fixed)/e.text);
    }
    return std::clamp(scale,1.f,requested);
}
inline float debugRestoreColumnWidth(float current,float original,float factor) {
    return std::isfinite(current) && std::isfinite(original) && std::isfinite(factor) && factor>1 && factor<=2 &&
        original>=0 && std::abs(current-original*factor)<.5f ? original : current;
}
// RefreshCurrentMenu rebuilds arrays every half second. Their addresses belong
// to input validation; unchanged authored text/layout can keep its measured fit.
struct DebugFontRefreshKey {
    DebugColumnCacheOwner owner;
    std::uint64_t canvas=0,generation=0,scene=0,metrics=0;
    std::uint32_t main=0,sub=0,rows=0,columns=0,width=0,height=0;
    float clipX=0,clipY=0,requested=0;
    bool operator==(const DebugFontRefreshKey&) const = default;
};
inline bool debugReuseFontMeasurements(const DebugFontRefreshKey& previous,const DebugFontRefreshKey& current,
                                       const float* widths,const float* baseline,std::size_t count,float factor) {
    if(previous!=current || !current.owner.ui || !current.owner.engine || !current.owner.client ||
        !current.owner.world || !current.canvas || !current.generation || !current.scene || !current.metrics ||
        count!=current.columns || count>16 || (count && (!widths || !baseline)) ||
        !std::isfinite(current.requested) || current.requested<1 || current.requested>2 ||
        !std::isfinite(factor) || factor<1 || factor>current.requested)return false;
    for(std::size_t i=0;i<count;++i) {
        if(!std::isfinite(widths[i]) || !std::isfinite(baseline[i]) || widths[i]<0 || baseline[i]<0 ||
            widths[i]>16384 || baseline[i]>16384 ||
            debugRestoreColumnWidth(widths[i],baseline[i],factor)>baseline[i]+.5f)return false;
    }
    return true;
}
enum class DebugFontFramePlan { Continue, ReuseRefresh, MeasureOriginal };
inline DebugFontFramePlan debugFontFramePlan(const DebugFontRefreshKey& previous,const DebugFontRefreshKey& current,
                                            bool sameStorage,const float* widths,const float* baseline,std::size_t count,float factor) {
    if(previous==current && current.metrics && sameStorage)return DebugFontFramePlan::Continue;
    return debugReuseFontMeasurements(previous,current,widths,baseline,count,factor) ?
        DebugFontFramePlan::ReuseRefresh : DebugFontFramePlan::MeasureOriginal;
}
inline std::uint32_t debugVisibleTop(std::uint32_t count,std::uint32_t selected,
                                    std::uint32_t top,std::uint32_t visible) {
    if(!count || selected>=count || !visible || visible>512)return top;
    const auto last=count>visible?count-visible:0;
    top=std::min(top,last);
    if(selected<top)top=selected;
    else if(selected-top>=visible)top=selected-visible+1;
    return std::min(top,last);
}
// The original arrows skip informational rows. A page key can reveal their
// hidden prefix/tail when an otherwise handled arrow leaves selection still.
inline bool debugPageFallback(Action action,std::uint32_t count,std::uint32_t before,
                              std::uint32_t after,std::uint32_t top,std::uint32_t visible) {
    if(!count || count>65536 || before>=count || after!=before || top>=count ||
        !visible || visible>512)return false;
    if(action==Action::UiDown)return after<count-1 && visible<count-top;
    return action==Action::UiUp && after>0 && top>0;
}

// Original UPlayerManagerInteraction::InputKey delegate parameter block. The
// event is a byte (not the native EInputEvent argument's stack word); UObject's
// ProcessDelegate writes the scripted bool return into the final word.
struct DebugInputParameters {
    std::uint32_t controllerId=0,keyIndex=0,keyNumber=0;
    std::uint8_t event=0,padding[3]{};
    std::uint32_t amountBits=0x3F800000,gamepad=0,handled=0;
};
static_assert(sizeof(DebugInputParameters)==28 && offsetof(DebugInputParameters,event)==12 &&
    offsetof(DebugInputParameters,amountBits)==16 && offsetof(DebugInputParameters,gamepad)==20 &&
    offsetof(DebugInputParameters,handled)==24,"Original Debug input delegate ABI");

// LoadGearBuilder binds the ready row's tertiary command to BuildWeapon/BuildItem,
// while InputKey maps Y to that slot. The visible hint remains the original text.
struct DebugBuildFooterRange { std::size_t prefix=0,label=0; };
constexpr Action kDebugBuildFooterAction=Action::NextWeapon;
inline bool debugBuildFooterRange(std::string_view menu,std::string_view command,
                                  std::string_view hint,DebugBuildFooterRange& range) {
    constexpr std::string_view prefix="(X) Back up a step    ";
    constexpr std::string_view weapon="(Y) Build this Gun";
    constexpr std::string_view item="(Y) Build this ";
    if(menu!="Gear Builder" || hint.size()>256 || !hint.starts_with(prefix))return false;
    const auto label=hint.substr(prefix.size());
    if((command=="BuildWeapon" && label!=weapon) ||
       (command=="BuildItem" && (!label.starts_with(item) || label.size()==item.size())) ||
       (command!="BuildWeapon" && command!="BuildItem"))return false;
    for(char value:hint)if(static_cast<unsigned char>(value)<32 || static_cast<unsigned char>(value)>126)return false;
    range={prefix.size(),label.size()};return true;
}
struct DebugBuildFooterRect { float left=0,top=0,right=0,bottom=0; };
// Prefix/label dimensions come from the original static Canvas::ClippedStrLen.
// Reject wrapped/clipped paint; an enlarged hit area must not steal any row.
inline bool debugBuildFooterRect(float x,float y,float paintedWidth,float paintedHeight,
                                 float measuredWidth,float measuredHeight,float prefixWidth,float labelWidth,
                                 float rowBottom,std::uint32_t width,std::uint32_t height,DebugBuildFooterRect& out) {
    for(float value:{x,y,paintedWidth,paintedHeight,measuredWidth,measuredHeight,prefixWidth,labelWidth,rowBottom})
        if(!std::isfinite(value))return false;
    if(!width || !height || x<0 || y<0 || x>=width || y>=height || paintedWidth<=0 || paintedHeight<=0 ||
       prefixWidth<=0 || labelWidth<=0 || std::abs(measuredWidth-paintedWidth)>2 ||
       std::abs(measuredHeight-paintedHeight)>1 || prefixWidth+labelWidth>measuredWidth+2)return false;
    const float pad=std::max(0.f,(.04f*std::min(width,height)-paintedHeight)/2);
    out={x+prefixWidth,std::max({0.f,y-pad,rowBottom+.008f*height+1}),
         std::min(float(width),x+prefixWidth+labelWidth),std::min(float(height),y+paintedHeight+pad)};
    return out.left<out.right && out.top<out.bottom;
}

struct DebugTouchCommand {
    enum Kind { None, Tap, Scroll, PreviousTab, NextTab, Select } kind = None;
    float x=0,y=0;
    int steps=0;
};
// Small pointer-only core, independent of UObject/Canvas lifetimes. A second
// finger or competing command cancels the intent until every finger is up.
class DebugMenuGesture {
public:
    explicit DebugMenuGesture(bool selectOnDown = false) : selectOnDown_(selectOnDown) {}
    void clear() { ids_.fill(-1); primary_=-1; cancelled_=false; axis_=0; }
    void cancel() { if(captured())cancelled_=true; }
    bool captured() const { return primary_>=0; }
    DebugTouchCommand pointer(const PointerEvent& event,std::uint32_t width,std::uint32_t height,float rowPitch) {
        int slot=find(event.pointerId);
        const bool numeric=std::isfinite(event.surfaceX)&&std::isfinite(event.surfaceY)&&
            event.surfaceX>=0&&event.surfaceX<=1&&event.surfaceY>=0&&event.surfaceY<=1;
        if (!numeric || !width || !height || event.pointerId<0 ||
            static_cast<unsigned>(event.phase)>static_cast<unsigned>(PointerPhase::Cancel)) cancelled_=true;
        if (event.phase==PointerPhase::Down) {
            if (slot>=0) {cancelled_=true;return {};}
            slot=find(-1); if (slot<0) {cancelled_=true;return {};}
            const bool empty=primary_<0;
            ids_[slot]=event.pointerId;
            if (empty) {primary_=event.pointerId;startX_=event.surfaceX;startY_=event.surfaceY;anchorY_=startY_;axis_=0;}
            else cancelled_=true;
            if (selectOnDown_ && !cancelled_ && empty) return {DebugTouchCommand::Select,startX_,startY_,0};
            return {};
        }
        if (slot<0) return {};
        DebugTouchCommand result;
        if (!cancelled_ && event.pointerId==primary_ && numeric) {
            const float dx=(event.surfaceX-startX_)*width,dy=(event.surfaceY-startY_)*height;
            const float slop=.018f*std::min(width,height);
            if (!axis_ && std::hypot(dx,dy)>slop) axis_=std::abs(dx)>std::abs(dy)?1:2;
            if (axis_==2 && (event.phase==PointerPhase::Move || event.phase==PointerPhase::Up)) {
                const float pitch=std::max(std::isfinite(rowPitch)?rowPitch*1.5f:0.f,.025f*height);
                const float distance=(anchorY_-event.surfaceY)*height;
                const int steps=std::clamp(static_cast<int>(distance/pitch),-8,8);
                if (steps) {anchorY_-=steps*pitch/height;result.kind=DebugTouchCommand::Scroll;result.steps=steps;}
            } else if (event.phase==PointerPhase::Up) {
                if (!axis_) {result.kind=DebugTouchCommand::Tap;result.x=startX_;result.y=startY_;}
                else if (axis_==1 && std::abs(dx)>.04f*std::min(width,height))
                    result.kind=dx<0?DebugTouchCommand::NextTab:DebugTouchCommand::PreviousTab;
            }
        }
        if (event.phase==PointerPhase::Cancel) {cancelled_=true;result={};}
        if (event.phase==PointerPhase::Up || event.phase==PointerPhase::Cancel) {
            ids_[slot]=-1;
            if (std::all_of(ids_.begin(),ids_.end(),[](int id){return id<0;})) clear();
        }
        return result;
    }
private:
    int find(int id) const {for(std::size_t i=0;i<ids_.size();++i) if(ids_[i]==id) return static_cast<int>(i);return -1;}
    std::array<std::int32_t,kMaxPointers> ids_=[]{std::array<std::int32_t,kMaxPointers> a{};a.fill(-1);return a;}();
    std::int32_t primary_=-1;
    bool cancelled_=false;
    const bool selectOnDown_;
    int axis_=0;
    float startX_=0,startY_=0,anchorY_=0;
};
// Original native Canvas debug menu. All commands use its original script InputKey delegate;
// this sink owns only touch capture and never changes the cheat actions.
class Ue3DebugMenuSink final : public EngineSink {
public:
    using LifetimeGeneration = std::uint64_t (*)();
    Ue3DebugMenuSink(DynarmicHost&, LifetimeGeneration);
    ~Ue3DebugMenuSink() override;
    std::uint32_t prepare(std::uint32_t completeViewport, std::uint32_t controller,
                          std::uint64_t sceneIdentity);
    bool verified() const;
    void beginTouchFrame(const Frame&) override;
    void action(const ActionEdge&) override;
    void pointer(const PointerEvent&) override;
    void movement(float, float) override {}
    void relativeLook(float, float) override {}
    void endTouchFrame(const Frame&) override;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace bl2::touch
