#include "bl2_touch_ui_sink.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_ui_model.h"
#include "bl2_touch_settings.h"
#include "bl2_touch_gfx_transport.h"
#include "bl2_touch_widget_registry.h"
#include "bl2_touch_widget_commands.h"
#include "bl2_touch_text_chat.h"
#include "bl2_locale.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include "thunk_manager.h"
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace bl2::touch {
std::string CaptureBl2StatusInventoryGate(const UiModelMemory&,std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
namespace {
constexpr std::uint32_t kMovieKey = 0x01CC5E7C;
struct Key { Action action; std::uint32_t rva; const char* name; };
constexpr Key keys[] = {
    {Action::Back,0x03009FB8,"Escape"}, {Action::Confirm,0x03009FA0,"Enter"},
    {Action::UiUp,0x03009FF0,"Up"}, {Action::UiDown,0x0300A000,"Down"},
    {Action::UiLeft,0x03009FE8,"Left"}, {Action::UiRight,0x03009FF8,"Right"}
};
struct Target {
    std::uint32_t viewport = 0, controller = 0, controllerId = 0, world = 0;
    std::uint32_t gfx = 0, movie = 0, owner = 0, interface = 0, handler = 0;
    std::uint32_t width = 0, height = 0, capabilities = 0;
    std::uint64_t controllerToken = 0, ownerToken = 0, generation = 0, scene = 0;
    bool network = false;
    UiDirectModel direct;
};
std::uint32_t floatWord(float value) {
    std::uint32_t result = 0; std::memcpy(&result, &value, sizeof(result)); return result;
}
// Preserve InputKey's filter/delegate phases and its single MouseUp. Only the
// accepted touch release supplies coordinates in that original stack event.
struct UiNativeReleaseScope {
    std::uint32_t owner=0,movie=0,gfx=0,interface=0,controllerId=0,index=0,number=0,x=0,y=0;
    std::function<bool()> current;
    unsigned delivered=0,rejected=0;
};
thread_local UiNativeReleaseScope* uiNativeReleaseScope=nullptr;
struct UiNativeReleaseActivation {
    UiNativeReleaseScope* previous;
    explicit UiNativeReleaseActivation(UiNativeReleaseScope& scope):previous(uiNativeReleaseScope) {uiNativeReleaseScope=&scope;}
    ~UiNativeReleaseActivation() {uiNativeReleaseScope=previous;}
};
template<class Read,class Write>
bool UiAdaptNativeRelease(const std::array<std::uint32_t,16>& regs,Read read,Write write) {
    auto* scope=uiNativeReleaseScope;if(!scope) return true;
    // Nested original input for another movie keeps its own native event.
    if(regs[9]!=scope->movie || regs[7]!=scope->gfx) return true;
    const auto sp=regs[13];
    if(scope->delivered || scope->rejected || sp<0x10000 || (sp&7) || sp>UINT32_MAX-0x70 ||
        regs[6]!=scope->owner || regs[9]!=scope->movie || regs[7]!=scope->gfx || regs[5]!=scope->controllerId ||
        regs[4]!=scope->index || regs[0]!=scope->interface || regs[2]!=scope->interface || regs[1]!=sp+0x1C ||
        read(sp+0x64)!=scope->index || read(sp+0x68)!=scope->number || read(sp+0x6C)!=1 ||
        read(sp+0x1C)!=3 || read(sp+0x30)!=0 || !scope->current || !scope->current()) {
        ++scope->rejected;return false;
    }
    write(sp+0x24,scope->x);write(sp+0x28,scope->y);++scope->delivered;return true;
}
std::uint32_t uiNativeReleaseBase=0;
bool InstallUiNativeRelease(DynarmicHost& host,std::uint32_t base) {
    if(uiNativeReleaseBase) return uiNativeReleaseBase==base;
    const auto& elf=host.GetElfLoader();
    if(elf.GetImageSize()!=0x03089000 || std::uint64_t(base)+elf.GetImageSize()>UINT32_MAX ||
        host.FindSymbol("_ZN10FGFxEngine8InputKeyEiP9FGFxMovie5FName11EInputEvent")!=base+kMovieKey ||
        host.FindSymbol("KEY_LeftMouseButton")!=base+0x03009F68 ||
        !elf.IsReadOnlyRange(base+0x01CC6348,12) || !elf.IsCodeRange(base+0x01CC6348,12) ||
        host.MemoryRead32(base+0x01CC6348)!=0xE5923000 || host.MemoryRead32(base+0x01CC634C)!=0xE5933090 ||
        host.MemoryRead32(base+0x01CC6350)!=0xE12FFF33) return false;
    const std::array<std::uint32_t,4> words{0xE5923000,0xE5933090,0xE51FF004,base+0x01CC6350};
#if defined(BL2_NATIVE_ARM32)
    const auto replay=host.CreateCodeVeneer(words);if(!replay) return false;
#else
    const auto replay=host.GuestAlloc(sizeof(words),4);if(!replay) return false;
    for(std::size_t i=0;i<words.size();++i) host.MemoryWrite32(replay+static_cast<std::uint32_t>(i*4),words[i]);
    host.AddExecutableRange(replay,sizeof(words));host.InvalidateGuestCode(replay,sizeof(words));
#endif
    const auto thunk=host.GetThunkManager().RegisterThunk("bl2_ui_owned_mouse_release",
        [replay,base](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
            const bool deliver=UiAdaptNativeRelease(jit.Regs(),[&](std::uint32_t p) {return current.MemoryRead32(p);},
                [&](std::uint32_t p,std::uint32_t value) {current.MemoryWrite32(p,value);});
            jit.Regs()[15]=deliver?replay:base+0x01CC6354;
        });
    if(!thunk) return false;
    host.MemoryWrite32(base+0x01CC6348,0xE51FF004);host.MemoryWrite32(base+0x01CC634C,thunk);
    host.InvalidateGuestCode(base+0x01CC6348,8);uiNativeReleaseBase=base;return true;
}
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
// Only the already-running, exact-movie callback owns this stack observation.
// No UObject lookup or guest callback runs from either native observer.
struct UiKeyReturnScope {
    std::uint32_t owner=0,movie=0,controllerId=0,index=0,number=0,event=0;
    std::array<std::uint32_t,2> visits{},values{},rejected{};
};
thread_local UiKeyReturnScope* uiKeyReturnScope=nullptr;
struct UiKeyReturnActivation {
    UiKeyReturnScope* previous;
    explicit UiKeyReturnActivation(UiKeyReturnScope* scope):previous(uiKeyReturnScope) {uiKeyReturnScope=scope;}
    ~UiKeyReturnActivation() {uiKeyReturnScope=previous;}
};
template<class Read>
void UiObserveKeyReturn(unsigned phase,const std::array<std::uint32_t,16>& regs,Read read) {
    auto* scope=uiKeyReturnScope;
    if(!scope || phase>1) return;
    const auto sp=regs[13];
    if(!scope->owner || sp<0x10000 || (sp&7) || sp>UINT32_MAX-0x30 ||
        (phase==0 && (regs[6]!=scope->owner || regs[9]!=scope->movie || regs[5]!=scope->controllerId || regs[4]!=scope->index)) ||
        (phase==1 && (regs[4]!=scope->owner || regs[9]!=scope->movie || !scope->visits[0] || scope->values[0]))) {
        ++scope->rejected[phase];return;
    }
    const auto args=sp+(phase==0?0x1Cu:0x10u);
    if(read(args)!=scope->controllerId || read(args+4)!=scope->index || read(args+8)!=scope->number ||
        (read(args+12)&255)!=scope->event) {++scope->rejected[phase];return;}
    ++scope->visits[phase];scope->values[phase]=read(args+16);
}
std::uint32_t uiKeyReturnProbeBase=0;
bool InstallUiKeyReturnProbe(DynarmicHost& host,std::uint32_t base) {
    if(uiKeyReturnProbeBase) return uiKeyReturnProbeBase==base;
    // This constructor is reached only from SHA-admitted InstallBl2TouchRuntime,
    // before guest JNI_OnLoad/workers. Relocated memory is not the immutable file.
    const auto& elf=host.GetElfLoader();
    if(elf.GetImageSize()!=0x03089000 || std::uint64_t(base)+elf.GetImageSize()>UINT32_MAX ||
        host.FindSymbol("_ZN10FGFxEngine8InputKeyEiP9FGFxMovie5FName11EInputEvent")!=base+0x01CC5E7C ||
        host.FindSymbol("_ZN15UGFxMoviePlayer8InputKeyEi5FNameh")!=base+0x01CB7410) return false;
    constexpr std::array<std::uint32_t,2> sites{0x01CC5FE0,0x01CB74E0};
    constexpr std::array<std::array<std::uint32_t,2>,2> originals{{{0xE59DA02C,0xE35A0000},{0xE59D0020,0xE2900000}}};
    for(unsigned i=0;i<sites.size();++i)
        if(!elf.IsReadOnlyRange(base+sites[i],12) || !elf.IsCodeRange(base+sites[i],12) ||
            host.MemoryRead32(base+sites[i])!=originals[i][0] || host.MemoryRead32(base+sites[i]+4)!=originals[i][1] ||
            host.MemoryRead32(base+sites[i]+8)!=(i==0?0x1AFFFFC6u:0x13A00001u)) return false;
    std::array<std::uint32_t,2> thunks{};
    for(unsigned i=0;i<sites.size();++i) {
        const std::array<std::uint32_t,4> words{originals[i][0],originals[i][1],0xE51FF004,base+sites[i]+8};
        const auto replay=host.CreateCodeVeneer(words);if(!replay) return false;
        thunks[i]=host.GetThunkManager().RegisterThunk(i==0?"bl2_ui_actual_filter_return":"bl2_ui_actual_delegate_return",
            [i,replay](Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                UiObserveKeyReturn(i,jit.Regs(),[&](std::uint32_t p) {return current.MemoryRead32(p);});
                jit.Regs()[15]=replay;
            });
        if(!thunks[i]) return false;
    }
    // Both original pairs and both replays are ready before either site changes.
    for(unsigned i=0;i<sites.size();++i) {
        host.MemoryWrite32(base+sites[i],0xE51FF004);host.MemoryWrite32(base+sites[i]+4,thunks[i]);
        host.InvalidateGuestCode(base+sites[i],8);
    }
    uiKeyReturnProbeBase=base;return true;
}
#endif
} // namespace

struct Ue3UiSink::State {
    DynarmicHost& host;
    LifetimeGeneration generation;
    const std::uint32_t base;
    const bool tapTest;
    bool signatures = false, preparedValid = false, frameValid = false;
    unsigned logs = 0;
    std::string lastAdmission;
    std::uint32_t scratch = 0;
    std::uint64_t epoch = 0;
    std::uint64_t titleToken = 0, titleReadyAt = 0;
    unsigned titleAttempts = 0;
    unsigned networkLogs = 0;
    std::array<unsigned,4> mapLogs{};
    bool fpsBound = false, fpsPreferenceKnown = false, fpsPreference = false;
    Target prepared{}, frameTarget{};
    UiTransactions transactions;
    UiMapGesture mapGesture;
    UiMapDeferredPoint queuedWaypoint;
    Target queuedWaypointOwner;
    UiMapGesture itemGesture;
    UiInventoryGesture inventoryGesture;
    std::unique_ptr<GfxHostTransport> inventoryTransport,inventoryProbe;
    std::unique_ptr<GfxSession> inventorySession;
    InventoryWidgetTarget inventoryTarget;
    InventoryDragSource inventoryFresh{};
    GfxIdentity inventoryDragData{};
    UiStatusInventoryModel inventoryStatus;
    std::array<GfxIdentity,3> inventoryStatusPanels{};
    Target inventoryOwner{};
    std::optional<InventoryDragUpdate> inventoryStart,inventoryFinal;
    std::uint64_t inventoryPressedAt=0;
    bool inventoryPressed=false,inventoryCleanup=false,inventoryHoverBoundary=false;
    bool inventoryReleaseReady=false;
    bool inventoryDropQueued=false;
    bool inventorySourcePrimed=false,inventoryPrimePending=false;
    UiTextChatKeyboard chatKeyboard;
    std::unique_ptr<GfxHostTransport> chatService,chatGeometry;
    TextChatOwner chatOwner{};
    std::uint64_t chatObject=0;
    std::uint64_t chatAttemptAt=0;
    unsigned chatAttempts=0;
    std::unique_ptr<GfxHostTransport> geometry;
    std::unique_ptr<GfxHostTransport> characterTransport;
    std::unique_ptr<GfxSession> characterSession;
    CharacterWidgetTarget characterTarget;
    std::unique_ptr<GfxHostTransport> ageGateTransport;
    std::unique_ptr<GfxSession> ageGateSession;
    AgeGateWidgetTarget ageGateTarget;
    UiAgeGateModel ageGateModel;
    Target ageGateOwner{};
    bool ageGateGesture = false;
    std::unique_ptr<GfxHostTransport> footerTransport;
    std::unique_ptr<GfxSession> footerSession;
    MapLegendTarget footerTarget;
    std::function<bool(const ActionEdge&)> footerAction;
    std::uint64_t footerScaleOwner=0,footerScaleGeneration=0,footerScaleAt=0;
    std::int32_t footerScaleScreen=-1;
    NativeFooterFamily footerScaleFamily=NativeFooterFamily::Generic;
    float footerScale=0;
    unsigned footerScaleAttempts=0;
    bool footerScaleReady=false;
    bool characterWasSelected = false, mapDragHeld = false, itemDragHeld = false;
    // A drag on an overflowing original description scrolls its text by
    // lines; a tap or a swipe elsewhere keeps the ordinary transaction.
    struct TextScroll {
        TextScrollCapture capture;
        std::int32_t pointer=-1,line=1;
        float startX=0,startY=0;
        bool active=false;
    } textScroll;
    unsigned textScrollLogs=0;
    std::int32_t dialogAtDown = -1;
    std::uint32_t mouseName = 0;
    std::uint64_t characterProbeOwner = 0;
    unsigned characterProbes = 0, characterInputs = 0;
    unsigned characterCaptures = 0;
    std::array<unsigned,2> characterRoutes{};
    unsigned footerCaptures = 0,footerRoutes = 0,keyRejects = 0;
    unsigned chatDiagnostics = 0;
    std::uint64_t qaTrainingOwner = 0;
    std::string lastChatDiagnostic;
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
    unsigned inventoryDiagnosticGestures = 0;
    unsigned inventoryDiagnostics = 0;
    std::array<std::uint64_t,32> inventoryDiagnosticKeys{};
    unsigned inventoryTerminalDiagnostics = 0;
    std::array<std::uint64_t,8> inventoryTerminalDiagnosticKeys{};
    unsigned inventoryInputDiagnostics = 0;
    std::array<std::uint64_t,12> inventoryInputDiagnosticKeys{};
    unsigned inventoryGateDiagnostics = 0;
    unsigned keyReturnDiagnostics = 0;
    std::array<unsigned,2> inventoryDropDiagnostics{};
    std::array<std::array<std::uint64_t,4>,2> inventoryDropDiagnosticKeys{};
#endif

    bool inventoryDiagnosticEnabled() const {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(frameTarget.direct.movieClass!="StatusMenuExGFxMovie" ||
            (inventoryDiagnostics>=inventoryDiagnosticKeys.size() && inventoryTerminalDiagnostics>=inventoryTerminalDiagnosticKeys.size())) return false;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        return property[0]=='1' && !property[1];
#else
        return false;
#endif
    }
    void inventoryDiagnosticGesture() {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(frameTarget.direct.movieClass!="StatusMenuExGFxMovie" || inventoryDiagnosticGestures>=3) return;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if(property[0]!='1' || property[1]) return;
        if(inventoryDiagnosticGestures++) inventoryDiagnostics=inventoryTerminalDiagnostics=0;
#endif
    }
    void inventoryDiagnostic(const char* stage,const char* reason,const GfxSession* session=nullptr,
        const InventoryWidgetObservation* capture=nullptr,const InventoryDragObservation* manager=nullptr,
        const UiStatusInventoryModel* model=nullptr,float x=-1,float y=-1,int result=-1,
        const GfxIdentity* hovered=nullptr,std::uint32_t hoveredType=0) {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(!inventoryDiagnosticEnabled()) return;
        const bool terminal=inventoryDiagnostics>=inventoryDiagnosticKeys.size() &&
            ((std::strcmp(stage,"fresh-source")==0 && manager && manager->started) ||
             std::strcmp(stage,"finish")==0 || std::strcmp(stage,"finish-manager")==0);
        auto& count=terminal?inventoryTerminalDiagnostics:inventoryDiagnostics;
        auto* keys=terminal?inventoryTerminalDiagnosticKeys.data():inventoryDiagnosticKeys.data();
        const auto limit=terminal?inventoryTerminalDiagnosticKeys.size():inventoryDiagnosticKeys.size();
        if(count>=limit) return;
        const InventoryWidgetObservation emptyCapture{};const InventoryDragObservation emptyManager{};
        const UiStatusInventoryModel emptyModel{};
        const GfxIdentity emptyHover{};const auto& h=hovered?*hovered:emptyHover;
        const auto& c=capture?*capture:emptyCapture;const auto& m=manager?*manager:emptyManager;
        const auto& native=model?*model:emptyModel;
        auto hash=1469598103934665603ull;
        const auto word=[&](std::uint64_t value) {for(unsigned i=0;i<8;++i) {hash^=value&255;hash*=1099511628211ull;value>>=8;}};
        const auto text=[&](const char* value) {while(*value) {hash^=static_cast<unsigned char>(*value++);hash*=1099511628211ull;}};
        text(stage);text(reason);text(native.reason.c_str());text(c.panel);
        word(frameTarget.ownerToken);word(frameTarget.capabilities);word(frameTarget.direct.screen);word(result);
        word(preparedValid);word(frameValid);word(frameTarget.direct.valid);word(inventoryPressed);word(inventoryGesture.captured());
        word(inventoryFinal.has_value());word(c.step);word(c.index);word(c.hitMask);word(c.selectableMask);
        for(const auto type:c.types) word(type);
        for(const auto type:c.managerTypes) word(type);
        word(c.topId.data);word(c.panelId.data);word(c.cellId.data);
        word(native.valid);word(native.top);word(native.backpack);word(native.equipped);
        word(m.known);word(m.active);word(m.started);word(m.inDragType);word(m.relatedType);
        word(m.manager.data);word(m.origin.data);word(m.payload.data);
        word(hoveredType);word(h.interface);word(h.data);
        word(inventoryTarget.source.cell.type);word(inventoryTarget.source.cell.interface);word(inventoryTarget.source.cell.data);
        word(inventoryTarget.rendererId.type);word(inventoryTarget.rendererId.interface);word(inventoryTarget.rendererId.data);
        for(unsigned i=0;i<count;++i) if(keys[i]==hash) return;
        keys[count++]=hash;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "inventory stage=%s reason=%s result=%d owner=%08X movie=%08X screen=%d caps=%u "
            "epoch=%llu scene=%llu generation=%llu prepared=%u frame=%u direct=%u family=%u "
            "captured=%u pressed=%u final=%u session=%s calls=%u point=(%.6f,%.6f) "
            "native=(%u,%s,%08X,%08X,%08X) getters=(%08X,%04X,%u,%08X,%04X,%u) "
            "capture=(%s,%u,%d,%u,%X,%X) types=(%u,%u,%u,%u,%u,%u) managerTypes=(%u,%u,%u,%u) "
            "stagePoint=(%.2f,%.2f) clips=(%08X,%08X,%08X) manager=(%u,%u,%u,%u,%u,%08X,%08X,%08X) "
            "hover=(%u,%08X,%08X) expectedCell=(%u,%08X,%08X) renderer=(%u,%08X,%08X)",
            stage,reason,result,frameTarget.owner,frameTarget.movie,frameTarget.direct.screen,frameTarget.capabilities,
            static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(frameTarget.scene),
            static_cast<unsigned long long>(frameTarget.generation),preparedValid?1u:0u,frameValid?1u:0u,
            frameTarget.direct.valid?1u:0u,static_cast<unsigned>(inventoryFamily()),inventoryGesture.captured()?1u:0u,
            inventoryPressed?1u:0u,inventoryFinal?1u:0u,session?(session->reason()?session->reason():"okay"):"none",session?session->calls():0,
            x,y,native.valid?1u:0u,native.reason.c_str(),static_cast<std::uint32_t>(native.top>>32),
            static_cast<std::uint32_t>(native.backpack>>32),static_cast<std::uint32_t>(native.equipped>>32),
            native.backpackInventory.function,native.backpackInventory.flags,native.backpackInventory.bytes,
            native.equippedInventory.function,native.equippedInventory.flags,native.equippedInventory.bytes,
            c.panel,c.step,c.index,c.tested,c.hitMask,c.selectableMask,c.types[0],c.types[1],c.types[2],c.types[3],c.types[4],c.types[5],
            c.managerTypes[0],c.managerTypes[1],c.managerTypes[2],c.managerTypes[3],c.stageX,c.stageY,c.topId.data,c.panelId.data,c.cellId.data,
            m.known?1u:0u,m.active?1u:0u,m.started?1u:0u,m.inDragType,m.relatedType,m.manager.data,m.origin.data,m.payload.data,
            hoveredType,h.interface,h.data,inventoryTarget.source.cell.type,inventoryTarget.source.cell.interface,inventoryTarget.source.cell.data,
            inventoryTarget.rendererId.type,inventoryTarget.rendererId.interface,inventoryTarget.rendererId.data);
#else
        (void)stage;(void)reason;(void)session;(void)capture;(void)manager;(void)model;(void)x;(void)y;(void)result;
        (void)hovered;(void)hoveredType;
#endif
    }
    bool inventoryDropDiagnosticEnabled() const {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(inventoryDropDiagnostics[0]>=4 && inventoryDropDiagnostics[1]>=4) return false;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        return property[0]=='1' && !property[1];
#else
        return false;
#endif
    }
    void inventoryDropDiagnostic(const char* stage,const InventoryDragObservation& observed) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        const unsigned phase=std::strcmp(stage,"own-ack")==0?0u:std::strcmp(stage,"pre-up")==0?1u:2u;
        if(phase>1 || inventoryDropDiagnostics[phase]>=4 || !observed.started ||
            inventoryOwner.direct.movieClass!="StatusMenuExGFxMovie" || !inventoryDropDiagnosticEnabled()) return;
        std::uint64_t key=1469598103934665603ull;
        const auto hashWord=[&](std::uint64_t value) {for(unsigned i=0;i<8;++i){key^=value&255;key*=1099511628211ull;value>>=8;}};
        hashWord(inventoryOwner.ownerToken);hashWord(inventoryOwner.scene);hashWord(inventoryOwner.generation);hashWord(epoch);
        for(const auto& id:{observed.manager,observed.origin,observed.payload}) {hashWord(id.interface);hashWord(id.data);hashWord(id.extra);hashWord(id.type);}
        auto& count=inventoryDropDiagnostics[phase];auto& keys=inventoryDropDiagnosticKeys[phase];
        if(std::find(keys.begin(),keys.begin()+count,key)!=keys.begin()+count) return;
        keys[count++]=key; // Failed reads are bounded too; ownACK cannot spend preUP's reserve.
        const auto owner=inventoryOwner;
        GfxHostTransport transport(host,[this,owner] {
            return current() && frameTarget.ownerToken==owner.ownerToken && frameTarget.interface==owner.interface &&
                frameTarget.scene==owner.scene && frameTarget.generation==owner.generation;
        });
        GfxSession observer(transport,base,owner.interface,transport.scratch(),32,false);
        GfxIdentity managerId{},targetId{},parentId{};std::uint32_t targetType=0xFFFFFFFFu,parentType=0xFFFFFFFFu;
        std::string name;const char* reason="manager-current";const char* relation="unknown";bool known=false;
        const auto cls=observer.variable("_global.gfx.managers.DragManager"),manager=observer.member(cls,"_instance");
        const auto owns=[&] {
            const auto inDrag=observer.member(manager,"_inDrag"),origin=observer.member(manager,"pressOrigin"),
                related=observer.member(manager,"relatedObject"),payload=observer.member(manager,"_data");
            bool active=false;GfxIdentity actualOrigin{},actualPayload{};
            return observer.identity(manager,managerId) && managerId==inventoryTarget.managerId && managerId==observed.manager &&
                observer.boolean(inDrag,active) && active && observer.type(related)==1 &&
                observer.identity(origin,actualOrigin) && actualOrigin==inventoryTarget.source.panel && actualOrigin==observed.origin &&
                observer.identity(payload,actualPayload) && actualPayload==observed.payload && observer.good();
        };
        if(owns()) {
            const auto drop=observer.member(manager,"dropTarget");targetType=observer.type(drop);
            if(targetType==1) {known=true;reason="drop-null";relation="null";}
            else if(targetType==10 && observer.identity(drop,targetId)) {
                const auto parent=observer.member(drop,"_parent"),label=observer.member(drop,"_name");parentType=observer.type(parent);
                known=observer.identity(parent,parentId) && observer.string(label,name,64);
                reason=known?"drop-target-owned":"drop-target-read";
                if(known) relation=parentId==inventoryStatusPanels[2]?"equipped-cell":
                    targetId==inventoryStatusPanels[1]?"storage-panel":parentId==inventoryStatusPanels[1]?"storage-child":"other";
            } else reason="drop-target-type";
            const auto again=observer.member(manager,"dropTarget");GfxIdentity repeated{};
            const bool same=targetType==10?observer.identity(again,repeated) && repeated==targetId:observer.type(again)==targetType;
            if(!same || !owns()) {known=false;reason="drop-target-changed";}
        }
        if(!observer.good()) {known=false;reason=observer.reason()?observer.reason():"observer-unavailable";}
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "inventory-drop stage=%s known=%u reason=%s owner=%08X movie=%08X epoch=%llu generation=%llu "
            "manager=(%08X,%08X,%08X) target=(%u,%08X,%08X) parent=(%u,%08X,%08X) name=%s relation=%s calls=%u boundary=before-native-up",
            stage,known?1u:0u,reason,owner.owner,owner.movie,static_cast<unsigned long long>(epoch),
            static_cast<unsigned long long>(owner.generation),observed.manager.data,observed.origin.data,observed.payload.data,
            targetType,targetId.interface,targetId.data,parentType,parentId.interface,parentId.data,name.c_str(),relation,observer.calls());
#else
        (void)stage;(void)observed;
#endif
    }
    void inventoryInputDiagnostic(const char* stage,const Target& target) {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(target.direct.movieClass!="StatusMenuExGFxMovie" || target.direct.screen!=3 ||
            inventoryInputDiagnostics>=inventoryInputDiagnosticKeys.size()) return;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if(property[0]!='1' || property[1]) return;
        const auto state=CaptureBl2StatusInventoryInputState({this,copyMemory},base,target.owner);
        bool panelKnown=false,panelDownKnown=false,panelDown=false,cellDownKnown=false,cellDown=false;
        bool mouseKnown=false;double mouseX=0,mouseY=0;
        std::uint32_t hoverType=0,pressType=0;GfxIdentity hover{};
        if(inventoryTarget && inventoryStatus.valid && current() && target.ownerToken==inventoryOwner.ownerToken) {
            const auto model=CaptureBl2StatusInventory({this,copyMemory},base,target.owner);
            const bool equipped=inventoryTarget.source.panel==inventoryStatusPanels[2];
            if(model.valid && model.owner==inventoryStatus.owner && model.top==inventoryStatus.top &&
                model.backpack==inventoryStatus.backpack && model.equipped==inventoryStatus.equipped) {
                GfxHostTransport transport(host,[this,target] {return current() && frameTarget.ownerToken==target.ownerToken;});
                GfxSession observer(transport,base,target.interface,transport.scratch(),24,false);
                const auto panel=observer.pin(static_cast<std::uint32_t>((equipped?model.equipped:model.backpack)>>32)+0x3C);
                GfxIdentity actual{};panelKnown=observer.identity(panel,actual) && actual==inventoryTarget.source.panel;
                if(panelKnown) {
                    const auto down=observer.member(panel,"bMouseDown"),over=observer.member(panel,"currentOverClip");
                    panelDownKnown=observer.boolean(down,panelDown);hoverType=observer.type(over);observer.identity(over,hover);
                    const auto cell=equipped?observer.member(panel,inventoryTarget.label.c_str()):over;
                    if(observer.identity(cell,actual) && actual==inventoryTarget.source.cell) {
                        const auto downCell=observer.member(cell,"bMouseDown"),press=observer.member(cell,"onPress");
                        cellDownKnown=observer.boolean(downCell,cellDown);pressType=observer.type(press);
                    }
                    const auto root=observer.variable("_root"),x=observer.member(root,"_xmouse"),y=observer.member(root,"_ymouse");
                    mouseKnown=observer.number(x,mouseX) && observer.number(y,mouseY) && std::isfinite(mouseX) && std::isfinite(mouseY);
                }
            }
        }
        auto hash=1469598103934665603ull;
        const auto word=[&](std::uint64_t value) {for(unsigned i=0;i<8;++i) {hash^=value&255;hash*=1099511628211ull;value>>=8;}};
        const auto text=[&](const char* value) {while(*value) {hash^=static_cast<unsigned char>(*value++);hash*=1099511628211ull;}};
        text(stage);text(state.reason.c_str());text(state.delegateName.c_str());text(state.queuedName.c_str());text(state.triggerName.c_str());
        text(state.movieReason.c_str());text(state.movieDelegateName.c_str());text(state.movieDelegateClass.c_str());
        word(state.valid);word(state.owner);word(state.top);word(state.delegateObject);word(state.queuedObject);
        word(state.doingEquip);word(state.setupFinished);
        word(state.movieValid);word(state.movieDelegateObject);word(state.localPlayerOwnerIndex);
        word(state.captureInput);word(state.discardNonOwnerInput);
        word(panelKnown);word(panelDownKnown);word(panelDown);word(cellDownKnown);word(cellDown);
        word(hoverType);word(hover.data);word(pressType);
        word(mouseKnown);word(floatWord(static_cast<float>(mouseX)));word(floatWord(static_cast<float>(mouseY)));
        for(unsigned i=0;i<inventoryInputDiagnostics;++i) if(inventoryInputDiagnosticKeys[i]==hash) return;
        inventoryInputDiagnosticKeys[inventoryInputDiagnostics++]=hash;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "inventory-input stage=%s known=%u reason=%s owner=%08X top=%08X epoch=%llu generation=%llu "
            "delegate=(%08X,%s) queued=(%08X,%s) trigger=%s equip=%u setup=%u "
            "movieInput=(%u,%s) movieDelegate=(%08X,%s,%s) localOwner=%d capture=%u discard=%u "
            "sourceIndex=%d panel=(%u,%u,%u) cell=(%u,%u,%u) hover=(%u,%08X) mouse=(%u,%.2f,%.2f)",
            stage,state.valid && state.owner==target.ownerToken?1u:0u,state.reason.c_str(),target.owner,
            static_cast<std::uint32_t>(state.top>>32),static_cast<unsigned long long>(epoch),
            static_cast<unsigned long long>(target.generation),static_cast<std::uint32_t>(state.delegateObject>>32),
            state.delegateName.c_str(),static_cast<std::uint32_t>(state.queuedObject>>32),state.queuedName.c_str(),
            state.triggerName.c_str(),state.doingEquip?1u:0u,state.setupFinished?1u:0u,state.movieValid?1u:0u,
            state.movieReason.c_str(),static_cast<std::uint32_t>(state.movieDelegateObject>>32),
            state.movieDelegateName.c_str(),state.movieDelegateClass.c_str(),state.localPlayerOwnerIndex,
            state.captureInput?1u:0u,state.discardNonOwnerInput?1u:0u,inventoryTarget.index,
            panelKnown?1u:0u,panelDownKnown?1u:0u,panelDown?1u:0u,cellDownKnown?1u:0u,cellDown?1u:0u,pressType,hoverType,hover.data,
            mouseKnown?1u:0u,mouseX,mouseY);
