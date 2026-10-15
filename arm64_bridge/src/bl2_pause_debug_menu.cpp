#include "bl2_pause_debug_menu.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "obb_sha256.h"
#include "thunk_manager.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_settings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
[[maybe_unused]] constexpr char kTag[] = "Arm32Bridge_PauseDebug";
constexpr char kSha[] = "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr std::size_t kFileBytes = 49885152;
constexpr std::uint32_t kImageBytes = 0x03089000;
constexpr std::uint32_t kSerialize = 0x00F6A190;
constexpr std::uint32_t kReturn = 0x00F6A2E0;
constexpr std::uint32_t kObjects = 0x02FBA268;
constexpr std::uint32_t kNames = 0x02FB00C8;
bool verified = false;
std::uint32_t installedVeneer = 0;
std::uint32_t installedFirstWord = 0;
std::uint32_t pausePopulate = 0;
bool lastVisibility = false, visibilityKnown = false;

class Reader {
public:
    explicit Reader(DynarmicHost& value) : host(value), base(value.GetElfLoader().GetBaseAddress()) {}
    bool span(std::uint32_t p, std::size_t n) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() && n <= host.GetGuestMemorySize() - p;
    }
    std::uint32_t u32(std::uint32_t p) const { return span(p, 4) ? host.MemoryRead32(p) : 0; }
    bool array(std::uint32_t p, std::uint32_t& data, std::uint32_t& count) const {
        if (!span(p, 12)) return false;
        data = u32(p); count = u32(p + 4);
        const auto capacity = u32(p + 8);
        return count <= capacity && capacity <= 2000000 && (!count || span(data, std::size_t(count) * 4));
    }
    bool object(std::uint32_t p) const {
        if (!span(p, 0xA0) || (u32(p + 0xC) & 0x20000002u)) return false;
        std::uint32_t data = 0, count = 0;
        if (!array(base + kObjects, data, count)) return false;
        const auto index = u32(p + 0x20);
        return index < count && u32(data + index * 4) == p;
    }
    std::string name(std::uint32_t p) const {
        if (!span(p, 8) || u32(p + 4)) return {};
        std::uint32_t data = 0, count = 0;
        if (!array(base + kNames, data, count) || u32(p) >= count) return {};
        const auto entry = u32(data + u32(p) * 4);
        if (!span(entry, 136)) return {};
        std::string result;
        for (std::uint32_t i = 0; i != 128; ++i) {
            const auto ch = host.MemoryRead8(entry + 8 + i);
            if (!ch) return result;
            if (ch < 32 || ch > 126) return {};
            result += static_cast<char>(ch);
        }
        return {};
    }
    std::string objectName(std::uint32_t p) const { return object(p) ? name(p + 0x2C) : std::string{}; }
    std::string className(std::uint32_t p) const { return object(p) ? objectName(u32(p + 0x34)) : std::string{}; }
    std::uint32_t scriptRef(std::uint32_t p) const {
        if (!span(p, 8)) return 0;
        const auto low = u32(p);
        return u32(p + 4) == (low & 0x80000000u ? 0xFFFFFFFFu : 0) ? low : 0;
    }
    std::uint64_t token(std::uint32_t p) const {
        return object(p) ? (std::uint64_t(p) << 32) | u32(p + 0x20) : 0;
    }
    struct Property {
        std::uint32_t address = 0, offset = 0, size = 0, extra = 0;
        explicit operator bool() const { return address != 0; }
    };
    Property property(std::uint32_t type, const char* wanted, const char* kind) const {
        std::array<std::uint32_t,32> parents{};
        for (std::size_t depth = 0; type && depth != parents.size(); ++depth) {
            if (std::find(parents.begin(),parents.begin()+depth,type) != parents.begin()+depth || !object(type)) return {};
            parents[depth] = type;
            const auto total = u32(type + 0x48) & 0x00FFFFFFu;
            if (!total || total > 1048576) return {};
            auto field = u32(type + 0x44);
            for (unsigned i = 0; field && i != 2048; ++i) {
                if (!object(field) || u32(field + 0x28) != type) return {};
                if (objectName(field) == wanted) {
                    Property p{field,u32(field+0x54),u32(field+0x44),u32(field+0x74)};
                    if (className(field) != kind || u32(field+0x40) != 1 || !p.size || p.size > 4096 ||
                        p.offset > total || p.size > total-p.offset) return {};
                    return p;
                }
                field = u32(field + 0x3C);
            }
            type = u32(type + 0x40);
        }
        return {};
    }
    Property objectProperty(std::uint32_t p, const char* name, const char* kind) const {
        return object(p) ? property(u32(p + 0x34),name,kind) : Property{};
    }
    std::uint32_t function(std::uint32_t type,const char* wanted) const {
        std::array<std::uint32_t,32> parents{};
        for (std::size_t depth=0;type && depth!=parents.size();++depth) {
            if (std::find(parents.begin(),parents.begin()+depth,type)!=parents.begin()+depth || !object(type)) return 0;
            parents[depth]=type;
            auto field=u32(type+0x44);
            for (unsigned i=0;field && i!=2048;++i) {
                if (!object(field) || u32(field+0x28)!=type) return 0;
                if (objectName(field)==wanted) return className(field)=="Function"?field:0;
                field=u32(field+0x3C);
            }
            type=u32(type+0x40);
        }
        return 0;
    }
    Property inner(const Property& p, const char* kind) const {
        if (!p || p.size != 12 || !object(p.extra) || u32(p.extra + 0x28) != p.address ||
            className(p.extra) != kind || u32(p.extra + 0x40) != 1) return {};
        Property result{p.extra,0,u32(p.extra + 0x44),u32(p.extra + 0x74)};
        return result.size && result.size <= 4096 ? result : Property{};
    }
    bool typedArray(std::uint32_t p, std::uint32_t stride, std::uint32_t limit,
                    std::uint32_t& data, std::uint32_t& count) const {
        if (!span(p,12) || !stride) return false;
        data=u32(p); count=u32(p+4);
        return count<=u32(p+8) && u32(p+8)<=limit && (!count || span(data,std::size_t(count)*stride));
    }
    std::uint64_t stringHash(std::uint32_t p) const {
        std::uint32_t data=0,count=0;
        // This original Android image uses one-byte TCHAR in FString.
        if (!typedArray(p,1,4096,data,count) || count<2 || host.MemoryRead8(data+count-1)) return 0;
        std::uint64_t value=1469598103934665603ull;
        for (std::uint32_t i=0;i<count-1;++i) value=(value^host.MemoryRead8(data+i))*1099511628211ull;
        return value ^ count;
    }
    std::string text(std::uint32_t p,std::uint32_t limit) const {
        std::uint32_t data=0,count=0;
        if(!typedArray(p,1,4096,data,count) || !count || count>limit+1 || host.MemoryRead8(data+count-1))return {};
        std::string value;value.reserve(count-1);
        for(std::uint32_t i=0;i<count-1;++i) {
            const auto ch=host.MemoryRead8(data+i);if(ch<32 || ch>126)return {};
            value.push_back(static_cast<char>(ch));
        }
        return value;
    }
    float f32(std::uint32_t p) const {
        const auto bits=u32(p); float value; std::memcpy(&value,&bits,4); return value;
    }
    DynarmicHost& host;
    std::uint32_t base;
};

void updatePauseVisibility(std::uint32_t function, DynarmicHost& host) {
    Reader r(host);
    if (!r.span(function, 0xA0) || r.u32(function + 0x50) != 1091) return;
    if (r.objectName(function) != "Populate" || r.className(function) != "Function" ||
        r.u32(function + 0x7C) != 0x00020002 || host.MemoryRead8(function + 0x85) != 1 ||
        host.MemoryRead16(function + 0x86) != 4 || host.MemoryRead16(function + 0x88) != 0xFFFF) return;
    const auto owner = r.u32(function + 0x28);
    if (r.objectName(owner) != "WillowScrollingListDataProviderPause" || r.className(owner) != "Class" ||
        r.objectName(r.u32(owner + 0x28)) != "WillowGame" ||
        r.objectName(r.u32(owner + 0x40)) != "WillowScrollingListDataProviderBase") return;
    const auto script = r.u32(function + 0x4C), capacity = r.u32(function + 0x54);
    if (capacity < 1091 || capacity > 4096 || !r.span(script, capacity)) return;
    const auto gate = script + 0x367;
    // Only the three native-ref 8-byte windows and the one reviewed jump byte
    // are excluded here; each ref is checked by its real metadata below.
    constexpr std::uint8_t prefix[] = {
        0x07,0xE6,0x03,0x81,0x12,0x20, 0,0,0,0,0,0,0,0, 0x0A,0x00,
        0,0,0,0,0,0,0,0, 0x00,0x1C, 0,0,0,0,0,0,0,0, 0x16,0x16
    };
    const auto jump = host.MemoryRead8(gate + 1);
    if (jump != 0xE6 && jump != 0x8B) return;
    for (std::uint32_t i = 0; i != sizeof(prefix); ++i) {
        if (i == 1 || (i >= 6 && i < 14) || (i >= 16 && i < 24) || (i >= 26 && i < 34)) continue;
        if (host.MemoryRead8(gate + i) != prefix[i]) return;
    }
    const auto shippingClass = r.scriptRef(gate + 6);
    const auto shippingReturn = r.scriptRef(gate + 16);
    const auto shippingFunction = r.scriptRef(gate + 26);
    if (r.objectName(shippingClass) != "WillowGameInfo" || r.className(shippingClass) != "Class" ||
        r.objectName(shippingFunction) != "IsShippingBuild" || r.className(shippingFunction) != "Function" ||
        r.u32(shippingFunction + 0x28) != shippingClass || r.u32(shippingFunction + 0x7C) != 0x00022401 ||
        r.objectName(shippingReturn) != "ReturnValue" || r.className(shippingReturn) != "BoolProperty" ||
        r.u32(shippingReturn + 0x28) != shippingFunction) return;
    const auto row = script + 0x38B;
    const auto list = r.scriptRef(row + 2);
    if (host.MemoryRead8(row) != 0x19 || host.MemoryRead8(row + 1) != 0x00 ||
        r.objectName(list) != "TheList" || r.className(list) != "ObjectProperty" || r.u32(list + 0x28) != function ||
        r.u32(list + 0x40) != 1 || r.u32(list + 0x44) != 4 ||
        (host.MemoryRead64(list + 0x48) & 0x480u) != 0x80u ||
        host.MemoryRead16(row + 10) != 0x46 || host.MemoryRead64(row + 12) != 0 ||
        host.MemoryRead8(row + 20) != 0 || host.MemoryRead8(row + 21) != 0x1B ||
        r.name(row + 22) != "AddListItem" || host.MemoryRead8(row + 30) != 0x2C ||
        host.MemoryRead8(row + 31) != 3 || host.MemoryRead8(row + 32) != 0x1F) return;
    constexpr char caption[] = "$WillowMenu.WillowScrollingListDataProviderPause.Debug";
    for (std::uint32_t i = 0; i != sizeof(caption); ++i)
        if (host.MemoryRead8(row + 33 + i) != static_cast<std::uint8_t>(caption[i])) return;
    const auto tail = row + 33 + sizeof(caption);
    if (host.MemoryRead8(tail) != 0x28 || host.MemoryRead8(tail + 1) != 0x4A || host.MemoryRead8(tail + 2) != 0x16 ||
        tail + 3 != script + 0x3E6 || host.MemoryRead8(script + 1088) != 0x04 ||
        host.MemoryRead8(script + 1089) != 0x0B || host.MemoryRead8(script + 1090) != 0x53) return;
    const bool visible=bl2::touch::Bl2DebugMenuVisible();
    const auto desired=visible?0x8Bu:0xE6u;
    pausePopulate=function;
    if (jump != desired) {
        host.MemoryWrite8(gate + 1,desired);
        HORN_LOGI(kTag,"pause Debug event3 visible=%u: original Populate gate updated; manager untouched",visible?1u:0u);
    }
}
void afterSerialize(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    Reader r(host);
    // The normal epilogue holds this in R4 and archive in R5.
    const auto archive=jit.Regs()[5];
    if (r.span(archive,0x18) && r.u32(archive+0x14)) updatePauseVisibility(jit.Regs()[4],host);
}
} // namespace

void RefreshBl2PauseDebugMenu(DynarmicHost& host) {
    const bool wanted=bl2::touch::Bl2DebugMenuVisible();
    if (!visibilityKnown || wanted!=lastVisibility) {
        lastVisibility=wanted; visibilityKnown=true;
        if (pausePopulate) updatePauseVisibility(pausePopulate,host);
    }
}

