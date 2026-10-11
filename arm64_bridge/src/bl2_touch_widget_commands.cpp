#include "bl2_touch_widget_commands.h"
#include "bl2_touch_settings.h"
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cmath>
#include <cstring>
#include <memory>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace bl2::touch {
namespace {
struct Value {
    GfxSession& s; GfxHandle h;
    ~Value() { s.release(h); }
    operator GfxHandle() const { return h; }
    explicit operator bool() const { return bool(h); }
    GfxHandle take() { const auto value=h;h={};return value; }
};
bool sameIdentity(GfxSession& s, GfxHandle v, const GfxIdentity& expected) {
    GfxIdentity actual; return s.identity(v,actual) && actual == expected;
}
bool number(GfxSession& s, GfxHandle object, const char* name, double& result) {
    const Value v{s,s.member(object,name)}; return v && s.number(v,result);
}
bool boolean(GfxSession& s, GfxHandle object, const char* name, bool& result) {
    const Value v{s,s.member(object,name)}; return v && s.boolean(v,result,std::strcmp(name,"disabled")==0);
}
bool sameTarget(const WidgetTarget& a, const WidgetTarget& b) {
    return a.id && a.id == b.id && a.index == b.index && a.eventId == b.eventId && a.kind == b.kind &&
        a.enabled && b.enabled && a.visible && b.visible && a.caption == b.caption &&
        a.rendererType == b.rendererType && a.renderer == b.renderer && a.data == b.data &&
        a.inner == b.inner && a.slider == b.slider &&
        // Selection art, thumb and autosized text animate independently of the
        // live row. Range/axis and pinned AS identities validate the setting.
        a.minimum == b.minimum && a.maximum == b.maximum &&
        a.choices==b.choices && a.choiceLabels==b.choiceLabels &&
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
struct SettingsCallbacks {
    GfxSession& s; GfxHandle list, listeners, slider, spinner;
    bool active = false;
    SettingsCallbacks(GfxSession& session, GfxHandle object) : s(session), list(object) {
        listeners = s.member(list,"_listeners"); slider = s.member(list,"onSliderValueChange");
        spinner = s.member(list,"onSpinnerValueChange");
        const Value prototype{s,s.member(list,"__proto__")}, empty{s,s.point(0,0)};
        const Value quietSlider{s,s.member(prototype,"onSliderValueChange")};
        const Value quietSpinner{s,s.member(prototype,"onSpinnerValueChange")};
        active = listeners && slider && spinner && prototype && empty && quietSlider && quietSpinner &&
            s.setMember(list,"_listeners",empty) && s.setMember(list,"onSliderValueChange",quietSlider) &&
            s.setMember(list,"onSpinnerValueChange",quietSpinner);
    }
    ~SettingsCallbacks() {
        if (s.good()) {
            if (listeners) s.setMember(list,"_listeners",listeners);
            if (slider) s.setMember(list,"onSliderValueChange",slider);
            if (spinner) s.setMember(list,"onSpinnerValueChange",spinner);
        }
        s.release(spinner); s.release(slider); s.release(listeners);
    }
};
}
InventoryWidgetTarget CaptureBl2InventoryWidget(GfxSession& s,InventoryDragFamily family,
    std::uint32_t width,std::uint32_t height,float x,float y,
    const std::function<std::uint64_t(std::int32_t,bool)>& nativeInventory,
    const std::array<GfxIdentity,3>& statusPanels,InventoryWidgetObservation* observation) {
    if(observation) *observation={};
    const auto note=[&](const char* reason,unsigned step,const char* panel="none",std::int32_t index=-1) {
        if(observation && step>=observation->step) {
            observation->reason=reason;observation->step=step;observation->panel=panel;observation->index=index;
        }
    };
    const auto gate=[&](bool okay,const char* reason,unsigned step,const char* panel="none",std::int32_t index=-1) {
        if(!okay) note(reason,step,panel,index);return okay;
    };
    const bool status=family==InventoryDragFamily::StatusInventory;
    if(!s.good() || family<InventoryDragFamily::Bank || family>InventoryDragFamily::StatusInventory ||
        (status && (!nativeInventory || !statusPanels[0].data || !statusPanels[1].data || !statusPanels[2].data)) ||
        !width || !height || !std::isfinite(x) || !std::isfinite(y) || x<0 || x>1 || y<0 || y>1) {
        note("capture-input",1);return {};
    }
    GfxRect frame{};GfxViewport viewport{};WidgetPoint a{},b{};
    if(!s.frameRect(frame) || !s.viewport(viewport) ||
        !NormalizeWidgetPoint(frame,viewport,width,height,{frame.left,frame.top},a) ||
        !NormalizeWidgetPoint(frame,viewport,width,height,{frame.right,frame.bottom},b) ||
        b.x<=a.x || b.y<=a.y || x<a.x || x>b.x || y<a.y || y>b.y) {
        note("frame-or-viewport",2);return {};
    }
    const double stageX=frame.left+(x-a.x)*(frame.right-frame.left)/(b.x-a.x),
                 stageY=frame.top+(y-a.y)*(frame.bottom-frame.top)/(b.y-a.y);
    if(observation) {observation->stageX=stageX;observation->stageY=stageY;}
    const Value point{s,s.point(stageX,stageY)};
    const Value root{s,s.variable("_root")},top{s,s.member(root,status ? "inventory" :
        (family==InventoryDragFamily::Trading ? "trading" : "main"))};
    GfxIdentity rootId{},topId{};
    const auto shown=[&](GfxHandle object) {
        bool visible=false;double alpha=0;
        return s.type(object)==10 && boolean(s,object,"_visible",visible) && visible &&
            number(s,object,"_alpha",alpha) && alpha>0 && alpha<=100;
    };
    const Value topParent{s,s.member(top,"_parent")};
    if(observation) {observation->types[0]=s.type(root);observation->types[1]=s.type(top);}
    if(!point || !shown(root) || !shown(top) || !s.identity(root,rootId) ||
        !s.identity(top,topId) || !sameIdentity(s,topParent,rootId) || (status && topId!=statusPanels[0])) {
        note("root-top-identity-visibility",3);return {};
    }
    if(observation) observation->topId=topId;
    const auto managerFor=[&](InventoryWidgetTarget& out) {
        const Value managerClass{s,s.variable("_global.gfx.managers.DragManager")},
            instance{s,s.member(managerClass,"_instance")};
        if(observation) {observation->managerTypes[0]=s.type(managerClass);observation->managerTypes[1]=s.type(instance);}
        if(!s.object(managerClass) || !s.object(instance)) return false;
        Value manager{s,s.invoke(managerClass,"__get__instance")};
        const Value cancel{s,s.member(manager,"cancelDrag")};GfxIdentity instanceId{};
        if(observation) {observation->managerTypes[2]=s.type(manager);observation->managerTypes[3]=s.type(cancel);}
        if(!s.identity(instance,instanceId) || !s.identity(manager,out.managerId) ||
            out.managerId!=instanceId || !s.object(cancel) || !s.good()) return false;
        out.manager=manager.take();return true;
    };
    for(const auto* path:status ? std::array{"storagePanel","equippedPanel"} :
        (family==InventoryDragFamily::Trading ? std::array{"backpack","offers"} :
                                               std::array{"storagePanel","playerPanel"})) {
        Value panel{s,s.member(top,path)};
        const Value parent{s,s.member(panel,"_parent")};
        GfxIdentity panelId{};bool allowed=false;
        const bool equipped=status && std::strcmp(path,"equippedPanel")==0;
        if(observation) observation->types[2]=s.type(panel);
        if(!shown(panel) || !s.identity(panel,panelId) || !sameIdentity(s,parent,topId) ||
            (status && panelId!=statusPanels[equipped?2:1])) {note("panel-identity-visibility",4,path);continue;}
        if(observation) observation->panelId=panelId;
        if(equipped || (family==InventoryDragFamily::Trading && std::strcmp(path,"offers")==0)) {
            if(!nativeInventory) continue;
            const Value selected{s,equipped?s.undefined():s.member(panel,"currentSelectedCell")};
            for(std::int32_t i=0;i<(equipped?8:4);++i) {
                const auto name="cell"+std::to_string(i+1);Value cell{s,s.member(panel,name.c_str())};
                const Value cellParent{s,s.member(cell,"_parent")},cellName{s,s.member(cell,"_name")},
                    px{s,s.member(point,"x")},py{s,s.member(point,"y")},
                    hit{s,s.invoke(cell,"hitTest",{px,py})},selectable{s,s.invoke(cell,"CanBeSelected")};
                InventoryWidgetTarget out;bool inside=false,canSelect=false;
                if(observation) {++observation->tested;observation->types[3]=s.type(cell);
                    observation->types[4]=s.type(hit);observation->types[5]=s.type(selectable);}
                if(!gate(shown(cell),"cell-visibility",5,path,i) ||
                    !gate(s.identity(cell,out.source.cell),"cell-identity",5,path,i) ||
                    !gate(sameIdentity(s,cellParent,panelId),"cell-parent",5,path,i) ||
                    !gate(equipped || sameIdentity(s,selected,out.source.cell),"cell-selection",5,path,i) ||
                    !gate(s.string(cellName,out.label,16) && out.label==name,"cell-name",5,path,i) ||
                    !gate(s.boolean(hit,inside),"cell-hit-type",6,path,i) ||
                    !gate(inside,"cell-outside",6,path,i)) continue;
                if(observation) {observation->hitMask|=1u<<i;observation->cellId=out.source.cell;}
                if(!gate(s.boolean(selectable,canSelect) && canSelect,"cell-not-selectable",7,path,i)) continue;
                if(observation) observation->selectableMask|=1u<<i;
                const auto item=nativeInventory(i,true);if(!item || !s.good()) {note("native-item-query",8,path,i);continue;}
                out.source={panelId,out.source.cell,item};out.rendererId=out.source.cell;out.index=i;
                out.nativeItem=item;
                if(!managerFor(out)) {note("manager-instance",9,path,i);continue;}
                note("captured",10,path,i);
                out.panel=panel.take();out.cell=cell.take();return out;
            }
            continue;
        }
        if(!boolean(s,panel,"bCanDrag",allowed) || !allowed) {note("panel-can-drag",4,path);continue;}
        const Value list{s,s.member(panel,"list")},listOwner{s,s.member(list,"owningPanel")};
        // Genuine GetHitCell also checks arrows/sort controls. Only its actual
        // InventoryCell with a bound renderer/data is a transferable source.
        if(!shown(list) || !sameIdentity(s,listOwner,panelId)) {note("list-identity-visibility",4,path);continue;}
        Value cell{s,s.invoke(list,"GetHitCell",{point},true)};
        Value renderer{s,s.member(cell,"_parent")},data{s,s.member(renderer,"data")};
        const Value boundCell{s,s.member(renderer,"cell")},selectable{s,s.invoke(cell,"CanBeSelected")},
            label{s,s.invoke(renderer,"__get__label")};
        InventoryWidgetTarget out;bool canSelect=false;double index=-1;
        if(observation) {++observation->tested;observation->types[3]=s.type(cell);observation->types[5]=s.type(selectable);}
        if(!shown(cell) || !shown(renderer) || !s.identity(cell,out.source.cell) ||
            !s.identity(renderer,out.rendererId) || !sameIdentity(s,boundCell,out.source.cell) ||
            !s.boolean(selectable,canSelect) || !canSelect || s.type(data)!=8 || !s.identity(data,out.dataId) ||
            !number(s,renderer,"index",index) || index<0 || index>4095 || std::floor(index)!=index ||
            !s.string(label,out.label,512) || out.label.empty()) {note("list-cell-renderer-data",5,path);continue;}
        if(observation) {observation->cellId=out.source.cell;observation->selectableMask|=1u;}
        bool invalid=false;
        for(const auto* flag:{"frozen","bSoldOut","bUnloadable"}) {
            const Value value{s,s.member(data,flag)};bool enabled=false;
            // Original setData treats omitted/undefined optional flags as
            // false. A failed session or a present malformed value is not
            // that original default.
            if((value && (!s.boolean(value,enabled,true) || enabled)) || !s.good()) { invalid=true;break; }
        }
        if(invalid) {note("list-item-flags",7,path,static_cast<std::int32_t>(index));continue;}
        if(family==InventoryDragFamily::Trading || status) {
            out.nativeItem=nativeInventory?nativeInventory(static_cast<std::int32_t>(index),false):0;
            if(!out.nativeItem || !s.good()) {note("native-item-query",8,path,static_cast<std::int32_t>(index));continue;}
        }
        if(!managerFor(out)) {note("manager-instance",9,path,static_cast<std::int32_t>(index));continue;}
        auto hash=1469598103934665603ull;
        const auto add=[&](std::uint64_t value) { for(unsigned i=0;i<8;++i) { hash^=value&255;hash*=1099511628211ull;value>>=8; } };
        add(static_cast<std::uint64_t>(index));add(out.dataId.interface);add(out.dataId.data);add(out.dataId.extra);add(out.dataId.type);
        for(const auto c:out.label) { hash^=static_cast<unsigned char>(c);hash*=1099511628211ull; }
        out.source.panel=panelId;out.source.content=hash?hash:1;out.index=static_cast<std::int32_t>(index);
        note("captured",10,path,out.index);
        out.panel=panel.take();out.cell=cell.take();out.renderer=renderer.take();out.data=data.take();
        return out;
    }
    return {};
}
bool SameBl2InventoryWidget(const InventoryWidgetTarget& a,const InventoryWidgetTarget& b) {
    return a && b && a.source==b.source && a.rendererId==b.rendererId && a.dataId==b.dataId &&
        a.managerId==b.managerId && a.index==b.index && a.label==b.label && a.nativeItem==b.nativeItem;
}
bool Bl2InventoryDragStarted(GfxSession& s,const InventoryWidgetTarget& target,GfxIdentity* data,bool* activeOrUnknown,
                            InventoryDragObservation* observation) {
    bool active=false;
    const Value inDrag{s,s.member(target.manager,"_inDrag")},related{s,s.member(target.manager,"relatedObject")},
        origin{s,s.member(target.manager,"pressOrigin")},source{s,s.member(target.manager,"_data")};GfxIdentity actual{},originId{};
    // Original StartDragging passes its panel as pressOrigin and NULL as
    // relatedObject. Requiring the latter to be the panel rejects every real
    // native drag; a foreign origin is still never ours to finish or cancel.
    const bool known=s.boolean(inDrag,active);
    if(activeOrUnknown) *activeOrUnknown=!known || active;
    const bool started=target && sameIdentity(s,target.manager,target.managerId) && known && active &&
        s.identity(origin,originId) && originId==target.source.panel && s.type(related)==1 && s.identity(source,actual);
    if(observation) {
        *observation={};observation->known=known;observation->active=active;observation->started=started;
        observation->inDragType=s.type(inDrag);observation->relatedType=s.type(related);
        observation->manager=target.managerId;observation->origin=originId;observation->payload=actual;
    }
    if(started && data) *data=actual;
    return started;
}
bool CancelBl2InventoryDrag(GfxSession& s,const InventoryWidgetTarget& target) {
    if(!Bl2InventoryDragStarted(s,target)) return false;
    const Value result{s,s.invoke(target.manager,"cancelDrag",{},true)};
    return result && s.good();
}
namespace {
#if defined(__ANDROID__)
std::atomic<unsigned> footerDiagnosticLines{0};
#endif
bool footerDiagnostics() {
#if defined(__ANDROID__)
    if(footerDiagnosticLines.load()>=40) return false;
    char value[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",value);
    return value[0]=='1' && !value[1];
#else
    return false;
#endif
}
void footerLog(const char* format,...) {
#if defined(__ANDROID__)
    if(!footerDiagnostics() || footerDiagnosticLines.fetch_add(1)>=40) return;
    va_list args;va_start(args,format);
    __android_log_vprint(ANDROID_LOG_INFO,"BL2TouchDiagnostics",format,args);va_end(args);
#else
    (void)format;
#endif
}
// Named EditTexts in the shipped MissionInterface/SharedWillowMission,
// LatentReward, StatusMenu, ItemInspect, FastTravelStation, TitleMenusClik,
// Options, LoadCharacter/Matchmaking2, NetworkOptions, PlayerInfo,
// GearboxAccount, ServerBrowser and Marketplace movies. Never walk arbitrary
// display-tree text or explanatory buttonTooltip fields as actionable hints.
constexpr const char* genericFooterPaths[]={
    "_root.tooltips.tooltips", "_root.missions.tooltips", "_root.reward.tooltips",
    "_root.latentRewardOuter.missionReward.tooltips", "_root.itemInspect.tooltips.tooltips",
    "_root.teleport.tooltips.tooltips", "_root.lobby.tooltips", "_root.menu.tooltips",
    "_root.menu.inner.tooltips", "_root.menu.networkOptionsPopup.inner.tooltips",
    "_root.menu.playerInfoPopup.inner.tooltips", "_root.gearboxAccount.tooltips",
    "_root.serverBrowser.tooltips", "_root.marketplace.tooltips1", "_root.marketplace.tooltips2"
};
bool footerVisible(GfxSession& s,GfxHandle clip) {
    bool shown=false;double alpha=0;
    return s.type(clip)==10 && boolean(s,clip,"_visible",shown) && shown &&
        number(s,clip,"_alpha",alpha) && alpha>0 && alpha<=100;
}
bool footerFields(GfxSession& s,NativeFooterFamily family,const char* path,GfxHandle root,MapLegendTarget& out,
                  const char** reason=nullptr) {
    GfxIdentity rootId{};
    if(reason) *reason="root-visible-identity";
    if(!footerVisible(s,root) || !s.identity(root,rootId)) return false;
    if(family==NativeFooterFamily::Generic) {
        out.text=s.variable(path);out.parent=s.member(out.text,"_parent");
        if(reason) *reason="text-parent-visible-identity";
        if(!footerVisible(s,out.text) || !footerVisible(s,out.parent) ||
            !s.identity(out.text,out.textId) || !s.identity(out.parent,out.parentId)) return false;
        auto id=out.parentId;
        GfxHandle ancestor=out.parent;
        std::string parentPath=path;parentPath.resize(parentPath.rfind('.'));
        for(unsigned depth=0;depth<6;++depth) {
            if(parentPath=="_root") {
                if(reason) *reason=id==rootId?"owned":"root-chain-identity";
                if(ancestor.id!=out.parent.id) s.release(ancestor);return id==rootId;
            }
            const Value expected{s,s.variable(parentPath.c_str())};
            if(reason) *reason="parent-path-identity";
            if(!sameIdentity(s,expected,id)) break;
            const auto next=s.member(ancestor,"_parent");
            if(ancestor.id!=out.parent.id) s.release(ancestor);
            ancestor=next;
            parentPath.resize(parentPath.rfind('.'));
            if(reason) *reason="ancestor-visible-identity";
            if(!footerVisible(s,ancestor) || !s.identity(ancestor,id)) break;
        }
        if(ancestor.id!=out.parent.id) s.release(ancestor);
        return false;
    }
    const bool twoPanel=family==NativeFooterFamily::TwoPanel || family==NativeFooterFamily::Vending;
    const Value main{s,twoPanel?s.member(root,"main"):s.undefined()};
    out.parent=family==NativeFooterFamily::Training?s.variable("_root"):
        (twoPanel?s.member(main,"tooltipsWrapper"):s.member(root,"tooltips"));
    out.text=s.member(out.parent,"tooltips");
    const Value parent{s,s.member(out.text,"_parent")},
        owner{s,family==NativeFooterFamily::Training?s.undefined():s.member(out.parent,"_parent")},
        mainOwner{s,twoPanel?s.member(main,"_parent"):s.undefined()};
    GfxIdentity mainId{};
    if(reason) *reason="specialized-owner-visible";
    const bool owned=footerVisible(s,out.parent) && footerVisible(s,out.text) &&
        (twoPanel ? (footerVisible(s,main) && s.identity(main,mainId) && sameIdentity(s,mainOwner,rootId) &&
                     sameIdentity(s,owner,mainId)):
         sameIdentity(s,family==NativeFooterFamily::Training?out.parent:owner,rootId)) &&
        s.identity(out.parent,out.parentId) && sameIdentity(s,parent,out.parentId) && s.identity(out.text,out.textId);
    if(owned && reason) *reason="owned";
    return owned;
}
void footerFieldLog(GfxSession& s,const char* phase,const char* path,const char* reason,GfxHandle root,
                    const MapLegendTarget& target,const MapLegendObservation* observation=nullptr) {
    if(!footerDiagnostics() || !s.object(target.text)) return;
    std::string plain=target.caption,html=target.html,snapshotOwner;
    double x=0,y=0,sx=0,sy=0,rotation=0,savedScale=0;
    double rootAlpha=0,parentAlpha=0,textAlpha=0;
    bool rootVisible=false,parentVisible=false,textVisible=false;
    GfxIdentity rootId{},parentId=target.parentId,textId=target.textId;
    s.identity(root,rootId);s.identity(target.parent,parentId);s.identity(target.text,textId);
    std::uint32_t boundsMethod=0,charactersMethod=0,snapshotType=0;
    const bool queried=s.hasCallBudget(32);
    if(queried) {
        boolean(s,root,"_visible",rootVisible);number(s,root,"_alpha",rootAlpha);
        boolean(s,target.parent,"_visible",parentVisible);number(s,target.parent,"_alpha",parentAlpha);
        boolean(s,target.text,"_visible",textVisible);number(s,target.text,"_alpha",textAlpha);
        if(plain.empty()) {const Value v{s,s.member(target.text,"text")};s.string(v,plain,1024);}
        if(html.empty()) {const Value v{s,s.member(target.text,"htmlText")};s.string(v,html,1024);}
        number(s,target.text,"_x",x);number(s,target.text,"_y",y);number(s,target.text,"_xscale",sx);
        number(s,target.text,"_yscale",sy);number(s,target.text,"_rotation",rotation);
        const Value bounds{s,s.member(target.text,"getBounds")},characters{s,s.member(target.text,"getCharBoundaries")},
            snapshot{s,s.member(target.text,"_bl2TouchFooterOriginal")};
        boundsMethod=s.type(bounds);charactersMethod=s.type(characters);snapshotType=s.type(snapshot);
        if(s.object(snapshot)) {
            const Value owner{s,s.member(snapshot,"owner")};s.string(owner,snapshotOwner,192);number(s,snapshot,"scale",savedScale);
        }
    }
    const auto sample=[](const std::string& text,std::size_t limit) {
        std::string out;for(std::size_t i=0;i<text.size() && i<limit;++i) {
            const auto c=text[i];if(c=='\n') out+="\\n";else if(c=='\r') out+="\\r";
            else if(c=='\t') out+="\\t";else out+=c;
        }return out;
    };
    std::string hex;constexpr char digits[]="0123456789ABCDEF";
    for(std::size_t i=0;i<plain.size() && i<96;++i) {
        const auto c=static_cast<unsigned char>(plain[i]);hex+=digits[c>>4];hex+=digits[c&15];hex+=' ';
    }
    footerLog("footer-native phase=%s path=%s reason=%s session=%s calls=%u extra=%u "
        "types=(%u,%u,%u) ids=(%08X,%08X,%08X) visible=(%u,%.1f;%u,%.1f;%u,%.1f) "
        "methods=(bounds:%u,chars:%u) scaleRequested=%.3f transform=(%.3f,%.3f,%.3f,%.3f,%.3f) "
        "snapshot=(type:%u,scale:%.3f,owner:%s) command=%u utf16=(%u,%u) measured=%u charType=%u "
        "char=(%.3f,%.3f,%.3f,%.3f) hit=%u plainBytes=%zu htmlBytes=%zu plain={%s} html={%s} plainHex={%s}",
        phase,path?path:"specialized",reason,s.reason()?s.reason():"okay",s.calls(),queried?1u:0u,
        s.type(root),s.type(target.parent),s.type(target.text),rootId.data,parentId.data,textId.data,
        rootVisible?1u:0u,rootAlpha,parentVisible?1u:0u,parentAlpha,textVisible?1u:0u,textAlpha,
        boundsMethod,charactersMethod,Bl2InterfaceScale(),x,y,sx,sy,rotation,
        snapshotType,savedScale,snapshotOwner.c_str(),static_cast<unsigned>(target.command),target.first,target.count,
        observation?observation->measured:0u,observation?observation->rectType:0u,
        observation?observation->character.left:0,observation?observation->character.top:0,
        observation?observation->character.right:0,observation?observation->character.bottom:0,
        observation && observation->hit?1u:0u,plain.size(),html.size(),sample(plain,512).c_str(),sample(html,1024).c_str(),hex.c_str());
}
bool footerSpace(char c) {return c==' ' || c=='\t' || c=='\r' || c=='\n';}
std::string footerLower(std::string value) {
    for(auto& c:value) if(c>='A' && c<='Z') c=static_cast<char>(c-'A'+'a');
    return value;
}
NativeFooterCommand footerGlyph(const std::string& glyph) {
    constexpr struct {const char* name;NativeFooterCommand command;} buttons[]={
        {"A",NativeFooterCommand::ButtonA},{"B",NativeFooterCommand::ButtonB},
        {"X",NativeFooterCommand::ButtonX},{"Y",NativeFooterCommand::ButtonY},
        {"Back",NativeFooterCommand::ButtonBack},{"Start",NativeFooterCommand::ButtonStart},
        {"LSB",NativeFooterCommand::ButtonLSB},{"RSB",NativeFooterCommand::ButtonRSB},
        {"LB",NativeFooterCommand::ButtonLB},{"RB",NativeFooterCommand::ButtonRB},
        {"LT",NativeFooterCommand::ButtonLT},{"RT",NativeFooterCommand::ButtonRT}
    };
    for(const auto& button:buttons)
        if(glyph==std::string("android_")+button.name || glyph==std::string("xbox360_")+button.name) return button.command;
    constexpr struct {const char* name;NativeFooterCommand command;} ps3[]={
        {"X",NativeFooterCommand::ButtonA},{"Circle",NativeFooterCommand::ButtonB},
        {"Square",NativeFooterCommand::ButtonX},{"Triangle",NativeFooterCommand::ButtonY},
        {"Select",NativeFooterCommand::ButtonBack},{"Start",NativeFooterCommand::ButtonStart},
        {"LSB",NativeFooterCommand::ButtonLSB},{"RSB",NativeFooterCommand::ButtonRSB},
        {"L1",NativeFooterCommand::ButtonLB},{"R1",NativeFooterCommand::ButtonRB},
        {"L2",NativeFooterCommand::ButtonLT},{"R2",NativeFooterCommand::ButtonRT}
    };
    for(const auto& button:ps3) if(glyph==std::string("ps3_")+button.name) return button.command;
    // DPad and LS/RS are directional/axis symbols, not a particular key or
    // stick click. Keyboard, foreign linkage names and aliases stay unknown.
    return NativeFooterCommand::None;
}
struct FooterMatch {std::size_t start=0,bytes=0;NativeFooterCommand command=NativeFooterCommand::None;std::uint32_t first=0,count=0;};
bool footerUtf16(const std::string& text,std::vector<FooterMatch>& matches,std::vector<std::uint32_t>* anchors=nullptr) {
    std::uint32_t units=0,first=0,last=0;bool painted=false;
    const auto line=[&] {
        if(anchors && painted) {anchors->push_back(first);anchors->push_back(last);}
        painted=false;
    };
    for(std::size_t i=0;i<text.size();) {
        for(auto& match:matches) if(i==match.start) match.first=units;
        const auto lead=static_cast<unsigned char>(text[i]);unsigned n=0;std::uint32_t cp=0;
        if(lead<0x80) {n=1;cp=lead;}
        else if((lead&0xE0)==0xC0) {n=2;cp=lead&31;}
        else if((lead&0xF0)==0xE0) {n=3;cp=lead&15;}
        else if((lead&0xF8)==0xF0) {n=4;cp=lead&7;}
        else return false;
        if(i+n>text.size()) return false;
        for(unsigned k=1;k<n;++k) {
            const auto c=static_cast<unsigned char>(text[i+k]);if((c&0xC0)!=0x80) return false;
            cp=(cp<<6)|(c&63);
        }
        if((n==2 && cp<128) || (n==3 && cp<2048) || (n==4 && cp<65536) ||
            cp>0x10FFFF || (cp>=0xD800 && cp<=0xDFFF)) return false;
        if(cp=='\r' || cp=='\n') line();
        else if(cp!=' ' && cp!='\t') {if(!painted) first=units;last=units+(cp>65535?1u:0u);painted=true;}
        units+=cp>65535?2u:1u;i+=n;
        for(auto& match:matches) if(i==match.start+match.bytes) match.count=units-match.first;
        if(units>512) return false;
    }
    line();
    if(anchors) {
        // The native text represents inline IMG symbols by one space. Include
        // their proven indices as well as each paragraph's painted endpoints.
        for(const auto& match:matches) anchors->push_back(match.first);
        std::sort(anchors->begin(),anchors->end());anchors->erase(std::unique(anchors->begin(),anchors->end()),anchors->end());
        if(anchors->empty() || anchors->size()>24) return false;
    }
    return std::all_of(matches.begin(),matches.end(),[](const FooterMatch& m){return m.count!=0;});
}
bool footerCharacterRect(GfxSession& s,GfxHandle text,std::uint32_t i,GfxRect& out,
                         MapLegendObservation* observation=nullptr) {
    const Value index{s,s.number(i)},rect{s,s.invoke(text,"getCharBoundaries",{index})};
    if(observation) observation->rectType=s.type(rect);
    double x=0,y=0,w=0,h=0;
    if(!rect || !s.object(rect) || !number(s,rect,"x",x) || !number(s,rect,"y",y) ||
        !number(s,rect,"width",w) || !number(s,rect,"height",h) || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0 || w>1000 || h>1000) return false;
    out={x,y,x+w,y+h};
    if(observation) {++observation->measured;observation->character=out;}
    return true;
}
bool footerHtml(const std::string& html,const std::string& caption,std::vector<FooterMatch>& matches) {
    if(html.empty() || html.size()>1024) return false;
    // GetText's inline-image representation varies by GFx build. Reconstruct
    // both supported one-character forms and require exact live text equality
    // before using any UTF-16 index. Omitted images or unknown HTML never
    // become guessed icon rectangles.
    for(const auto* marker:{"\xEF\xBF\xBC"," "}) {
        std::string plain;std::vector<FooterMatch> images;bool valid=true,paragraphEnd=false;
        for(std::size_t at=0;at<html.size() && valid;) {
            if(html[at]=='<') {
                const auto end=html.find('>',at+1);if(end==std::string::npos) {valid=false;break;}
                const auto tag=html.substr(at+1,end-at-1),lower=footerLower(tag);
                auto begin=lower.find_first_not_of(" \t\r\n/");
                if(begin==std::string::npos) {valid=false;break;}
                const auto name=lower.substr(begin,lower.find_first_of(" \t\r\n/",begin)-begin);
                if(name=="img") {
                    auto src=lower.find("src",begin+name.size());
                    if(src==std::string::npos || !src || !footerSpace(lower[src-1])) {valid=false;break;}
                    src+=3;while(src<tag.size() && footerSpace(tag[src])) ++src;
                    if(src>=tag.size() || tag[src++]!='=') {valid=false;break;}
                    while(src<tag.size() && footerSpace(tag[src])) ++src;
                    if(src>=tag.size() || (tag[src]!='\'' && tag[src]!='"')) {valid=false;break;}
                    const auto quote=tag[src++];const auto last=tag.find(quote,src);
                    if(last==std::string::npos || lower.find("src",last+1)!=std::string::npos || images.size()>=12) {valid=false;break;}
                    images.push_back({plain.size(),0,footerGlyph(tag.substr(src,last-src))});plain+=marker;paragraphEnd=false;
                } else if(name=="br") {plain+='\n';paragraphEnd=false;}
                else if(name=="p" && lower.find_first_not_of(" \t\r\n")==lower.find('/')) {
                    // Native AVM1 GetText inserts CR between HTML paragraphs,
                    // with no trailing CR after the final authored paragraph.
                    plain+='\r';paragraphEnd=true;
                }
                else if(name!="font" && name!="p" && name!="textformat" && name!="b" && name!="i" && name!="u") valid=false;
                at=end+1;
            } else if(html[at]=='&') {
                const auto end=html.find(';',at+1);if(end==std::string::npos || end-at>8) {valid=false;break;}
                const auto entity=html.substr(at,end-at+1);
                if(entity=="&amp;") plain+='&';else if(entity=="&lt;") plain+='<';else if(entity=="&gt;") plain+='>';
                else if(entity=="&quot;") plain+='"';else if(entity=="&apos;") plain+='\'';
                else if(entity=="&nbsp;") plain+="\xC2\xA0";else valid=false;
                at=end+1;paragraphEnd=false;
            } else {plain+=html[at++];paragraphEnd=false;}
        }
        if(paragraphEnd && !plain.empty()) plain.pop_back();
        if(!valid || plain!=caption || images.empty()) continue;
        for(std::size_t i=0;i<images.size();++i) {
            auto match=images[i];auto end=i+1<images.size()?images[i+1].start:plain.size();
            while(end>match.start+std::strlen(marker) && footerSpace(plain[end-1])) --end;
            match.bytes=end-match.start;
            if(match.command!=NativeFooterCommand::None) matches.push_back(match);
        }
        return !matches.empty();
    }
    return false;
}
MapLegendTarget captureNativeFooter(GfxSession& s,NativeFooterFamily family,const char* path,
    std::uint32_t width,std::uint32_t height,float x,float y,MapLegendObservation* observation) {
    MapLegendTarget out;
    MapLegendObservation diagnostic;
    if(!observation && footerDiagnostics()) observation=&diagnostic;
    if(observation) *observation={};
    GfxHandle traceRoot{};const char* ownerReason="not-read";
    const auto reject=[&](const char* reason) {
        if(observation) observation->reason=reason;
        const auto detail=std::string(reason)+"/"+ownerReason;
        footerFieldLog(s,"capture",path,detail.c_str(),traceRoot,out,observation);
        s.release(out.text);s.release(out.parent);return MapLegendTarget{};
    };
    if (!s.good() || !width || !height || !std::isfinite(x) || !std::isfinite(y) || x<0 || x>1 || y<0 || y>1 ||
        (family<NativeFooterFamily::Map || family>NativeFooterFamily::Generic)) return reject("input-shape");
    const bool twoPanel=family==NativeFooterFamily::TwoPanel || family==NativeFooterFamily::Vending;
    const Value root{s,s.variable("_root")};
    traceRoot=root.h;
    const bool owned=footerFields(s,family,path,root,out,&ownerReason);
    if(observation) { observation->rootType=s.type(root);observation->parentType=s.type(out.parent);observation->textType=s.type(out.text); }
    if(!owned) return reject("footer-owner-visible");
    const Value caption{s,s.member(out.text,"text")};
    const bool captionString=s.string(caption,out.caption,1024);
    if(observation) observation->caption=out.caption;
    if (!captionString || out.caption.empty()) return reject("footer-caption");
    struct Word { const char* text;NativeFooterCommand command; };
    // MapBar's GFx_Accept hint: the original A/Enter release runs
    // PlaceCustomObjective at the crosshair, exactly like on a controller.
    constexpr Word legendWords[]={{"Legend",NativeFooterCommand::Legend},{"LEGEND",NativeFooterCommand::Legend},
        {"legend",NativeFooterCommand::Legend},{"Легенда",NativeFooterCommand::Legend},
        {"ЛЕГЕНДА",NativeFooterCommand::Legend},{"легенда",NativeFooterCommand::Legend},
        {"Waypoint",NativeFooterCommand::Accept},{"WAYPOINT",NativeFooterCommand::Accept},
        {"waypoint",NativeFooterCommand::Accept},{"Маршрутная точка",NativeFooterCommand::Accept},
        {"МАРШРУТНАЯ ТОЧКА",NativeFooterCommand::Accept},{"маршрутная точка",NativeFooterCommand::Accept}};
    constexpr Word inventoryWords[]={{"Inspect",NativeFooterCommand::Inspect},{"INSPECT",NativeFooterCommand::Inspect},
        {"Осмотреть",NativeFooterCommand::Inspect},{"ОСМОТРЕТЬ",NativeFooterCommand::Inspect},
        {"Trash/Favourite",NativeFooterCommand::Mark},{"TRASH/FAVOURITE",NativeFooterCommand::Mark},
        {"Trash/Favorite",NativeFooterCommand::Mark},{"TRASH/FAVORITE",NativeFooterCommand::Mark},
        {"Хлам/любимое",NativeFooterCommand::Mark},{"ХЛАМ/ЛЮБИМОЕ",NativeFooterCommand::Mark},
        {"Sort",NativeFooterCommand::Sort},{"SORT",NativeFooterCommand::Sort},
        {"Отсортировать",NativeFooterCommand::Sort},{"ОТСОРТИРОВАТЬ",NativeFooterCommand::Sort},
        {"Select/Compare",NativeFooterCommand::Select},{"SELECT/COMPARE",NativeFooterCommand::Select},
        {"Выбрать/сравнить",NativeFooterCommand::Select},{"ВЫБРАТЬ/СРАВНИТЬ",NativeFooterCommand::Select},
        {"Compare",NativeFooterCommand::Select},{"COMPARE",NativeFooterCommand::Select},
        {"Сравнить",NativeFooterCommand::Select},{"СРАВНИТЬ",NativeFooterCommand::Select},
        {"Drop",NativeFooterCommand::Drop},{"DROP",NativeFooterCommand::Drop},
        {"Бросить",NativeFooterCommand::Drop},{"БРОСИТЬ",NativeFooterCommand::Drop},
        {"Close",NativeFooterCommand::Close},{"CLOSE",NativeFooterCommand::Close},
        {"Закрыть",NativeFooterCommand::Close},{"ЗАКРЫТЬ",NativeFooterCommand::Close}};
    constexpr Word transferWords[]={{"Select",NativeFooterCommand::Accept},{"SELECT",NativeFooterCommand::Accept},
        {"Take",NativeFooterCommand::Accept},{"TAKE",NativeFooterCommand::Accept},
        {"Swap",NativeFooterCommand::Accept},{"SWAP",NativeFooterCommand::Accept},
        {"Выбор",NativeFooterCommand::Accept},{"ВЫБОР",NativeFooterCommand::Accept},
        {"Поменять",NativeFooterCommand::Accept},{"ПОМЕНЯТЬ",NativeFooterCommand::Accept},
        {"Cancel",NativeFooterCommand::Close},{"CANCEL",NativeFooterCommand::Close},
        {"Отмена",NativeFooterCommand::Close},{"ОТМЕНА",NativeFooterCommand::Close}};
    constexpr Word vendingWords[]={{"Sell Trash",NativeFooterCommand::SellTrash},{"SELL TRASH",NativeFooterCommand::SellTrash},
        {"Продать весь хлам",NativeFooterCommand::SellTrash},{"ПРОДАТЬ ВЕСЬ ХЛАМ",NativeFooterCommand::SellTrash},
        {"Purchase",NativeFooterCommand::Accept},{"PURCHASE",NativeFooterCommand::Accept},
        {"Приобрести",NativeFooterCommand::Accept},{"ПРИОБРЕСТИ",NativeFooterCommand::Accept},
        {"Sell",NativeFooterCommand::Accept},{"SELL",NativeFooterCommand::Accept},
        {"Продать",NativeFooterCommand::Accept},{"ПРОДАТЬ",NativeFooterCommand::Accept},
        {"Buyback",NativeFooterCommand::Accept},{"BUYBACK",NativeFooterCommand::Accept},
        {"Выкупить",NativeFooterCommand::Accept},{"ВЫКУПИТЬ",NativeFooterCommand::Accept}};
    const auto* words=family==NativeFooterFamily::Map ? legendWords:inventoryWords;
    const auto wordCount=family==NativeFooterFamily::Training || family==NativeFooterFamily::Generic ? 0u:
        (family==NativeFooterFamily::Map ? std::size(legendWords):
         std::size(inventoryWords)+(twoPanel?std::size(transferWords):0)+
         (family==NativeFooterFamily::Vending?std::size(vendingWords):0));
    std::vector<FooterMatch> matches;
    if(family==NativeFooterFamily::Training) matches.push_back({0,out.caption.size(),NativeFooterCommand::TrainingAccept});
    if(family==NativeFooterFamily::Generic) {
        const Value html{s,s.member(out.text,"htmlText")};
        if(!s.string(html,out.html,1024) || !footerHtml(out.html,out.caption,matches)) return reject("footer-html-glyph-indexing");
    }
    for(std::size_t n=0;n<wordCount;++n) {
        const auto& word=twoPanel && n>=std::size(inventoryWords) ?
            (n>=std::size(inventoryWords)+std::size(transferWords)?
                vendingWords[n-std::size(inventoryWords)-std::size(transferWords)]:transferWords[n-std::size(inventoryWords)]):words[n];
        const auto length=std::strlen(word.text),at=out.caption.find(word.text);
        if (at==std::string::npos) continue;
        const auto space=[](char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r'; };
        if((at && !space(out.caption[at-1])) || (at+length<out.caption.size() && !space(out.caption[at+length]))) continue;
        // The longer genuine Sell Trash command is not the ordinary Sell
        // prefix. Its original LS handler differs from A/Enter transfer.
        const auto longer=[&](std::size_t pos) {
            return std::any_of(matches.begin(),matches.end(),[&](const FooterMatch& m) {return m.start==pos && m.bytes>length;});
        };
        if(longer(at)) continue;
        auto duplicate=out.caption.find(word.text,at+length);
        while(duplicate!=std::string::npos && longer(duplicate)) duplicate=out.caption.find(word.text,duplicate+length);
        if (duplicate!=std::string::npos ||
            std::any_of(matches.begin(),matches.end(),[&](const FooterMatch& m) {return m.command==word.command;}))
            return reject("legend-word-ambiguous");
        matches.push_back({at,length,word.command});
    }
    if(matches.empty() || matches.size()>(family==NativeFooterFamily::Generic?8u:6u)) return reject("legend-word-absent");
    if(!footerUtf16(out.caption,matches)) return reject("caption-utf8-units");
    for(const auto& match:matches) if(!match.count ||
        (family!=NativeFooterFamily::Training && match.count>32)) return reject("legend-units");
    double tx=0,ty=0,sx=0,sy=0,rotation=0;
    if (!number(s,out.text,"_x",tx) || !number(s,out.text,"_y",ty) || !number(s,out.text,"_xscale",sx) ||
        !number(s,out.text,"_yscale",sy) || !number(s,out.text,"_rotation",rotation) || rotation!=0 ||
        !std::isfinite(tx) || !std::isfinite(ty) || !std::isfinite(sx) || !std::isfinite(sy) || sx<=0 || sy<=0 || sx>10000 || sy>10000) return reject("footer-transform");
    out.scaleX=sx;out.scaleY=sy;
    if(observation) { observation->x=tx;observation->y=ty;observation->scaleX=sx;observation->scaleY=sy;observation->rotation=rotation; }
    const auto cell=[&](std::uint32_t i,GfxRect& r) {
        // Original exact-ink helper omits LineBuffer's horizontal alignment.
        // The public cell-bounds helper includes line+0x0C before advances;
        // centred native HTML footers therefore keep their real word origin.
        GfxRect local{};if(!footerCharacterRect(s,out.text,i,local,observation)) return false;
        r={tx+local.left*sx/100,ty+local.top*sy/100,tx+local.right*sx/100,ty+local.bottom*sy/100};return true;
    };
    const auto project=[&](const GfxRect& local,GfxRect& stage,GfxRect& frame,WidgetPoint& a,WidgetPoint& b) {
        GfxViewport viewport{};
        return s.projectedRect(out.parent,local,stage) && s.frameRect(frame) && s.viewport(viewport) &&
            NormalizeWidgetPoint(frame,viewport,width,height,{stage.left,stage.top},a) &&
            NormalizeWidgetPoint(frame,viewport,width,height,{stage.right,stage.bottom},b);
    };
    const double slop=.0125*std::min(width,height)*Bl2InterfaceScale();
    std::size_t selected=0;
    if(matches.size()>1) {
        // Query each command's actual first cell, then only the word beginning
        // closest to this touch. Its full native bounds still must contain UP.
        // This keeps the 512-call/32-value budget with six long Unicode hints.
        bool found=false;double nearest=-1;
        for(std::size_t i=0;i<matches.size();++i) {
            GfxRect local{},stage{},frame{};WidgetPoint a{},b{};
            if(!cell(matches[i].first,local) || !project(local,stage,frame,a,b)) return reject("legend-character-rectangle");
            if(x<a.x-slop/width || y<a.y-slop/height || y>b.y+slop/height || (found && a.x<=nearest)) continue;
            selected=i;nearest=a.x;found=true;
        }
        if(!found) return reject("legend-outside");
    }
    const auto& chosen=matches[selected];out.command=chosen.command;out.first=chosen.first;out.count=chosen.count;
    if(observation) {observation->first=out.first;observation->count=out.count;}
    GfxRect local{};bool measured=false;
    if(family==NativeFooterFamily::Training) {
        // Exact blue Training timeline: EditText33 has no variable binding,
        // AutoSize is off and its own box is [-2,-2,529,35.35]. It represents
        // one ready prompt; its original handler owns Close/OnClose semantics.
        const Value name{s,s.member(out.text,"_name")};std::string spelling;
        double activeFrame=0,actualWidth=0,actualHeight=0;
        if(!s.string(name,spelling,32) || spelling!="tooltips" ||
            !number(s,root,"_currentframe",activeFrame) || activeFrame!=2 ||
            !number(s,out.text,"_width",actualWidth) || !number(s,out.text,"_height",actualHeight) ||
            std::abs(actualWidth-531*sx/100)>.1 || std::abs(actualHeight-37.35*sy/100)>.1)
            return reject("training-field-shape");
        local={tx-2*sx/100,ty-2*sy/100,tx+529*sx/100,ty+35.35*sy/100};
    }
    for (std::uint32_t i=out.first;family!=NativeFooterFamily::Training && i<out.first+out.count;++i) {
        GfxRect r{};if(!cell(i,r)) return reject("legend-character-rectangle");
        if (!measured) { local=r;measured=true; }
        else { local.left=std::min(local.left,r.left);local.top=std::min(local.top,r.top);
               local.right=std::max(local.right,r.right);local.bottom=std::max(local.bottom,r.bottom); }
    }
    if(observation) observation->local=local;
    GfxRect stage{},frame{};WidgetPoint a{},b{};
    const bool projected=project(local,stage,frame,a,b);
    if(observation) { observation->projected=projected;observation->stage=stage; }
    if (!projected) return reject("legend-projection");
    if(observation) observation->frame=frame;
    // Expand only the native command's measured word by a small touch margin.
    // The neighbouring Waypoint/Zoom/Close hints remain outside this target.
    out.bounds={a.x-slop/width,a.y-slop/height,b.x+slop/width,b.y+slop/height};
    GfxViewport viewport{};WidgetPoint frameA{},frameB{};
    if(!s.viewport(viewport) || !NormalizeWidgetPoint(frame,viewport,width,height,{frame.left,frame.top},frameA) ||
        !NormalizeWidgetPoint(frame,viewport,width,height,{frame.right,frame.bottom},frameB)) return reject("footer-frame-clipping");
    out.bounds={std::max(out.bounds.left,frameA.x),std::max(out.bounds.top,frameA.y),
                std::min(out.bounds.right,frameB.x),std::min(out.bounds.bottom,frameB.y)};
    const bool hit=WidgetContains(out.bounds,{x,y});if(observation) { observation->bounds=out.bounds;observation->hit=hit; }
    const Value recheck{s,s.member(out.text,"text")};std::string actual;
    if(!s.good() || !hit || !s.string(recheck,actual,1024) || actual!=out.caption)
        return reject(s.good()?"legend-outside":"session-failed");
    if(family==NativeFooterFamily::Generic) {
        const Value html{s,s.member(out.text,"htmlText")};
        if(!s.string(html,actual,1024) || actual!=out.html) return reject("footer-html-changed");
    }
    if(observation) observation->reason="hit";
    footerFieldLog(s,"capture",path,"hit",root,out,observation);
    return out;
}
}
MapLegendTarget CaptureBl2NativeFooter(GfxSession& s,NativeFooterFamily family,
    std::uint32_t width,std::uint32_t height,float x,float y,MapLegendObservation* observation) {
    if(family!=NativeFooterFamily::Generic) return captureNativeFooter(s,family,nullptr,width,height,x,y,observation);
    MapLegendObservation diagnostic;
    if(!observation && footerDiagnostics()) observation=&diagnostic;
    unsigned observed=0;
    unsigned probed=0,fields=0;
    for(const auto* path:genericFooterPaths) {
        if(!s.good() || !s.hasCallBudget(40)) break;
        ++probed;
        MapLegendObservation current;
        auto target=captureNativeFooter(s,family,path,width,height,x,y,observation?&current:nullptr);
        const unsigned progress=current.hit?5u:(current.measured?4u:(!current.caption.empty()?3u:(current.textType==10?2u:1u)));
        if(current.textType>=8 && current.textType<=11) ++fields;
        if(observation && progress>=observed) {*observation=std::move(current);observed=progress;}
        if(target) return target;
    }
    footerLog("footer-native phase=capture-summary family=%u probed=%u fields=%u reason=%s rootType=%u session=%s calls=%u point=(%.6f,%.6f) surface=(%u,%u)",
        static_cast<unsigned>(family),probed,fields,observation?observation->reason:"unobserved",
        observation?observation->rootType:0u,s.reason()?s.reason():"okay",s.calls(),x,y,width,height);
    return {};
}
MapLegendTarget CaptureBl2MapLegend(GfxSession& s,std::uint32_t width,std::uint32_t height,float x,float y,
                                  MapLegendObservation* observation) {
    return CaptureBl2NativeFooter(s,NativeFooterFamily::Map,width,height,x,y,observation);
}
bool SameBl2MapLegend(const MapLegendTarget& a,const MapLegendTarget& b) {
    return a && b && a.command==b.command && a.parentId==b.parentId && a.textId==b.textId &&
        a.caption==b.caption && a.html==b.html && a.first==b.first && a.count==b.count &&
        a.scaleX==b.scaleX && a.scaleY==b.scaleY;
}
namespace {
bool scaleFooterField(GfxSession& s,const MapLegendTarget& field,NativeFooterFamily family,double scale,const char** reason) {
    *reason="scale-budget";
    if(!s.hasCallBudget(80)) return false;
    const Value caption{s,s.member(field.text,"text")};std::string text;
    double x=0,y=0,sx=0,sy=0,rotation=0;
    *reason="scale-caption-transform";
    if(!s.string(caption,text,1024) || text.empty() || !number(s,field.text,"_x",x) ||
        !number(s,field.text,"_y",y) || !number(s,field.text,"_xscale",sx) ||
        !number(s,field.text,"_yscale",sy) || !number(s,field.text,"_rotation",rotation) || rotation!=0 ||
        !std::isfinite(x) || !std::isfinite(y) || std::abs(x)>10000 || std::abs(y)>10000 ||
        !std::isfinite(sx) || !std::isfinite(sy) || sx<=0 || sy<=0 || sx>1000 || sy>1000) return false;
    const auto identity=[](const GfxIdentity& id) {
        return std::to_string(id.interface)+":"+std::to_string(id.data)+":"+
            std::to_string(id.extra)+":"+std::to_string(id.type);
    };
    const auto owner=identity(field.parentId)+"/"+identity(field.textId);
    const auto set=[&](GfxHandle object,const char* name,double value) {
        const Value v{s,s.number(value)};return v && s.setMember(object,name,v);
    };
    Value original{s,s.member(field.text,"_bl2TouchFooterOriginal")};
    double bx=x,by=y,bsx=sx,bsy=sy,cx=0,bottom=0,previous=1,ax=x,ay=y,asx=sx,asy=sy;
    if(original && s.type(original)!=0) {
        *reason="scale-snapshot-schema-owner";
        const Value signature{s,s.member(original,"owner")};std::string actual;
        if(s.type(original)!=8 || !s.string(signature,actual,192) || actual!=owner ||
            !number(s,original,"x",bx) || !number(s,original,"y",by) ||
            !number(s,original,"sx",bsx) || !number(s,original,"sy",bsy) ||
            !number(s,original,"cx",cx) || !number(s,original,"bottom",bottom) ||
            !number(s,original,"appliedX",ax) || !number(s,original,"appliedY",ay) ||
            !number(s,original,"appliedSx",asx) || !number(s,original,"appliedSy",asy) ||
            !number(s,original,"scale",previous) || previous<1 || previous>2 ||
            !std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bsx) || !std::isfinite(bsy) ||
            !std::isfinite(cx) || !std::isfinite(bottom) || bsx<=0 || bsy<=0 || bsx>1000 || bsy>1000 ||
            !std::isfinite(ax) || !std::isfinite(ay) || std::abs(ax)>10000 || std::abs(ay)>10000 ||
            !std::isfinite(asx) || !std::isfinite(asy) || asx<=0 || asy<=0 || asx>1000 || asy>1000) return false;
        // Do not overwrite a native animation/layout write or a copied snapshot
        // belonging to another parent/field. Each successful transform is ours.
        *reason="scale-foreign-transform";
        const double expectedX=ax,expectedY=ay,expectedSx=asx,expectedSy=asy;
        if(std::abs(x-expectedX)>.05 || std::abs(y-expectedY)>.05 ||
            std::abs(sx-expectedSx)>.05 || std::abs(sy-expectedSy)>.05) {
            footerLog("footer-native phase=scale-guard baseline=(%.9f,%.9f,%.9f,%.9f) anchor=(%.9f,%.9f) previous=%.9f "
                "expected=(%.9f,%.9f,%.9f,%.9f) actual=(%.9f,%.9f,%.9f,%.9f) delta=(%.9f,%.9f,%.9f,%.9f)",
                bx,by,bsx,bsy,cx,bottom,previous,expectedX,expectedY,expectedSx,expectedSy,
                x,y,sx,sy,x-expectedX,y-expectedY,sx-expectedSx,sy-expectedSy);
            return false;
        }
        if(previous==scale) {*reason="scale-unchanged";return true;}
    } else {
        if(scale==1) {*reason="scale-original100";return true;}
        std::vector<FooterMatch> matches;std::vector<std::uint32_t> anchors;
        const Value htmlValue{s,s.member(field.text,"htmlText")};std::string html;
        const bool parsed=s.string(htmlValue,html,1024) && footerHtml(html,text,matches);
        *reason="scale-html-glyph-indexing";
        if(family==NativeFooterFamily::Generic && !parsed) return false;
        *reason="scale-caption-utf8-units";
        if(!footerUtf16(text,matches,&anchors)) return false;
        *reason="scale-character-budget";
        if(!s.hasCallBudget(static_cast<std::uint32_t>(anchors.size())*6+96)) return false;
        // AVM1 TextField has getCharBoundaries but no MovieClip getBounds or
        // localToGlobal. The native cells retain alignment, paragraph origins
        // and inline-image dimensions; project only through the owned parent.
        GfxRect local{};bool measured=false;
        *reason="scale-character-rectangle";
        for(const auto i:anchors) {
            GfxRect cell{};if(!footerCharacterRect(s,field.text,i,cell)) return false;
            if(!measured) {local=cell;measured=true;}
            else {local.left=std::min(local.left,cell.left);local.top=std::min(local.top,cell.top);
                  local.right=std::max(local.right,cell.right);local.bottom=std::max(local.bottom,cell.bottom);}
        }
        if(!measured || local.right-local.left>4096 || local.bottom-local.top>1024) return false;
        GfxRect stage{},frame{};
        *reason="scale-parent-projection";
        if(!s.projectedRect(field.parent,{x+local.left*sx/100,y+local.top*sy/100,
            x+local.right*sx/100,y+local.bottom*sy/100},stage) || !s.frameRect(frame) ||
            stage.right<=frame.left || stage.left>=frame.right || stage.bottom<=frame.top || stage.top>=frame.bottom) return false;
        cx=(local.left+local.right)/2;bottom=local.bottom;
        s.release(original.h);original.h=s.point(bx,by);const Value signature{s,s.stringValue(owner)};
        *reason="scale-snapshot-write";
        if(!original || !signature || !s.setMember(original,"owner",signature) ||
            !set(original,"sx",bsx) || !set(original,"sy",bsy) || !set(original,"cx",cx) ||
            !set(original,"bottom",bottom) || !set(original,"scale",1) ||
            !set(original,"appliedX",ax) || !set(original,"appliedY",ay) ||
            !set(original,"appliedSx",asx) || !set(original,"appliedSy",asy) ||
            !s.setMember(field.text,"_bl2TouchFooterOriginal",original)) return false;
    }
    // Scaling this one EditText also scales its inline IMG symbols. Anchor its
    // actual painted cells at centre/bottom, so it grows upwards rather than
    // through the bottom viewport edge. No other menu child is transformed.
    // Native TextField scale setters also move its position. Apply scale first,
    // then the final anchor, and retain the actual quantized transform so the
    // next input boundary never mistakes our own native result for a UI write.
    *reason="scale-transform-budget";
    if(!s.hasCallBudget(24)) return false;
    *reason="scale-transform-write";
    if(!set(field.text,"_xscale",bsx*scale) || !set(field.text,"_yscale",bsy*scale) ||
        !set(field.text,"_x",bx+(1-scale)*cx*bsx/100) ||
        !set(field.text,"_y",by+(1-scale)*bottom*bsy/100)) return false;
    *reason="scale-transform-readback";
    if(!number(s,field.text,"_x",ax) || !number(s,field.text,"_y",ay) ||
        !number(s,field.text,"_xscale",asx) || !number(s,field.text,"_yscale",asy) ||
        !std::isfinite(ax) || !std::isfinite(ay) || std::abs(ax)>10000 || std::abs(ay)>10000 ||
        !std::isfinite(asx) || !std::isfinite(asy) || asx<=0 || asy<=0 || asx>1000 || asy>1000) return false;
    *reason="scale-applied-snapshot-write";
    const bool applied=set(original,"appliedX",ax) && set(original,"appliedY",ay) &&
        set(original,"appliedSx",asx) && set(original,"appliedSy",asy) && set(original,"scale",scale) && s.good();
    if(applied) *reason="scale-applied";
    return applied;
}
}
bool ScaleBl2NativeFooter(GfxSession& s,NativeFooterFamily family) {
    const double scale=Bl2InterfaceScale();
    if(!s.good() || !std::isfinite(scale) || scale<1 || scale>2 ||
        family<NativeFooterFamily::Map || family>NativeFooterFamily::Generic) return false;
    const Value root{s,s.variable("_root")};bool ready=false;
    unsigned probed=0,fields=0;
    const auto apply=[&](const char* path) {
        ++probed;
        const char* reason="not-read";MapLegendTarget field;const bool valid=footerFields(s,family,path,root,field,&reason);
        const Value parent{s,field.parent},text{s,field.text};
        if(s.object(field.text)) ++fields;
        const bool applied=valid && scaleFooterField(s,field,family,scale,&reason);
        footerFieldLog(s,"scale",path,reason,root,field);
        return applied;
    };
    if(family!=NativeFooterFamily::Generic) return apply(nullptr);
    for(const auto* path:genericFooterPaths) {
        if(!s.good() || !s.hasCallBudget(100)) break;
        if(apply(path)) ready=true;
    }
    footerLog("footer-native phase=scale-summary family=%u probed=%u fields=%u ready=%u rootType=%u requested=%.3f session=%s calls=%u",
        static_cast<unsigned>(family),probed,fields,ready?1u:0u,s.type(root),scale,s.reason()?s.reason():"okay",s.calls());
    return ready && s.good();
}
MapCursorTarget CaptureBl2MapCursor(GfxSession& s,GfxHandle map,GfxHandle mask,
    std::uint32_t width,std::uint32_t height,float x,float y) {
    MapCursorTarget out;
    const auto reject=[&](const char* reason) { out.reason=reason;return out; };
    if (!s.good() || !width || !height || !std::isfinite(x) || !std::isfinite(y) || x<0 || x>1 || y<0 || y>1 ||
        s.type(map)!=10 || s.type(mask)!=10) return reject("map-cursor-shape");
    const Value parent{s,s.member(mask,"_parent")};GfxIdentity expected;
    if (!s.identity(map,expected) || !sameIdentity(s,parent,expected)) return reject("map-mask-parent");
    for (const auto clip : {map,mask}) {
        bool visible=false;double alpha=0;
        if (!boolean(s,clip,"_visible",visible) || !visible || !number(s,clip,"_alpha",alpha) || alpha<=0 || alpha>100)
            return reject("map-cursor-hidden");
    }
    if (!number(s,map,"_xmouse",out.local.x) || !number(s,map,"_ymouse",out.local.y) ||
        !std::isfinite(out.local.x) || !std::isfinite(out.local.y) || std::abs(out.local.x)>100000 || std::abs(out.local.y)>100000)
        return reject("map-native-mouse");
    const Value bounds{s,s.invoke(mask,"getBounds",{map})};GfxRect local;
    if (!bounds || !s.object(bounds) || !number(s,bounds,"xMin",local.left) || !number(s,bounds,"yMin",local.top) ||
        !number(s,bounds,"xMax",local.right) || !number(s,bounds,"yMax",local.bottom) ||
        !std::isfinite(local.left) || !std::isfinite(local.top) || !std::isfinite(local.right) || !std::isfinite(local.bottom) ||
        local.right<=local.left || local.bottom<=local.top) return reject("map-mask-shape");
    const Value point{s,s.point(out.local.x,out.local.y)},result{s,s.invoke(map,"localToGlobal",{point})};
    WidgetPoint stage;GfxRect frame;GfxViewport viewport;
    if (!point || !result || !number(s,point,"x",stage.x) || !number(s,point,"y",stage.y) ||
        !s.frameRect(frame) || !s.viewport(viewport) || !NormalizeWidgetPoint(frame,viewport,width,height,stage,out.surface))
        return reject("map-cursor-roundtrip");
    if (std::abs(out.surface.x-x)>2.0/width || std::abs(out.surface.y-y)>2.0/height) {
        out.mousePending=true;return reject("map-mouse-not-advanced");
    }
    if (!WidgetContains(local,out.local)) return reject("map-cursor-outside-mask");
    out.valid=s.good();out.reason=out.valid ? "map-mouse-projected" : s.reason();return out;
}
namespace {
// UI_StatusMenu EditText526 under MissionSummaryPanel (SetDescriptionScrollValue,
// root SetScrollArrows), EditText118/117 of the challenge description panel,
// and UI_Mission EditText55 (QuestAccept SetDescriptionScrollValue/SetScrollArrows).
// The summary description also sits at _z=-600, so its whole panel owns swipes.
constexpr TextScrollTarget kMissionLogText{"_root.missions.missionSummary.description",true,"SetScrollArrows",
    {-86.15,-2,190.6,256.45}};
constexpr TextScrollTarget kChallengeText{"_root.challenges.challengesPanel.descriptionPanel.challengeDescription",
    false,nullptr,{-86.15,-2,172.95,63.25}};
constexpr TextScrollTarget kChallengeLevels{"_root.challenges.challengesPanel.descriptionPanel.challengeLevels",
    false,nullptr,{-86.15,-2,171.3,116.6}};
constexpr TextScrollTarget kQuestText{"_root.missions.description",false,"SetScrollArrows",{-86.15,-2,278.8,280.7}};
}
std::vector<const TextScrollTarget*> Bl2TextScrollTargets(std::string_view movieClass,std::int32_t screen) {
    if (movieClass=="StatusMenuExGFxMovie" && screen==1) return {&kMissionLogText};
    if (movieClass=="StatusMenuExGFxMovie" && screen==5) return {&kChallengeText,&kChallengeLevels};
    if (movieClass=="QuestAcceptGFxMovie") return {&kQuestText};
    return {};
}
bool Bl2TextScrollLocalRect(const TextScrollTarget& target,const std::array<double,7>& v,GfxRect& local) {
    local={};
    for (const auto value:v) if (!std::isfinite(value)) return false;
    const double sx=v[4]/100,sy=v[5]/100;
    if (std::abs(v[0])>=1000000 || std::abs(v[1])>=1000000 || sx<=0 || sx>10 || sy<=0 || sy>10 || v[6]!=0) return false;
    // The fixed authored box. A field with its own _z (the summary's -600)
    // reports both sides reduced by one depth factor (device: .965, swaying
    // by <.001), so only about the same factor on both axes is accepted,
    // never another field's shape.
    const double width=(target.box.right-target.box.left)*sx,height=(target.box.bottom-target.box.top)*sy;
    const double fx=v[2]/width,fy=v[3]/height;
    if (!std::isfinite(fx) || !std::isfinite(fy) || fx<.9 || fx>1.1 || std::abs(fx-fy)>.01) return false;
    local={v[0],v[1],v[0]+width,v[1]+height};
    return true;
}
std::int32_t Bl2TextScrollLine(std::int32_t start,std::int32_t maximum,double drag,double line) {
    if (maximum<1 || start<1 || start>maximum || !std::isfinite(drag) || !std::isfinite(line) || line<=0) return start;
    const double moved=std::round(-drag/line);
    return static_cast<std::int32_t>(std::clamp(start+moved,1.0,static_cast<double>(maximum)));
}
TextScrollCapture CaptureBl2TextScroll(GfxSession& s,std::string_view movieClass,std::int32_t screen,
    std::uint32_t width,std::uint32_t height,float x,float y) {
    TextScrollCapture rejected;
    const auto reject=[&](const char* reason) { rejected.reason=reason;return rejected; };
    if (!s.good() || !width || !height || !std::isfinite(x) || !std::isfinite(y) || x<0 || x>1 || y<0 || y>1)
        return reject("text-input");
    const auto targets=Bl2TextScrollTargets(movieClass,screen);
    GfxRect frame;GfxViewport viewport;
    if (targets.empty()) return reject("text-no-target");
    if (!s.frameRect(frame) || !s.viewport(viewport)) return reject("text-frame");
    const auto shown=[&](GfxHandle clip) {
        bool visible=false;double alpha=0;
        return boolean(s,clip,"_visible",visible) && visible && number(s,clip,"_alpha",alpha) &&
            std::isfinite(alpha) && alpha>0 && alpha<=100;
    };
    const auto normalize=[&](const GfxRect& stage,GfxRect& out) {
        WidgetPoint a,b;
        if (!NormalizeWidgetPoint(frame,viewport,width,height,{stage.left,stage.top},a) ||
            !NormalizeWidgetPoint(frame,viewport,width,height,{stage.right,stage.bottom},b) ||
            b.x<=a.x || b.y<=a.y) return false;
        out={a.x,a.y,b.x,b.y};return true;
    };
    const Value root{s,s.variable("_root")};GfxIdentity rootId;
    if (!s.identity(root,rootId)) return reject("text-root");
    for (const auto* target:targets) {
        const Value field{s,s.variable(target->field)};
        const Value parent{s,s.member(field,"_parent")};
        if (!field || (s.type(field)!=8 && s.type(field)!=10) || s.type(parent)!=10) { rejected.reason="text-field-type";continue; }
        if (!shown(field)) { rejected.reason="text-field-hidden";continue; }
        // Every ancestor below _root is shown: inactive challenge/badass
        // panels and other screens keep their fields but hide a parent.
        bool chain=true,reached=false;GfxHandle clip=s.member(field,"_parent");
        for (unsigned depth=0;chain && depth<10;++depth) {
            GfxIdentity id;
            if (!s.identity(clip,id)) {chain=false;break;}
            if (id==rootId) {reached=true;break;}
            chain=shown(clip);
            const auto next=s.member(clip,"_parent");s.release(clip);clip=next;
        }
        s.release(clip);
        if (!chain || !reached) { rejected.reason="text-ancestor-hidden";continue; }
        std::array<double,7> v{};
        constexpr const char* names[]{"_x","_y","_width","_height","_xscale","_yscale","_rotation"};
        bool okay=true;
        for (std::size_t i=0;okay && i<v.size();++i) okay=number(s,field,names[i],v[i]);
        rejected.metrics=v;
        GfxRect local,stage,box,hit;
        if (!okay || !Bl2TextScrollLocalRect(*target,v,local)) { rejected.reason="text-field-shape";continue; }
        if (!s.projectedRect(parent,local,stage) || !normalize(stage,box)) { rejected.reason="text-field-projection";continue; }
        hit=box;
        if (target->parentOwnsSwipe && (!s.projectedBounds(parent,stage) || !normalize(stage,hit))) {
            rejected.reason="text-panel-projection";continue;
        }
        rejected.bounds=hit;
        if (!WidgetContains(hit,{x,y})) { rejected.reason="text-outside";continue; }
        double scroll=0,maximum=0,bottom=0;
        if (!number(s,field,"scroll",scroll) || !number(s,field,"maxscroll",maximum) ||
            !number(s,field,"bottomScroll",bottom) || scroll!=std::floor(scroll) || maximum!=std::floor(maximum) ||
            bottom!=std::floor(bottom) || maximum<2 || maximum>100000 || scroll<1 || scroll>maximum || bottom<scroll) {
            const auto whole=[](double value) {
                return std::isfinite(value) ? static_cast<std::int32_t>(std::clamp(value,-1.0,100000.0)) : -1;
            };
            rejected.scroll=whole(scroll);rejected.maximum=whole(maximum);
            rejected.reason="text-not-overflowing";continue;
        }
        const double lines=bottom-scroll+1,pixels=(box.bottom-box.top)*height/lines;
        GfxIdentity id;
        if (!s.identity(field,id) || !std::isfinite(pixels) || pixels<4 || pixels>height || !s.good()) {
            rejected.linePixels=pixels;rejected.reason="text-line-height";continue;
        }
        TextScrollCapture out;
        out.target=target;out.field=id;out.bounds=hit;out.linePixels=pixels;out.metrics=v;out.reason="text-captured";
        out.scroll=static_cast<std::int32_t>(scroll);out.maximum=static_cast<std::int32_t>(maximum);
        return out;
    }
    return rejected;
}
bool ScrollBl2Text(GfxSession& s,const TextScrollCapture& capture,std::int32_t line) {
    if (!capture || !s.good()) return false;
    const Value field{s,s.variable(capture.target->field)};
    double maximum=0,current=0;
    if (!sameIdentity(s,field,capture.field) || !number(s,field,"maxscroll",maximum) || !number(s,field,"scroll",current) ||
        maximum!=std::floor(maximum) || maximum<1 || maximum>100000) return false;
    const double next=std::clamp(static_cast<double>(line),1.0,maximum);
    if (next==current) return true;
    const Value value{s,s.number(next)};
    if (!value || !s.setMember(field,"scroll",value)) return false;
    if (capture.target->arrows) {
        const Value root{s,s.variable("_root")},redrawn{s,s.invoke(root,capture.target->arrows,{},true)};
        (void)redrawn;
    }
    return s.good();
}
AgeGateWidgetTarget CaptureBl2AgeGateWidget(GfxSession& s,GfxHandle content,
    std::uint32_t width,std::uint32_t height,float x,float y) {
    AgeGateWidgetTarget out;
    const auto reject=[&] {
        for(auto field:out.fields) s.release(field);
        s.release(out.gate);return AgeGateWidgetTarget{};
    };
    const auto shown=[&](GfxHandle clip) {
        bool visible=false;double alpha=0;
        return s.type(clip)==10 && boolean(s,clip,"_visible",visible) && visible &&
            number(s,clip,"_alpha",alpha) && std::isfinite(alpha) && alpha>0 && alpha<=100;
    };
    if(!s.good() || !width || !height || !std::isfinite(x) || !std::isfinite(y) ||
        x<0 || x>1 || y<0 || y>1 || !shown(content)) return reject();
    const Value root{s,s.member(content,"_root")},parent{s,s.member(content,"_parent")},
        original{s,s.member(root,"gearboxAccount")},contentName{s,s.member(content,"_name")};
    GfxIdentity rootId{};std::string name;double frame=0;
    if(!shown(root) || !s.identity(root,rootId) || !s.identity(content,out.contentId) ||
        !sameIdentity(s,parent,rootId) || !sameIdentity(s,original,out.contentId) ||
        !s.string(contentName,name) || name!="gearboxAccount" ||
        !number(s,content,"_currentframe",frame) || frame!=4) return reject();
    Value gate{s,s.member(content,"ageGate")};
    const Value gateParent{s,s.member(gate,"_parent")},gateName{s,s.member(gate,"_name")};
    if(!shown(gate) || !s.identity(gate,out.gateId) || !sameIdentity(s,gateParent,out.contentId) ||
        !s.string(gateName,name) || name!="ageGate") return reject();
    out.content=content;out.gate=gate.take();
    constexpr std::array<const char*,3> names{"monthInput","dayInput","yearInput"};
    constexpr std::array<double,3> limits{2,2,4};
    GfxRect stage;GfxViewport viewport;
    if(!s.frameRect(stage) || !s.viewport(viewport)) return reject();
    for(unsigned i=0;i<names.size();++i) {
        Value field{s,s.member(out.gate,names[i])};
        const Value owner{s,s.member(field,"_parent")},label{s,s.member(field,"_name")},
            limit{s,s.invoke(field,"__get__maxChars")},text{s,s.invoke(field,"__get__text")};
        bool disabled=true;double maximum=0;
        if(!shown(field) || !s.identity(field,out.fieldIds[i]) || !sameIdentity(s,owner,out.gateId) ||
            !s.string(label,name) || name!=names[i] || !boolean(s,field,"disabled",disabled) || disabled ||
            !s.number(limit,maximum) || maximum!=limits[i] || !s.string(text,out.text[i],64)) return reject();
        GfxRect projected;WidgetPoint a{},b{};
        if(!s.projectedBounds(field,projected) ||
            !NormalizeWidgetPoint(stage,viewport,width,height,{projected.left,projected.top},a) ||
            !NormalizeWidgetPoint(stage,viewport,width,height,{projected.right,projected.bottom},b)) return reject();
        const GfxRect bounds{a.x,a.y,b.x,b.y};
        if(WidgetContains(bounds,{x,y})) {
            if(out.index>=0) return reject(); // Overlapping live fields are ambiguous.
            out.index=static_cast<std::int32_t>(i);out.bounds=bounds;
        }
        out.fields[i]=field.take();
    }
    if(!out || !s.good()) return reject();
    return out;
}
bool SameBl2AgeGateWidget(const AgeGateWidgetTarget& a,const AgeGateWidgetTarget& b) {
    return a && b && a.index==b.index && a.contentId==b.contentId && a.gateId==b.gateId &&
        a.fieldIds==b.fieldIds && a.text==b.text && a.bounds.left==b.bounds.left &&
        a.bounds.top==b.bounds.top && a.bounds.right==b.bounds.right && a.bounds.bottom==b.bounds.bottom;
}
bool Bl2AgeGateExclusivelyFocused(GfxSession& s,const AgeGateWidgetTarget& target) {
    if(!s.good() || !target || !sameIdentity(s,target.content,target.contentId) ||
        !sameIdentity(s,target.gate,target.gateId)) return false;
    for(unsigned i=0;i<target.fields.size();++i) {
        const Value value{s,s.invoke(target.fields[i],"__get__focused")};double focus=0;
        if(!sameIdentity(s,target.fields[i],target.fieldIds[i]) || !s.number(value,focus) ||
            !std::isfinite(focus) || focus<0 || focus>1 || (focus>0)!=(i==static_cast<unsigned>(target.index))) return false;
    }
    return s.good();
}
bool FocusBl2AgeGateWidget(GfxSession& s,const AgeGateWidgetTarget& target) {
    if(!s.good() || !target || !sameIdentity(s,target.content,target.contentId) ||
        !sameIdentity(s,target.gate,target.gateId) || !sameIdentity(s,target.fields[target.index],target.fieldIds[target.index])) return false;
    const Value one{s,s.number(1)},result{s,s.invoke(target.fields[target.index],"__set__focused",{one},true)};
    const Value focused{s,s.invoke(target.fields[target.index],"__get__focused")};double actual=0;
    // Selection's original focus-out may settle at the next movie advance.
    // All three fields must be exclusive on UP, immediately before native A.
    return result && s.number(focused,actual) && actual==1 && s.good();
}
CharacterWidgetTarget CaptureBl2CharacterWidget(GfxSession& s,GfxHandle content,
    std::int32_t screen,std::int32_t player,std::uint32_t width,std::uint32_t height,float x,float y,
    CharacterWidgetObservation* observation) {
    CharacterWidgetTarget out;
    if (observation) { *observation={};observation->contentType=s.type(content); }
    const auto reject=[&](const char* reason) { if(observation) observation->reason=reason;return out; };
    if (!s.good() || s.type(content) != 10 || player < 0 || player > 1 || screen < 1 || screen > 3 ||
        !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return reject("input-shape");
    const auto shown = [&](GfxHandle clip) {
        bool visible = false; double alpha = 0;
        return s.type(clip) == 10 && boolean(s,clip,"_visible",visible) && visible &&
            number(s,clip,"_alpha",alpha) && std::isfinite(alpha) && alpha > 0 && alpha <= 100;
    };
    const Value root{s,s.member(content,"_root")}, parent{s,s.member(content,"_parent")};
    GfxIdentity rootId,contentId;
    if (!shown(content) || !shown(root) || !s.identity(root,rootId) ||
        !sameIdentity(s,parent,rootId) || !s.identity(content,contentId)) return reject("content-root");
    GfxRect frame; GfxViewport viewport;
    if (!s.frameRect(frame) || !s.viewport(viewport)) return reject("viewport");
    const auto panelName = std::string("p") + std::to_string(player + 1) +
        (screen == 2 ? "CustomizationMenu" : "CustomizationList");
    Value panel{s,screen == 1 ? GfxHandle{} : s.member(content,panelName.c_str())};
    Value container{s,screen == 1 ? s.member(content,"characterNavigationGrid") :
        (screen == 2 ? s.member(content,panelName.c_str()) : s.member(panel,"scrollingList"))};
    if(observation) { observation->panelType=s.type(panel);observation->containerType=s.type(container); }
    GfxIdentity containerId;
    if (!shown(container) || !s.identity(container,containerId)) return reject("container");
    const Value containerParent{s,s.member(container,"_parent")};
    if (screen == 3) {
        GfxIdentity panelId; const Value panelParent{s,s.member(panel,"_parent")};
        if (!shown(panel) || !s.identity(panel,panelId) || !sameIdentity(s,panelParent,contentId) ||
            !sameIdentity(s,containerParent,panelId)) return reject("panel-parent");
    } else if (!sameIdentity(s,containerParent,contentId)) return reject("container-parent");
    // Character's CoreList owns _dataProvider. Its public getter returns that
    // same array; unlike the Options WillowList it has no `data` alias.
    Value provider{s,screen == 3 ? s.invoke(container,"__get__dataProvider") :
        s.member(container,screen == 1 ? "cells" : "menuEntries")};
    GfxIdentity providerId;
    double count = 0;
    if(observation) observation->providerType=s.type(provider);
    if (s.type(provider) != 9 || !s.identity(provider,providerId) || !number(s,provider,"length",count))
        return reject("provider-array");
    if(observation) observation->count=count;
    if (!std::isfinite(count) || count != std::floor(count) || count < 1 || count > (screen == 1 ? 32 : screen == 2 ? 4 : 128))
        return reject("provider-count");
    Value renderers{s,screen == 3 ? s.member(container,"renderers") : GfxHandle{}};
    double rendered = count,scroll = 0;
    if(observation) { observation->renderersType=s.type(renderers);observation->rendered=rendered; }
    if (screen == 3) {
        const Value committed{s,s.member(container,"_dataProvider")};
        bool disabled = true;
        const bool same=sameIdentity(s,committed,providerId);
        if(observation) observation->committed=same;
        if(!same) return reject("provider-commit");
        const bool known=boolean(s,container,"disabled",disabled);
        if(observation) { observation->disabledKnown=known;observation->disabled=disabled; }
        if(!known || disabled) return reject("container-disabled");
        if(s.type(renderers)!=9 || !number(s,renderers,"length",rendered)) return reject("renderer-array");
        if(observation) observation->rendered=rendered;
        if(rendered!=std::floor(rendered) || rendered<1 || rendered>8) return reject("renderer-count");
        // WillowStagePlacedScrollingList replaces the public scrollPosition
        // property with an empty getter. Its inherited original method still
        // reads _scrollPosition, the offset used by getRendererAt/populateData.
        const Value position{s,s.invoke(container,"__get__scrollPosition")};
        if(!position || !s.number(position,scroll)) return reject("scroll-value");
        if(observation) observation->scroll=scroll;
        if(scroll!=std::floor(scroll) || scroll<0 || scroll>std::max(0.0,count-rendered)) return reject("scroll-range");
    }
    for (std::uint32_t slot = 0; slot < rendered && slot + scroll < count && s.good(); ++slot) {
        Value widget{s,s.element(screen == 3 ? renderers.h : provider.h,slot)};
        const Value owner{s,s.member(widget,screen == 3 ? "owner" : "_parent")};
        CharacterWidgetObservation::Row* row=nullptr;
        if(observation && observation->rowCount<observation->rows.size()) {
            row=&observation->rows[observation->rowCount++];row->slot=slot;row->type=s.type(widget);
        }
        const auto skip=[&](const char* reason) { if(row) row->reason=reason; };
        double index = -1; bool disabled = true;
        const bool visible=shown(widget);if(row) row->shown=visible;
        if(!visible) { skip("widget-hidden");continue; }
        const bool owned=sameIdentity(s,owner,containerId);if(row) row->owner=owned;
        if(!owned) { skip("widget-owner");continue; }
        const bool known=boolean(s,widget,"disabled",disabled);
        if(row) { row->disabledKnown=known;row->disabled=disabled; }
        if(!known || disabled) { skip("widget-disabled");continue; }
        const bool indexed=number(s,widget,screen == 1 ? "characterIndex" : "index",index);
        if(row) row->index=index;
        if(!indexed || index!=slot+scroll) { skip("widget-index");continue; }
        if (screen == 1) {
            const Value name{s,s.member(widget,"_name")}; std::string actual;
            if (!s.string(name,actual) || actual != "cell" + std::to_string(slot)) { skip("widget-name");continue; }
        }
        std::string label;
        if (screen == 3) {
            const Value item{s,s.element(provider,static_cast<std::uint32_t>(index))};
            const Value bound{s,s.member(widget,"data")};
            std::string displayed;
            // Original CharacterSelectCustomizationListItemRenderer.setData
            // passes GetLabel(raw.toString()) to its superclass. GetLabel is
            // raw.split("####")[0]; NEW/EQUIPPED tags are handled separately.
            // Keep the entire provider string in the captured semantic key so
            // a tag or label refresh between DOWN and UP still cancels it.
            if(row) { row->itemType=s.type(item);row->dataType=s.type(bound); }
            const bool itemString=s.string(item,label);
            const bool dataString=itemString && s.string(bound,displayed);
            if(row) { row->label=label;row->displayed=displayed; }
            if (!dataString || displayed.empty() || displayed != label.substr(0,label.find("####"))) { skip("widget-label");continue; }
        }
        GfxRect stage; WidgetPoint a,b;
        const bool projected=s.projectedBounds(widget,stage);
        if(row) { row->projected=projected;row->stage=stage; }
        if(!projected) { skip("widget-projection");continue; }
        const bool normalized=NormalizeWidgetPoint(frame,viewport,width,height,{stage.left,stage.top},a) &&
            NormalizeWidgetPoint(frame,viewport,width,height,{stage.right,stage.bottom},b);
        if(row) row->normalized=normalized;
        if(!normalized) { skip("widget-normalization");continue; }
        const GfxRect bounds{a.x,a.y,b.x,b.y};
        const bool hit=WidgetContains(bounds,{x,y});if(row) { row->bounds=bounds;row->hit=hit; }
        if(!hit) { skip("widget-outside");continue; }
        GfxIdentity widgetId;
        if (!s.identity(widget,widgetId)) return reject("widget-identity");
        // Keep only the hit and its exact containers, never all row values.
        // Movie/data ownership survives asynchronous renderer refresh until UP.
        out.kind = screen == 1 ? CharacterWidgetKind::Portrait : screen == 2 ? CharacterWidgetKind::Menu : CharacterWidgetKind::Customization;
        out.index = static_cast<std::int32_t>(index); out.player = player; out.screen = screen;
        out.contentId = contentId; out.containerId = containerId; out.widgetId = widgetId; out.providerId = providerId;
        out.content = content; out.container = container.h; out.widget = widget.h; out.provider = provider.h;
        out.label = std::move(label); out.bounds = bounds;
        container.h = {};widget.h = {};provider.h = {};
        if(observation) observation->reason="hit";
        if(row) row->reason="hit";
        return out;
    }
    return reject(s.good() ? "no-row-hit" : "session-failed");
}
bool SameBl2CharacterWidget(const CharacterWidgetTarget& a,const CharacterWidgetTarget& b) {
    return a && b && a.kind == b.kind && a.index == b.index && a.player == b.player && a.screen == b.screen &&
        a.contentId == b.contentId && a.containerId == b.containerId && a.widgetId == b.widgetId &&
        a.providerId == b.providerId && a.label == b.label;
}
bool ConfirmBl2CharacterPortrait(const CharacterWidgetTarget& down,const CharacterWidgetTarget& up,
                                bool wasSelected,std::int32_t selected) {
    return down.kind == CharacterWidgetKind::Portrait && wasSelected && selected == down.index &&
        SameBl2CharacterWidget(down,up);
}
bool SelectBl2CharacterWidget(GfxSession& s,const CharacterWidgetTarget& target) {
    if (!s.good() || !target || target.kind == CharacterWidgetKind::Portrait ||
        !sameIdentity(s,target.container,target.containerId) || !sameIdentity(s,target.widget,target.widgetId)) return false;
    const Value index{s,s.number(target.index)};
    const Value result{s,s.invoke(target.container,target.kind == CharacterWidgetKind::Menu ?
        "SetSelectedEntry" : "__set__selectedIndex",{index},true)};
    return result && s.good();
}
bool ClickBl2CharacterCustomization(GfxSession& s,const CharacterWidgetTarget& target) {
    if (!s.good() || target.kind != CharacterWidgetKind::Customization ||
        !sameIdentity(s,target.container,target.containerId) || !sameIdentity(s,target.provider,target.providerId)) return false;
    // The original CharacterSelectCustomizationList sends (player,"click",index)
    // to its native-bound callback. GFx marshals its FString, not a POD copy.
    const Value parent{s,s.member(target.container,"_parent")};
    const Value player{s,s.number(target.player)}, event{s,s.stringValue("click")}, index{s,s.number(target.index)};
    const Value result{s,s.invoke(parent,"HandleCustomizationSelected",{player,event,index},true)};
    return result && s.good();
}
bool FitBl2PauseBackdrop(GfxSession& s, GfxHandle list,
                        GfxRect& visible, GfxRect& before, GfxRect& after) {
    if (!s.good() || !s.object(list) || !s.frameRect(visible)) return false;
    const Value root{s,s.member(list,"_root")}, menu{s,s.member(root,"menu")};
    const Value scrim{s,s.member(menu,"pauseMenuDeco")};
    GfxIdentity rootId, menuId;
    const Value menuParent{s,s.member(menu,"_parent")}, scrimParent{s,s.member(scrim,"_parent")};
    if (s.type(root) != 10 || s.type(menu) != 10 || s.type(scrim) != 10 ||
        !s.identity(root,rootId) || !s.identity(menu,menuId) ||
        !sameIdentity(s,menuParent,rootId) || !sameIdentity(s,scrimParent,menuId)) return false;
    for (const auto clip : {root.h,menu.h,scrim.h}) {
        bool shown = false; double alpha = 0, rotation = 0;
        if (!boolean(s,clip,"_visible",shown) || !shown || !number(s,clip,"_alpha",alpha) ||
            alpha <= 0 || alpha > 100 || !number(s,clip,"_rotation",rotation) || std::abs(rotation) > .001) return false;
    }
    double x = 0, y = 0, sx = 0, sy = 0;
    if (!number(s,root,"_x",x) || !number(s,root,"_y",y) || std::abs(x) > .001 || std::abs(y) > .001 ||
        !number(s,root,"_xscale",sx) || !number(s,root,"_yscale",sy) || std::abs(sx - 100) > .001 || std::abs(sy - 100) > .001 ||
        !number(s,menu,"_xscale",sx) || !number(s,menu,"_yscale",sy) || std::abs(sx - 100) > .001 || std::abs(sy - 100) > .001 ||
        !number(s,scrim,"_xscale",sx) || !number(s,scrim,"_yscale",sy) || sx <= 0 || sy <= 0) return false;
    const auto bounds = [&](GfxHandle target, GfxRect& out) {
        const Value rect{s,s.invoke(scrim,"getBounds",{target})};
        return number(s,rect,"xMin",out.left) && number(s,rect,"yMin",out.top) &&
            number(s,rect,"xMax",out.right) && number(s,rect,"yMax",out.bottom) &&
            std::isfinite(out.left) && std::isfinite(out.top) && std::isfinite(out.right) && std::isfinite(out.bottom) &&
            out.right > out.left && out.bottom > out.top;
    };
    GfxRect local;
    // TitleMenusClik sprite155 contains only shape154: solid RGBA(0,0,0,128),
    // local twip bounds [-15200,15200,-7600,7600]. Verify this exact shipped
    // content before changing the clip, never the separate lobbyMenuDeco.
    if (!bounds(scrim,local) || std::abs(local.left + 760) > .1 || std::abs(local.right - 760) > .1 ||
        std::abs(local.top + 380) > .1 || std::abs(local.bottom - 380) > .1 || !bounds(root,before)) return false;
    after = before;
    const auto covers = [&](const GfxRect& r) {
        return r.left <= visible.left + .1 && r.top <= visible.top + .1 &&
            r.right >= visible.right - .1 && r.bottom >= visible.bottom - .1;
    };
    if (covers(before)) return true;
    const GfxRect fit{std::min(before.left,visible.left),std::min(before.top,visible.top),
                      std::max(before.right,visible.right),std::max(before.bottom,visible.bottom)};
    if (fit.right - fit.left > 16384 || fit.bottom - fit.top > 16384) return false;
    const Value center{s,s.point((fit.left + fit.right) / 2,(fit.top + fit.bottom) / 2)};
    const Value converted{s,s.invoke(menu,"globalToLocal",{center})};
    if (!converted || !number(s,center,"x",x) || !number(s,center,"y",y)) return false;
    const Value scaleX{s,s.number(100 * (fit.right - fit.left) / (local.right - local.left))};
    const Value scaleY{s,s.number(100 * (fit.bottom - fit.top) / (local.bottom - local.top))};
    const Value offsetX{s,s.number(x)}, offsetY{s,s.number(y)};
    return scaleX && scaleY && offsetX && offsetY &&
        s.setMember(scrim,"_xscale",scaleX) && s.setMember(scrim,"_yscale",scaleY) &&
        s.setMember(scrim,"_x",offsetX) && s.setMember(scrim,"_y",offsetY) && bounds(root,after) && covers(after);
}
bool FormatBl2TouchSettingValue(GfxSession& s, GfxHandle inner, std::size_t id, double value) {
    if (id >= kTouchSettingCount || touchSettingSpecs[id].toggle || !std::isfinite(value)) return false;
    const Value field{s,s.member(inner,"currentValue")};
    const auto expected = TouchSettingValueText(id,value);
    bool written = false;
    if (s.object(field)) {
        const Value displayed{s,s.member(field,"text")};
        std::string current;
        if (s.string(displayed,current) && current == expected) return true;
        const Value text{s,s.stringValue(expected)};
        written = text && s.setMember(field,"text",text);
    }
#if defined(__ANDROID__)
    static const bool diagnostics = [] {
        char property[PROP_VALUE_MAX] = {};
        __system_property_get("debug.bl2bridge.touch_diagnostics",property);
        return property[0] == '1' && !property[1];
    }();
    static unsigned logs = 0;
    if (diagnostics && logs < 24 && s.hasCallBudget(1)) {
        ++logs;
        const Value text{s,s.object(field) ? s.member(field,"text") : GfxHandle{}};
        std::string actual; s.string(text,actual);
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "settings value id=%zu fieldType=%u written=%u expected='%s' actual='%s' calls=%u session=%s",
            id,s.type(field),written ? 1u : 0u,expected.c_str(),actual.c_str(),s.calls(),s.reason() ? s.reason() : "okay");
    }
#endif
    return written;
}
void FormatBl2TouchSettingsList(GfxSession& s, GfxHandle list, const GfxIdentity& expected,
                              const std::vector<std::int32_t>& events) {
    if (!s.good() || !s.object(list) || events.empty() || events.size() > 128) return;
    const Value data{s,s.member(list,"data")}, provider{s,s.member(list,"dataProvider")};
    double rows = 0, count = 0, total = 0;
    if (!sameIdentity(s,data,expected) || !sameIdentity(s,provider,expected) ||
        !number(s,data,"length",rows) || rows != events.size()) return;
    const Value renderers{s,s.member(list,"renderers")};
    if (!number(s,renderers,"length",count) || count < 0 || count > 128 || std::floor(count) != count ||
        !number(s,list,"totalRenderers",total) || total != count) return;
    GfxIdentity owner; if (!s.identity(list,owner)) return;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count) && s.hasCallBudget(34); ++i) {
        const Value renderer{s,s.element(renderers,i)}; double index = -1;
        if (!number(s,renderer,"index",index) || index < 0 || std::floor(index) != index || index >= events.size()) continue;
        const auto id = events[static_cast<std::size_t>(index)] - kTouchSettingsValue;
        if (id < 0 || id >= static_cast<int>(kTouchSettingCount) || touchSettingSpecs[id].toggle) continue;
        const Value row{s,s.element(data,static_cast<std::uint32_t>(index))}, attached{s,s.member(renderer,"data")};
        const Value actualOwner{s,s.member(renderer,"owner")}, inner{s,s.member(renderer,"inner")};
        const Value innerData{s,s.member(inner,"data")}, parent{s,s.member(inner,"_parent")};
        GfxIdentity rowId, rendererId; double value = 0;
        if (s.identity(row,rowId) && s.identity(renderer,rendererId) && sameIdentity(s,actualOwner,owner) &&
            sameIdentity(s,attached,rowId) && sameIdentity(s,innerData,rowId) && sameIdentity(s,parent,rendererId) &&
            number(s,row,"currentValue",value)) FormatBl2TouchSettingValue(s,inner,static_cast<std::size_t>(id),value);
    }
}
GfxHandle CaptureBl2OptionsOriginal(GfxSession& s, GfxHandle list, GfxHandle header,
                                   GfxHandle description, const UiModelSnapshot& model) {
    if (!s.good() || !s.object(list) || !s.object(header) || !model.valid ||
        model.movieClass != "OptionsGFxMovie" || !model.owner || !model.list || !model.provider ||
        !model.structure || model.eventIds.empty() || model.eventIds.size() > 128) return {};
    const auto key = std::to_string(model.owner) + ":" + std::to_string(model.list) + ":" +
        std::to_string(model.provider) + ":" + std::to_string(model.structure);
    Value saved{s,s.member(list,"_bl2TouchNativeOriginal")};
    if (s.object(saved)) {
        const Value actualKey{s,s.member(saved,"key")}; std::string actual;
        if (s.string(actualKey,actual) && actual == key) {
            const Value data{s,s.member(saved,"data")}; double count = 0;
            if (s.type(data) != 9 || !number(s,data,"length",count) || count != model.eventIds.size()) return {};
            const auto retained = saved.h; saved.h = {}; return retained;
        }
    }
    const Value data{s,s.member(list,"data")}, provider{s,s.member(list,"dataProvider")};
    GfxIdentity identity; double count = 0;
    // A new baseline comes only from the original committed native row count.
    // Never truncate a custom page or guess which existing rows are synthetic.
    if (s.type(data) != 9 || !s.identity(data,identity) || !sameIdentity(s,provider,identity) ||
        !number(s,data,"length",count) || count != model.eventIds.size()) return {};
    Value snapshot{s,s.point(0,0)}; const Value originalData{s,s.invoke(data,"slice")};
    const Value signature{s,s.stringValue(key)};
    if (!snapshot || s.type(originalData) != 9 || !s.setMember(snapshot,"key",signature) ||
        !s.setMember(snapshot,"data",originalData)) return {};
    const auto retain = [&](const char* name, GfxHandle from, const char* field) {
        const Value value{s,s.member(from,field)}; return value && s.setMember(snapshot,name,value);
    };
    if (!retain("header",header,"htmlText") || !retain("listeners",list,"_listeners") ||
        !retain("slider",list,"onSliderValueChange") || !retain("spinner",list,"onSpinnerValueChange") ||
        (s.object(description) && !retain("description",description,"htmlText")) ||
        !s.setMember(list,"_bl2TouchNativeOriginal",snapshot)) return {};
    const auto retained = snapshot.h; snapshot.h = {}; return retained;
}
double StepBl2InterfacePercent(double value, bool increase) {
    if (value == 125) return increase ? 130 : 120;
    return std::clamp(value + (increase ? 10 : -10),100.0,200.0);
}
WidgetTypography ScaleBl2WidgetTypography(GfxSession& s, GfxHandle list,
                                         const GfxIdentity& expected, const UiModelSnapshot& originalModel,
                                         const std::vector<std::int32_t>& currentEvents, bool allowLayoutChange) {
    WidgetTypography out;
    const double scale = Bl2InterfaceScale();
    // Frontend/Pause retain the original native provider. Options can replace
    // it with our committed rows; its current events describe that exact data.
    if (!originalModel.valid || (originalModel.movieClass != "FrontendGFxMovie" &&
        originalModel.movieClass != "PauseGFxMovie" && originalModel.movieClass != "OptionsGFxMovie")) return out;
    const bool nativeRows = originalModel.movieClass != "OptionsGFxMovie";
    const auto rowCount = nativeRows ? originalModel.eventIds.size() : currentEvents.size();
    if (!s.hasCallBudget(100) || !s.object(list) || !rowCount || rowCount > 128) return out;
    const Value data{s,s.member(list,"data")};
    const Value renderers{s,s.member(list,"renderers")};
    double rows = 0, count = 0, total = 0, scroll = 0;
    bool external = true;
    GfxIdentity owner;
    {
    const Value provider{s,s.member(list,"dataProvider")};
    if (!sameIdentity(s,data,expected) || !sameIdentity(s,provider,expected) || !s.identity(list,owner) ||
        !boolean(s,list,"externalRenderers",external) || external ||
        !number(s,data,"length",rows) || rows != rowCount ||
        !number(s,renderers,"length",count) || count < 1 || count > 128 || std::floor(count) != count ||
        !number(s,list,"totalRenderers",total) || total != count ||
        !number(s,list,"scrollPosition",scroll) || scroll < 0 || scroll >= rowCount || std::floor(scroll) != scroll) return out;
    }
    const auto occupied = static_cast<std::uint32_t>(std::min(count,rowCount - scroll));
    const auto attached = [&](GfxHandle renderer, double& index) {
        const Value actualOwner{s,s.member(renderer,"owner")}, actualData{s,s.member(renderer,"data")};
        if (!sameIdentity(s,actualOwner,owner) || !number(s,renderer,"index",index) ||
            index < 0 || std::floor(index) != index || index >= rowCount) return false;
        const Value row{s,s.element(data,static_cast<std::uint32_t>(index))};
        GfxIdentity rowId;
        return s.identity(row,rowId) && sameIdentity(s,actualData,rowId);
    };
    double baseHeight = 0;
    if (!number(s,list,"_bl2TouchRowHeight",baseHeight)) {
        if (nativeRows) {
            // Main/Pause automatic height uses a fresh template, not animated
            // selection artwork's bounding _height. Original drawLayout body
            //144..174 writes _y=i*rendererHeight+margin+paddingTop. Measure that
            // actual pitch from attached rows with the same coordinate parent.
            double previousIndex = -1, previousY = 0, pitch = 0;
            GfxIdentity previousParent;
            for (std::uint32_t i = 0; i < occupied && s.hasCallBudget(16); ++i) {
                const Value renderer{s,s.element(renderers,i)}, parent{s,s.member(renderer,"_parent")};
                double index = -1, y = 0; GfxIdentity parentId;
                if (!attached(renderer,index) || index != scroll + i || !number(s,renderer,"_y",y) || !s.identity(parent,parentId)) return out;
                if (previousIndex >= 0) {
                    if (parentId != previousParent || index <= previousIndex) return out;
                    const double measured = (y - previousY) / (index - previousIndex);
                    if (measured < 8 || measured > 160 || (pitch && std::abs(pitch - measured) > .2)) return out;
                    pitch = measured;
                }
                previousIndex = index; previousY = y; previousParent = parentId;
            }
            if (!pitch) return out;
            baseHeight = pitch;
        } else {
        const Value height{s,s.invoke(list,"__get__rowHeight")};
        // Original ScrollingList.draw (TitleMenusClik sprite191, body128..218)
        // measures an itemRenderer's _height when _rowHeight is undefined.
        // Published, attached renderers have already received that same height
        // through drawLayout/setSize. No fixed pitch or guessed private field.
        if (!s.number(height,baseHeight) || baseHeight < 8 || baseHeight > 160) {
            baseHeight = 0;
            for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count) && s.hasCallBudget(12); ++i) {
                const Value renderer{s,s.element(renderers,i)}; double index = -1, measured = 0;
                if (attached(renderer,index) && number(s,renderer,"_height",measured) && measured >= 8 && measured <= 160) {
                    baseHeight = measured; break;
                }
            }
        }
        }
        if (baseHeight < 8 || baseHeight > 160) return out;
        const Value original{s,s.number(baseHeight)};
        if (!s.setMember(list,"_bl2TouchRowHeight",original)) return out;
    }
    if (baseHeight < 8 || baseHeight > 160) return out;
    out.originalRowHeight = baseHeight;
    const double desiredHeight = nativeRows ? baseHeight : baseHeight * scale;
    double actualHeight = nativeRows ? baseHeight : 0;
    bool heightMatches = nativeRows;
    if (!nativeRows) {
        const Value height{s,s.invoke(list,"__get__rowHeight")};
        heightMatches = s.number(height,actualHeight) && std::abs(actualHeight - desiredHeight) <= .01;
    }
    const auto setNumber = [&](GfxHandle object, const char* name, double value) {
        const Value v{s,s.number(value)}; return v && s.setMember(object,name,v);
    };
    const auto baseline = [&](GfxHandle object, const char* name, double fallback, double& value) {
        if (number(s,object,name,value)) return std::isfinite(value);
        value = fallback; return std::isfinite(value) && setNumber(object,name,value);
    };
    const auto innerScale = [&](GfxHandle renderer, GfxHandle inner, double layoutHeight, bool change) {
        double sy = 0, innerSy = 0, innerY = 0, rotation = 0, innerRotation = 0;
        if (!number(s,renderer,"_yscale",sy) || sy <= 0 || sy > 1000 ||
            !number(s,inner,"_yscale",innerSy) || innerSy <= 0 || innerSy > 1000 ||
            !number(s,inner,"_y",innerY) || std::abs(innerY) > 2048 ||
            !number(s,renderer,"_rotation",rotation) || std::abs(rotation) > .01 ||
            !number(s,inner,"_rotation",innerRotation) || std::abs(innerRotation) > .01) return 0.0;
        double originalSy = 0, originalInnerSy = 0;
        if (!baseline(renderer,"_bl2TouchRendererScaleY",sy * baseHeight / layoutHeight,originalSy) ||
            !baseline(inner,"_bl2TouchInnerScaleY",innerSy,originalInnerSy) ||
            originalSy <= 0 || originalSy > 1000 || originalInnerSy <= 0 || originalInnerSy > 1000) return 0.0;
        // Button.draw sets _height=__height. Increasing rowHeight thus already
        // scales its children in Y. Undo that extra parent scale only on inner;
        // the outer selection/hit row remains enlarged. TextFormat supplies the
        // requested glyph size in both axes without multiplying their Y scale.
        const double wanted = originalInnerSy * originalSy / sy;
        if (wanted <= 0 || wanted > 1000 || (change && std::abs(innerSy - wanted) > .01 &&
            !setNumber(inner,"_yscale",wanted))) return 0.0;
        // The selected A glyph is an unnamed original timeline sibling of
        // inner (Options sprite38/TitleMenusClik sprite64 depth3). Their exact
        // scale9Grid keeps the circle's left-edge X fixed while stretching its
        // middle-slice Y. Save that original Rectangle, clear only this reviewed
        // grid while enlarged, and restore it at100. The native AS2 setter
        // clears its display-object grid for undefined; the getter returns
        // undefined for that zero rectangle, not a synthetic Rectangle object.
        if (change) {
            const Value grid{s,s.member(renderer,"scale9Grid")};
            const auto reviewedGrid = [&](GfxHandle value) {
                double x = 0,y = 0,w = 0,h = 0;
                return s.type(value) == 8 && number(s,value,"x",x) && number(s,value,"y",y) &&
                    number(s,value,"width",w) && number(s,value,"height",h) &&
                    std::abs(x + 9) < .01 && std::abs(y + 12) < .01 &&
                    std::abs(w - 190) < .01 && std::abs(h - 74) < .01;
            };
            // A cleared enlarged renderer needs no further grid write. Read its
            // saved Rectangle only when clearing/restoring a real grid; this
            // leaves the established per-frame text budget for visible rows.
            if (scale == 1 || (grid && s.type(grid) != 0)) {
                const Value saved{s,s.member(renderer,"_bl2TouchScale9Grid")};
                const bool savedGrid = saved && s.type(saved) != 0, currentGrid = grid && s.type(grid) != 0;
                if (savedGrid && !reviewedGrid(saved)) return 0.0;
                if (currentGrid && !reviewedGrid(grid)) return 0.0;
                if (scale > 1 && currentGrid) {
                    if (!savedGrid && !s.setMember(renderer,"_bl2TouchScale9Grid",grid)) return 0.0;
                    const Value clear{s,s.undefined()};
                    if (!s.setMember(renderer,"scale9Grid",clear)) return 0.0;
                    const Value actual{s,s.member(renderer,"scale9Grid")};
                    if (s.type(actual) != 0) return 0.0;
                } else if (scale == 1 && !currentGrid && savedGrid &&
                    !s.setMember(renderer,"scale9Grid",saved)) return 0.0;
            }
        }
        // With that grid cleared, an affine outer X matching its real Y resize
        // preserves the circle. Cancel X only on the named content container;
        // captions, values and tracks retain their original column coordinates.
        double sx = 0, innerSx = 0, originalSx = 0, originalInnerSx = 0;
        if (!number(s,renderer,"_xscale",sx) || sx <= 0 || sx > 1000 ||
            !number(s,inner,"_xscale",innerSx) || innerSx <= 0 || innerSx > 1000 ||
            !baseline(renderer,"_bl2TouchRendererScaleX",sx,originalSx) ||
            !baseline(inner,"_bl2TouchInnerScaleX",innerSx,originalInnerSx)) return 0.0;
        const auto outerX = originalSx * sy / originalSy;
        const auto contentX = originalInnerSx * originalSx / outerX;
        if (outerX <= 0 || outerX > 1000 || contentX <= 0 || contentX > 1000 ||
            (change && std::abs(sx - outerX) > .01 && !setNumber(renderer,"_xscale",outerX)) ||
            (change && std::abs(innerSx - contentX) > .01 && !setNumber(inner,"_xscale",contentX))) return 0.0;
        return (desiredHeight * 100 / sy - innerY) * 100 / (change ? wanted : innerSy);
    };
    if (!heightMatches) {
        if (!allowLayoutChange) return out; // Captured pointers keep their original renderer/axis.
        double selected = -1;
        const bool validSelection = number(s,list,"selectedIndex",selected) && selected >= 0 &&
            std::floor(selected) == selected && selected < rowCount;
        // Save actual scales before native drawLayout/setSize changes them.
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count) && s.hasCallBudget(30); ++i) {
            const Value renderer{s,s.element(renderers,i)}; double index = -1;
            if (!attached(renderer,index)) continue;
            const Value inner{s,s.member(renderer,"inner")}, parent{s,s.member(inner,"_parent")};
            GfxIdentity rendererId;
            if (s.identity(renderer,rendererId) && sameIdentity(s,parent,rendererId)) {
                innerScale(renderer,inner,actualHeight > 0 ? actualHeight : baseHeight,false);
                // The selected timeline retains resized artwork while the
                // same-index setter exits early. Restart its current original
                // Button state, preserving selection and all action handlers.
                // Button.setState uses gotoAndPlay/updateAfterStateChange;
                // unlike handleClick it dispatches no option activation.
                if (validSelection && index == selected && s.hasCallBudget(4)) {
                    const Value state{s,s.member(renderer,"state")}; std::string name;
                    if (s.string(state,name,64) && !name.empty()) {
                        const Value refreshed{s,s.invoke(renderer,"setState",{state},true)};
                        if (!refreshed || !s.good()) return out;
                    }
                }
            }
        }
        if (!s.hasCallBudget(8)) return out;
        const Value height{s,s.number(desiredHeight)}, changed{s,s.invoke(list,"__set__rowHeight",{height},true)};
        const Value actual{s,s.invoke(list,"__get__rowHeight")};
        if (!changed || !s.number(actual,actualHeight) || std::abs(actualHeight - desiredHeight) > .01) return out;
        // Let the original list recompute renderer count, layout and scrolling.
        // Its renderer array is invalidated here; format it on the next retained
        // presentation pass rather than writing fields of the previous layout.
        const Value validated{s,s.invoke(list,"validateNow",{},true)};
        if (validated && s.good()) {
            out.rowHeight = actualHeight;
            // ScrollingList.__set__selectedIndex returns immediately for an
            // unchanged selection. Its public scrollToIndex applies the new
            // renderer capacity through the original scroll/data path only.
            if (validSelection && s.hasCallBudget(4)) {
                const Value index{s,s.number(selected)}, shown{s,s.invoke(list,"scrollToIndex",{index},true)};
                const Value refreshed{s,s.invoke(list,"validateNow",{},true)};
                if (!shown || !refreshed) out.rowHeight = 0;
            }
        }
        return out;
    }
    out.rowHeight = actualHeight; out.renderers = static_cast<std::uint32_t>(count);
    struct TextGeometry { bool valid = false; double x = 0, right = 0, bottom = 0; };
    const auto scaleText = [&](GfxHandle field, double rightLimit, double space) {
        TextGeometry geometry;
        if (!s.object(field)) return geometry;
        bool shown = false; double alpha = 0;
        if (!boolean(s,field,"_visible",shown) || !shown || !number(s,field,"_alpha",alpha) || alpha <= 0) return geometry;
        const Value format{s,s.invoke(field,"getTextFormat")};
        double size = 0, height = 0, width = 0, textWidth = 0, textHeight = 0, x = 0, y = 0, sx = 0, sy = 0;
        if (!s.object(format) || !number(s,format,"size",size) || size < 4 || size > 120 ||
            !number(s,field,"_height",height) || height < 4 || height > 2048 ||
            !number(s,field,"_width",width) || width < 8 || width > 4096 ||
            !number(s,field,"textWidth",textWidth) || textWidth < 0 || textWidth > 16384 ||
            !number(s,field,"textHeight",textHeight) || textHeight < 0 || textHeight > 2048 ||
            !number(s,field,"_x",x) || !number(s,field,"_y",y) ||
            !number(s,field,"_xscale",sx) || sx <= 0 || sx > 1000 ||
            !number(s,field,"_yscale",sy) || sy <= 0 || sy > 1000) return geometry;
        double baseSize = 0, baseTextHeight = 0, baseWidth = 0;
        if (!baseline(field,"_bl2TouchFontSize",size,baseSize) ||
            !baseline(field,"_bl2TouchTextHeight",height,baseTextHeight) ||
            !baseline(field,"_bl2TouchTextWidth",width,baseWidth) ||
            baseSize < 4 || baseSize > 120 || baseTextHeight < 4 || baseTextHeight > 2048 ||
            baseWidth < 8 || baseWidth > 4096) return geometry;
        // The authored caption/value columns must stay separate. Fit long
        // localized labels using TextField's own measured glyph width instead
        // of cropping text or moving the next column.
        const double available = std::min(baseWidth,rightLimit - x);
        const double horizontal = textWidth > 0 ? (available - 4) * 100 * size / (sx * textWidth * baseSize) : scale;
        const double vertical = textHeight > 0 ? ((space - y - 2) * 100 / sy - 4) * size / (textHeight * baseSize) : scale;
        const double factor = std::clamp(std::min(horizontal,vertical),1.0,scale);
        const double desired = baseSize * factor;
        // Main/Pause keep their original row and selection animation geometry.
        // Grow a caption box only within the actual pitch; existing overscan
        // never shrinks. Options has its verified larger native row layout.
        const double desiredTextHeight = nativeRows ? std::max(baseTextHeight,std::min(baseTextHeight * scale,space - y - 2)) : baseTextHeight * scale;
        if (std::abs(size - desired) > .01) {
            const Value larger{s,s.number(desired)};
            if (!s.setMember(format,"size",larger)) return geometry;
            const Value future{s,s.invoke(field,"setNewTextFormat",{format},true)};
            const Value existing{s,s.invoke(field,"setTextFormat",{format},true)};
            if (!future || !existing) return geometry;
        }
        if (std::abs(height - desiredTextHeight) > .01) {
            const Value box{s,s.number(desiredTextHeight)};
            if (!s.setMember(field,"_height",box)) return geometry;
        }
        ++out.fields;
        if (factor < scale - .01) ++out.fittedFields;
        // TextField width may cover both columns or auto-size after a format.
        // Compute the actual glyph edge using its original alignment instead.
        std::string align; const Value alignment{s,s.member(format,"align")}; s.string(alignment,align,16);
        const double glyphWidth = textWidth * desired / size * sx / 100;
        const double left = align == "right" ? x + width - glyphWidth - 2 * sx / 100 :
            align == "center" ? x + (width - glyphWidth) / 2 : x + 2 * sx / 100;
        geometry = {true,left,left + glyphWidth,y + (textHeight * desired / size + 4) * sy / 100};
        return geometry;
    };
    struct SliderGeometry { bool valid = false; double y = 0, sy = 0, top = 0, bottom = 0; };
    const auto sliderGeometry = [&](GfxHandle slider) {
        SliderGeometry g;
        const Value track{s,s.member(slider,"track")}, trackParent{s,s.member(track,"_parent")};
        const Value thumb{s,s.member(slider,"thumb")}, thumbParent{s,s.member(thumb,"_parent")};
        GfxIdentity sliderId;
        if (!s.identity(slider,sliderId) || !sameIdentity(s,trackParent,sliderId) ||
            !sameIdentity(s,thumbParent,sliderId) || !number(s,slider,"_y",g.y) ||
            !number(s,slider,"_yscale",g.sy) || g.sy <= 0 || g.sy > 1000) return g;
        const Value trackBounds{s,s.invoke(track,"getBounds",{slider})}, thumbBounds{s,s.invoke(thumb,"getBounds",{slider})};
        double thumbTop = 0, thumbBottom = 0;
        if (!number(s,trackBounds,"yMin",g.top) || !number(s,trackBounds,"yMax",g.bottom) ||
            !number(s,thumbBounds,"yMin",thumbTop) || !number(s,thumbBounds,"yMax",thumbBottom)) return g;
        // Original Slider(sprite187).draw/updateThumb owns this real thumb.
        // Its rectangle can extend below the thin track; reserve both, rather
        // than letting a visible thumb enter the following caption's slot.
        g.top = std::min(g.top,thumbTop); g.bottom = std::max(g.bottom,thumbBottom);
        g.valid = g.bottom > g.top && g.bottom - g.top <= 512;
        return g;
    };
    double savedResume = 0;
    number(s,list,"_bl2TouchTypographyNext",savedResume);
    const auto start = savedResume >= 0 && savedResume < occupied && std::floor(savedResume) == savedResume ?
        static_cast<std::uint32_t>(savedResume) : 0u;
    std::uint32_t processed = 0;
    // Smaller scales expose more rows. Resume bounded work on the next input
    // boundary instead of repeatedly formatting only the same first rows.
    for (; processed < occupied && s.hasCallBudget(160); ++processed) {
        const auto i = (start + processed) % occupied;
        const Value renderer{s,s.element(renderers,i)}; double index = -1;
        if (!attached(renderer,index)) continue;
        const Value inner{s,s.member(renderer,"inner")};
        GfxIdentity rendererId, innerId;
        {
            const Value parent{s,s.member(inner,"_parent")};
            if (!s.identity(renderer,rendererId) || !sameIdentity(s,parent,rendererId) || !s.identity(inner,innerId)) continue;
        }
        std::string rendererType;
        {
            const Value item{s,s.element(data,static_cast<std::uint32_t>(index))}, type{s,s.member(item,"rendererType")};
            if (!s.string(type,rendererType)) continue;
            // Replaying the selected Button state can republish its inner at
            // original frame1/default: caption is rebound by
            // updateAfterStateChange, but that frame hides value and slider.
            // Original renderer.setData is the only path that restores the
            // owned row's rendererType and initializes its actual controls.
            // Never rebind a renderer while a captured drag uses its axis.
            double frame = 0;
            if (!nativeRows && (rendererType == "slider" || rendererType == "spinner") &&
                number(s,inner,"_currentframe",frame) && frame == 1) {
                if (!allowLayoutChange || !s.hasCallBudget(32)) continue;
                GfxIdentity itemId;
                const Value bound{s,s.member(renderer,"data")};
                if (!s.identity(item,itemId) || !sameIdentity(s,bound,itemId)) continue;
                SettingsCallbacks callbacks(s,list);
                if (!callbacks.active) continue;
                const Value rebound{s,s.invoke(renderer,"setData",{item},true)};
                if (!rebound || !s.good()) return out;
                // The original setData may republish children. Measure their
                // current identities on the next bounded presentation pass.
                continue;
            }
        }
        double space = 0;
        if (nativeRows) {
            double sy = 0, innerSy = 0, innerY = 0;
            if (number(s,renderer,"_yscale",sy) && sy > 0 && sy <= 1000 &&
                number(s,inner,"_yscale",innerSy) && innerSy > 0 && innerSy <= 1000 &&
                number(s,inner,"_y",innerY) && std::abs(innerY) <= 2048)
                space = (actualHeight * 100 / sy - innerY) * 100 / innerSy;
        } else space = innerScale(renderer,inner,actualHeight,true);
        if (!space) continue;
        const Value slider{s,!nativeRows && rendererType == "slider" ? s.member(inner,"slider") : GfxHandle{}};
        SliderGeometry control;
        if (slider) {
            const Value parent{s,s.member(slider,"_parent")};
            if (sameIdentity(s,parent,innerId)) control = sliderGeometry(slider);
        }
        // Leave room for the actual control plus two2px clearances. A selected
        // caption may have larger native glyphs; fit it by the real row pitch
        // before attempting to put the control below those glyphs.
        const double textSpace = control.valid ? space - (control.bottom - control.top) * control.sy / 100 - 2 : space;
        const bool setting = rendererType == "slider" || rendererType == "spinner";
        TextGeometry valueGeometry;
        if (setting) {
            const Value value{s,s.member(inner,"currentValue")}, parent{s,s.member(value,"_parent")};
            if (sameIdentity(s,parent,innerId)) valueGeometry = scaleText(value,4096,textSpace);
        }
        double captionRight = valueGeometry.valid ? valueGeometry.x - 4 : 4096;
        if (rendererType == "spinner" && valueGeometry.valid) {
            const Value arrow{s,s.member(inner,"spinnerLeftArrow")};
            if (s.object(arrow)) {
            // UI_Options.Options moves this arrow from currentValue.textWidth.
            // Run that original layout after changing the value's TextFormat.
            const Value updated{s,s.invoke(inner,"updateSpinnerArrows",{},true)};
            const Value bounds{s,s.invoke(arrow,"getBounds",{inner})}; double left = 0;
            if (updated && number(s,bounds,"xMin",left)) captionRight = std::min(captionRight,left - 4);
            }
        }
        TextGeometry captionGeometry;
        {
            const Value caption{s,s.member(inner,"textField")}, parent{s,s.member(caption,"_parent")};
            if (sameIdentity(s,parent,innerId)) captionGeometry = scaleText(caption,captionRight,textSpace);
        }
        if (control.valid && (captionGeometry.valid || valueGeometry.valid)) {
            double originalY = 0;
            if (!baseline(slider,"_bl2TouchSliderY",control.y,originalY)) continue;
            const double glyphBottom = std::max(captionGeometry.bottom,valueGeometry.bottom);
            const double minimumY = glyphBottom + 2 - control.top * control.sy / 100;
            const double maximumY = space - 2 - control.bottom * control.sy / 100;
            const bool originalScale = scale == 1;
            const bool fits = originalScale || minimumY <= maximumY;
            if (fits) {
                const double wanted = originalScale ? originalY : std::clamp(originalY,minimumY,maximumY);
                if (std::abs(control.y - wanted) > .01) setNumber(slider,"_y",wanted);
            }
#if defined(__ANDROID__)
            static const bool diagnostics = [] {
                char property[PROP_VALUE_MAX] = {};
                __system_property_get("debug.bl2bridge.touch_diagnostics",property);
                return property[0] == '1' && !property[1];
            }();
            static unsigned logs = 0;
            if (diagnostics && index == 0 && logs++ < 24)
                __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                    "settings slider layout row=%.0f space=%.2f glyphBottom=%.2f control=(%.2f,%.2f) original=%.2f allowed=(%.2f,%.2f) fit=%u",
                    index,space,glyphBottom,control.top,control.bottom,originalY,minimumY,maximumY,fits ? 1u : 0u);
#endif
        }
    }
    const auto next = (start + processed) % occupied;
    if (s.hasCallBudget(2) && next != savedResume) setNumber(list,"_bl2TouchTypographyNext",next);
    return out;
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
        // DOWN may select and redraw this renderer without changing its data
        // identity. Recheck the semantic row before its eventual UP activation.
        const Value label{s_,s_.member(rowData_,"label")}, type{s_,s_.member(rowData_,"rendererType")};
        std::string caption, rendererType;
        if (!s_.string(label,caption) || caption != row->caption ||
            !s_.string(type,rendererType) || rendererType != row->rendererType) {
            reason_ = "binding-live-row-content"; return;
        }
        if (row->kind == WidgetKind::Slider || row->kind==WidgetKind::Spinner) {
            inner_ = keep(s_.member(renderer_,"inner"));
            const Value innerData{s_,s_.member(inner_,"data")};
            const Value parent{s_,s_.member(inner_,"_parent")};
            if (!sameIdentity(s_,inner_,row->inner) || !sameIdentity(s_,innerData,row->data) || !sameIdentity(s_,parent,row->renderer)) {
                reason_ = "binding-live-slider"; return;
            }
            if(row->kind==WidgetKind::Slider) {
                slider_=keep(s_.member(inner_,"slider"));
                if(!sameIdentity(s_,slider_,row->slider) || !boolean(s_,slider_,"disabled",disabled) || disabled) {reason_="binding-live-slider";return;}
            } else {
                choices_=keep(s_.member(rowData_,"spinnerChoices"));double count=0;
                if(!sameIdentity(s_,choices_,row->choices) || !number(s_,choices_,"length",count) || count!=row->maximum+1) {reason_="binding-live-spinner";return;}
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
    if(!s.good()) return {WidgetDeliveryState::Failed,s.reason() ? s.reason() : "command-session-failed",0};
    if (!exactModelStillCurrent || controllerId > 3 || command.route != widgets.route ||
        !retained.matches(model,widgets,rowRequired) || !exactModelStillCurrent() ||
        (rowRequired && !sameTarget(retained.target(),command.target))) return {};
    // A second session reads/pins the currently attached values. The old retained
    // values prevent allocator ABA but are never blindly treated as still attached.
    WidgetBinding live(s,model,widgets,rowRequired ? &command.target : nullptr);
    if (!live.valid()) return {WidgetDeliveryState::Rejected,live.reason() ? live.reason() : "binding-stale",0};
    const auto setting = command.target.eventId - kTouchSettingsValue;
    const bool custom = rowRequired && command.target.eventId >= kTouchSettingsOpen;
    std::unique_ptr<SettingsCallbacks> quiet;
    if (custom) {
        quiet = std::make_unique<SettingsCallbacks>(s,live.list());
        if (!quiet->active || !exactModelStillCurrent()) return {WidgetDeliveryState::Rejected,"touch-settings-callbacks",0};
    }
    if (command.kind == WidgetCommandKind::Select) {
        double selected = -1;
        if (!number(s,live.list(),"selectedIndex",selected) || !exactModelStillCurrent()) return {};
        if (selected == command.target.index) return {WidgetDeliveryState::Unchanged,"selection-unchanged",selected};
        const Value index{s,s.number(command.target.index)};
        if (!index || !exactModelStillCurrent()) return {};
        return invoke(s,live.list(),"__set__selectedIndex",{index});
    }
    if (command.kind == WidgetCommandKind::Click) {
        if (!exactModelStillCurrent()) return {};
        if (custom && command.target.kind == WidgetKind::Button) {
            double selected = -1;
            if (!number(s,live.list(),"selectedIndex",selected)) return {};
            if (selected != command.target.index) {
                const Value index{s,s.number(command.target.index)};
                if (!index || !exactModelStillCurrent()) return {};
                return invoke(s,live.list(),"__set__selectedIndex",{index});
            }
        }
        if (command.target.eventId == kTouchSettingsOpen || command.target.eventId == kTouchSettingsBack) {
            RequestBl2TouchSettingsUi(command.target.eventId == kTouchSettingsOpen ? 1u : 2u);
            return {WidgetDeliveryState::Dispatched,"touch-settings-page-requested",0};
        }
        if (command.target.eventId == kTouchSettingsGraphicsOpen || command.target.eventId == kTouchSettingsNativeBack) {
            RequestBl2TouchSettingsUi(command.target.eventId == kTouchSettingsGraphicsOpen ? 32u : 16u);
            return {WidgetDeliveryState::Dispatched,"touch-settings-page-requested",0};
        }
        if (command.target.eventId == kTouchSettingsEdit || command.target.eventId == kTouchSettingsReset ||
            command.target.eventId == kTouchSettingsCalibrate) {
            CommandBl2TouchSettings(static_cast<std::uint32_t>(command.target.eventId - kTouchSettingsBack));
            if (command.target.eventId == kTouchSettingsEdit) RequestBl2TouchSettingsUi(2u
                | 8u
            );
            return {WidgetDeliveryState::Dispatched,"touch-settings-command-requested",0};
        }
        if (command.target.kind == WidgetKind::Slider || command.target.kind == WidgetKind::Spinner) {
            double selected = -1;
            if (!number(s,live.list(),"selectedIndex",selected)) return {};
            if (selected != command.target.index) {
                const Value index{s,s.number(command.target.index)};
                if (!index || !exactModelStillCurrent()) return {};
                return invoke(s,live.list(),"__set__selectedIndex",{index});
            }
            auto next = command;
            if (command.target.kind == WidgetKind::Spinner) {
                double current = -1;
                if (!number(s,live.rowData(),"currentValue",current) || std::floor(current) != current ||
                    current < 0 || current > command.target.maximum) return {};
                next.kind = WidgetCommandKind::Spinner;
                next.spinnerIndex = current == command.target.maximum ? 0 : static_cast<std::int32_t>(current) + 1;
            } else {
                double current = 0;
                if (!(setting == 17 ? number(s,live.rowData(),"currentValue",current) : number(s,live.slider(),"value",current)) ||
                    current < command.target.minimum || current > command.target.maximum) return {};
                const auto value = current >= command.target.maximum ? command.target.minimum :
                    setting == 17 ? StepBl2InterfacePercent(current,true) :
                    std::min(current + command.target.increment,command.target.maximum);
                next.kind = WidgetCommandKind::Slider;
                next.sliderFraction = (value - command.target.minimum) / (command.target.maximum - command.target.minimum);
            }
            return ApplyBl2WidgetCommand(s,retained,model,widgets,next,controllerId,exactModelStillCurrent);
        }
        if (command.target.kind != WidgetKind::Button) return {};
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
    if(command.kind==WidgetCommandKind::Spinner) {
        const auto& target=command.target;
        double current=-1;
        if(target.kind!=WidgetKind::Spinner || command.spinnerIndex<0 || command.spinnerIndex>target.maximum ||
            !number(s,live.rowData(),"currentValue",current) || std::floor(current)!=current || current<0 || current>target.maximum) return {};
        if(current==command.spinnerIndex) return {WidgetDeliveryState::Unchanged,"spinner-unchanged",current};
        const Value value{s,s.number(command.spinnerIndex)};
        if(!value || !s.hasCallBudget(2) || !exactModelStillCurrent()) return {};
        if(!s.setMember(live.rowData(),"currentValue",value)) return {WidgetDeliveryState::Failed,"spinner-index-set-failed",current};
        auto result=invoke(s,live.inner(),"onSpinnerChange",{});
        if (result.state == WidgetDeliveryState::Dispatched && setting >= 0 && setting < static_cast<int>(kTouchSettingCount) &&
            !SetBl2TouchSetting(static_cast<std::size_t>(setting),command.spinnerIndex))
            return {WidgetDeliveryState::Failed,"touch-setting-range",0};
        result.actualValue=command.spinnerIndex;return result;
    }
    if (command.kind != WidgetCommandKind::Slider || command.target.kind != WidgetKind::Slider ||
        !std::isfinite(command.sliderFraction) || command.sliderFraction < 0 || command.sliderFraction > 1) return {};
    const auto& target = command.target;
    double min = 0,max = 0,increment = 0;
    if (!number(s,live.slider(),"minimum",min) || !number(s,live.slider(),"maximum",max) ||
        !number(s,live.slider(),"snapInterval",increment) || min != target.minimum || max != target.maximum ||
        increment != target.increment || max <= min || increment <= 0 || !exactModelStillCurrent()) return {};
    const Value value{s,s.number(min + command.sliderFraction * (max - min))};
    if (!value || !s.hasCallBudget(4)) return {WidgetDeliveryState::Rejected,"slider-command-call-budget",0};
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
    if (delivery.state == WidgetDeliveryState::Dispatched && setting >= 0 && setting < static_cast<int>(kTouchSettingCount) &&
        !SetBl2TouchSetting(static_cast<std::size_t>(setting),actual))
        return {WidgetDeliveryState::Failed,"touch-setting-range",actual};
    if (delivery.state == WidgetDeliveryState::Dispatched && setting >= 0 && setting < static_cast<int>(kTouchSettingCount)) {
        FormatBl2TouchSettingValue(s,live.inner(),static_cast<std::size_t>(setting),actual);
    }
    delivery.actualValue = actual;
    return delivery;
}
}