#else
        (void)stage;(void)target;
#endif
    }

    std::string inventoryGateObservation(const Target& target) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(target.direct.movieClass!="StatusMenuExGFxMovie" || target.direct.screen!=3 || inventoryGateDiagnostics>=8) return {};
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if(property[0]!='1' || property[1]) return {};
        ++inventoryGateDiagnostics;
        return CaptureBl2StatusInventoryGate({this,copyMemory},base,target.owner,target.controller,target.controllerId);
#else
        (void)target;return {};
#endif
    }
    void inventoryGateDiagnostic(const Target& target,std::uint32_t index,std::uint32_t number,
        std::uint32_t event,std::uint32_t handled,const std::string& before) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(before.empty()) return;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "inventory-key-gate owner=%08X controller=%08X sentControllerId=%u fname=(%u,%u) event=%u handled=%u epoch=%llu generation=%llu before={%s}",
            target.owner,target.controller,target.controllerId,index,number,event,handled,
            static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(target.generation),before.c_str());
#else
        (void)target;(void)index;(void)number;(void)event;(void)handled;(void)before;
#endif
    }

    bool footerDiagnosticEnabled() const {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(footerCaptures>=12) return false;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        return property[0]=='1' && !property[1];
#else
        return false;
#endif
    }
    void footerCaptureDiagnostic(const char* event,const MapLegendObservation& o,const GfxSession& session,float x,float y) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        ++footerCaptures;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "legend capture=%s owner=%08X screen=%d reason=%s session=%s calls=%u types=(%u,%u,%u,%u) "
            "caption=%s units=(%u,%u) measured=%u transform=(%.2f,%.2f,%.2f,%.2f,%.2f) "
            "char=(%.2f,%.2f,%.2f,%.2f) local=(%.2f,%.2f,%.2f,%.2f) projected=%u hit=%u "
            "bounds=(%.6f,%.6f,%.6f,%.6f) point=(%.6f,%.6f)",
            event,frameTarget.owner,frameTarget.direct.screen,o.reason,session.reason()?session.reason():"okay",session.calls(),
            o.rootType,o.parentType,o.textType,o.rectType,o.caption.c_str(),o.first,o.count,o.measured,
            o.x,o.y,o.scaleX,o.scaleY,o.rotation,o.character.left,o.character.top,o.character.right,o.character.bottom,
            o.local.left,o.local.top,o.local.right,o.local.bottom,o.projected?1u:0u,o.hit?1u:0u,
            o.bounds.left,o.bounds.top,o.bounds.right,o.bounds.bottom,x,y);
#else
        (void)event;(void)o;(void)session;(void)x;(void)y;
#endif
    }
    void footerRouteDiagnostic(const PointerEvent& event,bool current) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(event.phase!=PointerPhase::Down || frameTarget.direct.movieClass!="StatusMenuExGFxMovie" ||
            frameTarget.direct.screen!=2 || footerRoutes>=12) return;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if(property[0]!='1' || property[1]) return;
        ++footerRoutes;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "legend route=entry id=%d epoch=%llu currentEpoch=%llu screen=%d caps=%u current=%u mapHeld=%u footer=%u point=(%.6f,%.6f)",
            event.pointerId,static_cast<unsigned long long>(event.sourceEpoch),static_cast<unsigned long long>(epoch),
            frameTarget.direct.screen,frameTarget.capabilities,current?1u:0u,mapGesture.captured()?1u:0u,footerTarget?1u:0u,
            event.surfaceX,event.surfaceY);
#else
        (void)event;(void)current;
#endif
    }

    bool characterCaptureDiagnosticEnabled() const {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(frameTarget.direct.screen!=3 || characterCaptures>=12) return false;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        return property[0]=='1' && !property[1];
#else
        return false;
#endif
    }
    void characterCaptureDiagnostic(const char* event,const CharacterWidgetObservation& observed,
                                    const CharacterWidgetTarget& target,const GfxSession& session,float x,float y) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        ++characterCaptures;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "character capture=%s screen=%d player=%d owner=%08X clip=%08X reason=%s session=%s calls=%u "
            "types=(%u,%u,%u,%u,%u) count=%.0f rendered=%.0f scroll=%.0f committed=%u disabled=(%u,%u) "
            "point=(%.6f,%.6f) target=(%u,%d) rows=%u",
            event,frameTarget.direct.screen,frameTarget.direct.player,frameTarget.owner,
            static_cast<std::uint32_t>(frameTarget.direct.clip>>32),observed.reason,session.reason() ? session.reason() : "okay",session.calls(),
            observed.contentType,observed.panelType,observed.containerType,observed.providerType,observed.renderersType,
            observed.count,observed.rendered,observed.scroll,observed.committed?1u:0u,observed.disabledKnown?1u:0u,observed.disabled?1u:0u,
            x,y,static_cast<unsigned>(target.kind),target.index,observed.rowCount);
        for(unsigned i=0;i<observed.rowCount;++i) {
            const auto& row=observed.rows[i];
            __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                "character row=%u reason=%s types=(%u,%u,%u) shown=%u owner=%u disabled=(%u,%u) index=%.0f "
                "label=%s data=%s projected=%u normalized=%u hit=%u stage=(%.2f,%.2f,%.2f,%.2f) bounds=(%.6f,%.6f,%.6f,%.6f)",
                row.slot,row.reason,row.type,row.itemType,row.dataType,row.shown?1u:0u,row.owner?1u:0u,
                row.disabledKnown?1u:0u,row.disabled?1u:0u,row.index,row.label.c_str(),row.displayed.c_str(),
                row.projected?1u:0u,row.normalized?1u:0u,row.hit?1u:0u,
                row.stage.left,row.stage.top,row.stage.right,row.stage.bottom,
                row.bounds.left,row.bounds.top,row.bounds.right,row.bounds.bottom);
        }
#else
        (void)event;(void)observed;(void)target;(void)session;(void)x;(void)y;