bool VerifyBl2PauseDebugMenuInput(const std::uint8_t* bytes, std::size_t size) {
    verified = false;
    if (!bytes || size != kFileBytes) return false;
    ObbSha256::Digest digest{};
    if (!ObbSha256::OfMapping(bytes, size, digest)) return false;
    const auto hex = ObbSha256::Hex(digest);
    verified = std::strcmp(hex.data(), kSha) == 0;
    return verified;
}

bool InstallBl2PauseDebugMenu(DynarmicHost& host) {
    const auto& elf = host.GetElfLoader();
    const auto base = elf.GetBaseAddress();
    if (!verified || elf.GetImageSize() != kImageBytes || std::uint64_t(base) + kImageBytes > host.GetGuestMemorySize() ||
        elf.GetSymbolAddress("_ZN9UFunction9SerializeER8FArchive") != base + kSerialize ||
        elf.GetSymbolAddress("_ZN7UObject11GObjObjectsE") != base + kObjects ||
        elf.GetSymbolAddress("_ZN5FName5NamesE") != base + kNames) return false;
    const auto site = base + kReturn;
    if (installedVeneer && host.MemoryRead32(site) == 0xE51FF004 && host.MemoryRead32(site + 4) == installedVeneer &&
        host.MemoryRead32(installedVeneer) == installedFirstWord) return true;
    constexpr std::uint32_t prologue[] = {0xE92D4070,0xE1A05001,0xE5916014,0xE24DD008,0xE1A04000};
    for (std::uint32_t i = 0; i != 5; ++i)
        if (host.MemoryRead32(base + kSerialize + i * 4) != prologue[i]) return false;
    if (host.MemoryRead32(site) != 0xE28DD008 || host.MemoryRead32(site + 4) != 0xE8BD8070) return false;
#if defined(BL2_NATIVE_ARM32)
    // The normal epilogue itself returns to the saved caller PC on the stack.
    constexpr std::uint32_t words[] = {0xE28DD008, 0xE8BD8070};
    const auto continuation = host.CreateCodeVeneer(words);
    if (!continuation) return false;
    const auto veneer = host.GetThunkManager().RegisterThunk("bl2_pause_debug_after_function_serialize",
        [continuation](Dynarmic::A32::Jit& jit, DynarmicHost& currentHost) {
            afterSerialize(jit, currentHost);
            jit.Regs()[15] = continuation;
        });
    if (!veneer) return false;
#else
    const auto veneer = host.GuestAlloc(12, 4);
    if (!veneer) return false;
    const auto thunk = host.GetThunkManager().RegisterThunk("bl2_pause_debug_after_function_serialize", afterSerialize);
    const auto svc = thunk ? host.MemoryRead32(thunk) : 0;
    if ((svc & 0xFF000000u) != 0xEF000000u) return false;
    host.MemoryWrite32(veneer, svc);
    host.MemoryWrite32(veneer + 4, 0xE28DD008);
    host.MemoryWrite32(veneer + 8, 0xE8BD8070);
    host.AddExecutableRange(veneer, 12);
    host.InvalidateGuestCode(veneer, 12);
#endif
    host.MemoryWrite32(site, 0xE51FF004);
    host.MemoryWrite32(site + 4, veneer);
    host.InvalidateGuestCode(site, 8);
    if (host.MemoryRead32(site) != 0xE51FF004 || host.MemoryRead32(site + 4) != veneer) return false;
    installedVeneer = veneer;
    installedFirstWord = host.MemoryRead32(veneer);
    HORN_LOGI(kTag, "pause visibility hook installed at UFunction::Serialize normal epilogue; no cheat actions");
    return true;
}