#endif
    }
    void characterRouteDiagnostic(const PointerEvent& event,bool afterTransactions,bool current,bool owns,bool tap) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        const auto kind=afterTransactions?1u:0u;
        if(event.phase!=PointerPhase::Down || frameTarget.direct.screen!=3 ||
            frameTarget.direct.movieClass!="CharacterSelectionReduxGFxMovie" || characterRoutes[kind]>=12) return;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if(property[0]!='1' || property[1]) return;
        ++characterRoutes[kind];
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "character route=%s id=%d epoch=%llu currentEpoch=%llu scene=%llu screen=%d player=%d caps=%u "
            "prepared=%u frame=%u current=%u owns=%u tap=%u point=(%.6f,%.6f)",
            afterTransactions?"transactions":"entry",event.pointerId,static_cast<unsigned long long>(event.sourceEpoch),
            static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(frameTarget.scene),frameTarget.direct.screen,
            frameTarget.direct.player,frameTarget.capabilities,preparedValid?1u:0u,frameValid?1u:0u,current?1u:0u,
            owns?1u:0u,tap?1u:0u,event.surfaceX,event.surfaceY);
#else
        (void)event;(void)afterTransactions;(void)current;(void)owns;(void)tap;
#endif
    }

    void networkDiagnostic(const Target& target, const char* event, int direction = 0) {
#if defined(__ANDROID__)
        static const bool enabled = [] {
            char value[PROP_VALUE_MAX] = {};
            __system_property_get("debug.bl2bridge.touch_diagnostics",value);
            return value[0] == '1' && !value[1];
        }();
        if (enabled && target.network && networkLogs < 24) {
            ++networkLogs;
            __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                "network event=%s caps=%u movie=%08X owner=%08X viewport=(%u,%u) mouseable=%u scroll=%d",
                event,target.capabilities,target.movie,target.owner,target.width,target.height,read(target.owner + 0x238),direction);
        }
#else
        (void)target; (void)event; (void)direction;
#endif
    }
    void mapDiagnostic(const char* event,const UiMapState& state,float ratio=0,float delta=0,
                       double x=0,double y=0,const GfxRect& bounds={},bool dispatched=false) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        const auto kind=std::strcmp(event,"Legend-Down")==0 ? 0u : std::strcmp(event,"Legend-Up")==0 ? 1u :
            std::strcmp(event,"pinch")==0 ? 2u : 3u;
        if (mapLogs[kind]>=6) return;
        char value[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",value);
        if (value[0]!='1' || value[1]) return;
        ++mapLogs[kind];
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "map event=%s owner=%08X clip=%08X screen=%d dispatched=%u reason=%s "
            "speed=%.6f dt=%.6f logRatio=%.6f rate=%.6f cursor=(%.4f,%.4f) new=(%.4f,%.4f) "
            "clipSize=(%.4f,%.4f) bounds=(%.6f,%.6f,%.6f,%.6f)",
            event,frameTarget.owner,static_cast<std::uint32_t>(frameTarget.direct.clip>>32),frameTarget.direct.screen,
            dispatched ? 1u : 0u,state.reason.c_str(),state.zoomSpeed,state.deltaSeconds,ratio,delta,
            state.cursorX,state.cursorY,x,y,state.clipWidth,state.clipHeight,bounds.left,bounds.top,bounds.right,bounds.bottom);
#else
        (void)event;(void)state;(void)ratio;(void)delta;(void)x;(void)y;(void)bounds;(void)dispatched;
#endif
    }

    void admission(std::uint32_t viewport, std::uint32_t movie, std::uint32_t owner,
                   const std::string& reason, std::uint32_t caps) {
        if (!movie) return;
        const auto description = std::to_string(movie) + "/" + reason + "/" + std::to_string(caps);
        if (description == lastAdmission) return;
        lastAdmission = description;
        if (logs++ >= 24) return;
        (void)viewport; (void)owner;
        HORN_LOGI("BL2Bridge", "native-ui admission=%s caps=%u movie=%08X owner=%08X viewport=%08X "
            "gfxViewport=%08X size=%ux%u movieFlags=(%u,%u) ownerMovie=%08X mouseable=%u",
            reason.c_str(),caps,movie,owner,viewport,read(read(base + 0x0301F840) + 0x44),
            read(viewport + 0x3C),read(viewport + 0x40),read(movie + 0x4C),read(movie + 0x5C),
            read(owner + 0x3C),read(owner + 0x238));
    }

    State(DynarmicHost& h, LifetimeGeneration g, bool taps)
        : host(h), generation(g), base(h.GetElfLoader().GetBaseAddress()), tapTest(taps) {
        signatures = verify();
        inventoryReleaseReady=signatures && InstallUiNativeRelease(host,base);
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        // A refused observer leaves the admitted functional UI route untouched.
        if(signatures) HORN_LOGI("BL2TouchDiagnostics","native-key-return-probe ready=%u",InstallUiKeyReturnProbe(host,base)?1u:0u);
#endif
        fpsBound = host.GetElfLoader().GetSymbolAddress("GShowFpsCounter") == base + 0x0300A48C &&
            host.GetElfLoader().GetSymbolAddress("_Z14DrawFPSCounterP9FViewportP7FCanvasii") == base + 0x013E612C &&
            read(base + 0x013E612C) == 0xE59F02AC && read(base + 0x013E6134) == 0xE92D4FF0;
    }
    void synchronizeFps() {
        if (!fpsBound) return;
        bool initialized = false, wanted = false;
        { std::lock_guard<std::mutex> lock(touchSettingsState.mutex);
          initialized = touchSettingsState.initialized; wanted = touchSettingsState.values[18] != 0; }
        if (!initialized) return;
        const auto actual = read(base + 0x0300A48C);
        if (actual > 1) return;
        if (!fpsPreferenceKnown || wanted != fpsPreference) {
            host.MemoryWrite32(base + 0x0300A48C,wanted ? 1u : 0u);
            fpsPreferenceKnown = true; fpsPreference = wanted;
        } else if ((actual != 0) != wanted) {
            // Native DevTools "Stat FPS" toggles this same original counter.
            // Adopt that edit instead of fighting it each input frame.
            if (SetBl2TouchSetting(18,actual)) fpsPreference = actual != 0;
        }
    }
    bool span(std::uint32_t p, std::size_t size) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() && size <= host.GetGuestMemorySize() - p;
    }
    std::uint32_t read(std::uint32_t p) const { return span(p, 4) ? host.MemoryRead32(p) : 0; }
    static bool copyMemory(const void* context, std::uint32_t p, void* out, std::size_t bytes) {
        const auto& state = *static_cast<const State*>(context);
        if (!state.span(p,bytes)) return false;
#if defined(BL2_NATIVE_ARM32)
        std::memcpy(out,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p)),bytes);
#else
        if (!state.host.GetGuestMemoryBase()) return false;
        std::memcpy(out,state.host.GetGuestMemoryBase() + p,bytes);
#endif
        return true;
    }
    bool array(std::uint32_t p, std::uint32_t limit, std::uint32_t& data, std::uint32_t& count) const {
        if (!span(p, 12)) return false;
        data = read(p); count = read(p + 4);
        const auto capacity = read(p + 8);
        return count <= capacity && capacity <= limit && (!count || span(data, std::size_t(count) * 4));
    }
    std::uint64_t token(std::uint32_t object) const {
        std::uint32_t data = 0, count = 0;
        if (!span(object, 0x38) || !array(base + 0x02FBA268, 2000000, data, count)) return 0;
        const auto index = read(object + 0x20);
        if (index >= count || read(data + index * 4) != object) return 0;
        return (std::uint64_t(object) << 32) | index;
    }
    std::string name(std::uint32_t p) const {
        std::uint32_t names = 0, count = 0;
        if (!span(p, 8) || !array(base + 0x02FB00C8, 2000000, names, count) || read(p) >= count) return {};
        const auto entry = read(names + read(p) * 4);
        if (!span(entry, 136) || read(p + 4)) return {};
        std::string result;
        for (std::uint32_t i = 0; i < 128; ++i) {
            const auto c = host.MemoryRead8(entry + 8 + i);
            if (!c) return result;
            if (c < 32 || c > 126) return {};
            result += static_cast<char>(c);
        }
        return {};
    }
    bool code(std::uint32_t p) const {
        const auto address = p & ~1u;
        // Exact .text from the SHA-bound ELF; never call a vtable pointer in RAM.
        return address >= base + 0x00E823B0 && address < base + 0x02BB91EC &&
            host.MemoryReadCode(address).has_value();
    }
    bool verify() const {
        const auto& elf = host.GetElfLoader();
        if (!generation || elf.GetImageSize() != 0x03089000 ||
            std::uint64_t(base) + elf.GetImageSize() > host.GetGuestMemorySize()) return false;
        struct Signature { const char* symbol; std::uint32_t rva; std::array<std::uint32_t, 2> words; };
        constexpr Signature sites[] = {
            {"_ZN10FGFxEngine8InputKeyEiP9FGFxMovie5FName11EInputEvent", kMovieKey, {0xE24DD008,0xE92D4FF0}},
            {"_ZNK11FKdViewport8GetSizeXEv",0x01E2651C,{0xE590003C,0xE12FFF1E}},
            {"_ZNK11FKdViewport8GetSizeYEv",0x01E2652C,{0xE5900040,0xE12FFF1E}},
            {"_ZN16UGearboxGFxMovie16SetMouseableFlagEv",0x01C074F4,{0xE3A03001,0xE5803238}},
            {"_ZN15UGFxMoviePlayer11SetViewportEiiii",0x01CC52D4,{0xE59FC0C8,0xE92D47F0}}
        };
        for (const auto& site : sites) {
            if (elf.GetSymbolAddress(site.symbol) != base + site.rva) return false;
            for (std::size_t i = 0; i < site.words.size(); ++i)
                if (read(base + site.rva + static_cast<std::uint32_t>(i * 4)) != site.words[i]) return false;
        }
        // Exact original HandleEvent ABI call site: interface->vtable[0x90].
        return read(base + 0x01CC6348) == 0xE5923000 && read(base + 0x01CC634C) == 0xE5933090 &&
            read(base + 0x01CC6350) == 0xE12FFF33 &&
            read(base + 0x00F475C0) == 0xE590000C && read(base + 0x00F475C4) == 0xE7E00ED0 &&
            read(base + 0x01CC62B8) == 0 && read(base + 0x01CC62BC) == 2;
    }
    bool targetValid(const Target& t, bool requireScreen = true) const {
        if (!signatures || !generation || generation() != t.generation ||
            !t.ownerToken || !t.controllerToken || token(t.owner) != t.ownerToken ||
            token(t.controller) != t.controllerToken || read(base + 0x0300D940) != t.world ||
            // Match InputKey's IsPendingKill() + RF_Unreachable rejection before
            // the experimental direct MouseEvent path can bypass those checks.
            read(read(t.owner) + 0x40) != base + 0x00F475C0 || (read(t.owner + 0x0C) & 0x20000002u) ||
            !span(t.viewport, 0x44) || read(t.viewport + 0x3C) != t.width || read(t.viewport + 0x40) != t.height ||
            read(base + 0x0301F840) != t.gfx || !span(t.gfx, 0x1D4) ||
            !span(t.movie, 0x68) || read(t.movie + 0x64) != t.owner || read(t.owner + 0x3C) != t.movie ||
            !read(t.movie + 0x4C) || !read(t.movie + 0x5C) || read(t.movie + 0x34) != t.interface ||
            !span(t.interface, 4) || read(read(t.interface) + 0x90) != t.handler || !code(t.handler)) return false;
        if (requireScreen && t.direct.valid && !Bl2TouchDirectUiCurrent({this,copyMemory},base,t.direct)) return false;
        std::uint32_t data = 0, count = 0;
        if (!array(t.gfx + 0x28, 4, data, count) || !count || read(data) != t.movie) return false;
        const auto engine = read(base + 0x03008D20);
        if (!token(engine) || !array(engine + 0x48C, 4, data, count) || count != 1) return false;
        const auto local = read(data);
        return token(local) && read(local + 0x40) == t.controller && read(local + 0x64) == t.controllerId;
    }
    bool current() const {
        if (!frameValid || physicalOwnership().ownsInput(monotonicMilliseconds()) || !targetValid(frameTarget)) return false;
        const auto snapshot = sharedMailbox().snapshot();
        return snapshot.epoch == epoch && snapshot.context.sceneIdentity == frameTarget.scene &&
            (snapshot.context.capabilities & kUiNavigation) && !(snapshot.context.capabilities & PhysicalOwnsInput);
    }
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
    std::uint32_t keyReturnCallback(const Target& target,std::uint32_t index,std::uint32_t number,std::uint32_t event) {
        UiKeyReturnScope observation{};
        if(uiKeyReturnProbeBase==base && keyReturnDiagnostics<8 && target.ownerToken &&
            target.direct.movieClass=="StatusMenuExGFxMovie" && target.direct.screen==3) {
            char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
            if(property[0]=='1' && !property[1]) observation={target.owner,target.movie,target.controllerId,index,number,event,{},{},{}};
        }
        std::uint32_t handled=0;
        {
            UiKeyReturnActivation activation(observation.owner?&observation:nullptr);
            handled=host.RunGuestCallback(base+kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,event});
        }
        if(observation.owner) {
            ++keyReturnDiagnostics;
            __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                "inventory-native-return owner=%08X movie=%08X id=%u fname=(%u,%u) event=%u handled=%u epoch=%llu generation=%llu "
                "filter=(%u,%08X,%u) delegate=(%u,%08X,%u)",
                target.owner,target.movie,target.controllerId,index,number,event,handled,
                static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(target.generation),
                observation.visits[0],observation.values[0],observation.rejected[0],
                observation.visits[1],observation.values[1],observation.rejected[1]);
        }
        return handled;
    }
#endif
    bool keyName(std::uint32_t rva,const char* spelling) {
        const bool owned=current();
        const auto actual=owned ? name(base+rva) : std::string{};
        // Original FName::Init interns names through strcasecmp. Cooked keys
        // use ThumbStick while the exported KEY symbol uses Thumbstick.
        const auto lower=[](unsigned char c) { return c>='A' && c<='Z' ? c+('a'-'A') : c; };
        const bool matches=spelling && !actual.empty() && std::equal(actual.begin(),actual.end(),
            spelling,spelling+std::strlen(spelling),[&](unsigned char a,unsigned char b) {return lower(a)==lower(b);});
        inventoryDiagnostic("key-admission",!owned?"current-lost":!matches?"key-name-rejected":spelling,
            nullptr,nullptr,nullptr,nullptr,-1,-1,static_cast<int>(rva));
        if(!owned || !matches) {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
            if(keyRejects<8) {
                char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
                if(property[0]=='1' && !property[1]) {
                    ++keyRejects;
                    __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                        "native-ui key-reject rva=%08X expected=%s actual=%s current=%u owner=%08X epoch=%llu",
                        rva,spelling ? spelling : "",actual.c_str(),owned?1u:0u,frameTarget.owner,
                        static_cast<unsigned long long>(epoch));
                }
            }
#endif
            return false;
        }
        const auto target=frameTarget;
        const auto index=read(base+rva),number=read(base+rva+4);
        // Exact-movie overload retains the game's real UI handlers and timers.
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(std::strcmp(spelling,"Escape")==0) inventoryInputDiagnostic("Escape-before-press",target);
        const auto pressGate=std::strcmp(spelling,"Escape")==0?inventoryGateObservation(target):std::string{};
        const auto pressedHandled=keyReturnCallback(target,index,number,0);
#else
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,0});
#else
        if(!call(base+kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,0})) return false;
#endif
#endif
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        inventoryGateDiagnostic(target,index,number,0,pressedHandled,pressGate);
        if(std::strcmp(spelling,"Escape")==0) inventoryInputDiagnostic("Escape-after-press",target);
#endif
        const bool sentUp=current();
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(sentUp) {
            const auto releaseGate=std::strcmp(spelling,"Escape")==0?inventoryGateObservation(target):std::string{};
            const auto releasedHandled=keyReturnCallback(target,index,number,1);
            inventoryGateDiagnostic(target,index,number,1,releasedHandled,releaseGate);
        }
#else
#if defined(BL2_NATIVE_ARM32)
        if(sentUp) host.RunGuestCallback(base+kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,1});
#else
        if(sentUp && !call(base+kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,1})) return false;
#endif
#endif
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if(std::strcmp(spelling,"Escape")==0) inventoryInputDiagnostic("Escape-after-release",target);
#endif
        inventoryDiagnostic("key-original",spelling,nullptr,nullptr,nullptr,nullptr,-1,-1,sentUp?1:0);
        if(logs++<24) HORN_LOGI("BL2Bridge","native-ui key=%s movie=%08X epoch=%llu sentUp=%u targetRetained=%u",
            spelling,target.movie,static_cast<unsigned long long>(epoch),sentUp?1u:0u,current()?1u:0u);
        networkDiagnostic(target,spelling);return sentUp;
    }
    bool footerKey(NativeFooterCommand command) {
        if(!current()) return false;
        if(footerAction && (command==NativeFooterCommand::ButtonA || command==NativeFooterCommand::ButtonB)) {
            const auto action=command==NativeFooterCommand::ButtonA?Action::Confirm:Action::Back;
            if(footerAction({action,true,epoch,0})) {footerAction({action,false,epoch,0});return true;}
        }
        switch(command) {
        case NativeFooterCommand::Inspect:return keyName(0x0300A318,"XboxTypeS_RightThumbstick");
        case NativeFooterCommand::Mark:return keyName(0x0300A310,"XboxTypeS_LeftThumbstick");
        case NativeFooterCommand::Sort:return keyName(0x03009FD0,"PageDown");
        case NativeFooterCommand::Select:return keyName(0x0300A340,"XboxTypeS_X");
        case NativeFooterCommand::Drop:return keyName(0x0300A348,"XboxTypeS_Y");
        case NativeFooterCommand::Close:return keyName(0x03009FB8,"Escape");
        case NativeFooterCommand::Accept:return keyName(0x03009FA0,"Enter");
        case NativeFooterCommand::SellTrash:
            return frameTarget.direct.movieClass=="VendingMachineExGFxMovie" && keyName(0x0300A310,"XboxTypeS_LeftThumbstick");
        case NativeFooterCommand::ButtonA:return keyName(0x0300A330,"XboxTypeS_A");
        case NativeFooterCommand::ButtonB:return keyName(0x0300A338,"XboxTypeS_B");
        case NativeFooterCommand::ButtonX:return keyName(0x0300A340,"XboxTypeS_X");
        case NativeFooterCommand::ButtonY:return keyName(0x0300A348,"XboxTypeS_Y");
        case NativeFooterCommand::ButtonBack:return keyName(0x0300A320,"XboxTypeS_Back");
        case NativeFooterCommand::ButtonStart:return keyName(0x0300A328,"XboxTypeS_Start");
        case NativeFooterCommand::ButtonLSB:return keyName(0x0300A310,"XboxTypeS_LeftThumbstick");
        case NativeFooterCommand::ButtonRSB:return keyName(0x0300A318,"XboxTypeS_RightThumbstick");
        case NativeFooterCommand::ButtonLB:return keyName(0x0300A350,"XboxTypeS_LeftShoulder");
        case NativeFooterCommand::ButtonRB:return keyName(0x0300A358,"XboxTypeS_RightShoulder");
        case NativeFooterCommand::ButtonLT:return keyName(0x0300A360,"XboxTypeS_LeftTrigger");
        case NativeFooterCommand::ButtonRT:return keyName(0x0300A368,"XboxTypeS_RightTrigger");
        default:return false;
        }
    }
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
    // QA only: debug.bl2bridge.qa_training_tab=1..5 gives the next tab-0
    // Training prompt that ECHO tab during its delay, as a tab training does.
    void qaTrainingTab() {
        auto& d=frameTarget.direct;
        if(!d.trainingPrompt || d.screen!=0 || d.screenSize!=1 || qaTrainingOwner==frameTarget.ownerToken || !current()) return;
        char value[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.qa_training_tab",value);
        if(value[0]<'1' || value[0]>'5' || value[1]) return;
        const auto field=frameTarget.owner+d.screenOffset;if(!span(field,1)) return;
        host.MemoryWrite8(field,static_cast<std::uint8_t>(value[0]-'0'));d.screen=value[0]-'0';
        qaTrainingOwner=frameTarget.ownerToken;
        HORN_LOGI("BL2Bridge","native-ui QA training tab=%c owner=%08X",value[0],frameTarget.owner);
    }
#endif
    // A ready WillowGFxTrainingDialogBox with an ECHO tab (StatusMenuTab 1-5)
    // closes in its original HandleInputKey only for XboxTypeS_Back or the
    // StatusMenu/tab bindings, and OnClose then opens that tab; Enter and
    // Escape are ignored, so neither touch button could leave the message.
    // Select keeps the prompt's own action (XboxTypeS_Back). Back skips it:
    // StatusMenuTab=0 takes the original tab-0 branch, where Escape closes and
    // OnClose reopens the HUD. Tab 0 prompts accept Enter/Escape themselves.
    bool trainingKey(Action action) {
        if((action!=Action::Confirm && action!=Action::Back) || !current()) return false;
        auto& direct=frameTarget.direct;
        if(!direct.trainingPrompt || direct.screen<1 || direct.screen>5) return false;
        // The original swallows every key until the delayed prompt appears.
        if(!Bl2TouchTrainingPromptReady({this,copyMemory},base,direct)) return true;
        const bool skip=action==Action::Back;
        const auto keyAt=base+(skip?0x03009FB8u:0x0300A320u);
        const auto owner=frameTarget.owner,field=owner+direct.screenOffset;
        if(name(keyAt)!=(skip?"Escape":"XboxTypeS_Back") || direct.screenSize!=1 || !span(field,1)) return true;
        const auto tab=static_cast<std::uint8_t>(direct.screen);
        if(skip) { host.MemoryWrite8(field,0);direct.screen=0; }
        const auto edge=[&](std::uint32_t event) {
            return originalQuery(owner,direct.click,{frameTarget.controllerId,read(keyAt),read(keyAt+4),event,0xFFFFFFFFu},16).has_value();
        };
        // The release closes the movie inside the call, so its own answer is not
        // a success signal: only a release that never ran keeps the ECHO tab.
        bool released=false;
        if(edge(0) && current() && Bl2TouchTrainingPromptReady({this,copyMemory},base,frameTarget.direct)) { released=true;edge(1); }
        if(skip && !released && span(field,1) && token(owner)==frameTarget.ownerToken &&
            host.MemoryRead8(field)==0) { host.MemoryWrite8(field,tab);frameTarget.direct.screen=tab; }
        if(logs++<24) HORN_LOGI("BL2Bridge","native-ui training %s tab=%u released=%u owner=%08X",
            skip?"skip":"echo",static_cast<unsigned>(tab),released?1u:0u,owner);
        return true;
    }
    void key(Action action) {
        if(trainingKey(action)) return;
        // The gamepad face and shoulder keys keep the footer's case-insensitive
        // spelling check; the six navigation keys were matched exactly in prepare.
        switch(action) {
        case Action::UiAction:keyName(0x0300A340,"XboxTypeS_X");return;
        case Action::UiOther:keyName(0x0300A348,"XboxTypeS_Y");return;
        case Action::UiPreviousTab:keyName(0x0300A350,"XboxTypeS_LeftShoulder");return;
        case Action::UiNextTab:keyName(0x0300A358,"XboxTypeS_RightShoulder");return;
        default:break;
        }
        for(const auto& key:keys) if(key.action==action) {keyName(key.rva,key.name);return;}
    }
    // Vertical drag: one Up/Down per pitch. The age gate and a footer word own
    // the gesture they started on; their UP clears them without a command.
    void scroll(int steps) {
        if (!current() || !(frameTarget.capabilities & kUiTapTest) || ageGateGesture || footerTarget) return;
        const auto target=frameTarget;
        for (int i=0;i<std::abs(steps) && current();++i) key(steps>0 ? Action::UiDown : Action::UiUp);
        networkDiagnostic(target,"swipe",steps);
    }
    // Horizontal swipe: LB/RB where the movie's own handler pages tabs or
    // panels with them, otherwise Left/Right as on the D-pad.
    void tab(int direction) {
        if (!current() || !(frameTarget.capabilities & kUiTapTest) || ageGateGesture || footerTarget) return;
        const auto target=frameTarget;
        if (Bl2TouchSwipeTabsMovie(target.direct.movieClass)) key(direction>0 ? Action::UiNextTab : Action::UiPreviousTab);
        else key(direction>0 ? Action::UiRight : Action::UiLeft);
        networkDiagnostic(target,"tab",direction);
    }
    bool advanceStartTitle(const Target& target) {
        if (physicalOwnership().ownsInput(monotonicMilliseconds()) ||
            !Bl2TouchAtStartTitle({this,copyMemory},base,target.owner) ||
            name(base + 0x0300A328) != "XboxTypeS_Start") return false;
        const auto now = monotonicMilliseconds();
        if (titleToken != target.ownerToken) {
            titleToken = target.ownerToken; titleReadyAt = now + 150; titleAttempts = 0;
        }
        if (titleAttempts >= 3 || now < titleReadyAt || !targetValid(target)) return false;
        ++titleAttempts; titleReadyAt = now + 750;
        const auto index = read(base + 0x0300A328), number = read(base + 0x0300A32C);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,0});
#else
        if(!call(base + kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,0})) return false;
#endif
        if (targetValid(target) && Bl2TouchAtStartTitle({this,copyMemory},base,target.owner))
{
#if defined(BL2_NATIVE_ARM32)
            host.RunGuestCallback(base + kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,1});
#else
            if(!call(base + kMovieKey,{target.gfx,target.controllerId,target.movie,index,number,1})) return false;
#endif
        }
        HORN_LOGI("BL2Bridge","native-ui title Start via original handler owner=%08X attempt=%u",target.owner,titleAttempts);
        return true;
    }
#if !defined(BL2_NATIVE_ARM32)
    bool call(std::uint32_t pc,const std::vector<std::uint32_t>& args) {
        std::uint32_t returned=0;return host.CallGuest(pc,args,returned);
    }
#endif
    bool mouse(std::uint32_t type, float x, float y) {
        return mouseFor(frameTarget,type,x,y);
    }
    bool mouseFor(const Target& target,std::uint32_t type,float x,float y,bool cleanup=false) {
        if (!(cleanup ? targetValid(target,false) : current()) || !scratch || !(target.capabilities & kUiTapTest)) return false;
        // Scaleform::GFx::MouseEvent, copied field-for-field from InputKey's
        // 0x1CC6304..6350 path. The native stage performs its viewport transform.
        for (std::uint32_t offset = 0; offset < 28; offset += 4) host.MemoryWrite32(scratch + offset, 0);
        host.MemoryWrite32(scratch, type);
        host.MemoryWrite32(scratch + 8, floatWord(x));
        host.MemoryWrite32(scratch + 12, floatWord(y));
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(target.handler, {target.interface,scratch});
        return true;
#else
        return call(target.handler, {target.interface,scratch});
#endif
    }
    bool inventoryMouseUpFor(const Target& target,float x,float y,bool cleanup=false) {
        if(!inventoryReleaseReady || !(cleanup?targetValid(target,false):current()) ||
            !(target.capabilities&kUiTapTest) || !std::isfinite(x) || !std::isfinite(y) ||
            name(base+0x03009F68)!="LeftMouseButton" || read(base+0x03009F6C)!=0) return false;
        UiNativeReleaseScope nativeRelease{target.owner,target.movie,target.gfx,target.interface,target.controllerId,
            read(base+0x03009F68),0,floatWord(x),floatWord(y),[&] {return cleanup?targetValid(target,false):current();},0,0};
        UiNativeReleaseActivation activation(nativeRelease);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+kMovieKey,{target.gfx,target.controllerId,target.movie,nativeRelease.index,nativeRelease.number,1});
#else
        if(!call(base+kMovieKey,{target.gfx,target.controllerId,target.movie,nativeRelease.index,nativeRelease.number,1})) return false;