namespace bl2::touch {
namespace {
constexpr std::uint32_t kDebugInputKey=0x024365F0;
constexpr std::uint32_t kProcessDelegate=0x00F9782C;
constexpr std::uint32_t kNativeInputDelegateName=0x030097F0;
constexpr std::uint32_t kDebugFooterMeasure=0x0183977C;
struct DebugKey { Action action; std::uint32_t rva; const char* name; };
constexpr DebugKey debugKeys[] = {
    {Action::Back,0x03009FB8,"Escape"}, {Action::Confirm,0x03009FA0,"Enter"},
    {Action::UiUp,0x03009FF0,"Up"}, {Action::UiDown,0x0300A000,"Down"},
    {Action::UiLeft,0x03009FE8,"Left"}, {Action::UiRight,0x03009FF8,"Right"},
    {Action::Reload,0x0300A120,"X"}, {Action::NextWeapon,0x0300A128,"Y"},
    {Action::Skill,0x0300A0F0,"R"}, {Action::Sprint,0x0300A0C0,"L"},
    {Action::PreviousWeapon,0x0300A288,"LeftBracket"}, {Action::Inventory,0x0300A298,"RightBracket"}
};
constexpr DebugKey debugPageKeys[] = {
    {Action::UiUp,0x03009FC8,"PageUp"}, {Action::UiDown,0x03009FD0,"PageDown"}
};
struct DebugTarget {
    std::uint32_t viewport=0,controller=0,controllerId=0,ui=0,client=0,engine=0,world=0,width=0,height=0;
    std::uint64_t controllerToken=0,clientToken=0,engineToken=0,uiToken=0,generation=0,scene=0;
    Reader::Property open{},owner{},semaphore{},viewportUi{},engineViewport{},inputDelegate{};
};
constexpr std::uint32_t kDebugCanvasTextSite=0x0184FB18;
constexpr std::uint32_t kDebugCanvasMeasureSite=DebugStrLenFrameAbi::measureSite;
constexpr std::uint32_t kDebugCanvasScaleSite=0x0184FA7C;
constexpr std::uint32_t kFrameCounter=0x02F91B88;
struct CanvasObserver {
    void* context=nullptr;
    void (*capture)(void*,Dynarmic::A32::Jit&,DynarmicHost&)=nullptr;
    void (*saveMeasureFrame)(void*,Dynarmic::A32::Jit&,DynarmicHost&)=nullptr;
    void (*measure)(void*,Dynarmic::A32::Jit&,DynarmicHost&)=nullptr;
    void (*draw)(void*,Dynarmic::A32::Jit&,DynarmicHost&)=nullptr;
} canvasObserver;
std::uint32_t canvasVeneer=0,saveMeasureVeneer=0,measureVeneer=0,scaleVeneer=0;
void captureDebugCanvas(Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    if (canvasObserver.context && canvasObserver.capture) canvasObserver.capture(canvasObserver.context,jit,host);
}
void measureDebugCanvas(Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    if(canvasObserver.context && canvasObserver.measure)canvasObserver.measure(canvasObserver.context,jit,host);
}
void saveDebugMeasureFrame(Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    if(canvasObserver.context && canvasObserver.saveMeasureFrame)canvasObserver.saveMeasureFrame(canvasObserver.context,jit,host);
}
void scaleDebugCanvas(Dynarmic::A32::Jit& jit,DynarmicHost& host) {
    if(canvasObserver.context && canvasObserver.draw)canvasObserver.draw(canvasObserver.context,jit,host);
}
bool installCanvasSite(DynarmicHost& host,std::uint32_t rva,const std::array<std::uint32_t,2>& original,
                       const char* name,void(*callback)(Dynarmic::A32::Jit&,DynarmicHost&),std::uint32_t& installed) {
    const auto site=host.GetElfLoader().GetBaseAddress()+rva;
    if(installed && host.MemoryRead32(site)==0xE51FF004 && host.MemoryRead32(site+4)==installed)return true;
    if (host.MemoryRead32(site)!=original[0] || host.MemoryRead32(site+4)!=original[1]) return false;
#if defined(BL2_NATIVE_ARM32)
    const std::array<std::uint32_t,4> words{original[0],original[1],0xE51FF004,site+8};
    const auto continuation=host.CreateCodeVeneer(words);if(!continuation)return false;
    const auto veneer=host.GetThunkManager().RegisterThunk(name,
        [continuation,callback](Dynarmic::A32::Jit& jit,DynarmicHost& current){callback(jit,current);jit.Regs()[15]=continuation;});
    if(!veneer)return false;
#else
    const auto thunk=host.GetThunkManager().RegisterThunk(name,callback);
    const auto svc=thunk?host.MemoryRead32(thunk):0;
    const auto veneer=host.GuestAlloc(20,4);if(!veneer || (svc&0xFF000000)!=0xEF000000)return false;
    host.MemoryWrite32(veneer,svc);host.MemoryWrite32(veneer+4,original[0]);host.MemoryWrite32(veneer+8,original[1]);
    host.MemoryWrite32(veneer+12,0xE51FF004);host.MemoryWrite32(veneer+16,site+8);
    host.AddExecutableRange(veneer,20);host.InvalidateGuestCode(veneer,20);
#endif
    host.MemoryWrite32(site,0xE51FF004);host.MemoryWrite32(site+4,veneer);host.InvalidateGuestCode(site,8);
    installed=veneer;return true;
}
bool verifyCanvasText(DynarmicHost& host) {
    const auto& elf=host.GetElfLoader();const auto base=elf.GetBaseAddress();
    return ::verified && elf.GetSymbolAddress("_ZN7UCanvas12execDrawTextER6FFramePv")==base+0x0184F8C4 &&
        host.MemoryRead32(base+0x0184F8C4)==0xE92D4FF0 && host.MemoryRead32(base+0x0184F8C8)==0xED2D8B06;
}
bool installDebugCanvasHook(DynarmicHost& host) {
    // These two non-PC-relative loads join both text paths after drawing and
    // before FString destruction. This observer changes no guest register.
    return verifyCanvasText(host) && installCanvasSite(host,kDebugCanvasTextSite,{0xE59D3008,0xE59D0030},
        "bl2_touch_debug_canvas_text",captureDebugCanvas,canvasVeneer);
}
bool installDebugFontHooks(DynarmicHost& host) {
    const auto& elf=host.GetElfLoader();const auto base=elf.GetBaseAddress();
    if(!verifyCanvasText(host) || elf.GetSymbolAddress("_ZN7UCanvas10execStrLenER6FFramePv")!=base+0x018510A4 ||
        host.MemoryRead32(base+0x018510A4)!=0xE92D4FF0 || host.MemoryRead32(base+0x018510B0)!=0xE24DD034 ||
        host.MemoryRead32(base+0x018510B4)!=0xE1A05001 || host.MemoryRead32(base+0x018510C0)!=0xE1A04000 ||
        host.MemoryRead32(base+0x01851240)!=0xEBFFFE3C || elf.GetSymbolAddress("GFrameCounter")!=base+kFrameCounter)return false;
    // R5 initially holds FFrame but is reused for Canvas.OrgX at +0x11D4.
    // Its 0x34-byte local allocation has an unused word at SP+0x2C: FString
    // occupies +20/+24/+28, dummy outputs +10/+14, result outputs +18/+1C.
    // Save the original frame before that reuse and validate it again below.
    // StrLen's R2/R3 are the native WrappedStrLenf ScaleX/Y inputs; DrawText's
    // SP+10/SP+18 are its parsed ScaleX/Y, after the original engine factor.
    // No PC-relative instruction is displaced and no font object is changed.
    return installCanvasSite(host,DebugStrLenFrameAbi::saveSite,
        {DebugStrLenFrameAbi::saveWord0,DebugStrLenFrameAbi::saveWord1},
        "bl2_touch_debug_canvas_measure_frame",saveDebugMeasureFrame,saveMeasureVeneer) &&
        installCanvasSite(host,kDebugCanvasMeasureSite,{0xE58D0000,0xE1A00004},
        "bl2_touch_debug_canvas_measure_scale",measureDebugCanvas,measureVeneer) &&
        installCanvasSite(host,kDebugCanvasScaleSite,{0xE5D63000,0xE3130001},
        "bl2_touch_debug_canvas_draw_scale",scaleDebugCanvas,scaleVeneer);
}
enum class DebugHitKind { Row, MainHeader, SubHeader, BuildFooter };
struct DebugHit {
    DebugHitKind kind=DebugHitKind::Row;
    std::uint32_t index=0;
    std::uint64_t hash=0;
    float left=0,top=0,right=0,bottom=0;
};
struct DebugSchema {
    Reader::Property menu{},row{},columns{},column{},text{},selectable{},at{},top{},ipp{},layout{},bgX{},bgWidth{};
    Reader::Property mainHeaders{},subHeaders{},header{},headerText{},headerInterface{},activeMain{},activeSub{},stack{};
    Reader::Property headerIcon{},headerIconWidth{},columnLayouts{},columnLayout{},columnWidth{},columnPadding{};
    Reader::Property bgY{},bgHeight{},columnHeight{},descriptionX{},descriptionWidth{},columnFont{},columnHeading{};
    Reader::Property hint{},tertiary{},footerText{};
    std::array<Reader::Property,11> rowIntentText{};
    std::uint32_t footerFunction=0;
    bool ready=false,fontReady=false,footerReady=false,captureReady=false;
};
struct DebugContent {
    std::uint32_t data=0,count=0,top=0,ipp=0,at=0,main=0,sub=0;
    std::uint64_t key=0;
};
std::uint64_t debugMix(std::uint64_t seed,std::uint64_t word) {
    return seed^(word+0x9E3779B97F4A7C15ull+(seed<<6)+(seed>>2));
}
}
struct Ue3DebugMenuSink::State {
    DynarmicHost& host;
    LifetimeGeneration generation;
    bool signatures=false,preparedValid=false,frameValid=false,keyCancelled=false;
    std::uint64_t epoch=0,held=0;
    DebugTarget prepared{},target{};
    DebugTarget metadata{};
    std::uint64_t metadataEngineClass=0,metadataClientClass=0,metadataUiClass=0;
    DebugSchema schema{};
    std::uint64_t schemaClass=0,targetContent=0,captureContent=0,geometryContent=0,geometryAt=0;
    std::uint32_t inputFunction=0,inputScratch=0;
    std::uint64_t inputFunctionToken=0,inputClass=0;
    std::uint32_t geometryTop=0;
    float rowPitch=24,measuredRowPitch=0;
    DebugMenuGesture gesture{true};
    std::vector<DebugHit> geometry;
    DebugHit capturedHit{};
    DebugRowCaptureKey capturedRowKey{};
    bool hasCapturedHit=false,canvasHook=false,fontHooks=false;
    std::uint64_t fontContent=0,fontOwner=0,fontGeneration=0,fontScene=0,fontFrame=0;
    std::uint64_t fontCanvas=0;
    std::uint32_t fontColumns=0,fontColumnCount=0,fontViewportWidth=0,fontViewportHeight=0;
    DebugColumnCacheOwner fontCacheOwner;
    DebugFontRefreshKey fontRefreshKey;
    float fontFactor=1,fontRequested=1.25f,fontClipX=0,fontClipY=0;
    std::array<float,16> fontMinimum{},fontBaseline{};
    unsigned fontLogs=0,fontRefreshLogs=0;
    unsigned measureLogs=0,drawLogs=0;
    unsigned dispatchBudget=0;
    bool geometryLogged=false;
    int lastPaintRow=-1;
    float lastPaintY=0;
    unsigned logs=0,diagnosticLogs=0,pageLogs=0;
    bool footerMeasureVerified=false;
    std::uint32_t footerScratch=0;
    struct FooterMetrics {
        std::uint64_t font=0,generation=0,hint=0;
        std::uint32_t scaleX=0,scaleY=0;
        float width=0,height=0,prefix=0,label=0;
    } footerMetrics;
    unsigned footerLogs=0,footerRejectLogs=0;
    explicit State(DynarmicHost& h,LifetimeGeneration g):host(h),generation(g) {
        const auto& elf=host.GetElfLoader(); const auto base=elf.GetBaseAddress();
        signatures=::verified && generation && elf.GetImageSize()==kImageBytes &&
            elf.GetSymbolAddress("_ZN20UWillowUIInteraction8InputKeyEi5FName11EInputEventfj")==base+kDebugInputKey &&
            host.MemoryRead32(base+kDebugInputKey)==0xE92D4030 &&
            host.MemoryRead32(base+kDebugInputKey+4)==0xE24DD01C &&
            elf.GetSymbolAddress("_ZN7UObject15ProcessDelegateE5FNamePK15FScriptDelegatePvS4_")==base+kProcessDelegate &&
            host.MemoryRead32(base+kProcessDelegate)==0xE92D40F0 &&
            host.MemoryRead32(base+kProcessDelegate+4)==0xE1A06003 &&
            elf.GetSymbolAddress("ENGINE_OnReceivedNativeInputKey")==base+kNativeInputDelegateName &&
            elf.GetSymbolAddress("KEY_PageUp")==base+debugPageKeys[0].rva &&
            elf.GetSymbolAddress("KEY_PageDown")==base+debugPageKeys[1].rva;
        // Static method, no Canvas this argument: R0=Font, R1/R2=float bits,
        // R3=outX; caller stack[0]=outY, stack[4]=UTF16 text. It only measures.
        footerMeasureVerified=signatures &&
            elf.GetSymbolAddress("_ZN7UCanvas13ClippedStrLenEP5UFontffRiS2_PKs")==base+kDebugFooterMeasure &&
            host.MemoryRead32(base+kDebugFooterMeasure)==0xE92D4070 &&
            host.MemoryRead32(base+kDebugFooterMeasure+4)==0xE24DD028 &&
            host.MemoryRead32(base+kDebugFooterMeasure+8)==0xE59D4038 &&
            host.MemoryRead32(base+kDebugFooterMeasure+0x1C)==0xE585C000 &&
            host.MemoryRead32(base+kDebugFooterMeasure+0x20)==0xE584C000 &&
            host.MemoryRead32(base+kDebugFooterMeasure+0x30)==0xE59D103C;
        if (signatures) {
            canvasHook=installDebugCanvasHook(host);
            fontHooks=installDebugFontHooks(host);
            if (canvasHook || fontHooks) canvasObserver={this,[](void* context,Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                auto& self=*static_cast<State*>(context);if (&current==&self.host) self.capture(jit);
            },[](void* context,Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                auto& self=*static_cast<State*>(context);if (&current==&self.host)self.saveMeasureFrame(jit);
            },[](void* context,Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                auto& self=*static_cast<State*>(context);if (&current==&self.host)self.measureScale(jit);
            },[](void* context,Dynarmic::A32::Jit& jit,DynarmicHost& current) {
                auto& self=*static_cast<State*>(context);if (&current==&self.host)self.drawScale(jit);
            }};
        }
    }
    ~State() {if(canvasObserver.context==this) canvasObserver={};}
    void diagnostic(const char* event,const char* reason,std::uint32_t detail=0) {
        (void)event;(void)reason;(void)detail;
        if(diagnosticLogs>=48)return;
        ++diagnosticLogs;
        HORN_LOGI(kTag,"native Debug input event=%s reason=%s detail=%u epoch=%llu held=%llX content=%llX rows=%zu",
            event,reason,detail,static_cast<unsigned long long>(epoch),static_cast<unsigned long long>(held),
            static_cast<unsigned long long>(targetContent),geometry.size());
    }
    void readInputFunction(std::uint32_t ui) {
        Reader r(host);const auto type=r.u32(ui+0x34);const auto typeToken=r.token(type);
        if (inputClass==typeToken && inputFunctionToken && r.token(inputFunction)==inputFunctionToken) return;
        inputClass=typeToken;inputFunction=0;inputFunctionToken=0;
        const auto function=r.function(type,"InputKey");
        // Original cooked WillowUIInteraction.InputKey has five inputs and its
        // bool return: identical to PlayerManager's 28-byte delegate block.
        if (!function || r.u32(function+0x7C)!=0x00024802 || host.MemoryRead8(function+0x85)!=6 ||
            host.MemoryRead16(function+0x86)!=28 || host.MemoryRead16(function+0x88)!=24 ||
            r.u32(function+0x50)!=2353) {diagnostic("metadata","input-function-shape");return;}
        struct Param {const char* name;const char* kind;std::uint32_t offset,size;};
        constexpr Param parameters[]={{"ControllerId","IntProperty",0,4},{"Key","NameProperty",4,8},
            {"Event","ByteProperty",12,1},{"AmountDepressed","FloatProperty",16,4},
            {"bGamepad","BoolProperty",20,4},{"ReturnValue","BoolProperty",24,4}};
        for(const auto& expected:parameters) {
            const auto field=r.property(function,expected.name,expected.kind);
            if(!field || field.offset!=expected.offset || field.size!=expected.size ||
                !(host.MemoryRead64(field.address+0x48)&0x80u) ||
                (!std::strcmp(expected.kind,"BoolProperty") && field.extra!=1)) {
                diagnostic("metadata","input-parameter-shape",expected.offset);return;
            }
        }
        inputFunction=function;inputFunctionToken=r.token(function);
    }
    bool inputReceiver() const {
        Reader r(host);
        const auto& delegate=target.inputDelegate;
        if (!delegate || delegate.size!=12 || delegate.offset!=0x48 || !inputFunctionToken ||
            r.token(inputFunction)!=inputFunctionToken || r.u32(inputFunction+0x28)!=r.u32(target.ui+0x34) ||
            r.u32(inputFunction+0x7C)!=0x00024802 ||
            r.u32(r.u32(target.ui)+0x110)!=r.base+kProcessDelegate ||
            r.name(r.base+kNativeInputDelegateName)!="OnReceivedNativeInputKey") return false;
        const auto receiver=r.u32(target.ui+delegate.offset);
        return (!receiver || receiver==target.ui) && r.name(target.ui+delegate.offset+4)=="InputKey";
    }
    void readSchema(std::uint32_t ui) {
        Reader r(host);const auto type=r.u32(ui+0x34);const auto token=r.token(type);
        if (schemaClass==token && schema.ready) return;
        schema={};schemaClass=token;
        auto& f=schema;
        f.menu=r.property(type,"CurrentMenu","ArrayProperty");f.row=r.inner(f.menu,"StructProperty");
        if (!f.row || r.objectName(f.row.extra)!="tMenuData") return;
        f.columns=r.property(f.row.extra,"ColumnText","ArrayProperty");f.column=r.inner(f.columns,"StructProperty");
        if (!f.column) return;
        f.text=r.property(f.column.extra,"Text","StrProperty");
        f.selectable=r.property(f.row.extra,"bIsSelectable","BoolProperty");
        f.at=r.property(type,"Menu_At","IntProperty");f.top=r.property(type,"Menu_Top","IntProperty");
        f.ipp=r.property(type,"Menu_IPP","IntProperty");f.layout=r.property(type,"CurrentMenuLayout","StructProperty");
        if (!f.layout || r.objectName(f.layout.extra)!="tMenuLayout") return;
        f.bgX=r.property(f.layout.extra,"BackgroundXPos","IntProperty");f.bgWidth=r.property(f.layout.extra,"BackgroundXL","IntProperty");
        f.mainHeaders=r.property(type,"MenuHeaders","ArrayProperty");f.subHeaders=r.property(type,"SubMenuHeaders","ArrayProperty");
        f.header=r.inner(f.mainHeaders,"StructProperty");const auto subHeader=r.inner(f.subHeaders,"StructProperty");
        if (!f.header || !subHeader || subHeader.extra!=f.header.extra || subHeader.size!=f.header.size) return;
        f.headerText=r.property(f.header.extra,"Header","StrProperty");
        f.headerInterface=r.property(f.header.extra,"InterfaceName","StrProperty");
        f.activeMain=r.property(type,"ActiveMenu","IntProperty");f.activeSub=r.property(type,"ActiveSubMenu","IntProperty");
        f.stack=r.property(type,"MenuStack","ArrayProperty");const auto stackInner=r.inner(f.stack,"StrProperty");
        f.ready=f.text && f.text.size==12 && f.selectable && f.selectable.size==4 && f.selectable.extra &&
            !(f.selectable.extra&(f.selectable.extra-1)) && f.at && f.at.size==4 && f.top && f.top.size==4 &&
            f.ipp && f.ipp.size==4 && f.bgX && f.bgX.size==4 && f.bgWidth && f.bgWidth.size==4 &&
            f.headerText && f.headerText.size==12 && f.activeMain && f.activeMain.size==4 &&
            f.activeSub && f.activeSub.size==4 && stackInner && stackInner.size==12;
        if (!f.ready && logs<8) HORN_LOGI(kTag,"native Debug Canvas metadata unavailable for exact pointer geometry");
        if(!f.ready)return;
        f.hint=r.property(type,"MenuHint","StrProperty");
        f.tertiary=r.property(f.row.extra,"TertiaryActionCmd","StrProperty");
        f.footerFunction=r.function(type,"DrawShadowTextBounded");
        f.footerText=r.property(f.footerFunction,"Text","StrProperty");
        f.footerReady=f.hint && f.hint.size==12 && f.tertiary && f.tertiary.size==12 &&
            f.footerFunction && f.footerText && f.footerText.size==12 &&
            (host.MemoryRead64(f.footerText.address+0x48)&0x80u);
        constexpr const char* rowTextNames[]={"PrimaryActionCmd","SecondaryActionCmd","TertiaryActionCmd",
            "QuaternaryActionCmd","QuinaryActionCmd","OverCmd","LeaveCmd","Data","Hint","ItemDetail","DescriptionBoxStr"};
        f.captureReady=f.headerInterface && f.headerInterface.size==12;
        for(std::size_t i=0;i<f.rowIntentText.size();++i) {
            f.rowIntentText[i]=r.property(f.row.extra,rowTextNames[i],"StrProperty");
            f.captureReady=f.captureReady && f.rowIntentText[i] && f.rowIntentText[i].size==12;
        }
        f.headerIcon=r.property(f.header.extra,"bDrawIcon","BoolProperty");
        f.headerIconWidth=r.property(f.header.extra,"IconUL","IntProperty");
        f.columnLayouts=r.property(f.layout.extra,"ColumnLayout","ArrayProperty");
        f.columnLayout=r.inner(f.columnLayouts,"StructProperty");
        if(!f.columnLayout || r.objectName(f.columnLayout.extra)!="ColumnLayoutData")return;
        f.columnWidth=r.property(f.columnLayout.extra,"Width","FloatProperty");
        f.columnHeading=r.property(f.columnLayout.extra,"Header","StrProperty");
        f.columnFont=r.property(f.layout.extra,"ColumnDataFont","ObjectProperty");
        f.columnPadding=r.property(f.layout.extra,"ColumnPadding","IntProperty");
        f.bgY=r.property(f.layout.extra,"BackgroundYPos","IntProperty");
        f.bgHeight=r.property(f.layout.extra,"BackgroundYL","IntProperty");
        f.columnHeight=r.property(f.layout.extra,"ColumnDataYL","IntProperty");
        f.descriptionX=r.property(f.layout.extra,"DescriptionBoxXPos","IntProperty");
        f.descriptionWidth=r.property(f.layout.extra,"DescriptionBoxXL","IntProperty");
        f.fontReady=f.headerIcon && f.headerIcon.size==4 &&
            f.headerIcon.extra && !(f.headerIcon.extra&(f.headerIcon.extra-1)) &&
            f.columnHeading && f.columnHeading.size==12 && f.columnFont && f.columnFont.size==4 &&
            f.headerIconWidth && f.headerIconWidth.size==4 && f.columnWidth && f.columnWidth.size==4 &&
            f.columnPadding && f.columnPadding.size==4 && f.bgY && f.bgY.size==4 &&
            f.bgHeight && f.bgHeight.size==4 && f.columnHeight && f.columnHeight.size==4 &&
            f.descriptionX && f.descriptionX.size==4 && f.descriptionWidth && f.descriptionWidth.size==4;
    }
    DebugContent content(const DebugTarget& t) const {
        if (!schema.ready || !valid(t)) return {};
        Reader r(host);DebugContent c;
        if (!r.typedArray(t.ui+schema.menu.offset,schema.row.size,65536,c.data,c.count)) return {};
        c.at=r.u32(t.ui+schema.at.offset);c.top=r.u32(t.ui+schema.top.offset);c.ipp=r.u32(t.ui+schema.ipp.offset);
        c.main=r.u32(t.ui+schema.activeMain.offset);c.sub=r.u32(t.ui+schema.activeSub.offset);
        if (c.top>c.count || (c.count && c.at>=c.count) || c.ipp>512 || c.main>512 || c.sub>512) return {};
        c.key=debugMix(debugMix(t.uiToken,c.data),c.count);c.key=debugMix(debugMix(c.key,c.main),c.sub);
        for (const auto* field:{&schema.mainHeaders,&schema.subHeaders}) {
            std::uint32_t data=0,count=0;if(!r.typedArray(t.ui+field->offset,schema.header.size,512,data,count))return {};
            c.key=debugMix(debugMix(c.key,data),count);
        }
        std::uint32_t stack=0,count=0;if(!r.typedArray(t.ui+schema.stack.offset,12,128,stack,count))return {};
        c.key=debugMix(debugMix(c.key,stack),count);
        if (count) c.key=debugMix(c.key,r.stringHash(stack+(count-1)*12));
        if (!c.key)c.key=1;return c;
    }
    std::uint64_t rowHash(const DebugContent& c,std::uint32_t index) const {
        if (index>=c.count) return 0;
        Reader r(host);std::uint32_t columns=0,count=0;
        const auto row=c.data+index*schema.row.size;
        if (!r.typedArray(row+schema.columns.offset,schema.column.size,16,columns,count)) return 0;
        for (std::uint32_t i=0;i<count;++i) {
            const auto hash=r.stringHash(columns+i*schema.column.size+schema.text.offset);if(hash)return hash;
        }
        return 0;
    }
    bool selectable(const DebugContent& c,std::uint32_t index) const {
        Reader r(host);return index<c.count && (r.u32(c.data+index*schema.row.size+schema.selectable.offset)&schema.selectable.extra);
    }
    bool buildFooter(const DebugTarget& t,const DebugContent& c,DebugBuildFooterRange& range,std::string& hint) const {
        if(!schema.footerReady || c.at>=c.count || !selectable(c,c.at))return false;
        Reader r(host);std::uint32_t headers=0,count=0;
        if(!r.typedArray(t.ui+schema.mainHeaders.offset,schema.header.size,512,headers,count) || c.main>=count)return false;
        const auto menu=r.text(headers+c.main*schema.header.size+schema.headerText.offset,64);
        const auto command=r.text(c.data+c.at*schema.row.size+schema.tertiary.offset,64);
        hint=r.text(t.ui+schema.hint.offset,256);
        return debugBuildFooterRange(menu,command,hint,range);
    }
    bool measureFooter(std::uint32_t canvas,std::uint32_t stack,std::string_view hint,const DebugBuildFooterRange& range) {
        if(!footerMeasureVerified)return false;
        Reader r(host);const auto font=r.u32(canvas+0x3C);const auto fontToken=r.token(font);
        const auto scaleX=r.u32(stack+0x10),scaleY=r.u32(stack+0x18);
        const auto x=r.f32(stack+0x10),y=r.f32(stack+0x18);
        if(!fontToken || r.className(font)!="Font" || !std::isfinite(x) || !std::isfinite(y) ||
            x<=0 || y<=0 || x>8 || y>8)return false;
        const auto hash=r.stringHash(stack+0x30);
        if(footerMetrics.font==fontToken && footerMetrics.generation==prepared.generation &&
            footerMetrics.hint==hash && footerMetrics.scaleX==scaleX && footerMetrics.scaleY==scaleY)return true;
        if(!footerScratch)footerScratch=host.GuestAlloc(8+257*2,4);
        if(!r.span(footerScratch,8+257*2))return false;
        const auto measure=[&](std::string_view text,float& width,float& height) {
            host.MemoryWrite32(footerScratch,0xFFFFFFFF);host.MemoryWrite32(footerScratch+4,0xFFFFFFFF);
            for(std::size_t i=0;i<text.size();++i)host.MemoryWrite16(footerScratch+8+static_cast<std::uint32_t>(i*2),
                static_cast<unsigned char>(text[i]));
            host.MemoryWrite16(footerScratch+8+static_cast<std::uint32_t>(text.size()*2),0);
#if defined(BL2_NATIVE_ARM32)
            host.RunGuestCallback(r.base+kDebugFooterMeasure,{font,scaleX,scaleY,footerScratch,footerScratch+4,footerScratch+8});
#else
            std::uint32_t returned=0;
            if(!host.CallGuest(r.base+kDebugFooterMeasure,{font,scaleX,scaleY,footerScratch,footerScratch+4,footerScratch+8},returned)) return false;
#endif
            const auto w=static_cast<std::int32_t>(r.u32(footerScratch));
            const auto h=static_cast<std::int32_t>(r.u32(footerScratch+4));
            if(w<=0 || h<=0 || w>32768 || h>16384)return false;
            width=static_cast<float>(w);height=static_cast<float>(h);return true;
        };
        FooterMetrics metrics;float prefixHeight=0,labelHeight=0;
        if(!measure(hint,metrics.width,metrics.height) || !measure(hint.substr(0,range.prefix),metrics.prefix,prefixHeight) ||
            !measure(hint.substr(range.prefix,range.label),metrics.label,labelHeight) ||
            prefixHeight!=metrics.height || labelHeight!=metrics.height || !valid(prepared))return false;
        metrics.font=fontToken;metrics.generation=prepared.generation;metrics.hint=hash;
        metrics.scaleX=scaleX;metrics.scaleY=scaleY;footerMetrics=metrics;return true;
    }
    void footerRejected(const char* reason,const DebugContent& c,std::uint32_t canvas,std::uint32_t stack) {
        (void)reason;(void)c;(void)canvas;(void)stack;
#if HORN_ENABLE_LOGGING
        if(footerRejectLogs>=4)return;
        ++footerRejectLogs;Reader r(host);
        HORN_LOGI(kTag,"native Debug Gear Builder Y footer rejected: stage=%s selected=%u count=%u font=%08X scale=(%.3f,%.3f) painted=(%u,%u) measured=(%.0f,%.0f) prefix=%.0f label=%.0f",
            reason,c.at,c.count,r.u32(canvas+0x3C),r.f32(stack+0x10),r.f32(stack+0x18),
            r.u32(stack+0x20),r.u32(stack+0x28),footerMetrics.width,footerMetrics.height,footerMetrics.prefix,footerMetrics.label);
#endif
    }
    std::uint64_t headerHash(const DebugTarget& t,bool main,std::uint32_t index) const {
        Reader r(host);std::uint32_t data=0,count=0;
        const auto& property=main?schema.mainHeaders:schema.subHeaders;
        if (!r.typedArray(t.ui+property.offset,schema.header.size,512,data,count) || index>=count)return 0;
        return r.stringHash(data+index*schema.header.size+schema.headerText.offset);
    }
    void storeHit(DebugHit hit) {
        auto found=std::find_if(geometry.begin(),geometry.end(),[&](const DebugHit& old){return old.kind==hit.kind&&old.index==hit.index;});
        if(found!=geometry.end()) *found=hit;
        else if(geometry.size()<640) geometry.push_back(hit);
        geometryAt=monotonicMilliseconds();
        if (!geometryLogged && hit.kind==DebugHitKind::Row) {
            geometryLogged=true;
            HORN_LOGI(kTag,"native Debug Canvas row captured: index=%u rect=(%.1f,%.1f,%.1f,%.1f)",
                hit.index,hit.left,hit.top,hit.right,hit.bottom);
        }
    }
    bool fontScope(std::uint32_t canvas,std::uint32_t frame) const {
        if(!fontHooks || !preparedValid || !schema.fontReady || !valid(prepared))return false;
        Reader r(host);
        if(!r.object(canvas) || r.className(canvas)!="Canvas")return false;
        std::array<std::uint32_t,8> seen{};
        for(std::size_t depth=0;frame && depth<seen.size();++depth) {
            if(!r.span(frame,0x28) || r.u32(frame+0x14)!=prepared.ui ||
                std::find(seen.begin(),seen.begin()+depth,frame)!=seen.begin()+depth)return false;
            seen[depth]=frame;
            const auto function=r.u32(frame+0x10);
            if(!r.object(function) || r.className(function)!="Function" ||
                r.u32(function+0x28)!=r.u32(prepared.ui+0x34))return false;
            const auto name=r.objectName(function);
            if(name=="RenderMenu")return true;
            if(name!="RenderMenuHeaders" && name!="RenderSubMenuHeaders" && name!="DrawShadowText" &&
                name!="DrawShadowTextBounded" && name!="GetTextXPos" && name!="GetTextYPos")return false;
            frame=r.u32(frame+0x20);
        }
        return false;
    }
    void writeColumnWidth(std::uint32_t data,std::uint32_t i,float value) {
        std::uint32_t bits;std::memcpy(&bits,&value,4);
        host.MemoryWrite32(data+i*schema.columnLayout.size+schema.columnWidth.offset,bits);
    }
    std::uint64_t fontMetricSignature(const DebugContent& c,std::uint32_t layout,std::uint32_t columns,std::uint32_t count) const {
        // First whole-menu font call each frame, before the cache decision.
        // A native refresh can reuse every outer address while editing text.
        // Bound all work; larger menus keep the original measurement fallback.
        if(c.count>512 || count>16)return 0;
        Reader r(host);const auto font=r.u32(layout+schema.columnFont.offset);
        const auto fontToken=font?r.token(font):0;if(font && !fontToken)return 0;
        std::uint64_t signature=debugMix(1469598103934665603ull,fontToken);
        for(const auto* field:{&schema.bgX,&schema.bgWidth,&schema.bgY,&schema.bgHeight,&schema.columnHeight,
                              &schema.columnPadding,&schema.descriptionX,&schema.descriptionWidth})
            signature=debugMix(signature,r.u32(layout+field->offset));
        std::uint32_t remaining=65536;
        const auto text=[&](std::uint32_t p) {
            std::uint32_t data=0,size=0;
            if(!r.typedArray(p,1,4096,data,size) || size>remaining || (size && host.MemoryRead8(data+size-1)))return false;
            remaining-=size;signature=debugMix(debugMix(signature,size),r.stringHash(p));return true;
        };
        for(std::uint32_t i=0;i<count;++i)
            if(!text(columns+i*schema.columnLayout.size+schema.columnHeading.offset))return 0;
        for(const auto* field:{&schema.mainHeaders,&schema.subHeaders}) {
            std::uint32_t data=0,headers=0;if(!r.typedArray(prepared.ui+field->offset,schema.header.size,64,data,headers))return 0;
            signature=debugMix(signature,headers);
            for(std::uint32_t i=0;i<headers;++i) {
                const auto item=data+i*schema.header.size;
                if(!text(item+schema.headerText.offset))return 0;
                const auto icon=r.u32(item+schema.headerIcon.offset)&schema.headerIcon.extra;
                signature=debugMix(signature,icon);
                if(icon)signature=debugMix(signature,r.u32(item+schema.headerIconWidth.offset));
            }
        }
        for(std::uint32_t i=0;i<c.count;++i) {
            std::uint32_t data=0,size=0;
            if(!r.typedArray(c.data+i*schema.row.size+schema.columns.offset,schema.column.size,16,data,size))return 0;
            signature=debugMix(signature,size);
            for(std::uint32_t j=0;j<size;++j)
                if(!text(data+j*schema.column.size+schema.text.offset))return 0;
        }
        return signature?signature:1;
    }
    float fontScaleFor(std::uint32_t canvas) {
        Reader r(host);const auto c=content(prepared);if(!c.key)return 1;
        const float requested=Bl2InterfaceScale();
        const auto frame=host.MemoryRead64(r.base+kFrameCounter);
        const float clipX=r.f32(canvas+0x48),clipY=r.f32(canvas+0x4C);
        if(!std::isfinite(clipX) || !std::isfinite(clipY) || clipX<=0 || clipY<=0 ||
            clipX>prepared.width || clipY>prepared.height)return 1;
        // DrawShadowTextBounded temporarily narrows ClipX/OrgX. Its font must
        // use the same whole-menu factor already selected by native StrLen.
        const auto canvasToken=r.token(canvas);
        if(fontOwner==prepared.uiToken && fontGeneration==prepared.generation && fontScene==prepared.scene &&
            fontCanvas==canvasToken && fontViewportWidth==prepared.width && fontViewportHeight==prepared.height &&
            fontContent==c.key && fontFrame==frame && fontRequested==requested)return fontFactor;
        std::uint32_t columns=0,count=0;
        const auto layout=prepared.ui+schema.layout.offset;
        if(!r.typedArray(layout+schema.columnLayouts.offset,schema.columnLayout.size,16,columns,count))return 1;
        std::array<float,16> widths{};
        for(std::uint32_t i=0;i<count;++i) {
            widths[i]=r.f32(columns+i*schema.columnLayout.size+schema.columnWidth.offset);
            if(!std::isfinite(widths[i]) || widths[i]<0 || widths[i]>16384)return 1;
        }
        const DebugColumnCacheOwner currentOwner{prepared.uiToken,prepared.engineToken,prepared.clientToken,prepared.world};
        const bool sameOwner=fontCacheOwner==currentOwner;
        const bool sameColumns=sameOwner && columns==fontColumns && count==fontColumnCount;
        const bool sameStorage=sameColumns && fontGeneration==prepared.generation && fontScene==prepared.scene && fontContent==c.key && fontCanvas==canvasToken && fontClipX==clipX && fontClipY==clipY &&
            fontViewportWidth==prepared.width && fontViewportHeight==prepared.height;
        const DebugFontRefreshKey refresh{currentOwner,canvasToken,prepared.generation,prepared.scene,
            fontMetricSignature(c,layout,columns,count),c.main,c.sub,c.count,count,prepared.width,prepared.height,
            clipX,clipY,requested};
        const auto plan=debugFontFramePlan(fontRefreshKey,refresh,sameStorage,widths.data(),fontBaseline.data(),count,fontFactor);
        if(plan!=DebugFontFramePlan::Continue) {
            const bool reuse=plan==DebugFontFramePlan::ReuseRefresh;
            // Undo only our measured display cache, with the current registered
            // owner and the exact still-live ColumnLayout allocation. A newly
            // authored layout is left intact; native RenderMenu measures it.
            if(sameColumns && fontFactor>1)for(std::uint32_t i=0;i<count;++i) {
                const auto original=debugRestoreColumnWidth(widths[i],fontBaseline[i],fontFactor);
                if(original!=widths[i]) {
                    widths[i]=original;writeColumnWidth(columns,i,widths[i]);
                }
            }
            fontOwner=prepared.uiToken;fontGeneration=prepared.generation;fontScene=prepared.scene;
            fontCacheOwner=currentOwner;
            fontContent=c.key;fontFrame=frame;fontClipX=clipX;fontClipY=clipY;
            fontCanvas=canvasToken;fontViewportWidth=prepared.width;fontViewportHeight=prepared.height;
            fontColumns=columns;fontColumnCount=count;fontRequested=requested;fontRefreshKey=refresh;
            if(reuse) {
                // A native reset/reallocation of the same captions is not a
                // new font measurement. Preserve its fitted size in this
                // whole frame while fresh pointer geometry is captured.
                for(std::uint32_t i=0;i<count;++i)writeColumnWidth(columns,i,fontBaseline[i]*fontFactor);
                if(fontRefreshLogs++<4)HORN_LOGI(kTag,"native Debug font refresh retained scale=%.3f metrics=%llX rows=%u columns=%u; fresh pointer geometry",
                    fontFactor,static_cast<unsigned long long>(refresh.metrics),c.count,count);
                return fontFactor;
            }
            fontFactor=1;
            fontMinimum=widths;fontBaseline=widths;
            // One original frame supplies real font metrics, never guessed
            // glyph widths or a multiplied viewport/render-resolution setting.
            return 1;
        }
        fontFrame=frame;fontRequested=requested;fontRefreshKey.requested=requested;
        const float measuredFactor=fontFactor;
        for(std::uint32_t i=0;i<count;++i)
            fontBaseline[i]=std::max(fontMinimum[i],widths[i]/measuredFactor);
        const auto applyFactor=[&](float factor,float available) {
            (void)available;
            if(std::abs(factor-fontFactor)>.001f) {
                // Column widths are a native max-measurement cache. Reflow
                // from original metrics, including a live return to 100%.
                for(std::uint32_t i=0;i<count;++i)writeColumnWidth(columns,i,fontBaseline[i]*factor);
                fontFactor=factor;
                if(fontLogs<8) {
                    ++fontLogs;
                    HORN_LOGI(kTag,"native Debug font paired scale=%.3f requested=%.3f fit=%.1f columns=%u; original StrLen/DrawText reflow",fontFactor,requested,available,count);
                }
            }
            return fontFactor;
        };
        // Original 100% does not depend on the preceding enlarged paint or
        // its geometry cache; restore native measurement before pagination.
        if(requested==1)return applyFactor(1,0);
        const auto retain=[&] {return applyFactor(std::min(measuredFactor,requested),0);};
        const float scaleX=clipX/1024.f,scaleY=clipY/1024.f;
        const auto bgX=static_cast<std::int32_t>(r.u32(layout+schema.bgX.offset));
        const auto bgWidth=static_cast<std::int32_t>(r.u32(layout+schema.bgWidth.offset));
        const auto padding=static_cast<std::int32_t>(r.u32(layout+schema.columnPadding.offset));
        if(bgX<0 || bgWidth<=20 || bgWidth>16384 || padding<0 || padding>16384)return retain();
        // Header centring and body columns use the original background width;
        // reserve its authored margins and the real Canvas viewport edge.
        const float available=std::min((bgWidth-20)*scaleX,clipX-(bgX+20)*scaleX);
        DebugTextExtent extents[3]{};
        for(unsigned group=0;group<2;++group) {
            const auto& property=group?schema.subHeaders:schema.mainHeaders;
            std::uint32_t data=0,headers=0;
            if(!r.typedArray(prepared.ui+property.offset,schema.header.size,64,data,headers))return retain();
            auto& extent=extents[group];extent.fixed=headers?18.f*scaleX*(headers-1):0;
            for(std::uint32_t i=0;i<headers;++i) {
                const auto item=data+i*schema.header.size;
                float icon=0;
                if(group && (r.u32(item+schema.headerIcon.offset)&schema.headerIcon.extra)) {
                    const auto original=static_cast<std::int32_t>(r.u32(item+schema.headerIconWidth.offset));
                    if(original<0 || original>16384)return retain();
                    icon=original+4*scaleX;
                }
                // RenderMenuHeaders receives a local array; its Width cache
                // need not persist in MenuHeaders. Use its actual preceding
                // native painted caption, validated against this live header.
                const auto hash=r.stringHash(item+schema.headerText.offset);
                float width=0;
                if(hash) {
                    const auto kind=group?DebugHitKind::SubHeader:DebugHitKind::MainHeader;
                    const auto hit=std::find_if(geometry.begin(),geometry.end(),[&](const DebugHit& value) {
                        return value.kind==kind && value.index==i && value.hash==hash;
                    });
                    if(geometryContent!=c.key || hit==geometry.end())return retain();
                    width=hit->right-hit->left;
                    if(!std::isfinite(width) || width<=0 || width>prepared.width)return retain();
                }
                extent.text+=width/measuredFactor;extent.fixed+=icon;
            }
        }
        extents[2].fixed=count?padding*scaleX*(count-1):0;
        for(std::uint32_t i=0;i<count;++i) {
            extents[2].text+=fontBaseline[i];
        }
        const auto descriptionX=static_cast<std::int32_t>(r.u32(layout+schema.descriptionX.offset));
        const auto descriptionWidth=static_cast<std::int32_t>(r.u32(layout+schema.descriptionWidth.offset));
        if(descriptionX<0 || descriptionWidth<0 || descriptionX>16384 || descriptionWidth>16384)return retain();
        // RenderMenu pushes a side description to max(authored X, table-end
        // plus 15*UIScaleX). Keep its original available width when enlarging
        // the table, rather than forcing that pane into a narrower wrap box.
        if(descriptionX>0 && descriptionWidth>0) {
            const auto pane=std::min(descriptionWidth,std::max(0,bgX+bgWidth-descriptionX));
            extents[2].fixed+=(pane+15)*scaleX;
        }
        float factor=debugFontScale(available,extents,3,requested);
        // A compact vertical viewport must keep at least one original row.
        // Native RenderMenu recomputes Menu_IPP from the enlarged StrLen("Q");
        // this bound uses its actual preceding paint and authored body area.
        if(c.count) {
            const auto bgY=static_cast<std::int32_t>(r.u32(layout+schema.bgY.offset));
            const auto bgHeight=static_cast<std::int32_t>(r.u32(layout+schema.bgHeight.offset));
            const auto columnHeight=static_cast<std::int32_t>(r.u32(layout+schema.columnHeight.offset));
            float firstY=clipY;
            for(const auto& hit:geometry)if(hit.kind==DebugHitKind::Row)firstY=std::min(firstY,hit.top);
            const auto body=columnHeight>0?columnHeight:bgHeight;
            if(bgY<0 || bgY>16384 || body<=10 || body>16384 || geometryContent!=c.key || firstY>=clipY || c.ipp<1)factor=1;
            else {
                const float rows=rowPitch*std::min(c.count,2u);
                const DebugTextExtent vertical{std::max(0.f,firstY-bgY*scaleY+rows)/measuredFactor,0};
                factor=std::min(factor,debugFontScale((body-10)*scaleY-std::max(1.f,scaleY),&vertical,1,requested));
            }
        }
        return applyFactor(factor,available);
    }
    static bool scaledBits(std::uint32_t bits,float factor,std::uint32_t& result) {
        float value;std::memcpy(&value,&bits,4);
        if(!std::isfinite(value) || value<=0 || value>4 || !std::isfinite(factor) || factor<1 || factor>2)return false;
        value*=factor;std::memcpy(&result,&value,4);return true;
    }
    void scaleDiagnostic(bool draw,std::uint32_t canvas,std::uint32_t frame,float factor,bool scope,
                         std::uint32_t oldX,std::uint32_t oldY,bool xOk,bool yOk,std::uint32_t x,std::uint32_t y) {
        (void)draw;(void)canvas;(void)frame;(void)factor;(void)scope;(void)oldX;(void)oldY;
        (void)xOk;(void)yOk;(void)x;(void)y;
#if HORN_ENABLE_LOGGING
        Reader r(host);auto& budget=draw?drawLogs:measureLogs;
        if(budget>=8 || (scope && factor==1 && budget>=2) || !preparedValid ||
            !r.span(frame,0x28) || r.u32(frame+0x14)!=prepared.ui)return;
        ++budget;
        const auto parent=r.u32(frame+0x20),grand=r.span(parent,0x28)?r.u32(parent+0x20):0;
        const auto function=r.objectName(r.u32(frame+0x10));
        const auto parentName=r.span(parent,0x28)?r.objectName(r.u32(parent+0x10)):std::string{};
        const auto grandName=r.span(grand,0x28)?r.objectName(r.u32(grand+0x10)):std::string{};
        float valueX=0,valueY=0;std::memcpy(&valueX,&oldX,4);std::memcpy(&valueY,&oldY,4);
        HORN_LOGI(kTag,"native Debug font seam=%s scope=%u factor=%.3f args=%08X/%08X(%.3f/%.3f) accepted=%u/%u out=%08X/%08X frame=%s parent=%s grand=%s canvas=%08X cached=%.3f content=%llX",
            draw?"draw":"measure",scope?1u:0u,factor,oldX,oldY,valueX,valueY,xOk?1u:0u,yOk?1u:0u,x,y,
            function.c_str(),parentName.c_str(),grandName.c_str(),canvas,fontFactor,static_cast<unsigned long long>(fontContent));
#endif
    }
    void measureScale(Dynarmic::A32::Jit& jit) {
        if(!fontHooks || !preparedValid)return;
        Reader r(host);const auto canvas=jit.Regs()[4],stack=jit.Regs()[13];
        if(!r.span(stack,DebugStrLenFrameAbi::localBytes))return;
        const auto frame=r.u32(stack+DebugStrLenFrameAbi::frameSlot);const bool scope=fontScope(canvas,frame);
        if(!scope) {
#if HORN_ENABLE_LOGGING
            if(!preparedValid || measureLogs>=8)return;
#else
            return;
#endif
        }
        const float factor=scope?fontScaleFor(canvas):1;std::uint32_t x=0,y=0;
        const auto oldX=jit.Regs()[2],oldY=jit.Regs()[3];
        const bool xOk=scaledBits(oldX,factor,x),yOk=scaledBits(oldY,factor,y);
        scaleDiagnostic(false,canvas,frame,factor,scope,oldX,oldY,xOk,yOk,x,y);
        if(!scope || factor==1 || !xOk || !yOk)return;
        jit.Regs()[2]=x;jit.Regs()[3]=y;
    }
    void saveMeasureFrame(Dynarmic::A32::Jit& jit) {
        if(!fontHooks || !preparedValid)return;
        Reader r(host);const auto canvas=jit.Regs()[4],frame=jit.Regs()[5],stack=jit.Regs()[13];
        if(!r.span(stack,DebugStrLenFrameAbi::localBytes))return;
        // The slot is this invocation's unused padding, never Canvas storage.
        // Clear it for another caller so a reused native stack cannot inherit
        // an earlier UI frame. No foreign Canvas or input is changed.
        host.MemoryWrite32(stack+DebugStrLenFrameAbi::frameSlot,fontScope(canvas,frame)?frame:0);
    }
    void drawScale(Dynarmic::A32::Jit& jit) {
        const auto canvas=jit.Regs()[5],frame=jit.Regs()[4],stack=jit.Regs()[13];
        Reader r(host);if(!r.span(stack,0x1C))return;
        const bool scope=fontScope(canvas,frame);
        if(!scope) {
#if HORN_ENABLE_LOGGING
            if(!preparedValid || drawLogs>=8)return;
#else
            return;
#endif
        }
        const float factor=scope?fontScaleFor(canvas):1;std::uint32_t x=0,y=0;
        const auto oldX=r.u32(stack+0x10),oldY=r.u32(stack+0x18);
        const bool xOk=scaledBits(oldX,factor,x),yOk=scaledBits(oldY,factor,y);
        scaleDiagnostic(true,canvas,frame,factor,scope,oldX,oldY,xOk,yOk,x,y);
        if(!scope || factor==1 || !xOk || !yOk)return;
        host.MemoryWrite32(stack+0x10,x);host.MemoryWrite32(stack+0x18,y);
    }
    void capture(Dynarmic::A32::Jit& jit) {
        if (!preparedValid || !schema.ready || !valid(prepared)) return;
        Reader r(host);const auto canvas=jit.Regs()[5],frame=jit.Regs()[4],stack=jit.Regs()[13];
        if (!r.object(canvas) || r.className(canvas)!="Canvas" || !r.span(frame,0x28) || !r.span(stack,0x3C) ||
            r.u32(frame+0x14)!=prepared.ui) return;
        const auto function=r.u32(frame+0x10);
        if (r.className(function)!="Function" || r.u32(function+0x28)!=r.u32(prepared.ui+0x34))return;
        const auto functionName=r.objectName(function);
        if(functionName!="DrawShadowText" && functionName!="DrawShadowTextBounded")return;
        const auto parent=r.u32(frame+0x20);
        if(!r.span(parent,0x28) || r.u32(parent+0x14)!=prepared.ui)return;
        const auto parentName=r.objectName(r.u32(parent+0x10));
        const auto hash=r.stringHash(stack+0x30);if(!hash)return;
        const auto c=content(prepared);if(!c.key)return;
        const float x=r.f32(canvas+0x40)+r.f32(canvas+0x50),y=r.f32(canvas+0x44)+r.f32(canvas+0x54);
        const auto width=r.u32(stack+0x20),height=r.u32(stack+0x28);
        if(!std::isfinite(x)||!std::isfinite(y)||x<0||y<0||x>=prepared.width||y>=prepared.height||
            !height||height>prepared.height||width>prepared.width)return;
        if (geometryContent!=c.key || geometryTop!=c.top) {
            geometry.clear();geometryContent=c.key;geometryTop=c.top;lastPaintRow=-1;measuredRowPitch=0;
        }
        DebugHit hit;hit.hash=hash;hit.left=x;hit.top=y;hit.right=x+width;hit.bottom=y+height;
        if(functionName=="DrawShadowTextBounded") {
            DebugBuildFooterRange range;std::string hint;
            const auto locals=r.u32(frame+0x1C);
            if(parentName!="RenderMenu" || function!=schema.footerFunction ||
                !buildFooter(prepared,c,range,hint))return;
            if(!r.span(locals,schema.footerText.offset+12) ||
                r.stringHash(locals+schema.footerText.offset)!=hash ||
                hash!=r.stringHash(prepared.ui+schema.hint.offset)) {footerRejected("frame-text",c,canvas,stack);return;}
            if(!measureFooter(canvas,stack,hint,range)) {footerRejected("font-measure",c,canvas,stack);return;}
            float rowBottom=0;
            for(const auto& row:geometry)if(row.kind==DebugHitKind::Row)rowBottom=std::max(rowBottom,row.bottom);
            DebugBuildFooterRect rect;
            if(!debugBuildFooterRect(x,y,float(width),float(height),footerMetrics.width,footerMetrics.height,
                footerMetrics.prefix,footerMetrics.label,rowBottom,prepared.width,prepared.height,rect)) {
                footerRejected("paint-fit",c,canvas,stack);return;
            }
            hit.kind=DebugHitKind::BuildFooter;hit.index=c.at;
            hit.left=rect.left;hit.top=rect.top;hit.right=rect.right;hit.bottom=rect.bottom;
            storeHit(hit);
            if(footerLogs++<4)HORN_LOGI(kTag,"native Debug Gear Builder Y footer captured: selected=%u count=%u rect=(%.1f,%.1f,%.1f,%.1f) text=%s",
                c.at,c.count,hit.left,hit.top,hit.right,hit.bottom,hint.c_str());
            return;
        }
        if (parentName=="RenderMenu") {
            const auto end=std::min(c.count,c.top+c.ipp);
            int found=-1;
            // Paint order disambiguates repeated values; never choose a row by
            // text alone when the visible row order/Canvas Y disagrees.
            if(y<lastPaintY-.5f)lastPaintRow=-1;
            for(std::uint32_t i=c.top;i<end;++i) if(rowHash(c,i)==hash && selectable(c,i) &&
                (static_cast<int>(i)>lastPaintRow || (static_cast<int>(i)==lastPaintRow&&std::abs(y-lastPaintY)<.5f))) {found=i;break;}
            if(found<0)return;
            hit.kind=DebugHitKind::Row;hit.index=found;
            const auto layout=prepared.ui+schema.layout.offset;
            const auto bgX=static_cast<std::int32_t>(r.u32(layout+schema.bgX.offset));
            const auto bgWidth=r.u32(layout+schema.bgWidth.offset);const float scale=r.f32(canvas+0x48)/1024.f;
            if(bgX>=0&&bgWidth<=16384&&std::isfinite(scale)&&scale>0) {
                hit.left=std::max(0.f,(bgX+10)*scale+r.f32(canvas+0x40));
                hit.right=std::min(float(prepared.width),(bgX+bgWidth)*scale+r.f32(canvas+0x40));
            }
            if (lastPaintRow>=0 && found>lastPaintRow && y>lastPaintY) {
                const float pitch=(y-lastPaintY)/(found-lastPaintRow);
                if (pitch>=1 && pitch<=prepared.height) {
                    measuredRowPitch=pitch;
                    for(auto& old:geometry)if(old.kind==DebugHitKind::Row)old.bottom=old.top+pitch;
                }
            }
            rowPitch=measuredRowPitch>0?measuredRowPitch:float(height);
            hit.bottom=y+rowPitch;lastPaintRow=found;lastPaintY=y;
        } else if(parentName=="RenderMenuHeaders" || parentName=="RenderSubMenuHeaders") {
            const bool main=parentName=="RenderMenuHeaders";
            std::uint32_t data=0,count=0;const auto& property=main?schema.mainHeaders:schema.subHeaders;
            if(!r.typedArray(prepared.ui+property.offset,schema.header.size,512,data,count))return;
            std::uint32_t i=0;for(;i<count;++i)if(headerHash(prepared,main,i)==hash)break;
            if(i==count)return;
            hit.kind=main?DebugHitKind::MainHeader:DebugHitKind::SubHeader;hit.index=i;
        } else return;
        storeHit(hit);
    }
    bool hitAt(float x,float y,DebugHit& result) const {
        if (!geometryAt || monotonicMilliseconds()-geometryAt>500 || geometryContent!=targetContent)return false;
        float best=1e9f;bool found=false;
        for(const auto& hit:geometry) {
            const float padding=std::max(3.f,.004f*target.height),center=(hit.top+hit.bottom)/2;
            const float horizontal=hit.kind==DebugHitKind::BuildFooter?0.f:padding;
            if(x<hit.left-horizontal||x>hit.right+horizontal||y<hit.top-padding||y>hit.bottom+padding)continue;
            const float distance=std::abs(y-center);
            if(distance<best) {best=distance;result=hit;found=true;}
        }
        return found;
    }
    bool valid(const DebugTarget& t) const {
        Reader r(host);
        if (!signatures || !generation || generation()!=t.generation || !t.uiToken ||
            r.token(t.ui)!=t.uiToken || r.token(t.controller)!=t.controllerToken ||
            r.u32(r.base+0x0300D940)!=t.world || r.token(t.client)!=t.clientToken ||
            r.u32(t.client+t.viewportUi.offset)!=t.ui || r.u32(t.ui+0x28)!=t.client ||
            !(r.u32(t.ui+t.open.offset)&t.open.extra) || r.u32(t.ui+t.owner.offset)!=t.controller ||
            r.u32(t.ui+t.semaphore.offset)!=0 || !r.span(t.viewport,0x44) ||
            r.u32(t.viewport+0x3C)!=t.width || r.u32(t.viewport+0x40)!=t.height) return false;
        std::uint32_t data=0,count=0;
        const auto engine=r.u32(r.base+0x03008D20);
        if (engine!=t.engine || r.token(engine)!=t.engineToken || !t.engineViewport || t.engineViewport.size!=4 ||
            r.u32(engine+t.engineViewport.offset)!=t.client ||
            !r.array(engine+0x48C,data,count) || count!=1) return false;
        const auto local=r.u32(data);
        return r.object(local) && r.u32(local+0x40)==t.controller && r.u32(local+0x64)==t.controllerId;
    }
    const char* currentReason(bool checkContent=true) const {
        if (!frameValid) return "frame-invalid";
        if (physicalOwnership().ownsInput(monotonicMilliseconds())) return "physical-owner";
        if (!valid(target)) return "owner-lifetime";
        const auto snapshot=sharedMailbox().snapshot();
        if (checkContent && targetContent && content(target).key!=targetContent) return "content-changed";
        if (snapshot.epoch!=epoch) return "epoch-changed";
        if (snapshot.context.sceneIdentity!=target.scene) return "scene-changed";
        if (!(snapshot.context.capabilities&DebugCanvas)) return "debug-capability-lost";
        if (snapshot.context.capabilities&PhysicalOwnsInput) return "physical-capability";
        return nullptr;
    }
    bool current() const { return currentReason()==nullptr; }
    DebugRowCaptureKey rowCaptureKey(const DebugTarget& t,const DebugContent& c) const {
        if(!schema.captureReady || !schema.fontReady || !hasCapturedHit ||
            (capturedHit.kind!=DebugHitKind::Row && capturedHit.kind!=DebugHitKind::BuildFooter) ||
            !c.key || c.count>512 || !selectable(c,capturedHit.index) || !valid(t) ||
            !inputReceiver())return {};
        Reader r(host);const auto layout=t.ui+schema.layout.offset;
        std::uint32_t columns=0,columnCount=0;
        if(!r.typedArray(layout+schema.columnLayouts.offset,schema.columnLayout.size,16,columns,columnCount))return {};
        DebugRowCaptureKey key;
        key.owner={t.uiToken,t.engineToken,t.clientToken,t.world};key.controller=t.controllerToken;
        key.schema=r.token(r.u32(t.ui+0x34));key.input=inputFunctionToken;
        if(key.schema!=schemaClass || key.schema!=inputClass)return {};
        key.scene=t.scene;key.generation=t.generation;key.epoch=epoch;key.viewport=t.viewport;
        key.controllerId=t.controllerId;key.width=t.width;key.height=t.height;
        key.kind=static_cast<std::uint32_t>(capturedHit.kind);key.index=capturedHit.index;key.selected=c.at;
        key.count=c.count;key.top=c.top;key.ipp=c.ipp;key.main=c.main;key.sub=c.sub;key.requested=Bl2InterfaceScale();
        key.menu=fontMetricSignature(c,layout,columns,columnCount);if(!key.menu)return {};
        std::uint32_t remaining=65536;
        const auto text=[&](std::uint64_t& hash,std::uint32_t p) {
            std::uint32_t data=0,size=0;
            if(!r.typedArray(p,1,4096,data,size) || size>remaining || (size && host.MemoryRead8(data+size-1)))return false;
            remaining-=size;hash=debugMix(debugMix(hash,size),r.stringHash(p));return true;
        };
        // Labels/layout alone cannot identify the command behind a row. Bind
        // every original command and payload, as well as the routed menu stack.
        for(const auto* field:{&schema.mainHeaders,&schema.subHeaders}) {
            std::uint32_t data=0,count=0;
            if(!r.typedArray(t.ui+field->offset,schema.header.size,64,data,count))return {};
            for(std::uint32_t i=0;i<count;++i)
                if(!text(key.menu,data+i*schema.header.size+schema.headerInterface.offset))return {};
        }
        std::uint32_t stack=0,depth=0;if(!r.typedArray(t.ui+schema.stack.offset,12,128,stack,depth))return {};
        key.menu=debugMix(key.menu,depth);
        for(std::uint32_t i=0;i<depth;++i)if(!text(key.menu,stack+i*12))return {};
        key.commands=1469598103934665603ull;
        const auto row=c.data+capturedHit.index*schema.row.size;
        for(const auto& field:schema.rowIntentText)if(!text(key.commands,row+field.offset))return {};
        if(capturedHit.kind==DebugHitKind::BuildFooter) {
            DebugBuildFooterRange range;std::string hint;
            if(c.at!=capturedHit.index || !buildFooter(t,c,range,hint) ||
                r.stringHash(t.ui+schema.hint.offset)!=capturedHit.hash)return {};
            key.footer=capturedHit.hash;
        } else if(rowHash(c,capturedHit.index)!=capturedHit.hash)return {};
        return key;
    }
    bool capturedRowCurrent(const DebugTarget& t,const DebugContent& c) const {
        if(!gesture.captured() || !hasCapturedHit)return true;
        return debugCapturedRowCurrent(true,static_cast<std::uint32_t>(capturedHit.kind),
            capturedRowKey,rowCaptureKey(t,c));
    }
    bool preserveRowCapture(const DebugTarget& t,const DebugContent& c,bool ownSelection=false) {
        if(!gesture.captured() || !hasCapturedHit || currentReason(false))return false;
        const auto fresh=rowCaptureKey(t,c);
        if(!debugPreserveRowCapture(capturedRowKey,fresh,ownSelection))return false;
        const bool changed=c.key!=captureContent;
        capturedRowKey=fresh;captureContent=c.key;targetContent=c.key;
        if(changed)diagnostic("capture","identical-original-row-refresh",capturedHit.index);
        return true;
    }
    bool key(Action action,bool page=false) {
        if (const auto reason=currentReason()) {diagnostic("key",reason);return false;}
        if (!dispatchBudget) {diagnostic("key","dispatch-budget");return false;}
        if (!inputReceiver()) {diagnostic("key","script-delegate-mismatch");return false;}
        --dispatchBudget;
        Reader r(host);
        for (const auto& ordinary:debugKeys) if (ordinary.action==action) {
            if(page && action!=Action::UiUp && action!=Action::UiDown)return false;
            const auto& key=page?debugPageKeys[action==Action::UiDown?1:0]:ordinary;
            const auto address=r.base+key.rva;
            if (r.name(address)!=key.name) {diagnostic("key","key-name-mismatch",key.rva);return false;}
            const auto index=r.u32(address),number=r.u32(address+4);
            if(!inputScratch)inputScratch=host.GuestAlloc(sizeof(DebugInputParameters),4);
            if(!r.span(inputScratch,sizeof(DebugInputParameters))) {diagnostic("key","scratch-unavailable");return false;}
            const auto invoke=[&](std::uint8_t event) {
                DebugInputParameters parameters;
                parameters.controllerId=target.controllerId;parameters.keyIndex=index;parameters.keyNumber=number;parameters.event=event;
                std::array<std::uint32_t,sizeof(parameters)/4> words{};
                std::memcpy(words.data(),&parameters,sizeof(parameters));
                for(std::size_t i=0;i<words.size();++i)host.MemoryWrite32(inputScratch+static_cast<std::uint32_t>(i*4),words[i]);
                // Match UPlayerManagerInteraction::InputKey's ProcessDelegate
                // call. Its script delegate owns Menu_At, tab changes, command
                // handling and CloseMenu; the C++ InputKey is only a SceneClient
                // fallback and does not run the original Debug script.
#if defined(BL2_NATIVE_ARM32)
                host.RunGuestCallback(r.base+kProcessDelegate,{target.ui,r.u32(r.base+kNativeInputDelegateName),
                    r.u32(r.base+kNativeInputDelegateName+4),target.ui+target.inputDelegate.offset,inputScratch,0});
#else
                std::uint32_t returned=0;
                if(!host.CallGuest(r.base+kProcessDelegate,{target.ui,r.u32(r.base+kNativeInputDelegateName),
                    r.u32(r.base+kNativeInputDelegateName+4),target.ui+target.inputDelegate.offset,inputScratch,0},returned)) return false;
#endif
                return (r.u32(inputScratch+offsetof(DebugInputParameters,handled))&1u)!=0;
            };
#if HORN_ENABLE_LOGGING
            const auto before=content(target);
#endif
            const bool handled=invoke(0);
            // Original bracket/arrow navigation deliberately rebuilds CurrentMenu
            // and SubMenuHeaders. Accept that change only after our handled
            // navigation command, with every owner/epoch/scene guard still live.
            // This permits its release and the next step of a header selection;
            // unrelated content changes continue to cancel pending gestures.
            if(handled && debugCategoryNavigation(action) && !currentReason(false)) {
                const auto navigated=content(target);
                if(navigated.key)targetContent=navigated.key;
            }
#if HORN_ENABLE_LOGGING
            const auto after=content(target);
#endif
            if(!currentReason(false) && inputReceiver())invoke(1);
#if HORN_ENABLE_LOGGING
            if(diagnosticLogs<48) {
                ++diagnosticLogs;
                HORN_LOGI(kTag,"native Debug delegate key=%s handled=%u selected=%u->%u count=%u->%u top=%u->%u ipp=%u->%u headers=(%u,%u)->(%u,%u) opened=%u",
                    key.name,handled?1u:0u,before.at,after.at,before.count,after.count,before.top,after.top,
                    before.ipp,after.ipp,before.main,before.sub,after.main,after.sub,
                    (r.u32(target.ui+target.open.offset)&target.open.extra)?1u:0u);
            }
#endif
            return handled;
        }
        return false;
    }
    void pageDiagnostic(Action action,const char* stage,const DebugContent& before,const DebugContent& after) {
        (void)action;(void)stage;(void)before;(void)after;
#if HORN_ENABLE_LOGGING
        if(pageLogs>=8)return;
        ++pageLogs;
        HORN_LOGI(kTag,"native Debug page direction=%s stage=%s selected=%u->%u count=%u->%u top=%u->%u ipp=%u->%u geometry_entries=%zu pitch=%.1f",
            action==Action::UiDown?"Down":"Up",stage,before.at,after.at,before.count,after.count,
            before.top,after.top,before.ipp,after.ipp,geometry.size(),rowPitch);
#endif
    }
    bool navigate(Action action,bool* paged=nullptr) {
        if(paged)*paged=false;
        if(action!=Action::UiUp && action!=Action::UiDown)return key(action);
        const auto before=content(target);
        if(!before.key) {pageDiagnostic(action,"missing-content",before,{});return false;}
        if(!key(action)) {pageDiagnostic(action,"arrow-unhandled",before,{});return false;}
        if(const auto reason=currentReason()) {pageDiagnostic(action,reason,before,{});return false;}
        const auto after=content(target);
        if(after.key!=before.key) {pageDiagnostic(action,"content-changed",before,after);return false;}
        if(!debugPageFallback(action,after.count,before.at,after.at,after.top,after.ipp)) {
            if(after.at==before.at)pageDiagnostic(action,"page-not-needed",before,after);
            return after.at!=before.at || after.top!=before.top;
        }
        // Re-enter the same guarded original delegate with the real Page key.
        // Its UpdateLeave/UpdateOver, row clamp and scrolling stay authoritative.
        if(!key(action,true)) {pageDiagnostic(action,"page-unhandled",before,after);return false;}
        if(const auto reason=currentReason()) {pageDiagnostic(action,reason,before,{});return false;}
        const auto next=content(target);
        if(next.key!=before.key) {pageDiagnostic(action,"page-content-changed",before,next);return false;}
        if(next.at==after.at && next.top==after.top) {pageDiagnostic(action,"page-no-progress",before,next);return false;}
        pageDiagnostic(action,"page-applied",before,next);
        gesture.cancel();hasCapturedHit=false;
        if(paged)*paged=true;
        return true;
    }
    void tap(bool activate = true) {
        if (!hasCapturedHit || !current()) return;
        auto c=content(target);
        if(!c.key || c.key!=captureContent)return;
        if(capturedHit.kind==DebugHitKind::BuildFooter) {
            if (!activate) return;
            DebugBuildFooterRange range;std::string hint;Reader r(host);
            if(c.at!=capturedHit.index || !buildFooter(target,c,range,hint) ||
                r.stringHash(target.ui+schema.hint.offset)!=capturedHit.hash)return;
            // Preserve the script's real tertiary path: BuildWeapon/BuildItem,
            // clear parts, CloseMenu, then ordinary gameplay pickup of its loot.
            key(kDebugBuildFooterAction);
        } else if(capturedHit.kind==DebugHitKind::Row) {
            const auto row=capturedHit.index;
            if(rowHash(c,row)!=capturedHit.hash || !selectable(c,row))return;
            if(c.at==row) {if (activate) key(Action::Confirm);return;}
            // Preserve original UpdateLeave/UpdateOver and unselectable rows;
            // a tap selects, the following tap runs the original primary cmd.
            for(unsigned i=0;i<64&&c.at!=row;++i) {
                const auto before=c.at;
                if(!key(c.at<row?Action::UiDown:Action::UiUp) || !current())return;
                c=content(target);
                if(c.key!=captureContent || rowHash(c,row)!=capturedHit.hash || c.at==before)return;
                if ((before<row && c.at>row)||(before>row && c.at<row))return;
            }
        } else {
            if (!activate) return; // Category transitions remain UP-only.
            const bool main=capturedHit.kind==DebugHitKind::MainHeader;
            const auto index=capturedHit.index;
            if(headerHash(target,main,index)!=capturedHit.hash)return;
            for(unsigned i=0;i<64&&(main?c.main:c.sub)!=index;++i) {
                const auto before=main?c.main:c.sub;
                const Action direction=main?(before<index?Action::Inventory:Action::PreviousWeapon):
                    (before<index?Action::UiRight:Action::UiLeft);
                if(!key(direction) || !valid(target) || physicalOwnership().ownsInput(monotonicMilliseconds()))return;
                c=content(target);
                if(!c.key || (main?c.main:c.sub)==before || headerHash(target,main,index)!=capturedHit.hash)return;
                targetContent=c.key;
                if(!current())return;
            }
        }
    }
};
Ue3DebugMenuSink::Ue3DebugMenuSink(DynarmicHost& host,LifetimeGeneration generation)
    :state_(std::make_unique<State>(host,generation)) {}
Ue3DebugMenuSink::~Ue3DebugMenuSink()=default;
bool Ue3DebugMenuSink::verified() const { return state_->signatures; }
std::uint32_t Ue3DebugMenuSink::prepare(std::uint32_t viewport,std::uint32_t controller,std::uint64_t scene) {
    auto& s=*state_; s.preparedValid=false;
    if (!s.signatures || !viewport || !controller || !scene) return 0;
    Reader r(s.host); DebugTarget t;
    t.viewport=viewport; t.controller=controller; t.scene=scene;
    t.world=r.u32(r.base+0x0300D940); t.width=r.u32(viewport+0x3C); t.height=r.u32(viewport+0x40);
    if (!t.width || !t.height || t.width>16384 || t.height>16384) return 0;
    const auto engine=r.u32(r.base+0x03008D20); t.engine=engine; t.engineToken=r.token(engine);
    if (!t.engineToken) return 0;
    t.generation=s.generation();
    const auto engineClass=r.token(r.u32(engine+0x34));
    const bool sameEngine=s.metadata.generation==t.generation && s.metadata.scene==scene &&
        s.metadata.world==t.world && s.metadata.engineToken==t.engineToken && s.metadataEngineClass==engineClass;
    // +0x48C is GamePlayers: +0x490 is its element count, never a UObject.
    // FViewport::Client is a native interface subobject; resolve its UObject
    // owner through the real reflected Engine.GameViewport property instead.
    t.engineViewport=sameEngine?s.metadata.engineViewport:r.objectProperty(engine,"GameViewport","ObjectProperty");
    if (!t.engineViewport || t.engineViewport.size!=4) return 0;
    t.client=r.u32(engine+t.engineViewport.offset);t.clientToken=r.token(t.client);if(!t.clientToken)return 0;
    const auto clientClass=r.token(r.u32(t.client+0x34));
    const bool sameClient=s.metadata.generation==t.generation && s.metadata.scene==scene &&
        s.metadata.world==t.world && s.metadata.clientToken==t.clientToken && s.metadataClientClass==clientClass;
    if (sameClient) t.viewportUi=s.metadata.viewportUi;
    else {
        if(r.className(t.client)!="WillowGameViewportClient")return 0;
        t.viewportUi=r.objectProperty(t.client,"ViewportUI","ObjectProperty");
    }
    if (!t.viewportUi || t.viewportUi.size!=4) return 0;
    t.ui=r.u32(t.client+t.viewportUi.offset);
    t.uiToken=r.token(t.ui);if(!t.uiToken)return 0;
    const auto uiClass=r.token(r.u32(t.ui+0x34));
    const bool sameUi=sameClient && s.metadata.uiToken==t.uiToken && s.metadataUiClass==uiClass;
    if (sameUi) {
        t.open=s.metadata.open;t.owner=s.metadata.owner;t.semaphore=s.metadata.semaphore;
        t.inputDelegate=s.metadata.inputDelegate;
    }
    else {
        if(r.className(t.ui)!="WillowUIInteraction")return 0;
        t.open=r.objectProperty(t.ui,"bOpened","BoolProperty");
        t.owner=r.objectProperty(t.ui,"OwnerPC","ObjectProperty");
        t.semaphore=r.objectProperty(t.ui,"BlockUIInputSemaphore","IntProperty");
        t.inputDelegate=r.objectProperty(t.ui,"__OnReceivedNativeInputKey__Delegate","DelegateProperty");
        s.schema={};s.schemaClass=0;s.geometry.clear();s.geometryContent=0;s.measuredRowPitch=0;
    }
    if (!t.open || t.open.size!=4 || !t.open.extra || (t.open.extra&(t.open.extra-1)) ||
        !t.owner || t.owner.size!=4 || !t.semaphore || t.semaphore.size!=4 ||
        !t.inputDelegate || t.inputDelegate.size!=12 || t.inputDelegate.offset!=0x48) return 0;
    s.metadata=t;s.metadataEngineClass=engineClass;s.metadataClientClass=clientClass;s.metadataUiClass=uiClass;
    // The common gameplay path stops after the cached bOpened read. Metadata
    // remains bound to registered owner/class tokens, scene and GC generation.
    if(!(r.u32(t.ui+t.open.offset)&t.open.extra)) {s.fontContent=0;return 0;}
    t.controllerToken=r.token(controller);
    std::uint32_t data=0,count=0;
    if (!r.array(engine+0x48C,data,count) || count!=1) return 0;
    t.controllerId=r.u32(r.u32(data)+0x64);
    if (!s.valid(t)) return 0;
    s.readSchema(t.ui);
    s.readInputFunction(t.ui);
    auto c=s.content(t);
    if(s.fontHooks && s.fontOwner==t.uiToken && s.fontContent==c.key && s.fontFactor>1 && c.ipp) {
        const auto visible=debugVisibleTop(c.count,c.at,c.top,c.ipp);
        if(visible!=c.top) {
            // Only the display window follows the original selected row after
            // native font measurement changes its page capacity. No command or
            // Menu_At is changed; an old captured touch is cancelled safely.
            s.host.MemoryWrite32(t.ui+s.schema.top.offset,visible);c=s.content(t);
            s.gesture.cancel();s.hasCapturedHit=false;
        }
    }
    if (s.geometryContent!=c.key || s.geometryTop!=c.top) {
        s.geometry.clear();s.geometryContent=c.key;s.geometryTop=c.top;s.lastPaintRow=-1;s.measuredRowPitch=0;
    }
    s.prepared=t; s.preparedValid=true;
    if (s.logs++==0) HORN_LOGI(kTag,"native Debug touch admitted: UI=%08X controller=%08X size=%ux%u",t.ui,controller,t.width,t.height);
    return UiNavigation|DebugCanvas|(s.canvasHook && c.key?DirectUi:0u);
}
void Ue3DebugMenuSink::beginTouchFrame(const Frame& frame) {
    auto& s=*state_;
    const bool valid=frame.fresh && s.preparedValid && frame.context.sceneIdentity==s.prepared.scene &&
        (frame.context.capabilities&DebugCanvas) && s.valid(s.prepared);
    const auto c=valid?s.content(s.prepared):DebugContent{};
    const auto content=c.key;
    const bool capturedCurrent=valid && s.capturedRowCurrent(s.prepared,c);
    const bool identicalRefresh=content!=s.targetContent && !frame.resetReasons && valid && frame.epoch==s.epoch &&
        capturedCurrent && s.preserveRowCapture(s.prepared,c);
    if (frame.resetReasons || !valid || frame.epoch!=s.epoch || !capturedCurrent ||
        (content!=s.targetContent && !identicalRefresh)) {
        if(s.held || s.gesture.captured()) s.diagnostic("cancel",frame.resetReasons?"frame-reset":
            !valid?"prepared-invalid":frame.epoch!=s.epoch?"epoch-changed":
            !capturedCurrent?"captured-row-changed":"content-changed",frame.resetReasons);
        s.held=0;s.keyCancelled=false;s.gesture.clear();s.hasCapturedHit=false;
    }
    s.epoch=frame.epoch; s.frameValid=valid; s.target=s.prepared;
    s.targetContent=content;s.dispatchBudget=64;
}
void Ue3DebugMenuSink::action(const ActionEdge& edge) {
    auto& s=*state_;
    if(const auto reason=s.currentReason()) {if(s.preparedValid)s.diagnostic("action",reason);return;}
    if(edge.sourceEpoch!=s.epoch) {s.diagnostic("action","source-epoch");return;}
    if(!(kDebugMenuActions&actionBit(edge.action))) {s.diagnostic("action","unavailable-action");return;}
    const auto bit=actionBit(edge.action);
    if (edge.down) {
        if ((s.held && !(s.held&bit)) || s.gesture.captured()) s.keyCancelled=true;
        s.gesture.cancel();s.hasCapturedHit=false;
        s.held|=bit;
    } else {
        const bool commit=(s.held&bit) && !s.keyCancelled;
        s.held&=~bit;
        if (!s.held) s.keyCancelled=false;
        if (commit) s.navigate(edge.action);
        else s.diagnostic("action","release-cancelled",static_cast<std::uint32_t>(edge.action));
    }
}
void Ue3DebugMenuSink::pointer(const PointerEvent& event) {
    auto& s=*state_;
    const auto reason=s.currentReason();
    if (reason || event.sourceEpoch!=s.epoch || !s.targetContent) {
        if(s.preparedValid)s.diagnostic("pointer",reason?reason:event.sourceEpoch!=s.epoch?"source-epoch":"missing-content");
        s.gesture.clear();s.hasCapturedHit=false;return;
    }
    const auto c=s.content(s.target);
    if(c.key!=s.targetContent || (s.gesture.captured()&&c.key!=s.captureContent)) {
        s.gesture.clear();s.hasCapturedHit=false;return;
    }
    // UP may share a frame with an original in-place menu refresh. Check its
    // captured command before gesture.pointer retires the capture on UP.
    if(!s.capturedRowCurrent(s.target,c)) {
        s.diagnostic("cancel","captured-row-changed");
        s.gesture.clear();s.hasCapturedHit=false;return;
    }
    if(event.phase==PointerPhase::Down && !s.gesture.captured()) {
        s.captureContent=c.key;
        s.hasCapturedHit=s.hitAt(event.surfaceX*s.target.width,event.surfaceY*s.target.height,s.capturedHit);
        s.capturedRowKey=s.rowCaptureKey(s.target,c);
        if(s.diagnosticLogs<48) {
            ++s.diagnosticLogs;
            HORN_LOGI(kTag,"native Debug pointer down=(%.1f,%.1f) hit=%u kind=%u index=%u rows=%zu age=%llu pitch=%.1f",
                event.surfaceX*s.target.width,event.surfaceY*s.target.height,s.hasCapturedHit?1u:0u,
                s.hasCapturedHit?static_cast<unsigned>(s.capturedHit.kind):0u,s.hasCapturedHit?s.capturedHit.index:0u,
                s.geometry.size(),static_cast<unsigned long long>(monotonicMilliseconds()-s.geometryAt),s.rowPitch);
        }
    }
    const auto command=s.gesture.pointer(event,s.target.width,s.target.height,s.rowPitch);
    if(s.held) {s.gesture.cancel();s.keyCancelled=true;s.hasCapturedHit=false;return;}
    if(command.kind==DebugTouchCommand::Select) {
        s.tap(false);
        // Native DOWN selects through the original arrows. Record only that
        // exact requested selection before a subsequent half-second refresh.
        if(s.hasCapturedHit && s.capturedHit.kind==DebugHitKind::Row)
            s.preserveRowCapture(s.target,s.content(s.target),true);
    }
    else if(command.kind==DebugTouchCommand::Tap) s.tap();
    else if(command.kind==DebugTouchCommand::Scroll) {
        s.hasCapturedHit=false;
        for(int i=0;i<std::abs(command.steps);++i) {
            const auto before=s.content(s.target);
            bool paged=false;
            if(before.key!=s.captureContent || !s.navigate(command.steps>0?Action::UiDown:Action::UiUp,&paged))break;
            if(paged)break;
            const auto after=s.content(s.target);if(after.key!=before.key||after.at==before.at)break;
        }
    } else if(command.kind==DebugTouchCommand::PreviousTab || command.kind==DebugTouchCommand::NextTab) {
        s.hasCapturedHit=false;s.key(command.kind==DebugTouchCommand::PreviousTab?Action::UiLeft:Action::UiRight);
    }
    if(!s.gesture.captured())s.hasCapturedHit=false;
}
void Ue3DebugMenuSink::endTouchFrame(const Frame&) {
    auto& s=*state_;
    if (!s.current()) { s.held=0;s.keyCancelled=false;s.frameValid=false;s.gesture.clear();s.hasCapturedHit=false; }
}
} // namespace bl2::touch