#endif
        return nativeRelease.delivered==1 && !nativeRelease.rejected;
    }
    InventoryDragFamily inventoryFamily() const {
        if(!inventoryReleaseReady) return InventoryDragFamily::None;
        const auto& cls=frameTarget.direct.movieClass;
        if(cls=="BankGFxMovie") return InventoryDragFamily::Bank;
        if(cls=="StashGFxMovie") return InventoryDragFamily::Stash;
        if(cls=="VendingMachineExGFxMovie") return InventoryDragFamily::Vending;
        if(cls=="TradingGFxMovie") return InventoryDragFamily::Trading;
        if(cls=="StatusMenuExGFxMovie" && frameTarget.direct.valid && frameTarget.direct.screen==3)
            return InventoryDragFamily::StatusInventory;
        return InventoryDragFamily::None;
    }
    InventoryDragOwner inventoryIdentity() const {
        return {inventoryFamily(),epoch,frameTarget.scene,frameTarget.generation,
            frameTarget.ownerToken,frameTarget.controllerToken,token(frameTarget.world),
            frameTarget.viewport,frameTarget.width,frameTarget.height};
    }
    void clearInventory() {
        inventoryStart.reset();inventoryFinal.reset();inventoryPressed=false;inventoryPressedAt=0;inventoryFresh={};inventoryCleanup=false;
        inventoryHoverBoundary=inventoryDropQueued=inventorySourcePrimed=inventoryPrimePending=false;
        inventoryStatus={};inventoryStatusPanels={};
        inventoryTarget={};inventoryDragData={};inventorySession.reset();inventoryTransport.reset();inventoryOwner={};
    }
    InventoryWidgetTarget captureInventory(GfxSession& session,float x,float y,bool retainStatus=false) {
        if(inventoryFamily()!=InventoryDragFamily::StatusInventory)
            return CaptureBl2InventoryWidget(session,inventoryFamily(),frameTarget.width,frameTarget.height,x,y,
                [this](std::int32_t index,bool offers) {return tradingInventory(index,offers);});
        const auto model=CaptureBl2StatusInventory({this,copyMemory},base,frameTarget.owner);
        if(!model.valid || model.owner!=frameTarget.ownerToken || !current()) {
            inventoryDiagnostic("capture-native",!model.valid?model.reason.c_str():"owner-or-current",&session,nullptr,nullptr,&model,x,y);
            return {};
        }
        InventoryWidgetObservation observed{};const bool diagnostic=inventoryDiagnosticEnabled();
        std::array<GfxIdentity,3> panels{};
        const std::array tokens{model.top,model.backpack,model.equipped};
        for(std::size_t i=0;i<panels.size();++i) {
            const auto clip=session.pin(static_cast<std::uint32_t>(tokens[i]>>32)+0x3C);
            if(diagnostic) observed.types[i]=session.type(clip);
            const bool valid=session.identity(clip,panels[i]);session.release(clip);
            if(!valid) {
                inventoryDiagnostic("capture-pin","native-panel-value",&session,&observed,nullptr,&model,x,y,static_cast<int>(i));
                return {};
            }
        }
        auto target=CaptureBl2InventoryWidget(session,inventoryFamily(),frameTarget.width,frameTarget.height,x,y,
            [this,&model](std::int32_t index,bool equipped) -> std::uint64_t {
                if(index<0 || index>(equipped?7:4095)) return 0;
                const auto item=originalQuery(static_cast<std::uint32_t>((equipped?model.equipped:model.backpack)>>32),
                    equipped?model.equippedInventory:model.backpackInventory,{static_cast<std::uint32_t>(index),0xFFFFFFFFu},4);
                if(!item || !*item || !current()) {
                    inventoryDiagnostic("native-getter",!item?"query-rejected":!*item?"empty-item":"current-lost",
                        nullptr,nullptr,nullptr,&model,-1,-1,index);
                    return 0;
                }
                const auto fresh=CaptureBl2StatusInventory({this,copyMemory},base,frameTarget.owner);
                if(!fresh.valid || fresh.owner!=model.owner || fresh.top!=model.top || fresh.backpack!=model.backpack ||
                    fresh.equipped!=model.equipped || fresh.backpackInventory.token!=model.backpackInventory.token ||
                    fresh.equippedInventory.token!=model.equippedInventory.token) {
                    inventoryDiagnostic("native-getter","changed-native-panels",nullptr,nullptr,nullptr,&fresh,-1,-1,index);
                    return 0;
                }
                const auto identity=Bl2TouchInventoryToken({this,copyMemory},base,*item);
                inventoryDiagnostic("native-getter",identity?"item-owned":"item-token-rejected",nullptr,nullptr,nullptr,&fresh,-1,-1,index);
                return identity;
            },panels,diagnostic?&observed:nullptr);
        inventoryDiagnostic("capture-widget",diagnostic?observed.reason:(target?"captured":"rejected"),&session,
            diagnostic?&observed:nullptr,nullptr,&model,x,y,target?1:0);
        if(target && retainStatus) {inventoryStatus=model;inventoryStatusPanels=panels;}
        return target;
    }
    InventoryWidgetTarget inventoryHit(float x,float y) {
        if(!current() || inventoryFamily()==InventoryDragFamily::None) return {};
        if(!inventoryProbe) inventoryProbe=std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession session(*inventoryProbe,base,frameTarget.interface,inventoryProbe->scratch(),512,true);
        return captureInventory(session,x,y);
    }
    bool inventoryDown(float x,float y) {
        if(inventoryPressed || inventoryGesture.captured() || !current() || !inventoryIdentity()) {
            inventoryDiagnostic("down","busy-current-or-identity",nullptr,nullptr,nullptr,nullptr,x,y,0);return false;
        }
        inventoryOwner=frameTarget;
        inventoryTransport=std::make_unique<GfxHostTransport>(host,[this] {
            return inventoryCleanup ? targetValid(inventoryOwner,false) : current() &&
                frameTarget.ownerToken==inventoryOwner.ownerToken && frameTarget.interface==inventoryOwner.interface;
        });
        inventorySession=std::make_unique<GfxSession>(*inventoryTransport,base,frameTarget.interface,inventoryTransport->scratch(),512,true);
        inventoryTarget=captureInventory(*inventorySession,x,y,true);
        if(!inventoryTarget) {inventoryDiagnostic("down","capture-rejected",inventorySession.get(),nullptr,nullptr,nullptr,x,y,0);clearInventory();return false;}
        bool active=false;
        InventoryDragObservation observed{};
        const bool started=Bl2InventoryDragStarted(*inventorySession,inventoryTarget,nullptr,&active,inventoryDiagnosticEnabled()?&observed:nullptr);
        inventoryDiagnostic("down-manager",started?"own-active":active?"active-or-unknown":"idle",inventorySession.get(),nullptr,&observed,nullptr,x,y,started?1:0);
        if(started || (inventoryFamily()==InventoryDragFamily::StatusInventory && active)) {
            clearInventory();return false;
        }
        inventoryDiagnosticGesture();inventoryInputDiagnostic("admitted",inventoryOwner);
        // Original AddMouseButton clears a pending AddMouseMove. Prime the
        // authored currentOverClip in a separate original movie frame before
        // any held press; no native button/action is sent on physical DOWN.
        inventoryHoverBoundary=false;
        if(!mouse(1,std::floor(x*(frameTarget.width-1)),std::floor(y*(frameTarget.height-1)))) {
            clearInventory();return false;
        }
        inventoryDiagnostic("down","original-hover-queued",inventorySession.get(),nullptr,nullptr,nullptr,x,y,1);
        inventoryDiagnostic("down","admitted",inventorySession.get(),nullptr,nullptr,nullptr,x,y,1);
        inventoryFresh=inventoryTarget.source;return true;
    }
    bool statusInventoryItemCurrent() {
        if(inventoryFamily()!=InventoryDragFamily::StatusInventory || !inventoryStatus.valid ||
            inventoryStatus.owner!=inventoryOwner.ownerToken || !inventoryTarget.nativeItem || !inventorySession || !current()) return false;
        const auto same=[&](const UiStatusInventoryModel& model) {
            return model.valid && model.owner==inventoryStatus.owner && model.top==inventoryStatus.top &&
                model.backpack==inventoryStatus.backpack && model.equipped==inventoryStatus.equipped &&
                model.backpackInventory.token==inventoryStatus.backpackInventory.token &&
                model.equippedInventory.token==inventoryStatus.equippedInventory.token;
        };
        const bool equipped=inventoryTarget.source.panel==inventoryStatusPanels[2];
        if((!equipped && inventoryTarget.source.panel!=inventoryStatusPanels[1]) ||
            inventoryTarget.index<0 || inventoryTarget.index>(equipped?7:4095)) return false;
        const auto panelsCurrent=[&](const UiStatusInventoryModel& model) {
            const std::array tokens{model.top,model.backpack,model.equipped};
            for(std::size_t i=0;i<tokens.size();++i) {
                const auto clip=inventorySession->pin(static_cast<std::uint32_t>(tokens[i]>>32)+0x3C);
                GfxIdentity actual{};
                bool valid=inventorySession->identity(clip,actual) && actual==inventoryStatusPanels[i];
                if(valid && (i==0 || i==(equipped?2u:1u))) {
                    const auto visible=inventorySession->member(clip,"_visible"),alpha=inventorySession->member(clip,"_alpha");
                    bool shown=false;double opacity=0;
                    valid=inventorySession->boolean(visible,shown) && shown && inventorySession->number(alpha,opacity) && opacity>0 && opacity<=100;
                    inventorySession->release(alpha);inventorySession->release(visible);
                }
                if(valid && i==(equipped?2u:1u)) {
                    const auto parent=inventorySession->member(clip,"_parent");GfxIdentity owner{};
                    valid=inventorySession->identity(parent,owner) && owner==inventoryStatusPanels[0];inventorySession->release(parent);
                    if(valid && !equipped) {
                        const auto canDrag=inventorySession->member(clip,"bCanDrag");bool allowed=false;
                        valid=inventorySession->boolean(canDrag,allowed) && allowed;inventorySession->release(canDrag);
                    }
                }
                inventorySession->release(clip);if(!valid) return false;
            }
            return inventorySession->good();
        };
        const auto model=CaptureBl2StatusInventory({this,copyMemory},base,frameTarget.owner);
        if(!same(model) || !panelsCurrent(model)) return false;
        const auto item=originalQuery(static_cast<std::uint32_t>((equipped?model.equipped:model.backpack)>>32),
            equipped?model.equippedInventory:model.backpackInventory,{static_cast<std::uint32_t>(inventoryTarget.index),0xFFFFFFFFu},4);
        if(!item || !*item || !current()) return false;
        const auto fresh=CaptureBl2StatusInventory({this,copyMemory},base,frameTarget.owner);
        return same(fresh) && panelsCurrent(fresh) &&
            Bl2TouchInventoryToken({this,copyMemory},base,*item)==inventoryTarget.nativeItem && current();
    }
    InventoryDragSource freshInventorySource() {
        if(inventoryPressed && inventorySession && inventoryTarget.nativeItem &&
            (inventoryFamily()==InventoryDragFamily::Trading || inventoryFamily()==InventoryDragFamily::StatusInventory) && current()) {
            inventorySession->beginInputBoundary();GfxIdentity data{};bool active=false;
            InventoryDragObservation observed{};
            const bool started=Bl2InventoryDragStarted(*inventorySession,inventoryTarget,&data,&active,
                (inventoryDiagnosticEnabled()
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
                 || inventoryDropDiagnosticEnabled()
#endif
                )?&observed:nullptr);
            inventoryDiagnostic("fresh-manager",started?"own-ack":active?"active-or-unknown":"queued-or-idle",inventorySession.get(),nullptr,&observed);
            if(started) {
                // Original StartOffer freezes and resorts this very item. The
                // acknowledged manager keeps the original payload/pressOrigin;
                // native OfferingThing and FrozenThing must still be our item.
                if(inventoryDragData.data && inventoryDragData!=data) {
                    inventoryDiagnostic("fresh-source","changed-payload",inventorySession.get(),nullptr,&observed);return {};
                }
                if(inventoryFamily()==InventoryDragFamily::Trading &&
                    Bl2TouchTradingHeldItem({this,copyMemory},base,frameTarget.owner)!=inventoryTarget.nativeItem) return {};
                if(inventoryFamily()==InventoryDragFamily::StatusInventory) {
                    // Original drag owns its payload after ACK. Rollout/card
                    // animation can retire the source hit area; the typed native
                    // panel/index must still return the very same inventory item.
                    if(!statusInventoryItemCurrent()) {
                        inventoryDiagnostic("fresh-source","native-held-item-changed",inventorySession.get(),nullptr,&observed);return {};
                    }
                    inventoryDragData=data;
                    inventoryDiagnostic("fresh-source","native-held-item-owned",inventorySession.get(),nullptr,&observed);
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
                    inventoryDropDiagnostic("own-ack",observed);
#endif
                    return inventoryTarget.source;
                }
                inventoryDragData=data;
                if(inventoryFamily()==InventoryDragFamily::Trading) return inventoryTarget.source;
            } else if(inventoryFamily()==InventoryDragFamily::StatusInventory && active) return {};
        }
        const auto fresh=inventoryHit(inventoryGesture.startX(),inventoryGesture.startY());
        const bool same=SameBl2InventoryWidget(inventoryTarget,fresh);
        inventoryDiagnostic("fresh-source",same?"same-item":"changed-or-rejected",nullptr,nullptr,nullptr,nullptr,
            inventoryGesture.startX(),inventoryGesture.startY(),same?1:0);
        return same?fresh.source:InventoryDragSource{};
    }
    void finishInventory(const InventoryDragUpdate& update) {
        if(!inventoryPressed || !inventorySession || !targetValid(inventoryOwner,false)) {
            inventoryDiagnostic("finish","no-safe-press",inventorySession.get(),nullptr,nullptr,nullptr,update.x,update.y);
            clearInventory();return;
        }
        inventorySession->beginInputBoundary();
        inventoryCleanup=update.command==InventoryDragCommand::Cancel;
        GfxIdentity data{};bool active=false;
        InventoryDragObservation observed{};
        const bool started=Bl2InventoryDragStarted(*inventorySession,inventoryTarget,&data,&active,
            (inventoryDiagnosticEnabled()
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
             || inventoryDropDiagnosticEnabled()
#endif
            )?&observed:nullptr);
        inventoryDiagnostic("finish-manager",update.command==InventoryDragCommand::Cancel?"cancel":"drop",
            inventorySession.get(),nullptr,&observed,nullptr,update.x,update.y,static_cast<int>(update.command));
        if(inventoryOwner.direct.movieClass=="StatusMenuExGFxMovie" && active && !started) {
            inventoryDiagnostic("finish","foreign-or-unknown-manager",inventorySession.get(),nullptr,&observed);
            clearInventory();return; // Never release or cancel a foreign active manager.
        }
        if(started && inventoryDragData.data && data!=inventoryDragData) {
            // Another original drag replaced ours in the same panel. Neither
            // cancelDrag nor a synthetic UP may finish that foreign transfer.
            inventoryDiagnostic("finish","changed-payload",inventorySession.get(),nullptr,&observed);clearInventory();return;
        }
        if(started) inventoryDragData=data;
        if(!started && monotonicMilliseconds()-inventoryPressedAt<=250) {
            // HandleEvent queues Down/Move. Cancel/UP from the same Android
            // batch must wait for the original AS drag, never race it and drop.
            inventoryFinal=update;inventoryCleanup=false;
            inventoryDiagnostic("finish","waiting-original-ack",inventorySession.get(),nullptr,&observed);return;
        }
        const bool cancel=update.command==InventoryDragCommand::Cancel ||
            (started && inventoryOwner.direct.movieClass=="StatusMenuExGFxMovie" && !statusInventoryItemCurrent());
        inventoryCleanup=cancel;
        if(cancel) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
            inventoryInputDiagnostic("cancel-before",inventoryOwner);
#endif
            // SwitchToQueuedInputHandler must see Released before cancelDrag's
            // FinishEquip(None) restores NormalInputKey. HandleEvent queues the
            // outside MouseUp; the original frame consumes it after cancelDrag.
            const bool move=mouseFor(inventoryOwner,1,-1,-1,true),up=inventoryMouseUpFor(inventoryOwner,-1,-1,true);
            if(started && !CancelBl2InventoryDrag(*inventorySession,inventoryTarget)) {
                inventoryDiagnostic("finish","original-cancel-rejected",inventorySession.get(),nullptr,&observed);
                inventoryCleanup=false;clearInventory();return;
            }
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
            inventoryInputDiagnostic("cancel-after-manager",inventoryOwner);
#endif
            // Original release-outside clears native press capture. It cannot
            // turn a cancelled drag into an item click at its former origin.
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
            inventoryInputDiagnostic("cancel-after-release-queued",inventoryOwner);
#endif
            inventoryDiagnostic("finish","cancel-release-outside",inventorySession.get(),nullptr,&observed,nullptr,update.x,update.y,(move?1:0)|(up?2:0));
        } else if(started && current()) {
            if(!inventoryDropQueued) {
                inventoryFinal=update;inventoryHoverBoundary=false;
                inventoryDropQueued=mouse(1,std::floor(update.x*(frameTarget.width-1)),std::floor(update.y*(frameTarget.height-1)));
                inventoryDiagnostic("finish","original-drop-move-queued",inventorySession.get(),nullptr,&observed,nullptr,update.x,update.y,inventoryDropQueued?1:0);
                if(!inventoryDropQueued) {auto cancel=update;cancel.command=InventoryDragCommand::Cancel;finishInventory(cancel);}
                return;
            }
            // AddMouseButton discards pending motion; let the original movie
            // consume the final target before releasing the captured press.
            if(!inventoryHoverBoundary) return;
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
            inventoryDropDiagnostic("pre-up",observed);
#endif
            const bool up=inventoryMouseUpFor(inventoryOwner,std::floor(update.x*(frameTarget.width-1)),std::floor(update.y*(frameTarget.height-1)));
            inventoryDiagnostic("finish","original-drop-queued",inventorySession.get(),nullptr,&observed,nullptr,update.x,update.y,1|(up?2:0));
            inventoryInputDiagnostic("after-native-release",inventoryOwner);
        } else {
            inventoryInputDiagnostic("before-no-ack-release",inventoryOwner);
            inventoryCleanup=true;
            const bool move=mouseFor(inventoryOwner,1,-1,-1,true),up=inventoryMouseUpFor(inventoryOwner,-1,-1,true);
            inventoryDiagnostic("finish","no-ack-release-outside",inventorySession.get(),nullptr,&observed,nullptr,update.x,update.y,(move?1:0)|(up?2:0));
        }
        inventoryCleanup=false;clearInventory();
    }
    void inventoryUpdate(const InventoryDragUpdate& update
        ,bool realMove=false
    ) {
        if(update.command==InventoryDragCommand::Cancel || update.command==InventoryDragCommand::Drop) {
            finishInventory(update);return;
        }
        if(!current() || !inventorySession || !inventoryTarget || update.source!=inventoryTarget.source) {
            inventoryDiagnostic("update","current-session-or-source",inventorySession.get(),nullptr,nullptr,nullptr,update.x,update.y,static_cast<int>(update.command));return;
        }
        if(update.command==InventoryDragCommand::Click) {
            inventoryDiagnostic("update","short-click",inventorySession.get(),nullptr,nullptr,nullptr,update.x,update.y);
            const UiTap point{update.x,update.y,0};tap(point);clearInventory();return;
        }
        if(update.command==InventoryDragCommand::StartMove || inventoryStart) {
            const bool retry=inventoryStart.has_value();
            auto start=inventoryStart.value_or(update);start.x=update.x;start.y=update.y;
            inventoryStart=start;
            const auto forwardMove=[&] {
                if(!realMove || inventoryPrimePending) return;
                // The first rollOver installs onMouseMove but does not populate
                // currentOverClip. Forward real captured motion while waiting;
                // deferred frame retries must not synthesize another MOVE.
                inventoryHoverBoundary=false;
                const bool queued=mouse(1,std::floor(update.x*(frameTarget.width-1)),std::floor(update.y*(frameTarget.height-1)));
                inventoryDiagnostic("update","original-prepress-move-queued",inventorySession.get(),nullptr,nullptr,nullptr,update.x,update.y,queued?1:0);
                if(!queued) clearInventory();
            };
            if(!inventoryHoverBoundary) {
                inventoryDiagnostic("update","waiting-hover-boundary",inventorySession.get());
                forwardMove();
                return;
            }
            inventoryPrimePending=false;
            inventorySession->beginInputBoundary();
            const auto hovered=inventorySession->member(inventoryTarget.panel,"currentOverClip");GfxIdentity cell{};
            const bool over=inventorySession->identity(hovered,cell) && cell==inventoryTarget.source.cell;
            const auto hoveredType=inventoryDiagnosticEnabled()?inventorySession->type(hovered):0;
            inventorySession->release(hovered);
            const bool status=inventoryFamily()==InventoryDragFamily::StatusInventory;
            const auto onPress=over && status?inventorySession->member(inventoryTarget.cell,"onPress"):GfxHandle{};
            const bool pressReady=over && (!status || inventorySession->type(onPress)==8);
            inventorySession->release(onPress);
            if(!inventorySession->good()) {clearInventory();return;}
            if(!pressReady) {
                // DragOver can leave currentOverClip at a drop destination
                // without restoring its onPress. A genuine outward MOVE must
                // precede the origin prime so authored RollOver can rearm it.
                inventoryDiagnostic("update",over?"waiting-original-press-callback":"waiting-original-hover",inventorySession.get(),nullptr,nullptr,nullptr,-1,-1,-1,&cell,hoveredType);
                if(!over && retry && !inventorySourcePrimed) {
                    // Normalize the captured touch origin once after motion;
                    // newer samples wait until its authored hover is consumed.
                    inventorySourcePrimed=inventoryPrimePending=true;inventoryHoverBoundary=false;
                    const bool queued=mouse(1,std::floor(start.startX*(frameTarget.width-1)),std::floor(start.startY*(frameTarget.height-1)));
                    inventoryDiagnostic("update","original-source-prime-queued",inventorySession.get(),nullptr,nullptr,nullptr,start.startX,start.startY,queued?1:0);
                    if(!queued) clearInventory();
                    return;
                }
                forwardMove();
                return;
            }
            bool active=false;
            const bool started=Bl2InventoryDragStarted(*inventorySession,inventoryTarget,nullptr,&active);
            if(started || (inventoryFamily()==InventoryDragFamily::StatusInventory && active)) {
                inventoryDiagnostic("update","hover-manager-busy",inventorySession.get());clearInventory();return;
            }
            inventoryInputDiagnostic("before-native-down",inventoryOwner);
            inventoryStart.reset();
            const auto x=std::floor(start.startX*(frameTarget.width-1)),y=std::floor(start.startY*(frameTarget.height-1));
            inventoryPressed=mouse(2,x,y);inventoryPressedAt=monotonicMilliseconds();
            inventoryDiagnostic("update","original-down-queued",inventorySession.get(),nullptr,nullptr,nullptr,start.startX,start.startY,inventoryPressed?1:0,&cell,hoveredType);
            inventoryInputDiagnostic("after-native-down",inventoryOwner);
        }
        if(inventoryPressed) {
            const bool queued=mouse(1,std::floor(update.x*(frameTarget.width-1)),std::floor(update.y*(frameTarget.height-1)));
            inventoryDiagnostic("update","original-move-queued",inventorySession.get(),nullptr,nullptr,nullptr,update.x,update.y,queued?1:0);
        }
    }
    void releaseInventory() {
        if(const auto update=inventoryGesture.cancel()) inventoryUpdate(*update);
        else if(inventoryPressed) {
            auto update=inventoryFinal.value_or(InventoryDragUpdate{});
            update.command=InventoryDragCommand::Cancel;finishInventory(update);
        } else clearInventory();
    }
    bool originalMethodFor(const Target& target,std::uint32_t object,const UiNativeMethod& fn,
        std::initializer_list<std::uint32_t> args,bool cleanup = false) {
        if (!(cleanup ? targetValid(target,false) : current()) || !scratch || !fn.function || fn.bytes != args.size()*4 ||
            token(fn.function) != fn.token || read(fn.function + 0x7C) != fn.flags ||
            read(read(object)+0x10C) != base + 0x010E826C ||
            read(base + 0x010E826C) != 0xE92D4FF0 || read(base + 0x010E8270) != 0xED2D8B06) return false;
        const auto identity = token(object); if (!identity) return false;
        unsigned i = 0; for (const auto word : args) host.MemoryWrite32(scratch + i++*4,word);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + 0x010E826C,{object,fn.function,scratch,0});
#else
        if(!call(base + 0x010E826C,{object,fn.function,scratch,0})) return false;
#endif
        return token(object) == identity;
    }
    bool originalMethod(std::uint32_t object,const UiNativeMethod& fn,std::initializer_list<std::uint32_t> args) {
        return originalMethodFor(frameTarget,object,fn,args);
    }
    std::optional<std::uint32_t> originalQuery(std::uint32_t object,const UiNativeMethod& fn,
        std::initializer_list<std::uint32_t> words,std::uint32_t returnOffset) {
        const auto before=frameTarget;const auto objectToken=token(object);
        if(!current() || !objectToken || !scratch || !fn.function || fn.bytes!=words.size()*4 || returnOffset+4>fn.bytes ||
            token(fn.function)!=fn.token || read(fn.function+0x7C)!=fn.flags || read(read(object)+0x10C)!=base+0x010E826C ||
            read(base+0x010E826C)!=0xE92D4FF0 || read(base+0x010E8270)!=0xED2D8B06) return {};
        unsigned i=0;for(const auto word:words) host.MemoryWrite32(scratch+i++*4,word);
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base+0x010E826C,{object,fn.function,scratch,0});
#else
        if(!call(base+0x010E826C,{object,fn.function,scratch,0})) return {};
#endif
        if(!current() || frameTarget.ownerToken!=before.ownerToken || frameTarget.interface!=before.interface ||
            frameTarget.generation!=before.generation || token(object)!=objectToken || token(fn.function)!=fn.token ||
            read(fn.function+0x7C)!=fn.flags) return {};
        return read(scratch+returnOffset);
    }
    std::uint64_t tradingInventory(std::int32_t index,bool offers) {
        if(!current() || inventoryFamily()!=InventoryDragFamily::Trading) return 0;
        const auto model=offers?CaptureBl2TradingOffer({this,copyMemory},base,frameTarget.owner,index):
            CaptureBl2TradingBackpack({this,copyMemory},base,frameTarget.owner,index);
        if(!model.valid || model.owner!=frameTarget.ownerToken) return 0;
        if(offers) {
            const auto allowed=originalQuery(static_cast<std::uint32_t>(model.panel>>32),model.canStart,{0xFFFFFFFFu},0);
            if(!allowed || *allowed!=1) return 0;
        }
        const auto item=originalQuery(offers?frameTarget.owner:static_cast<std::uint32_t>(model.panel>>32),
            model.inventory,{static_cast<std::uint32_t>(index),0xFFFFFFFFu},4);
        if(!item || !*item || !current()) return 0;
        return Bl2TouchInventoryToken({this,copyMemory},base,*item);
    }
    TextChatOwner chatIdentity(const UiTextChatModel& model,const TextChatInput& input,std::uint32_t activity) const {
        return {epoch,frameTarget.scene,frameTarget.generation,model.owner,frameTarget.controllerToken,
            token(frameTarget.world),frameTarget.movie,frameTarget.interface,frameTarget.viewport,
            frameTarget.width,frameTarget.height,activity,input.chat,input.input};
    }
    void stopChatEpisode(const TextChatKeyboardState& service) {
        if(chatKeyboard.pending() || !chatOwner || !service.known || !service.active || service.open ||
            service.activity!=chatOwner.activity || token(read(base+0x0300D940))!=chatOwner.world ||
            token(static_cast<std::uint32_t>(chatOwner.controller>>32))!=chatOwner.controller ||
            !span(chatOwner.viewport,0x44) || read(chatOwner.viewport+0x3C)!=chatOwner.width ||
            read(chatOwner.viewport+0x40)!=chatOwner.height || read(read(base+0x0301F840)+0x44)!=chatOwner.viewport+4) return;
        const auto engine=read(base+0x03008D20);std::uint32_t players=0,count=0;
        if(!token(engine) || !array(engine+0x48C,4,players,count) || count!=1) return;
        const auto local=read(players);
        if(!token(local) || token(read(local+0x40))!=chatOwner.controller ||
            !Bl2TouchTextChatStopped({this,copyMemory},base,chatOwner.owner,chatObject)) return;
        chatOwner={};chatObject=0;chatAttempts=0;chatAttemptAt=0;
    }
    void pollChat(const Frame& frame) {
        if(!frame.fresh || (!chatKeyboard.pending() && !chatOwner && frameTarget.direct.movieClass!="TextChatGFxMovie")) return;
        // Service observation must survive this exact keyboard's IME reset.
        // No GFx operations use its scratch or bypass their normal owner check.
        if(!chatService) chatService=std::make_unique<GfxHostTransport>(host,[this] {return signatures;});
        const auto service=ObserveBl2TextChatKeyboard(*chatService,base);
        // Original Stop removes this movie's focus without destroying ChatObj.
        // A stopped same-owner episode is different from an IME/OnPause reset.
        stopChatEpisode(service);
        const bool allowed=current() && frameTarget.direct.movieClass=="TextChatGFxMovie";
        const auto model=allowed?CaptureBl2TextChat({this,copyMemory},base,frameTarget.owner):UiTextChatModel{};
        TextChatInput input;TextChatOwner identity;bool ended=false;
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        std::uint32_t observedType=0,observedCalls=0;
        double observedFocus=-1;int observedVisible=-1;bool observedParent=false;
        std::string observedReason="not-captured";
#endif
        if(model.valid && model.owner==frameTarget.ownerToken && service.known && service.active) {
            if(!chatGeometry) chatGeometry=std::make_unique<GfxHostTransport>(host,[this] {return current();});
            GfxSession session(*chatGeometry,base,frameTarget.interface,chatGeometry->scratch(),512,false);
            const auto chat=session.pin(static_cast<std::uint32_t>(model.chat>>32)+0x3C);
            if(CaptureBl2TextChatInput(session,chat,input)) {
                identity=chatIdentity(model,input,service.activity);
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
                observedType=input.input.type;observedParent=true;observedVisible=1;observedFocus=1;
#endif
            }
            else {
                const auto field=session.member(chat,"chatInput"),parent=session.member(field,"_parent"),
                    shown=session.member(field,"_visible"),focus=session.invoke(field,"__get__focused");
                GfxIdentity expected{},actual{};bool visible=true;double focused=1;
                ended=session.identity(chat,expected) && session.identity(parent,actual) && actual==expected &&
                    ((session.boolean(shown,visible) && !visible) || (session.number(focus,focused) && focused==0)) && session.good();
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
                observedType=session.type(field);observedParent=expected.data && actual==expected;
                if(session.boolean(shown,visible)) observedVisible=visible?1:0;
                session.number(focus,observedFocus);
#endif
            }
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
            observedCalls=session.calls();observedReason=session.reason()?session.reason():"okay";
#endif
        }
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        const auto diagnose=[&](const char* stage,int shown=-1) {
            if(chatDiagnostics>=8) return;
            char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
            if(property[0]!='1' || property[1]) return;
            const auto state=std::to_string(frameTarget.owner)+"/"+stage+"/"+model.reason+"/"+
                std::to_string(service.known)+std::to_string(service.active)+std::to_string(service.open)+std::to_string(service.done)+
                "/"+std::to_string(static_cast<bool>(identity))+"/"+std::to_string(observedType)+"/"+std::to_string(observedFocus)+
                "/"+std::to_string(frame.resetReasons)+"/"+std::to_string(chatAttempts)+"/"+std::to_string(shown);
            if(state==lastChatDiagnostic) return;
            lastChatDiagnostic=state;++chatDiagnostics;
            __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                "chat stage=%s owner=%08X allowed=%u model=%u reason=%s service=(%u,%u,%u,%u,%08X) "
                "captured=%u fieldType=%u parent=%u visible=%d focus=%.0f session=%s calls=%u "
                "bounds=(%.2f,%.2f,%.2f,%.2f) reset=%u attempts=%u pending=%u shown=%d",
                stage,frameTarget.owner,allowed?1u:0u,model.valid?1u:0u,model.reason.c_str(),
                service.known?1u:0u,service.active?1u:0u,service.open?1u:0u,service.done?1u:0u,service.activity,
                identity?1u:0u,observedType,observedParent?1u:0u,observedVisible,observedFocus,
                observedReason.c_str(),observedCalls,input.bounds.left,input.bounds.top,input.bounds.right,input.bounds.bottom,
                frame.resetReasons,chatAttempts,chatKeyboard.pending()?1u:0u,shown);
        };
        diagnose("capture");
#endif
        if(chatKeyboard.pending()) {
            const bool acknowledged=chatKeyboard.acknowledgeModalEpoch(identity,service,frame.resetReasons,monotonicMilliseconds());
            const auto result=chatKeyboard.poll(*chatService,base,identity,service,allowed && static_cast<bool>(identity),
                frame.resetReasons!=0 && !acknowledged,monotonicMilliseconds());
            if(!result) return;
            if(!current() || result->owner!=identity || !model.valid) return;
            if(!result->canceled) {
                GfxSession session(*chatGeometry,base,frameTarget.interface,chatGeometry->scratch(),512,true);
                const auto chat=session.pin(static_cast<std::uint32_t>(model.chat>>32)+0x3C);
                if(!AssignBl2TextChatInput(session,chat,input,result->text)) return;
            }
            const auto keyAt=base+(result->canceled?0x03009FB8u:0x03009FA0u);
            if(name(keyAt)!=(result->canceled?"Escape":"Enter")) return;
            // The original TextChat handler owns Say/cancel. Never call Say
            // directly or dispatch a completion to Character's keyboard route.
            originalQuery(frameTarget.owner,model.input,{frameTarget.controllerId,read(keyAt),read(keyAt+4),0,0xFFFFFFFFu},16);
            if(current()) originalQuery(frameTarget.owner,model.input,
                {frameTarget.controllerId,read(keyAt),read(keyAt+4),1,0xFFFFFFFFu},16);
            return;
        }
        if(ended) {chatOwner={};chatObject=0;chatAttempts=0;chatAttemptAt=0;}
        if(!identity || service.open || frame.resetReasons || physicalOwnership().ownsInput(monotonicMilliseconds())) return;
        // Modal retirement is not a new authored StartTextChat episode. Keep
        // the once-opened input across touch-only epochs; completion itself
        // retains the full strict epoch/scene/generation identity above.
        auto episode=identity;episode.epoch=chatOwner.epoch;episode.scene=chatOwner.scene;episode.generation=chatOwner.generation;
        if(chatOwner!=episode) {chatOwner=identity;chatObject=model.chat;chatAttempts=0;chatAttemptAt=0;}
        const auto now=monotonicMilliseconds();
        if(chatAttempts>=3 || now<chatAttemptAt) return;
        ++chatAttempts;chatAttemptAt=now+750;
        const bool shown=ShowBl2TextChatKeyboard(*chatService,base,chatService->scratch(),
            Bl2RussianLocaleSelected()?"ЧАТ":"CHAT",input.text);
        if(shown && chatKeyboard.opened(identity,now))
            chatAttempts=3;
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        diagnose("show",shown?1:0);
#endif
    }
    bool chatTap(float x,float y) {
        if(!current() || frameTarget.direct.movieClass!="TextChatGFxMovie") return false;
        if(chatKeyboard.pending()) return true;
        const auto model=CaptureBl2TextChat({this,copyMemory},base,frameTarget.owner);
        if(!model.valid) return false;
        if(!chatGeometry) chatGeometry=std::make_unique<GfxHostTransport>(host,[this] {return current();});
        GfxSession session(*chatGeometry,base,frameTarget.interface,chatGeometry->scratch(),512,false);
        const auto chat=session.pin(static_cast<std::uint32_t>(model.chat>>32)+0x3C);TextChatInput input;
        GfxRect frame;GfxViewport viewport;WidgetPoint a{},b{};
        if(!CaptureBl2TextChatInput(session,chat,input) || !session.frameRect(frame) || !session.viewport(viewport) ||
            !NormalizeWidgetPoint(frame,viewport,frameTarget.width,frameTarget.height,{input.bounds.left,input.bounds.top},a) ||
            !NormalizeWidgetPoint(frame,viewport,frameTarget.width,frameTarget.height,{input.bounds.right,input.bounds.bottom},b) ||
            !WidgetContains({a.x,a.y,b.x,b.y},{x,y})) return false;
        chatAttempts=0;chatAttemptAt=0;return true;
    }
    void clearCharacter() {
        characterTarget = {}; characterWasSelected = false;
        characterSession.reset(); characterTransport.reset();
    }
    void clearAgeGate() {
        ageGateGesture=false;ageGateTarget={};ageGateModel={};ageGateOwner={};
        ageGateSession.reset();ageGateTransport.reset();
    }
    bool ageGateOwnerCurrent() const {
        return current() && ageGateModel.valid && targetValid(ageGateOwner) &&
            frameTarget.ownerToken==ageGateOwner.ownerToken && frameTarget.movie==ageGateOwner.movie &&
            frameTarget.interface==ageGateOwner.interface && frameTarget.scene==ageGateOwner.scene &&
            frameTarget.viewport==ageGateOwner.viewport && frameTarget.controllerToken==ageGateOwner.controllerToken;
    }
    bool ageGateCurrent() const {
        if(!ageGateOwnerCurrent()) return false;
        const auto model=CaptureBl2AgeGate({this,copyMemory},base,frameTarget.owner);
        return model.valid && model.owner==ageGateModel.owner && model.clip==ageGateModel.clip &&
            model.structure==ageGateModel.structure && model.input.token==ageGateModel.input.token;
    }
    AgeGateWidgetTarget ageGateHit(GfxSession& session,float x,float y) {
        if(!ageGateCurrent()) return {};
        const auto clip=session.pin(static_cast<std::uint32_t>(ageGateModel.clip>>32)+0x3C);
        return CaptureBl2AgeGateWidget(session,clip,frameTarget.width,frameTarget.height,x,y);
    }
    bool ageGateDown(float x,float y) {
        clearAgeGate();
        if(!current() || frameTarget.direct.movieClass!="GearboxAccountGFxMovie") return false;
        ageGateModel=CaptureBl2AgeGate({this,copyMemory},base,frameTarget.owner);
        if(!ageGateModel.valid) {clearAgeGate();return false;}
        ageGateOwner=frameTarget;
        ageGateTransport=std::make_unique<GfxHostTransport>(host,[this] {return ageGateOwnerCurrent();});
        ageGateSession=std::make_unique<GfxSession>(*ageGateTransport,base,frameTarget.interface,ageGateTransport->scratch(),512,true);
        ageGateTarget=ageGateHit(*ageGateSession,x,y);
        if(!ageGateTarget) {clearAgeGate();return false;}
        ageGateGesture=true;
        // FocusOn/Selection is the original public setter. It cannot submit
        // an age or open the keyboard; only the native released A branch does.
        if(!ageGateCurrent() || !FocusBl2AgeGateWidget(*ageGateSession,ageGateTarget)) ageGateTarget.index=-1;
        return true;
    }
    void ageGateMove(float x,float y) {
        if(!ageGateGesture || !ageGateTarget || !ageGateOwnerCurrent()) {ageGateTarget.index=-1;return;}
        GfxHostTransport transport(host,[this] {return ageGateOwnerCurrent();});
        GfxSession session(transport,base,frameTarget.interface,transport.scratch(),512,false);
        if(!SameBl2AgeGateWidget(ageGateTarget,ageGateHit(session,x,y))) ageGateTarget.index=-1;
    }
    void ageGateUp(float x,float y,bool scrolled) {
        if(!scrolled && ageGateGesture && ageGateTarget && ageGateOwnerCurrent()) {
            GfxHostTransport transport(host,[this] {return ageGateOwnerCurrent();});
            GfxSession session(transport,base,frameTarget.interface,transport.scratch(),512,false);
            const auto fresh=ageGateHit(session,x,y);
            const auto keyAt=base+0x0300A330u;
            if(SameBl2AgeGateWidget(ageGateTarget,fresh) && Bl2AgeGateExclusivelyFocused(session,fresh) &&
                name(keyAt)=="XboxTypeS_A" && ageGateCurrent()) {
                // Match the original typed20-byte HandleInputKey. Pressed0
                // is inert here; recheck focus/owner before Released1 opens
                // this field's original ConsolePlayerInput keyboard.
                originalQuery(frameTarget.owner,ageGateModel.input,
                    {frameTarget.controllerId,read(keyAt),read(keyAt+4),0,0xFFFFFFFFu},16);
                if(ageGateCurrent() && Bl2AgeGateExclusivelyFocused(session,fresh))
                    originalQuery(frameTarget.owner,ageGateModel.input,
                        {frameTarget.controllerId,read(keyAt),read(keyAt+4),1,0xFFFFFFFFu},16);
            }
        }
        clearAgeGate();
    }
    void clearFooter() { footerTarget={};footerSession.reset();footerTransport.reset(); }
    void clearTextScroll() { textScroll={}; }
    void textScrollDown(const PointerEvent& event) {
        clearTextScroll();
        if (!current()) return;
        if (Bl2TextScrollTargets(frameTarget.direct.movieClass,frameTarget.direct.screen).empty()) {
            if (frameTarget.direct.movieClass=="StatusMenuExGFxMovie") {
                TextScrollCapture none;none.reason="text-no-target";textScrollDiagnostic(none,nullptr,event);
            }
            return;
        }
        if (!geometry) geometry=std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession s(*geometry,base,frameTarget.interface,geometry->scratch(),512,false);
        const auto capture=CaptureBl2TextScroll(s,frameTarget.direct.movieClass,frameTarget.direct.screen,
            frameTarget.width,frameTarget.height,event.surfaceX,event.surfaceY);
        textScrollDiagnostic(capture,&s,event);
        if (capture) textScroll={capture,event.pointerId,capture.scroll,event.surfaceX,event.surfaceY,false};
    }
    void textScrollDiagnostic(const TextScrollCapture& capture,const GfxSession* s,const PointerEvent& event) {
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
        if (textScrollLogs>=24) return;
        char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if (property[0]!='1' || property[1]) return;
        ++textScrollLogs;
        const auto& v=capture.metrics;const auto& b=capture.bounds;
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "text-scroll class=%s screen=%d reason=%s session=%s calls=%u point=(%.4f,%.4f) "
            "metrics=(%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f) bounds=(%.4f,%.4f,%.4f,%.4f) scroll=%d/%d line=%.2f",
            frameTarget.direct.movieClass.c_str(),frameTarget.direct.screen,capture.reason,!s ? "none" : s->reason() ? s->reason() : "okay",s ? s->calls() : 0u,
            event.surfaceX,event.surfaceY,v[0],v[1],v[2],v[3],v[4],v[5],v[6],b.left,b.top,b.right,b.bottom,
            capture.scroll,capture.maximum,capture.linePixels);
#else
        (void)capture;(void)s;(void)event;
#endif
    }
    // True once the drag belongs to the description; its transaction is then
    // dropped, so neither a tap nor a list-navigation swipe follows on UP.
    bool textScrollMove(const PointerEvent& event) {
        auto& t=textScroll;
        if (t.pointer!=event.pointerId || (!t.active && !t.capture) || !current()) return t.active;
        const float dx=(event.surfaceX-t.startX)*static_cast<float>(frameTarget.width);
        const float dy=(event.surfaceY-t.startY)*static_cast<float>(frameTarget.height);
        if (!t.active) {
            const float slop=.015f*static_cast<float>(std::min(frameTarget.width,frameTarget.height));
            if (!std::isfinite(dx) || !std::isfinite(dy) || std::abs(dy)<=slop || std::abs(dy)<=std::abs(dx)) return false;
            t.active=true;transactions.clear();
        }
        if (!t.capture) return true;
        const auto line=Bl2TextScrollLine(t.capture.scroll,t.capture.maximum,dy,t.capture.linePixels);
        if (line!=t.line) {
            GfxSession s(*geometry,base,frameTarget.interface,geometry->scratch(),192,true);
            if (ScrollBl2Text(s,t.capture,line)) t.line=line;
            else t.capture={}; // Replaced field: keep owning the drag, change nothing more.
        }
        return true;
    }
    void clearWaypoint() { queuedWaypoint.clear();queuedWaypointOwner={}; }
    std::optional<NativeFooterFamily> footerFamily() const {
        if(frameTarget.capabilities & MapGestures) return NativeFooterFamily::Map;
        if(frameTarget.direct.valid && frameTarget.direct.movieClass=="StatusMenuExGFxMovie" && frameTarget.direct.screen==3)
            return NativeFooterFamily::StatusInventory;
        const auto& cls=frameTarget.direct.movieClass;
        if(cls=="VendingMachineExGFxMovie") return NativeFooterFamily::Vending;
        if(cls=="TwoPanelInterfaceGFxMovie" || cls=="BankGFxMovie" || cls=="StashGFxMovie")
            return NativeFooterFamily::TwoPanel;
        if(cls=="TradingGFxMovie") return NativeFooterFamily::Trading;
        if(frameTarget.direct.trainingPrompt && Bl2TouchTrainingPromptReady({this,copyMemory},base,frameTarget.direct))
            return NativeFooterFamily::Training;
        if(Bl2TouchReviewedUiMovie(cls)) return NativeFooterFamily::Generic;
        return {};
    }
    void scaleFooter(bool force=false) {
        const auto family=footerFamily();
        if(!current() || !family || footerTarget || inventoryGesture.captured() || mapGesture.captured() || itemGesture.captured()) return;
        const auto scale=Bl2InterfaceScale();
        if(footerScaleOwner!=frameTarget.ownerToken || footerScaleGeneration!=frameTarget.generation ||
            footerScaleScreen!=frameTarget.direct.screen || footerScaleFamily!=*family || footerScale!=scale) {
            footerScaleOwner=frameTarget.ownerToken;footerScaleGeneration=frameTarget.generation;
            footerScaleScreen=frameTarget.direct.screen;footerScaleFamily=*family;footerScale=scale;
            footerScaleReady=false;footerScaleAttempts=0;footerScaleAt=0;
        }
        const auto now=monotonicMilliseconds();
        if(!force && (footerScaleReady || footerScaleAttempts>=8 || now<footerScaleAt)) return;
        // Movie initialization may populate the footer after its first frame.
        // Retry briefly; later dynamic hints are checked again before DOWN.
        ++footerScaleAttempts;footerScaleAt=now+250;
        GfxHostTransport transport(host,[this] {return current();});
        GfxSession session(transport,base,frameTarget.interface,transport.scratch(),512,true);
        footerScaleReady=ScaleBl2NativeFooter(session,*family);
    }
    MapLegendTarget footerHit(float x,float y) {
        const auto family=footerFamily();if (!current() || !family) return {};
        if (!geometry) geometry=std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession s(*geometry,base,frameTarget.interface,geometry->scratch(),512,false);
        std::optional<MapLegendObservation> observed;if(footerDiagnosticEnabled()) observed.emplace();
        const auto target=CaptureBl2NativeFooter(s,*family,frameTarget.width,frameTarget.height,x,y,observed?&*observed:nullptr);
        if(observed) footerCaptureDiagnostic("Hit",*observed,s,x,y);
        return target;
    }
    void footerDown(float x,float y) {
        clearFooter();
        const auto family=footerFamily();if (!current() || !family) return;
        footerTransport=std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        footerSession=std::make_unique<GfxSession>(*footerTransport,base,frameTarget.interface,footerTransport->scratch(),512,false);
        std::optional<MapLegendObservation> observed;if(footerDiagnosticEnabled()) observed.emplace();
        footerTarget=CaptureBl2NativeFooter(*footerSession,*family,frameTarget.width,frameTarget.height,x,y,observed?&*observed:nullptr);
        if(observed) footerCaptureDiagnostic("Down",*observed,*footerSession,x,y);
        if (footerTarget) mapDiagnostic("Legend-Down",{},0,0,x,y,footerTarget.bounds,false);
        if (!footerTarget) clearFooter();
    }
    CharacterWidgetTarget characterHit(float x,float y) {
        if (!current() || frameTarget.direct.movieClass != "CharacterSelectionReduxGFxMovie") return {};
        if (!geometry) geometry = std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession s(*geometry,base,frameTarget.interface,geometry->scratch(),512,false);
        const auto content = s.pin(static_cast<std::uint32_t>(frameTarget.direct.clip >> 32) + 0x3C);
        std::optional<CharacterWidgetObservation> observed;
        if(characterCaptureDiagnosticEnabled()) observed.emplace();
        const auto target=CaptureBl2CharacterWidget(s,content,frameTarget.direct.screen,frameTarget.direct.player,
            frameTarget.width,frameTarget.height,x,y,observed?&*observed:nullptr);
        if(observed) characterCaptureDiagnostic("Hit",*observed,target,s,x,y);
        return target;
    }
    void characterDown(float x,float y) {
        clearCharacter();
        if (!current() || frameTarget.direct.movieClass != "CharacterSelectionReduxGFxMovie") return;
        characterTransport = std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        characterSession = std::make_unique<GfxSession>(*characterTransport,base,frameTarget.interface,
            characterTransport->scratch(),512,true);
        const auto content = characterSession->pin(static_cast<std::uint32_t>(frameTarget.direct.clip >> 32) + 0x3C);
        std::optional<CharacterWidgetObservation> observed;
        if(characterCaptureDiagnosticEnabled()) observed.emplace();
        characterTarget = CaptureBl2CharacterWidget(*characterSession,content,frameTarget.direct.screen,
            frameTarget.direct.player,frameTarget.width,frameTarget.height,x,y,observed?&*observed:nullptr);
        if(observed) characterCaptureDiagnostic("Down",*observed,characterTarget,*characterSession,x,y);
        if (!characterTarget) { clearCharacter();return; }
        if (characterTarget.kind == CharacterWidgetKind::Portrait) {
            std::int32_t selected = -1;
            if (!Bl2TouchDirectUiCurrent({this,copyMemory},base,frameTarget.direct,&selected)) { clearCharacter();return; }
            characterWasSelected = selected == characterTarget.index;
            if (!originalMethod(frameTarget.owner,frameTarget.direct.select,
                {frameTarget.direct.player,static_cast<std::uint32_t>(characterTarget.index)})) clearCharacter();
        } else {
            const bool selected=SelectBl2CharacterWidget(*characterSession,characterTarget);
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
            if(observed) __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                "character selection screen=%d kind=%u index=%d okay=%u session=%s calls=%u",
                frameTarget.direct.screen,static_cast<unsigned>(characterTarget.kind),characterTarget.index,selected?1u:0u,
                characterSession->reason()?characterSession->reason():"okay",characterSession->calls());
#endif
            if(!selected) clearCharacter();
        }
        if (characterTarget && characterInputs++ < 24) HORN_LOGI("BL2Bridge",
            "native-ui character Down kind=%u index=%d previouslySelected=%u bounds=(%.6f,%.6f,%.6f,%.6f)",
            static_cast<unsigned>(characterTarget.kind),characterTarget.index,characterWasSelected ? 1u : 0u,
            characterTarget.bounds.left,characterTarget.bounds.top,characterTarget.bounds.right,characterTarget.bounds.bottom);
    }
    void characterUp(float x,float y) {
        const auto fresh = characterHit(x,y);
        if (!characterTarget || !characterSession || !current() ||
            !SameBl2CharacterWidget(characterTarget,fresh)) { clearCharacter();return; }
        const auto index = static_cast<std::uint32_t>(characterTarget.index);
        if (characterTarget.kind == CharacterWidgetKind::Portrait) {
            std::int32_t selected = -1;
            if (Bl2TouchDirectUiCurrent({this,copyMemory},base,frameTarget.direct,&selected) &&
                ConfirmBl2CharacterPortrait(characterTarget,fresh,characterWasSelected,selected))
                originalMethod(frameTarget.owner,frameTarget.direct.click,{frameTarget.controllerId,index});
        } else if (characterTarget.kind == CharacterWidgetKind::Menu) {
            originalMethod(frameTarget.owner,frameTarget.direct.menuClick,{frameTarget.direct.player,index});
        } else {
            characterSession->beginInputBoundary();
            ClickBl2CharacterCustomization(*characterSession,characterTarget);
        }
        clearCharacter();
    }
    int dialogHit(float x,float y) {
        if (!current() || !Bl2TouchNativeDialogMovie(frameTarget.direct.movieClass)) return -1;
        if (!geometry) geometry = std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession s(*geometry,base,frameTarget.interface,geometry->scratch(),384,false);
        GfxRect frame; GfxViewport viewport;
        if (!s.good() || !s.frameRect(frame) || !s.viewport(viewport)) return -1;
        int hit = -1;
        for (int i = 0; i < frameTarget.direct.screen; ++i) {
            const auto path = "button" + std::to_string(i);
            auto button = s.variable(path.c_str()); GfxRect bounds; WidgetPoint a,b;
            bool visible = false,disabled = true;
            auto shown = s.member(button,"_visible"), blocked = s.member(button,"disabled");
            const bool available = s.boolean(shown,visible) && visible && s.boolean(blocked,disabled,true) && !disabled;
            s.release(blocked);s.release(shown);
            const bool mapped = available && s.projectedBounds(button,bounds) &&
                NormalizeWidgetPoint(frame,viewport,frameTarget.width,frameTarget.height,{bounds.left,bounds.top},a) &&
                NormalizeWidgetPoint(frame,viewport,frameTarget.width,frameTarget.height,{bounds.right,bounds.bottom},b);
            s.release(button);
            if (mapped && WidgetContains({a.x,a.y,b.x,b.y},{x,y})) {
                if (hit >= 0) return -1;
                // Original GetLogicalIndex/GetWidgetIndex reverse Buttons.
                hit = frameTarget.direct.screen - 1 - i;
            }
        }
        return hit;
    }
    bool mapHit(float x,float y) {
        if (!current() || !frameTarget.direct.mask) return false;
        if (!geometry) geometry = std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession s(*geometry,base,frameTarget.interface,geometry->scratch(),192,false);
        auto mask = s.pin(frameTarget.direct.mask + 0x3C); GfxRect bounds,frame; GfxViewport viewport; WidgetPoint a,b;
        const bool okay = s.good() && s.projectedBounds(mask,bounds) && s.frameRect(frame) && s.viewport(viewport) &&
            NormalizeWidgetPoint(frame,viewport,frameTarget.width,frameTarget.height,{bounds.left,bounds.top},a) &&
            NormalizeWidgetPoint(frame,viewport,frameTarget.width,frameTarget.height,{bounds.right,bounds.bottom},b) &&
            WidgetContains({a.x,a.y,b.x,b.y},{x,y}); s.release(mask);return okay;
    }
    bool mapKeyFor(const Target& target,bool down,bool cleanup = false) {
        if (!(cleanup ? !down && targetValid(target,false) : current()) || !(target.capabilities & MapGestures)) return false;
        // The original native InputKey accepts this registered FName; search
        // the name table once, not a fabricated mouse key numeric constant.
        std::uint32_t names = 0,count = 0;
        if (!array(base + 0x02FB00C8,2000000,names,count)) return false;
        const auto matches = [&](std::uint32_t index) {
            if (index >= count) return false;
            const auto entry = read(names + index*4);
            if (!span(entry + 8,16)) return false;
            char text[16]{}; copyMemory(this,entry + 8,text,16);
            return std::memcmp(text,"LeftMouseButton",16) == 0;
        };
        std::uint32_t index = mouseName;
        if (!matches(index)) for (index = 0; index < count; ++index) {
            const auto entry = read(names + index*4);
            if (!span(entry + 8,16)) continue;
            char text[16]{}; copyMemory(this,entry + 8,text,16);
            if (std::memcmp(text,"LeftMouseButton",16) == 0) break;
        }
        if (index == count) return false;
        mouseName = index;
#if defined(BL2_NATIVE_ARM32)
        host.RunGuestCallback(base + kMovieKey,{target.gfx,target.controllerId,target.movie,index,0,down ? 0u : 1u});
#else
        if(!call(base + kMovieKey,{target.gfx,target.controllerId,target.movie,index,0,down ? 0u : 1u})) return false;
#endif
        mapDragHeld = down; return true;
    }
    bool mapKey(bool down) { return mapKeyFor(frameTarget,down); }
    void releaseDrag() {
        clearWaypoint();
        // Screen changes may retire the old direct model while its exact
        // native movie is still focused. Release only that owned drag; a dead
        // movie or changed GC/world is never called during cleanup.
        const auto target = frameTarget;
        if (mapDragHeld) {
            mapKeyFor(target,false,true);
            originalMethodFor(target,target.owner,target.direct.stopDrag,{},true);
        }
        if (itemDragHeld) originalMethodFor(target,target.owner,target.direct.stopDrag,{0},true);
        mapDragHeld = itemDragHeld = false;
        mapGesture.clear();itemGesture.clear();
    }
    void characterDiagnostic(const Target& target) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        char property[PROP_VALUE_MAX]{};
        __system_property_get("debug.bl2bridge.touch_diagnostics",property);
        if (property[0] != '1' || property[1] || characterProbes >= 4 ||
            characterProbeOwner == target.ownerToken || target.direct.movieClass != "CharacterSelectionReduxGFxMovie") return;
        characterProbeOwner = target.ownerToken; ++characterProbes;
        const auto source = Bl2TouchMovieSource({this,copyMemory},base,target.owner);
        GfxHostTransport transport(host,[&] { return targetValid(target); });
        GfxSession session(transport,base,target.interface,transport.scratch(),192,false);
        auto root = session.variable("_root");
        auto clip = session.pin(static_cast<std::uint32_t>(target.direct.clip >> 32) + 0x3C);
        const auto text = [&](GfxHandle object,const char* key) {
            auto field = session.member(object,key); std::string result;
            session.string(field,result,256); session.release(field); return result;
        };
        const auto url = text(root,"_url"), path = text(clip,"_target"), clipName = text(clip,"_name");
        GfxRect bounds{}; const bool measured = session.projectedBounds(clip,bounds);
        __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
            "character source=%s url=%s clip=%s name=%s state=%d selected=%d measured=%u bounds=(%.2f,%.2f,%.2f,%.2f) session=%s",
            source.c_str(),url.c_str(),path.c_str(),clipName.c_str(),target.direct.screen,target.direct.selected,
            measured ? 1u : 0u,bounds.left,bounds.top,bounds.right,bounds.bottom,session.reason() ? session.reason() : "okay");
        session.release(clip); session.release(root);
#else
        (void)target;
#endif
    }
    void mapUpdate(const UiMapUpdate& u) {
        if (!current()) return;
        mouse(1,std::floor(u.x*(frameTarget.width-1)),std::floor(u.y*(frameTarget.height-1)));
        if (u.stop && mapDragHeld) mapKey(false);
        if (u.start) mapKey(true);
        const auto& d = frameTarget.direct;
        if (mapDragHeld && (u.dx || u.dy)) originalMethod(frameTarget.owner,d.drag,
            {static_cast<std::uint32_t>(u.dx),static_cast<std::uint32_t>(u.dy)});
        if (u.zoom) {
            const auto state=CaptureBl2MapState({this,copyMemory},base,d);
            const auto delta=state.valid ? Bl2MapZoomDelta(u.zoom,state.zoomSpeed,state.deltaSeconds) : std::nullopt;
            const bool dispatched=delta && current() && originalMethod(frameTarget.owner,d.zoom,{floatWord(*delta),0});
            mapDiagnostic("pinch",state,u.zoom,delta ? *delta : 0,0,0,{},dispatched);
        }
        if (u.waypoint) {
            waypoint(u.x,u.y,true);
        }
    }
    void waypoint(float x,float y,bool mayQueue) {
        if (!current() || !(frameTarget.capabilities & MapGestures)) return;
        const auto target=frameTarget;
        auto state=CaptureBl2MapState({this,copyMemory},base,target.direct);
        if (!state.valid) { mapDiagnostic("waypoint",state,0,0,x,y,{},false);return; }
        if (!geometry) geometry=std::make_unique<GfxHostTransport>(host,[this] { return current(); });
        GfxSession session(*geometry,base,target.interface,geometry->scratch(),192,false);
        const auto map=session.pin(static_cast<std::uint32_t>(target.direct.clip>>32)+0x3C);
        const auto mask=session.pin(target.direct.mask+0x3C);
        const auto point=CaptureBl2MapCursor(session,map,mask,target.width,target.height,x,y);
        session.release(mask);session.release(map);
        if (!point.valid) {
            state.reason=point.reason;
            mapDiagnostic("waypoint",state,0,0,point.local.x,point.local.y,{},false);
            if (point.mousePending && mayQueue && current()) {
                queuedWaypoint.store({x,y,0},epoch,target.scene,monotonicMilliseconds());queuedWaypointOwner=target;
            }
            return;
        }
        // Capture uses the native inherited3D inverse and a forward roundtrip.
        // Revalidate the exact reflected Vector2D before its UI-only write.
        const auto fresh=CaptureBl2MapState({this,copyMemory},base,target.direct);
        const auto object=static_cast<std::uint32_t>(target.direct.clip>>32);
        if (!current() || !fresh.valid || fresh.map!=state.map || fresh.world!=state.world ||
            fresh.cursorProperty!=state.cursorProperty || fresh.cursorOffset!=state.cursorOffset ||
            fresh.cursorX!=state.cursorX || fresh.cursorY!=state.cursorY ||
            std::abs(point.local.x)*2>fresh.clipWidth || std::abs(point.local.y)*2>fresh.clipHeight ||
            !span(object+fresh.cursorOffset,8)) { state.reason="map-cursor-revalidation";mapDiagnostic("waypoint",state);return; }
        host.MemoryWrite32(object+fresh.cursorOffset,floatWord(static_cast<float>(point.local.x)));
        host.MemoryWrite32(object+fresh.cursorOffset+4,floatWord(static_cast<float>(point.local.y)));
        const bool dispatched=current() && originalMethod(object,target.direct.info,{}) && current() &&
            originalMethod(object,target.direct.waypoint,{});
        state.reason=dispatched ? "map-native-cursor-placed" : "map-native-cursor-command-rejected";
        mapDiagnostic("waypoint",state,0,0,point.local.x,point.local.y,{},dispatched);
    }
    void itemUpdate(const UiMapUpdate& u) {
        if (!current()) return;
        mouse(1,std::floor(u.x*(frameTarget.width-1)),std::floor(u.y*(frameTarget.height-1)));
        const auto& d = frameTarget.direct;
        if (u.stop && itemDragHeld) { originalMethod(frameTarget.owner,d.stopDrag,{0});itemDragHeld = false; }
        if (u.start) itemDragHeld = originalMethod(frameTarget.owner,d.startDrag,{0});
        if (itemDragHeld && (u.dx || u.dy)) originalMethod(frameTarget.owner,d.drag,
            {static_cast<std::uint32_t>(u.dx),static_cast<std::uint32_t>(u.dy)});
        // Item inspection has no pinch/waypoint command. The reused gesture
        // cancels its held rotation when a second finger is introduced.
    }
    void tap(const UiTap& point) {
        if (!current() || !(frameTarget.capabilities & kUiTapTest)) return;
        const auto target = frameTarget;
        if(ageGateGesture) {ageGateUp(point.x,point.y,false);return;}
        if (footerTarget) {
            bool dispatched=false;
            if(SameBl2MapLegend(footerTarget,footerHit(point.x,point.y))) {
                switch(footerTarget.command) {
                case NativeFooterCommand::Legend:
                    dispatched=originalMethod(static_cast<std::uint32_t>(target.direct.clip>>32),target.direct.legend,{});break;
                case NativeFooterCommand::TrainingAccept:
                    if(Bl2TouchTrainingPromptReady({this,copyMemory},base,target.direct)) {
                        const auto keyAt=base+(target.direct.screen==0?0x03009FA0u:0x0300A320u);
                        if(name(keyAt)!=(target.direct.screen==0?"Enter":"XboxTypeS_Back")) break;
                        originalQuery(target.owner,target.direct.click,{target.controllerId,read(keyAt),read(keyAt+4),0,0xFFFFFFFFu},16);
                        if(current() && Bl2TouchTrainingPromptReady({this,copyMemory},base,target.direct)) {
                            dispatched=true;
                            originalQuery(target.owner,target.direct.click,{target.controllerId,read(keyAt),read(keyAt+4),1,0xFFFFFFFFu},16);
                        }
                    }
                    break;
                default:dispatched=footerKey(footerTarget.command);break;
                }
            }
            mapDiagnostic("Legend-Up",{},0,0,point.x,point.y,footerTarget.bounds,dispatched);
            clearFooter();return;
        }
        const float x = std::floor(point.x * static_cast<float>(target.width - 1));
        const float y = std::floor(point.y * static_cast<float>(target.height - 1));
        if (target.direct.movieClass == "CharacterSelectionReduxGFxMovie" && target.direct.screen >= 1 && target.direct.screen <= 3) {
            characterUp(point.x,point.y);
            return;
        }
        if (Bl2TouchNativeDialogMovie(target.direct.movieClass)) {
            if (dialogAtDown >= 0 && dialogHit(point.x,point.y) == dialogAtDown)
                originalMethod(target.owner,target.direct.click,{static_cast<std::uint32_t>(dialogAtDown),target.controllerId});
            return;
        }
        if(chatTap(point.x,point.y)) return;
        // No guest DOWN survives an Android gesture. Cancellation before this
        // commit has zero guest effects. Never call generic InputTouch as well.
        [[maybe_unused]] const bool complete = mouse(1,x,y) && mouse(2,x,y) && mouse(3,x,y);
        if (logs++ < 24) HORN_LOGI("BL2Bridge", "native-ui tap movie=%08X pixel=(%.0f,%.0f) size=%ux%u complete=%u",
            target.movie,x,y,target.width,target.height,complete ? 1u : 0u);
        networkDiagnostic(target,complete ? "tap" : "tap-cancelled");
    }
};

Ue3UiSink::Ue3UiSink(DynarmicHost& host, LifetimeGeneration generation, bool tapTest)
    : state_(std::make_unique<State>(host,generation,tapTest)) {}
Ue3UiSink::~Ue3UiSink() = default;
bool Ue3UiSink::verified() const { return state_->signatures; }
std::uint32_t Ue3UiSink::prepare(std::uint32_t viewport, std::uint32_t controller,
    std::uint32_t movie, std::uint32_t owner, std::uint64_t scene) {
    auto& s = *state_;
    if ((s.inventorySession || s.inventoryPressed) && (s.inventoryOwner.owner != owner ||
        s.inventoryOwner.scene != scene || s.inventoryOwner.viewport != viewport ||
        s.inventoryOwner.controller != controller || !s.targetValid(s.inventoryOwner,false))) {
        s.releaseInventory();
        if(!s.targetValid(s.inventoryOwner,false)) s.clearInventory();
    }
    if (s.characterSession && (s.frameTarget.owner != owner || s.frameTarget.scene != scene ||
        s.frameTarget.viewport != viewport || s.frameTarget.controller != controller || !s.targetValid(s.frameTarget))) s.clearCharacter();
    if (s.ageGateSession && (s.ageGateOwner.owner != owner || s.ageGateOwner.scene != scene ||
        s.ageGateOwner.viewport != viewport || s.ageGateOwner.controller != controller || !s.targetValid(s.ageGateOwner))) s.clearAgeGate();
    if ((s.mapDragHeld || s.itemDragHeld) && (s.frameTarget.owner != owner || s.frameTarget.scene != scene ||
        s.frameTarget.viewport != viewport || s.frameTarget.controller != controller || !s.targetValid(s.frameTarget))) s.releaseDrag();
    s.preparedValid = false;
    s.synchronizeFps();
    const auto reject = [&](const char* reason) { s.admission(viewport,movie,owner,reason,0); return 0u; };
    if (!s.signatures || !s.span(owner,0x23C) || !s.token(owner)) return reject("signature-or-owner");
    const auto cls = s.read(owner + 0x34);
    if (!s.token(cls)) return reject("owner-class");
    const auto name = s.name(cls + 0x2C);
    // Reviewed original interactive movie families, never an arbitrary focused
    // HUD/helper or service overlay without its shipped input contract.
    const bool directClass = name == "CharacterSelectionReduxGFxMovie" || name == "StatusMenuExGFxMovie";
    const bool additionalScreen = Bl2TouchReviewedUiMovie(name);
    if (name != "FrontendGFxMovie" && name != "PauseGFxMovie" &&
        name != "OptionsGFxMovie" && name != "WillowGFxDialogBox" &&
        name != "NetworkOptionsGFxMovie" && name != "WillowGFxMoviePressStart" && !directClass && !additionalScreen)
        return reject((std::string("class-not-reviewed/") + name).c_str());
    Target t;
    t.direct.movieClass=name;
    t.viewport = viewport; t.controller = controller; t.movie = movie; t.owner = owner; t.scene = scene;
    t.network = name == "NetworkOptionsGFxMovie";
    t.controllerToken = s.token(controller); t.ownerToken = s.token(owner);
    t.world = s.read(s.base + 0x0300D940); t.gfx = s.read(s.base + 0x0301F840);
    t.generation = s.generation(); t.width = s.read(viewport + 0x3C); t.height = s.read(viewport + 0x40);
    if (!t.width || !t.height || t.width > 16384 || t.height > 16384) return reject("viewport-size");
    std::uint32_t players = 0, count = 0;
    const auto engine = s.read(s.base + 0x03008D20);
    if (!s.token(engine) || !s.array(engine + 0x48C,4,players,count) || count != 1) return reject("local-player-count");
    const auto local = s.read(players);
    if (!s.token(local)) return reject("local-player");
    t.controllerId = s.read(local + 0x64);
    if (t.controllerId > 3) return reject("controller-id");
    t.interface = s.read(movie + 0x34); t.handler = s.read(s.read(t.interface) + 0x90);
    if (!s.targetValid(t)) return reject("movie-focus-lifetime");
    if (directClass || name == "ItemInspectionGFxMovie" || Bl2TouchNativeDialogMovie(name)) {
        t.direct = CaptureBl2DirectUi({&s,State::copyMemory},s.base,owner,0);
        // Observe the original movie/clip even when an optional reflected
        // handler rejects; the independently checked native focus owns this read.
        s.characterDiagnostic(t);
        if (!t.direct.valid) return reject(t.direct.reason.empty() ? "direct-ui-schema" : t.direct.reason.c_str());
    }
    if (name == "WillowGFxMoviePressStart" && s.advanceStartTitle(t)) return reject("title-advanced");
    for (const auto& key : keys) if (s.name(s.base + key.rva) != key.name) return reject("key-name");
    t.capabilities = kUiNavigation;
    // Network's shipped six buttons already have click/focus/rollover handlers.
    // Its controller-only Start/extOnLoad do not enable the mouseable path.
    // Use the original verified setter for this exact focused movie only.
    if ((t.network || directClass || additionalScreen) && s.tapTest && s.read(t.gfx + 0x44) == viewport + 4 && s.read(owner + 0x238) != 1 &&
        !physicalOwnership().ownsInput(monotonicMilliseconds())) {
#if defined(BL2_NATIVE_ARM32)
        s.host.RunGuestCallback(s.base + 0x01C074F4,{owner});
#else
        if(!s.call(s.base + 0x01C074F4,{owner})) return reject("guest-call-unavailable");
#endif
        if (!s.targetValid(t)) return reject("network-movie-changed");
    }
    // FGFxEngine stores the FViewport interface, complete FKdViewport+4. Admit
    // absolute taps only when this exact coordinate owner is the rendered one.
    if (s.tapTest && s.read(t.gfx + 0x44) == viewport + 4 && s.read(owner + 0x238) == 1 &&
        !(s.host.MemoryRead8(owner + 0xCA) & 2u)) {
        if (!s.scratch) s.scratch = s.host.GuestAlloc(32,8);
        if (s.scratch) t.capabilities |= kUiTapTest;
    }
    if ((t.capabilities & kUiTapTest) && t.direct.valid && t.direct.movieClass == "StatusMenuExGFxMovie" && t.direct.screen == 2)
        t.capabilities |= MapGestures;
    if (t.network && (s.prepared.movie != t.movie || s.prepared.capabilities != t.capabilities))
        s.networkDiagnostic(t,"admitted");
    s.prepared = t; s.preparedValid = true;
    s.characterDiagnostic(t);
    s.admission(viewport,movie,owner,name,t.capabilities);
    return t.capabilities;
}
void Ue3UiSink::beginTouchFrame(const Frame& frame) {
    auto& s = *state_;
    if (frame.resetReasons || frame.epoch != s.epoch || frame.context.sceneIdentity != s.frameTarget.scene) {
        s.releaseInventory();
        s.releaseDrag();s.clearCharacter();s.clearAgeGate();s.clearFooter();s.clearTextScroll();
    }
    s.epoch = frame.epoch; s.frameTarget = s.prepared;
    s.frameValid = s.preparedValid && frame.context.sceneIdentity == s.prepared.scene &&
        (frame.context.capabilities & kUiNavigation) && !(frame.context.capabilities & PhysicalOwnsInput);
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
    if(frame.fresh) s.qaTrainingTab();
#endif
    if(frame.fresh) s.scaleFooter();
    s.pollChat(frame);
    s.transactions.begin(frame.epoch,frame.context.sceneIdentity,s.current(),frame.resetReasons != 0,
        s.frameTarget.width,s.frameTarget.height,s.frameTarget.network
        || ((s.frameTarget.direct.movieClass != "CharacterSelectionReduxGFxMovie" || s.frameTarget.direct.screen == 3) &&
            !Bl2TouchNativeDialogMovie(s.frameTarget.direct.movieClass))
    );
    s.mapGesture.begin(frame.epoch,frame.context.sceneIdentity,s.current() && (frame.context.capabilities & MapGestures),
        frame.resetReasons != 0,s.frameTarget.width,s.frameTarget.height);
    s.itemGesture.begin(frame.epoch,frame.context.sceneIdentity,s.current() && s.frameTarget.direct.movieClass == "ItemInspectionGFxMovie",
        frame.resetReasons != 0,s.frameTarget.width,s.frameTarget.height);
    if(frame.fresh) {
        if(const auto update=s.inventoryGesture.begin(s.inventoryIdentity(),s.current(),frame.resetReasons!=0,monotonicMilliseconds()))
            s.inventoryUpdate(*update);
        if((s.inventoryGesture.captured() || s.inventoryFinal) && s.inventoryTarget && s.current()) {
            s.inventoryFresh=s.freshInventorySource();
            if(!s.inventoryFresh) s.releaseInventory();
            else s.inventoryHoverBoundary=true;
        }
        if(s.inventoryFinal) {
            auto update=*s.inventoryFinal;
            if(!s.inventoryFresh) update.command=InventoryDragCommand::Cancel;
            s.finishInventory(update);
        }
    }
    if (frame.resetReasons) { s.dialogAtDown = -1;s.mapDragHeld = false; }
    if (frame.fresh && s.queuedWaypoint.pending()) {
        const auto& previous=s.queuedWaypointOwner;
        bool uninterrupted=true;
        for (std::size_t i=0;i<frame.actionEdgeCount;++i) if(frame.actionEdges[i].down) uninterrupted=false;
        for (std::size_t i=0;i<frame.pointerEventCount;++i)
            if(frame.pointerEvents[i].phase==PointerPhase::Down || frame.pointerEvents[i].phase==PointerPhase::Cancel) uninterrupted=false;
        const bool same=uninterrupted && s.current() && s.targetValid(previous) &&
            previous.movie==s.frameTarget.movie && previous.interface==s.frameTarget.interface &&
            previous.ownerToken==s.frameTarget.ownerToken && previous.controllerToken==s.frameTarget.controllerToken &&
            previous.scene==s.frameTarget.scene && previous.generation==s.frameTarget.generation &&
            previous.viewport==s.frameTarget.viewport && previous.width==s.frameTarget.width && previous.height==s.frameTarget.height &&
            previous.direct.clip==s.frameTarget.direct.clip && previous.direct.mask==s.frameTarget.direct.mask &&
            (s.frameTarget.capabilities & MapGestures);
        const auto point=s.queuedWaypoint.take(frame.epoch,frame.context.sceneIdentity,monotonicMilliseconds(),same);
        s.queuedWaypointOwner={};
        if(point) s.waypoint(point->x,point->y,false);
    }
}
void Ue3UiSink::action(const ActionEdge& edge) {
    auto& s = *state_;
    if (edge.down && edge.sourceEpoch == s.epoch) { s.releaseInventory();s.releaseDrag();s.clearCharacter();s.clearAgeGate();s.clearFooter();s.clearTextScroll(); }
    const bool current=s.current();
    s.inventoryDiagnostic(edge.down?"action-down":"action-up",current?"current":"current-rejected",
        nullptr,nullptr,nullptr,nullptr,-1,-1,static_cast<int>(edge.action));
    if (!current) { s.transactions.clear(); return; }
    if (const auto command = s.transactions.action(edge)) {
        s.inventoryDiagnostic("action-command","original-key",nullptr,nullptr,nullptr,nullptr,-1,-1,static_cast<int>(*command));
        s.key(*command);
    }
}
void Ue3UiSink::setFooterActionHandler(std::function<bool(const ActionEdge&)> handler) {
    state_->footerAction=std::move(handler);
}
bool Ue3UiSink::footerPointer(const PointerEvent& event,bool allowCapture) {
    auto& s=*state_;
    if(event.phase==PointerPhase::Hover || event.sourceEpoch!=s.epoch || !s.current() ||
        !(s.frameTarget.capabilities & kUiTapTest)) return false;
    if(allowCapture && !s.footerTarget && event.phase==PointerPhase::Down) {
        s.scaleFooter(true);s.footerDown(event.surfaceX,event.surfaceY);
    }
    if(!s.footerTarget) return false;
    pointer(event);return true;
}
void Ue3UiSink::pointer(const PointerEvent& event) {
    auto& s = *state_;
    if (event.phase == PointerPhase::Hover) { hover(event); return; }
    const bool current=s.current();
    if(event.phase!=PointerPhase::Move)
        s.inventoryDiagnostic(event.phase==PointerPhase::Down?"pointer-down":event.phase==PointerPhase::Up?"pointer-up":"pointer-cancel",
            !current?"current-rejected":!(s.frameTarget.capabilities & kUiTapTest)?"tap-capability-rejected":s.footerTarget?"footer-captured":"current",
            nullptr,nullptr,nullptr,nullptr,event.surfaceX,event.surfaceY,static_cast<int>(event.pointerId));
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
    s.characterRouteDiagnostic(event,false,current,s.transactions.ownsPointer(event.pointerId),false);
    s.footerRouteDiagnostic(event,current);
#endif
    if (!current || !(s.frameTarget.capabilities & kUiTapTest)) { s.releaseInventory();s.releaseDrag();s.clearCharacter();s.clearAgeGate();s.clearFooter();s.clearTextScroll();s.transactions.clear(); return; }
    if(event.phase==PointerPhase::Down || event.phase==PointerPhase::Cancel) s.clearWaypoint();
    if(event.phase==PointerPhase::Down) s.scaleFooter(true);
    if(event.phase==PointerPhase::Down && !s.mapGesture.captured() && !s.inventoryGesture.captured() && !s.footerTarget)
        s.footerDown(event.surfaceX,event.surfaceY);
    if (s.frameTarget.capabilities & MapGestures) {
        const bool inside = s.mapHit(event.surfaceX,event.surfaceY);
        if (s.mapGesture.captured() || (!s.footerTarget && event.phase == PointerPhase::Down && inside)) {
            s.transactions.clear();
            if (const auto update = s.mapGesture.pointer(event,inside)) s.mapUpdate(*update);
            return;
        }
    }
    if(event.phase==PointerPhase::Down && s.inventoryFinal) {
        s.releaseInventory();
        if(s.inventoryFinal) return; // Finish original queued capture first.
    }
    if(s.inventoryGesture.captured() || (!s.footerTarget && event.phase==PointerPhase::Down && s.inventoryDown(event.surfaceX,event.surfaceY))) {
        s.transactions.clear();
        if(const auto update=s.inventoryGesture.pointer(event,s.inventoryFresh)) s.inventoryUpdate(*update,event.phase==PointerPhase::Move);
        else if(s.inventoryGesture.ownsPointer(event.pointerId) && !s.inventoryGesture.held() &&
            (event.phase==PointerPhase::Down || event.phase==PointerPhase::Move)) hover(event);
        if(!s.inventoryGesture.captured() && !s.inventoryPressed && !s.inventoryFinal) s.clearInventory();
        return;
    }
    if (s.frameTarget.direct.movieClass == "ItemInspectionGFxMovie") {
        s.transactions.clear();
        if (const auto update = s.itemGesture.pointer(event,true)) s.itemUpdate(*update);
        return;
    }
    if (event.phase == PointerPhase::Down) {
        // Track only; the DOWN still reaches the transaction for an ordinary tap.
        if (!s.textScroll.active && !s.footerTarget) s.textScrollDown(event);
    } else if (s.textScroll.pointer == event.pointerId) {
        if (event.phase == PointerPhase::Move && s.textScrollMove(event)) return;
        if (event.phase == PointerPhase::Up || event.phase == PointerPhase::Cancel) {
            const bool owned = s.textScroll.active;
            s.clearTextScroll();
            if (owned) return;
        }
    }
    if (event.phase == PointerPhase::Down) {
        s.dialogAtDown = -1;
    }
    const auto tap = s.transactions.pointer(event);
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
    s.characterRouteDiagnostic(event,true,current,s.transactions.ownsPointer(event.pointerId),tap.has_value());
#endif
    if (s.transactions.ownsPointer(event.pointerId) && (event.phase == PointerPhase::Down || event.phase == PointerPhase::Move)) {
        if (s.frameTarget.direct.movieClass == "CharacterSelectionReduxGFxMovie" && s.frameTarget.direct.screen >= 1 && s.frameTarget.direct.screen <= 3) {
            if (event.phase == PointerPhase::Down) s.characterDown(event.surfaceX,event.surfaceY);
            else if (s.characterTarget && !SameBl2CharacterWidget(s.characterTarget,s.characterHit(event.surfaceX,event.surfaceY))) s.clearCharacter();
        } else if(s.frameTarget.direct.movieClass=="GearboxAccountGFxMovie" &&
            (s.ageGateGesture || (event.phase==PointerPhase::Down && s.ageGateDown(event.surfaceX,event.surfaceY)))) {
            if(event.phase==PointerPhase::Move) s.ageGateMove(event.surfaceX,event.surfaceY);
        } else if(s.frameTarget.direct.trainingPrompt) { if (!s.transactions.dragging()) hover(event); }
        else if (Bl2TouchNativeDialogMovie(s.frameTarget.direct.movieClass)) {
            const auto hit = s.dialogHit(event.surfaceX,event.surfaceY);
            if (hit >= 0) { s.dialogAtDown = hit;s.originalMethod(s.frameTarget.owner,s.frameTarget.direct.hover,{static_cast<std::uint32_t>(hit)}); }
        } else if (!s.transactions.dragging()) hover(event); // A rollover would reselect rows under a scrolling finger.
    }
    if (tap) {
        if (tap->scrollDirection) s.scroll(tap->scrollDirection);
        else if (tap->tabDirection) s.tab(tap->tabDirection);
        else s.tap(*tap);
    }
    if (!s.transactions.ownsPointer(event.pointerId)) { s.clearCharacter();s.clearAgeGate();s.clearFooter(); }
}
void Ue3UiSink::hover(const PointerEvent& event) {
    auto& s = *state_;
    // A mouse cursor (Hover) highlights like the PC game's own cursor, but
    // never moves the movie's mouse under a finger, drag, footer or gesture.
    if (event.phase == PointerPhase::Hover && (!s.current() || !(s.frameTarget.capabilities & kUiTapTest) ||
        s.transactions.engaged() || s.mapGesture.captured() || s.itemGesture.captured() ||
        s.inventoryGesture.captured() || s.inventoryPressed || s.inventoryFinal || s.footerTarget ||
        s.ageGateGesture || s.textScroll.active || s.characterTarget)) return;
    if (event.sourceEpoch == s.epoch && (event.phase == PointerPhase::Down || event.phase == PointerPhase::Move ||
        event.phase == PointerPhase::Hover) &&
        std::isfinite(event.surfaceX) && std::isfinite(event.surfaceY) && event.surfaceX >= 0 && event.surfaceX <= 1 &&
        event.surfaceY >= 0 && event.surfaceY <= 1)
        s.mouse(1,std::floor(event.surfaceX*(s.frameTarget.width-1)),std::floor(event.surfaceY*(s.frameTarget.height-1)));
}
void Ue3UiSink::endTouchFrame(const Frame&) {
    if (!state_->current()) { state_->releaseInventory();state_->releaseDrag();state_->clearCharacter();state_->clearAgeGate();state_->clearFooter();state_->clearTextScroll();state_->transactions.clear(); }
    else if(state_->inventoryStart && state_->inventoryGesture.captured()) {
        // Process a deferred press only after this boundary's Cancel/UP/second
        // finger, so an ended intent can never start a native drag afterward.
        const auto update=*state_->inventoryStart;state_->inventoryUpdate(update);
    }
}
} // namespace bl2::touch
