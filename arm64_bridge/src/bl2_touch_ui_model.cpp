#include "bl2_touch_ui_model.h"
#include "bl2_touch_settings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <limits>
#include <utility>

namespace bl2::touch {
namespace {
constexpr std::size_t kReadBudget = 131072;
constexpr std::uint32_t kObjectLimit = 2000000;
struct Array { std::uint32_t data = 0, count = 0; };
struct Property {
    std::uint32_t address = 0, offset = 0, size = 0, extra = 0;
    explicit operator bool() const { return address != 0; }
};
std::uint64_t mix(std::uint64_t seed, std::uint64_t value) {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}

class Reader {
public:
    Reader(const UiModelMemory& memory, std::uint32_t base, UiModelSnapshot& output
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
        ,std::size_t diagnosticReadBudget=kReadBudget
#endif
        ) : memory_(memory), base_(base), output_(output)
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
        ,readBudget_(std::min(diagnosticReadBudget,4*kReadBudget))
#endif
        {}
    bool copy(std::uint32_t p, void* out, std::size_t n) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
        const auto readBudget=readBudget_;
#else
        constexpr auto readBudget=kReadBudget;
#endif
        if (!ok_ || !memory_.copy || p < 0x10000 || n > readBudget - output_.bytesRead ||
            n > std::uint64_t(std::numeric_limits<std::uint32_t>::max()) + 1 - p ||
            !memory_.copy(memory_.context,p,out,n)) {
            if(ok_) readBudgetExceeded_=n > readBudget - output_.bytesRead;
            fail("unreadable-or-budget"); return false;
        }
        output_.bytesRead += n; return true;
    }
    std::uint32_t u32(std::uint32_t p) {
        std::uint32_t value = 0; copy(p,&value,sizeof(value)); return value;
    }
    void fail(const char* reason) {
        if (ok_) output_.reason = reason;
        ok_ = false;
    }
    bool ok() const { return ok_; }
    bool readBudgetExceeded() const { return readBudgetExceeded_; }
    bool init() {
        // Native metadata ABI evidence is in analysis/reflection-abi.txt.
        // Runtime installation still requires the complete canonical guest SHA.
        constexpr std::array<std::pair<std::uint32_t,std::uint32_t>,16> signatures{{
            {0x00FC57D0,0xE5954044}, {0x00FC57E8,0xE594403C},
            {0x00FC57DC,0xE59330C4}, {0x00FC57E0,0xE3130902},
            {0x00F7AE14,0xE5900040}, {0x00F9738C,0xE5902054},
            {0x0101DE20,0xE2841040}, {0x0101CDB0,0xE5803044},
            {0x0101D0C0,0xE3A03008}, {0x0101D17C,0xE8920003},
            {0x0101D4D4,0xE5940074}, {0x0101D4F0,0xE3A0300C},
            {0x0102A1CC,0xE5952074}, {0x0102A1D8,0xE5924048},
            {0x0102A1E8,0xE7F74054}, {0x0101CB30,0xE5903074}
        }};
        if (std::uint64_t(base_) + 0x03089000 > std::numeric_limits<std::uint32_t>::max()) {
            fail("image-overflow"); return false;
        }
        for (const auto& [rva,word] : signatures)
            if (u32(base_ + rva) != word) { fail("reflection-abi"); return false; }
        objects_ = array(base_ + 0x02FBA268,4,kObjectLimit,kObjectLimit);
        names_ = array(base_ + 0x02FB00C8,4,kObjectLimit,kObjectLimit);
        if (!objects_.count || !names_.count) fail("empty-object-or-name-table");
        return ok_;
    }
    Array array(std::uint32_t p, std::uint32_t stride,
                std::uint32_t maxCount, std::uint32_t maxCapacity) {
        std::uint32_t a[3]{};
        if (!stride || !copy(p,a,sizeof(a))) return {};
        if (a[1] > a[2] || a[1] > maxCount || a[2] > maxCapacity ||
            (a[1] && (a[0] < 0x10000 || std::uint64_t(a[0]) + std::uint64_t(a[1]) * stride >
                std::uint64_t(std::numeric_limits<std::uint32_t>::max()) + 1))) {
            fail("array-shape"); return {};
        }
        return {a[0],a[1]};
    }
    std::uint64_t token(std::uint32_t p) {
        if (!p) { fail("null-object"); return 0; }
        // Refuse the last 1 MiB; all accepted UStruct sizes are bounded to that
        // amount, so subsequent object+metadata-offset arithmetic cannot wrap.
        if (p > std::numeric_limits<std::uint32_t>::max() - 1048576) {
            fail("object-overflow"); return 0;
        }
        const auto index = u32(p + 0x20);
        if (!ok_ || index >= objects_.count || u32(objects_.data + index * 4) != p ||
            (u32(p + 0x0C) & 0x20000002u)) {
            fail("dead-or-unregistered-object"); return 0;
        }
        return (std::uint64_t(p) << 32) | index;
    }
    std::string name(std::uint32_t p) {
        const auto index = u32(p), number = u32(p + 4);
        if (index >= names_.count) { fail("fname-index"); return {}; }
        const auto entry = u32(names_.data + index * 4);
        if (entry < 0x10000 || entry > std::numeric_limits<std::uint32_t>::max() - 136) {
            fail("fname-overflow"); return {};
        }
        std::string text;
        for (std::uint32_t i = 0; i < 128 && ok_; ++i) {
            unsigned char ch = 0;
            if (!copy(entry + 8 + i,&ch,1)) break;
            if (!ch) return number ? text + "_" + std::to_string(number - 1) : text;
            if (ch < 32 || ch > 126) { fail("non-identifier-fname"); break; }
            text += static_cast<char>(ch);
        }
        fail("unterminated-fname"); return {};
    }
    std::string objectName(std::uint32_t p) { return token(p) ? name(p + 0x2C) : std::string{}; }
    std::string className(std::uint32_t p) {
        return token(p) ? objectName(u32(p + 0x34)) : std::string{};
    }
    bool isA(std::uint32_t object, const char* wanted) {
        if (!token(object)) return false;
        auto cls = u32(object + 0x34);
        std::array<std::uint32_t,32> visited{};
        for (std::size_t i = 0; cls && i < visited.size(); ++i) {
            if (std::find(visited.begin(),visited.begin() + i,cls) != visited.begin() + i) break;
            visited[i] = cls;
            if (className(cls) != "Class") break;
            if (objectName(cls) == wanted) return true;
            cls = u32(cls + 0x40);
        }
        fail("unexpected-object-family"); return false;
    }
    Property property(std::uint32_t type, const char* wanted, const char* wantedKind, std::uint32_t dimension = 1) {
        std::array<std::uint32_t,32> parents{};
        for (std::size_t depth = 0; type && depth < parents.size() && ok_; ++depth) {
            if (std::find(parents.begin(),parents.begin() + depth,type) != parents.begin() + depth) {
                fail("super-cycle"); return {};
            }
            parents[depth] = type;
            const auto typeKind = className(type);
            if (typeKind != "Class" && typeKind != "ScriptStruct" && typeKind != "Function") { fail("not-a-struct"); return {}; }
            const auto size = u32(type + 0x48) & 0x00FFFFFFu;
            if (!size || size > 1048576) { fail("struct-size"); return {}; }
            auto field = u32(type + 0x44);
            std::array<std::uint32_t,512> fields{};
            std::size_t count = 0;
            while (field && ok_) {
                if (count == fields.size() || std::find(fields.begin(),fields.begin() + count,field) != fields.begin() + count) {
                    fail("field-cycle-or-limit"); return {};
                }
                fields[count++] = field;
                if (!token(field) || u32(field + 0x28) != type) { fail("field-owner"); return {}; }
                if (objectName(field) == wanted) {
                    if (className(field) != wantedKind || !(u32(u32(field + 0x34) + 0xC4) & 0x8000u)) {
                        fail("property-type"); return {};
                    }
                    Property p{field,u32(field + 0x54),u32(field + 0x44),0};
                    if (!dimension || dimension > 2 || u32(field + 0x40) != dimension || !p.size || p.size > 4096 || p.offset > size ||
                        std::uint64_t(p.size) * dimension > size - p.offset || (typeKind == "Class" && p.offset < 0x38)) {
                        fail("property-shape"); return {};
                    }
                    if (std::strcmp(wantedKind,"ArrayProperty") == 0 || std::strcmp(wantedKind,"BoolProperty") == 0 ||
                        std::strcmp(wantedKind,"StructProperty") == 0)
                        p.extra = u32(field + 0x74);
                    output_.properties.push_back({std::string(wanted),field,p.offset,p.size,p.extra});
                    return p;
                }
                field = u32(field + 0x3C);
            }
            type = u32(type + 0x40);
        }
        fail("missing-property"); return {};
    }
    Property objectProperty(std::uint32_t object, const char* wanted, const char* kind) {
        return token(object) ? property(u32(object + 0x34),wanted,kind) : Property{};
    }
    UiNativeMethod method(std::uint32_t object, const char* wanted, const char* declaring,
                          std::uint32_t flags, std::initializer_list<std::pair<const char*,const char*>> parameters,
                          std::uint16_t bytes, std::uint16_t returnOffset = 0xFFFF) {
        if (!token(object)) return {};
        auto type = u32(object + 0x34);
        for (unsigned depth = 0; type && depth < 32 && ok_; ++depth) {
            if (className(type) != "Class") break;
            auto item = u32(type + 0x44);
            for (unsigned i = 0; item && i < 512 && ok_; ++i) {
                if (!token(item) || u32(item + 0x28) != type) { fail("method-owner"); return {}; }
                if (objectName(item) == wanted) {
                    const auto layout = u32(item + 0x84);
                    if (objectName(type) != declaring || className(item) != "Function" ||
                        u32(item + 0x7C) != flags || ((layout >> 8) & 255) != parameters.size() ||
                        (layout >> 16) != bytes || (u32(item + 0x88) & 65535) != returnOffset) {
                        fail("method-schema"); return {};
                    }
                    unsigned offset = 0;
                    for (const auto& [name,kind] : parameters) {
                        auto p = property(item,name,kind);
                        const unsigned size = std::strcmp(kind,"ByteProperty") == 0 ? 1u :
                            (std::strcmp(kind,"NameProperty") == 0 ? 8u : 4u);
                        // Original UByteProperty::Link aligns at one byte;
                        // UNameProperty is two words and the following Bool
                        // still uses the original four-byte alignment.
                        const unsigned alignment=size==1?1u:4u;
                        offset=(offset+alignment-1)&~(alignment-1);
                        if (!p || p.offset != offset || p.size != size ||
                            (u32(p.address + 0x48) & 0x80u) == 0 ||
                            (std::strcmp(kind,"BoolProperty") == 0 && p.extra != 1)) {
                            fail("method-parameter"); return {};
                        }
                        offset += size;
                    }
                    return {item,flags,bytes,token(item)};
                }
                item = u32(item + 0x3C);
            }
            type = u32(type + 0x40);
        }
        fail("missing-native-method"); return {};
    }
    Property inner(const Property& p, const char* kind) {
        if (!p || p.size != 12 || !token(p.extra) || u32(p.extra + 0x28) != p.address ||
            className(p.extra) != kind || u32(p.extra + 0x40) != 1) {
            fail("array-inner"); return {};
        }
        Property result{p.extra,0,u32(p.extra + 0x44),0};
        if (!result.size || result.size > 4096) { fail("inner-size"); return {}; }
        if (std::strcmp(kind,"StructProperty") == 0) result.extra = u32(p.extra + 0x74);
        return result;
    }
    std::uint32_t valueObject(std::uint32_t object, const char* field) {
        const auto p = objectProperty(object,field,"ObjectProperty");
        if (!p || p.size != 4) { fail("object-property-size"); return 0; }
        const auto value = u32(object + p.offset);
        return token(value) ? value : 0;
    }
    std::uint32_t inheritedObject(std::uint32_t object, const char* family, const char* field) {
        if (!token(object)) return 0;
        auto cls = u32(object + 0x34);
        for (unsigned i = 0; cls && i < 32 && ok_; ++i) {
            if (className(cls) != "Class") break;
            if (objectName(cls) == family) {
                const auto p = property(cls,field,"ObjectProperty");
                if (!p || p.size != 4) { fail("inherited-object-property-size"); return 0; }
                const auto value = u32(object + p.offset); return token(value) ? value : 0;
            }
            cls = u32(cls + 0x40);
        }
        fail("missing-declaring-class"); return 0;
    }

private:
    UiModelMemory memory_;
    std::uint32_t base_;
    UiModelSnapshot& output_;
    bool ok_ = true;
    bool readBudgetExceeded_ = false;
    Array objects_{}, names_{};
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
    const std::size_t readBudget_;
#endif
};

UiNativeMethod keyHandler(Reader& r,std::uint32_t owner,const char* name,const char* declaring) {
    constexpr std::array<std::pair<const char*,const char*>,4> params{{
        {"ControllerId","IntProperty"},{"ukey","NameProperty"},{"uevent","ByteProperty"},{"ReturnValue","BoolProperty"}}};
    const auto fn=r.method(owner,name,declaring,0x00020002,
        {{"ControllerId","IntProperty"},{"ukey","NameProperty"},{"uevent","ByteProperty"},{"ReturnValue","BoolProperty"}},20,16);
    if(!fn.function || !r.ok()) return {};
    auto next=r.u32(fn.function+0x44);
    for(unsigned i=0;i<params.size();++i) {
        const auto p=r.property(fn.function,params[i].first,params[i].second);
        if(!p || p.address!=next || r.u32(p.address+0x48)!=(i==3?0x580u:0x80u) || r.u32(p.address+0x4C)) {
            r.fail("input-parameter-flags");return {};
        }
        next=r.u32(p.address+0x3C);
    }
    const bool chat=std::strcmp(name,"HandleTextChatInput")==0 && std::strcmp(declaring,"TextChatGFxMovie")==0;
    const bool account=std::strcmp(name,"HandleInputKey")==0 && std::strcmp(declaring,"GearboxAccountGFxMovie")==0;
    if(chat || account) {
        // Original ReturnValue.Next includes non-parameter string locals:
        // Chat33063 or Account23285->23284. ParmsSize remains exactly20.
        const std::array<const char*,2> locals=account?
            std::array<const char*,2>{"EmailInputDesc","PasswordInputDesc"}:
            std::array<const char*,2>{"ChatText",nullptr};
        unsigned offset=20;
        for(unsigned i=0;i<(account?2u:1u);++i) {
            const auto local=r.property(fn.function,locals[i],"StrProperty");
            if(!local || local.address!=next || local.offset!=offset || local.size!=12 ||
                r.u32(local.address+0x40)!=1 || r.u32(local.address+0x48)!=0x00400000u || r.u32(local.address+0x4C)) {
                r.fail("input-local-schema");return {};
            }
            next=r.u32(local.address+0x3C);offset+=12;
        }
        // UStruct packs MinAlignment in the high byte of this size word.
        if((r.u32(fn.function+0x48)&0x00FFFFFFu)!=offset) {r.fail("input-local-frame-size");return {};}
    }
    if(next || !r.ok()) {r.fail("input-parameter-order");return {};}
    return fn;
}
void captureList(Reader& r, std::uint32_t owner, UiModelSnapshot& out) {
    const auto list = r.valueObject(owner,"TheList");
    if (!r.ok() || !r.isA(list,"WillowScrollingList")) return;
    out.list = r.token(list);
    if (r.valueObject(list,"MyOwnerMovie") != owner) { r.fail("list-owner-mismatch"); return; }
    const auto idsProperty = r.objectProperty(list,"IndexToEventId","ArrayProperty");
    const auto idsInner = r.inner(idsProperty,"IntProperty");
    if (!idsInner || idsInner.size != 4) { r.fail("event-id-size"); return; }
    const auto ids = r.array(list + idsProperty.offset,4,256,4096);
    for (std::uint32_t i = 0; i < ids.count && r.ok(); ++i)
        out.eventIds.push_back(static_cast<std::int32_t>(r.u32(ids.data + i * 4)));
    const auto stackProperty = r.objectProperty(list,"DataProviderStack","ArrayProperty");
    const auto stackInner = r.inner(stackProperty,"StructProperty");
    if (!stackInner || r.className(stackInner.extra) != "ScriptStruct" ||
        r.objectName(stackInner.extra) != "DataProviderInfo") { r.fail("provider-struct"); return; }
    const auto providerField = r.property(stackInner.extra,"DataProvider","InterfaceProperty");
    const auto selectionField = r.property(stackInner.extra,"SelectedIndex","IntProperty");
    if (!providerField || !selectionField || providerField.size != 8 || selectionField.size != 4 ||
        providerField.offset + 8 > stackInner.size || selectionField.offset + 4 > stackInner.size) {
        r.fail("provider-field-shape"); return;
    }
    const auto stack = r.array(list + stackProperty.offset,stackInner.size,32,256);
    if (!stack.count) { r.fail("empty-provider-stack"); return; }
    out.providerDepth = stack.count;
    out.structure = mix(mix(out.owner,out.list),stack.count);
    for (std::uint32_t i = 0; i < stack.count && r.ok(); ++i) {
        const auto element = stack.data + i * stackInner.size;
        const auto provider = r.u32(element + providerField.offset);
        const auto token = r.token(provider);
        out.structure = mix(out.structure,token);
        // InterfacePointer may be zero for a script interface. Never dereference it.
        out.structure = mix(out.structure,r.u32(element + providerField.offset + 4));
        if (i + 1 == stack.count) {
            out.provider = token;
            out.providerClass = r.className(provider);
            out.selectedIndex = static_cast<std::int32_t>(r.u32(element + selectionField.offset));
        }
    }
    out.structure = mix(out.structure,out.eventIds.size());
    for (auto id : out.eventIds) out.structure = mix(out.structure,static_cast<std::uint32_t>(id));
}

void captureDialog(Reader& r, std::uint32_t owner, UiModelSnapshot& out) {
    const auto noCancel = r.objectProperty(owner,"bNoCancel","BoolProperty");
    const auto selected = r.objectProperty(owner,"CurrentSelection","IntProperty");
    if (!noCancel || noCancel.size != 4 || !noCancel.extra || (noCancel.extra & (noCancel.extra - 1)) ||
        !selected || selected.size != 4) { r.fail("dialog-scalar-shape"); return; }
    out.noCancel = (r.u32(owner + noCancel.offset) & noCancel.extra) != 0;
    out.selectedIndex = static_cast<std::int32_t>(r.u32(owner + selected.offset));
    const auto buttonsProperty = r.objectProperty(owner,"Buttons","ArrayProperty");
    const auto buttonsInner = r.inner(buttonsProperty,"StructProperty");
    if (!buttonsInner || r.className(buttonsInner.extra) != "ScriptStruct" ||
        r.objectName(buttonsInner.extra) != "DialogBoxButton") { r.fail("dialog-button-struct"); return; }
    const auto tag = r.property(buttonsInner.extra,"Tag","NameProperty");
    const auto keys = r.property(buttonsInner.extra,"Keys","ArrayProperty");
    const auto keyInner = r.inner(keys,"NameProperty");
    if (!tag || tag.size != 8 || tag.offset + 8 > buttonsInner.size ||
        !keys || keys.offset + 12 > buttonsInner.size || !keyInner || keyInner.size != 8) {
        r.fail("dialog-button-fields"); return;
    }
    const auto buttons = r.array(owner + buttonsProperty.offset,buttonsInner.size,16,64);
    out.structure = mix(mix(out.owner,out.noCancel),buttons.count);
    for (std::uint32_t i = 0; i < buttons.count && r.ok(); ++i) {
        const auto element = buttons.data + i * buttonsInner.size;
        UiDialogButtonModel button;
        button.tag = r.name(element + tag.offset);
        const auto keyValues = r.array(element + keys.offset,8,32,64);
        for (std::uint32_t j = 0; j < keyValues.count && r.ok(); ++j)
            button.keys.push_back(r.name(keyValues.data + j * 8));
        out.structure = mix(out.structure,button.tag.size());
        for (auto ch : button.tag) out.structure = mix(out.structure,static_cast<unsigned char>(ch));
        out.structure = mix(out.structure,button.keys.size());
        for (const auto& key : button.keys) {
            out.structure = mix(out.structure,key.size());
            for (auto ch : key) out.structure = mix(out.structure,static_cast<unsigned char>(ch));
        }
        out.buttons.push_back(std::move(button));
    }
}
} // namespace

bool Bl2TouchReviewedUiMovie(std::string_view name) {
    // Exact original interactive Movie classes. Panel GFxObjects, sequence
    // helpers and passive HUD/card/render-target Movies are not screen owners.
    constexpr std::string_view screens[] = {
        "FrontendGFxMovie","PauseGFxMovie","OptionsGFxMovie","WillowGFxDialogBox",
        "NetworkOptionsGFxMovie","WillowGFxMoviePressStart","CharacterSelectionReduxGFxMovie",
        "StatusMenuExGFxMovie","WillowInventoryGFxMovie","TwoPanelInterfaceGFxMovie",
        "BankGFxMovie","StashGFxMovie","VendingMachineExGFxMovie","FastTravelStationGFxMovie",
        "ItemInspectionGFxMovie","LANServerBrowserGFxMovie","QuestAcceptGFxMovie",
        "LatentRewardGFxMovie","VehicleSpawnStationGFxMovie","TradingGFxMovie",
        "EndOfGameGFxMovie","UBMGameOverGFxMovie","WillowGFxMovieCredits","MarketplaceGFxMovie",
        "ConfirmCharacterLobbyGFxMovie","CustomizationGFxMovie","GearboxAccountGFxMovie",
        "LobbyGFxMovie","MatchmakingLobbyGFxMovie","PlayerDetailsGFxMovie","TextChatGFxMovie",
        "WillowGFxColiseumOverlayMovie","HashDisplayGFxMovie","WillowGFxEditDialog",
        "WillowGFxLobbyLoadCharacter","WillowGFxTrainingDialogBox","WillowGFxTrainingDialogBox_NoPause"
    };
    return std::find(std::begin(screens),std::end(screens),name) != std::end(screens);
}
bool Bl2TouchNativeDialogMovie(std::string_view name) {
    return name == "WillowGFxDialogBox"
        || name == "WillowGFxTrainingDialogBox" || name == "WillowGFxTrainingDialogBox_NoPause"
        ;
}

UiDirectModel CaptureBl2DirectUi(const UiModelMemory& memory,std::uint32_t base,
                                std::uint32_t owner,std::uint32_t player) {
    UiDirectModel out; UiModelSnapshot evidence; Reader r(memory,base,evidence);
    if (player > 1 || !r.init() || !(out.owner = r.token(owner))) return out;
    out.movieClass = r.className(owner); out.player = player;
    const auto cls = r.u32(owner + 0x34);
    if (out.movieClass == "CharacterSelectionReduxGFxMovie") {
        const auto state = r.property(cls,"CurrentUIState","IntProperty",2);
        const auto selected = r.property(cls,"SelectedCharacterIndex","IntProperty",2);
        if (!state || state.size != 4 || !selected || selected.size != 4) r.fail("character-player-slots");
        else {
            out.screenField = state.address; out.screenOffset = state.offset; out.screenSize = 4; out.screenDimension = 2;
            out.selectionField = selected.address; out.selectionOffset = selected.offset;
            out.screen = static_cast<std::int32_t>(r.u32(owner + state.offset + player * 4));
            out.selected = static_cast<std::int32_t>(r.u32(owner + selected.offset + player * 4));
            if (out.screen < 0 || out.screen > 5 || out.selected < -1 || out.selected > 31) r.fail("character-state");
        }
        const auto cp = r.objectProperty(owner,"CharacterSelectClip","ObjectProperty");
        out.clipField = cp.address; out.clipOffset = cp.offset;
        const auto clip = r.valueObject(owner,"CharacterSelectClip");
        if (r.ok() && r.isA(clip,"CharacterSelectionGFxObject")) out.clip = r.token(clip);
        out.hover = r.method(owner,"HandleCharacterMouseOver","CharacterSelectionReduxGFxMovie",0x00040003,
            {{"PlayerIndex","IntProperty"},{"CharacterIndex","IntProperty"}},8);
        out.click = r.method(owner,"HandleCharacterClicked","CharacterSelectionReduxGFxMovie",0x00040003,
            {{"ControllerId","IntProperty"},{"CharacterIndex","IntProperty"}},8);
        out.select = r.method(owner,"SetSelectedCharacterIndex","CharacterSelectionReduxGFxMovie",0x00040003,
            {{"PlayerIndex","IntProperty"},{"NewSelectedIndex","IntProperty"}},8);
        if (out.screen == 2)
            out.menuClick = r.method(owner,"HandleCustomizationMenuEntryClick","CharacterSelectionReduxGFxMovie",0x00020002,
                {{"PlayerIndex","IntProperty"},{"EntryIndex","IntProperty"}},8);
    } else if (out.movieClass == "StatusMenuExGFxMovie") {
        const auto screen = r.objectProperty(owner,"CurrentScreen","ByteProperty");
        unsigned char value = 255;
        if (!screen || screen.size != 1 || !r.copy(owner + screen.offset,&value,1) || value > 5) r.fail("status-screen");
        else { out.screen = value; out.screenField = screen.address; out.screenOffset = screen.offset; out.screenSize = 1; }
        if (out.screen == 2) {
            const auto cp = r.objectProperty(owner,"StatusMenuMap","ObjectProperty");
            out.clipField = cp.address; out.clipOffset = cp.offset;
            const auto map = r.valueObject(owner,"StatusMenuMap");
            if (r.ok() && r.isA(map,"StatusMenuMapGFxObject")) out.clip = r.token(map);
            const auto mask = r.valueObject(map,"MaskClip");
            if (r.ok() && r.isA(mask,"GFxObject")) out.mask = mask;
            out.drag = r.method(owner,"HandleMouseDrag","StatusMenuExGFxMovie",0x00020802,
                {{"dx","IntProperty"},{"DY","IntProperty"}},8);
            out.stopDrag = r.method(owner,"StopMouseDrag","StatusMenuExGFxMovie",0x00020002,{},0);
            out.zoom = r.method(owner,"HandleMapZoom","StatusMenuExGFxMovie",0x00024002,
                {{"Delta","FloatProperty"},{"bLoopingAudio","BoolProperty"}},8);
            out.waypoint = r.method(map,"PlaceCustomObjective","StatusMenuMapGFxObject",0x00020401,{},0);
            out.legend = r.method(map,"ToggleLegend","StatusMenuMapGFxObject",0x00020002,{},0);
            out.info = r.method(map,"UpdateInfoBox","StatusMenuMapGFxObject",0x00020400,{},0);
        }
    } else if (out.movieClass == "ItemInspectionGFxMovie") {
        out.startDrag = r.method(owner,"StartMouseDrag","ItemInspectionGFxMovie",0x00020400,
            {{"bRMB","BoolProperty"}},4);
        out.drag = r.method(owner,"HandleMouseDrag","ItemInspectionGFxMovie",0x00020400,
            {{"dx","IntProperty"},{"DY","IntProperty"}},8);
        out.stopDrag = r.method(owner,"StopMouseDrag","ItemInspectionGFxMovie",0x00020400,
            {{"bRMB","BoolProperty"}},4);
    } else if (Bl2TouchNativeDialogMovie(out.movieClass)) {
        const auto dialog = CaptureBl2UiModel(memory,base,owner);
        if(!dialog.valid || dialog.buttons.size()>5) r.fail("dialog-buttons");
        else if(dialog.buttons.empty() && (out.movieClass=="WillowGFxTrainingDialogBox" ||
                                          out.movieClass=="WillowGFxTrainingDialogBox_NoPause")) {
            const auto delay=r.objectProperty(owner,"DelayUntilShowOk","FloatProperty"),
                tab=r.objectProperty(owner,"StatusMenuTab","ByteProperty");
            float seconds=0;unsigned char status=255;
            if(!delay || delay.size!=4 || !tab || tab.size!=1 ||
                r.u32(delay.address+0x48)!=0x2000 || r.u32(tab.address+0x48)!=0x2000 ||
                r.u32(delay.address+0x4C) || r.u32(tab.address+0x4C) ||
                !r.copy(owner+delay.offset,&seconds,4) || !std::isfinite(seconds) ||
                !r.copy(owner+tab.offset,&status,1) || status>5) r.fail("training-prompt-fields");
            else {
                out.trainingPrompt=true;out.screen=status;
                out.screenField=tab.address;out.screenOffset=tab.offset;out.screenSize=1;
                out.click=keyHandler(r,owner,"HandleInputKey","WillowGFxTrainingDialogBox");
            }
        }
        else if(dialog.buttons.empty()) r.fail("dialog-buttons");
        else {
            out.selected=dialog.selectedIndex;out.screen=static_cast<std::int32_t>(dialog.buttons.size());
            out.hover=r.method(owner,"OnWidgetFocused","WillowGFxDialogBox",0x00020002,{{"ButtonIndex","IntProperty"}},4);
            out.click=r.method(owner,"OnWidgetClicked","WillowGFxDialogBox",0x00020002,
                {{"ButtonIndex","IntProperty"},{"ControllerId","IntProperty"}},8);
        }
    } else r.fail("unreviewed-direct-ui");
    out.valid = r.ok(); out.reason = evidence.reason;
    out.structure = mix(mix(out.owner,out.clip),static_cast<std::uint32_t>(out.screen));
    return out;
}

UiTradingOfferModel CaptureBl2TradingOffer(const UiModelMemory& memory,std::uint32_t base,
    std::uint32_t owner,std::int32_t index) {
    UiTradingOfferModel out;UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto reject=[&](const char* reason) {out.reason=std::string(reason)+"/"+witness.reason;return out;};
    if(index<0 || index>3 || !r.init() || !r.isA(owner,"TradingGFxMovie")) return reject("offer-owner");
    out.owner=r.token(owner);
    const auto typed=[&](std::uint32_t object,const char* field,const char* kind) {
        const auto p=r.objectProperty(object,field,"ObjectProperty");
        if(!p || p.size!=4) return std::uint32_t{};
        const auto cls=r.u32(p.address+0x74),value=r.u32(object+p.offset);
        return r.className(cls)=="Class" && r.objectName(cls)==kind && r.isA(value,kind)?value:0;
    };
    const auto trading=typed(owner,"TradingPanel","TradingPanelGFxObject");
    const auto offers=trading?typed(trading,"OffersPanel","TradingOffersPanelGFxObject"):0;
    if(!offers || !r.ok()) return reject("offer-panel");
    out.panel=r.token(offers);
    const auto selected=r.objectProperty(offers,"SelectedCellSuffix","IntProperty"),
        browsing=r.objectProperty(offers,"bBrowsingCells","BoolProperty");
    if(!selected || selected.size!=4 || !browsing || browsing.size!=4 || !browsing.extra ||
        (browsing.extra&(browsing.extra-1)) || r.u32(offers+selected.offset)!=static_cast<std::uint32_t>(index+1) ||
        !(r.u32(offers+browsing.offset)&browsing.extra)) return reject("offer-selected-source");
    out.inventory=r.method(owner,"GetOfferedCellInventory","TradingGFxMovie",0x00020002,
        {{"CellIndex","IntProperty"},{"ReturnValue","ObjectProperty"}},8,4);
    out.canStart=r.method(offers,"CanStartOffer","TradingOffersPanelGFxObject",0x00020002,
        {{"ReturnValue","BoolProperty"}},4,0);
    if(!out.inventory.function || !out.canStart.function || !r.ok()) return reject("offer-methods");
    const auto input=r.property(out.inventory.function,"CellIndex","IntProperty"),
        result=r.property(out.inventory.function,"ReturnValue","ObjectProperty"),
        allowed=r.property(out.canStart.function,"ReturnValue","BoolProperty");
    const auto cls=result?r.u32(result.address+0x74):0;
    if(!input || !result || !allowed || r.u32(input.address+0x48)!=0x80 || r.u32(input.address+0x4C) ||
        r.u32(result.address+0x48)!=0x580 || r.u32(result.address+0x4C) ||
        r.u32(allowed.address+0x48)!=0x580 || r.u32(allowed.address+0x4C) ||
        r.u32(out.inventory.function+0x44)!=input.address || r.u32(input.address+0x3C)!=result.address ||
        r.u32(result.address+0x3C) || r.u32(out.canStart.function+0x44)!=allowed.address || r.u32(allowed.address+0x3C) ||
        r.className(cls)!="Class" || r.objectName(cls)!="WillowInventory" || !r.ok()) return reject("offer-return-type");
    out.valid=true;out.reason="offer-observed";return out;
}
namespace {
std::uint32_t tradingObject(Reader& r,std::uint32_t object,const char* name,const char* kind,const char* declaring) {
    const auto p=r.objectProperty(object,name,"ObjectProperty");
    const auto cls=p?r.u32(p.address+0x74):0;
    if(!p || p.size!=4 || r.objectName(r.u32(p.address+0x28))!=declaring ||
        r.u32(p.address+0x48) || r.u32(p.address+0x4C) ||
        r.className(cls)!="Class" || r.objectName(cls)!=kind) return 0;
    const auto value=r.u32(object+p.offset);return r.isA(value,kind)?value:0;
}
}
UiTradingOfferModel CaptureBl2TradingBackpack(const UiModelMemory& memory,std::uint32_t base,
    std::uint32_t owner,std::int32_t index) {
    UiTradingOfferModel out;UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto reject=[&](const char* reason) {out.reason=std::string(reason)+"/"+witness.reason;return out;};
    if(index<0 || index>4095 || !r.init() || !r.isA(owner,"TradingGFxMovie")) return reject("backpack-owner");
    out.owner=r.token(owner);
    const auto panel=tradingObject(r,owner,"TradingPanel","TradingPanelGFxObject","TradingGFxMovie");
    const auto backpack=panel?tradingObject(r,panel,"BackpackPanel","InventoryListPanelGFxObject","TradingPanelGFxObject"):0;
    if(!backpack || !r.ok()) return reject("backpack-panel");
    out.panel=r.token(backpack);
    out.inventory=r.method(backpack,"GetThingByIndex","InventoryListPanelGFxObject",0x00020002,
        {{"Index","IntProperty"},{"ReturnValue","ObjectProperty"}},8,4);
    if(!out.inventory.function || !r.ok()) return reject("backpack-method");
    const auto input=r.property(out.inventory.function,"Index","IntProperty"),
        result=r.property(out.inventory.function,"ReturnValue","ObjectProperty");
    const auto cls=result?r.u32(result.address+0x74):0;
    if(!input || !result || r.u32(input.address+0x48)!=0x80 || r.u32(input.address+0x4C) ||
        r.u32(result.address+0x48)!=0x580 || r.u32(result.address+0x4C) ||
        r.u32(out.inventory.function+0x44)!=input.address || r.u32(input.address+0x3C)!=result.address ||
        r.u32(result.address+0x3C) || r.className(cls)!="Class" || r.objectName(cls)!="WillowInventory" || !r.ok())
        return reject("backpack-return-type");
    out.valid=true;out.reason="backpack-item-getter";return out;
}
std::uint64_t Bl2TouchTradingHeldItem(const UiModelMemory& memory,std::uint32_t base,std::uint32_t owner) {
    UiModelSnapshot witness;Reader r(memory,base,witness);
    if(!r.init() || r.className(owner)!="TradingGFxMovie") return 0;
    const auto panel=tradingObject(r,owner,"TradingPanel","TradingPanelGFxObject","TradingGFxMovie");
    const auto parent=panel?tradingObject(r,panel,"ParentMovie","WillowInventoryGFxMovie","BaseTopLevelPanelGFxObject"):0;
    const auto offering=panel?tradingObject(r,panel,"OfferingThing","WillowInventory","TradingPanelGFxObject"):0;
    const auto frozen=parent?tradingObject(r,parent,"FrozenThing","WillowInventory","WillowInventoryGFxMovie"):0;
    return parent==owner && offering && offering==frozen && r.ok()?r.token(offering):0;
}
UiStatusInventoryModel CaptureBl2StatusInventory(const UiModelMemory& memory,std::uint32_t base,std::uint32_t owner) {
    UiStatusInventoryModel out;UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto reject=[&](const char* reason) {out.reason=std::string(reason)+"/"+witness.reason;return out;};
    if(!r.init() || r.className(owner)!="StatusMenuExGFxMovie") return reject("inventory-owner");
    out.owner=r.token(owner);
    const auto screen=r.objectProperty(owner,"CurrentScreen","ByteProperty");
    const auto screenEnum=screen?r.u32(screen.address+0x74):0;
    unsigned char value=255;
    if(!screen || screen.size!=1 || r.u32(screen.address+0x28)!=r.u32(owner+0x34) ||
        r.u32(screen.address+0x48) || r.u32(screen.address+0x4C) ||
        r.className(screenEnum)!="Enum" || r.objectName(screenEnum)!="EBackButtonScreen" ||
        !r.copy(owner+screen.offset,&value,1) || value!=3) return reject("inventory-screen");
    const auto top=tradingObject(r,owner,"InventoryPanel","StatusMenuInventoryPanelGFxObject","StatusMenuExGFxMovie");
    const auto backpack=top?tradingObject(r,top,"BackpackPanel","InventoryListPanelGFxObject","StatusMenuInventoryPanelGFxObject"):0;
    const auto equipped=top?tradingObject(r,top,"EquippedPanel","StatusMenuEquippedPanelGFxObject","StatusMenuInventoryPanelGFxObject"):0;
    if(!top || !backpack || !equipped || !r.ok()) return reject("inventory-panels");
    out.top=r.token(top);out.backpack=r.token(backpack);out.equipped=r.token(equipped);
    const auto getter=[&](std::uint32_t panel,const char* declaring) {
        const auto fn=r.method(panel,"GetThingByIndex",declaring,0x00020002,
            {{"Index","IntProperty"},{"ReturnValue","ObjectProperty"}},8,4);
        const auto input=r.property(fn.function,"Index","IntProperty"),
            result=r.property(fn.function,"ReturnValue","ObjectProperty");
        const auto cls=result?r.u32(result.address+0x74):0;
        if(!fn.function || !input || !result || r.u32(input.address+0x48)!=0x80 || r.u32(input.address+0x4C) ||
            r.u32(result.address+0x48)!=0x580 || r.u32(result.address+0x4C) ||
            r.u32(fn.function+0x44)!=input.address || r.u32(input.address+0x3C)!=result.address || r.u32(result.address+0x3C) ||
            r.className(cls)!="Class" || r.objectName(cls)!="WillowInventory" || !r.ok()) return UiNativeMethod{};
        return fn;
    };
    out.backpackInventory=getter(backpack,"InventoryListPanelGFxObject");
    out.equippedInventory=getter(equipped,"StatusMenuEquippedPanelGFxObject");
    if(!out.backpackInventory.function || !out.equippedInventory.function || !r.ok()) return reject("inventory-getters");
    out.valid=true;out.reason="inventory-panels-owned";return out;
}
UiStatusInventoryInputState CaptureBl2StatusInventoryInputState(const UiModelMemory& memory,std::uint32_t base,std::uint32_t owner) {
    UiStatusInventoryInputState out;UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto reject=[&](const char* reason) {out.reason=std::string(reason)+"/"+witness.reason;return out;};
    if(!r.init() || r.className(owner)!="StatusMenuExGFxMovie") return reject("input-owner");
    out.owner=r.token(owner);
    const auto field=[](Reader& reader,std::uint32_t object,const char* name,const char* kind,const char* declaring,std::uint32_t size,std::uint32_t flags,std::uint32_t type=0) {
        const auto p=type?reader.property(type,name,kind):reader.objectProperty(object,name,kind);
        if(!p || p.size!=size || reader.objectName(reader.u32(p.address+0x28))!=declaring ||
            reader.u32(p.address+0x48)!=flags || reader.u32(p.address+0x4C) || (p.offset&3)) return Property{};
        return p;
    };
    std::uint32_t top=0;
    const auto captureTop=[&] {
        const auto fail=[&](const char* reason) {
            out.reason=std::string(reason)+"/"+(r.readBudgetExceeded()?"read-budget":witness.reason)+
                "/bytes="+std::to_string(witness.bytesRead);return;
        };
        top=tradingObject(r,owner,"InventoryPanel","StatusMenuInventoryPanelGFxObject","StatusMenuExGFxMovie");
        if(!top || !r.ok()) return fail("input-panel");
        out.top=r.token(top);
        const auto delegate=field(r,top,"__OnInputKey__Delegate","DelegateProperty","BaseTopLevelPanelGFxObject",12,0x00400000),
            queued=field(r,top,"QueuedInputKey","DelegateProperty","BaseTopLevelPanelGFxObject",12,0x00400000),
            trigger=field(r,top,"QueuedTriggerKey","NameProperty","BaseTopLevelPanelGFxObject",8,0),
            doing=field(r,top,"bIsDoingEquip","BoolProperty","StatusMenuInventoryPanelGFxObject",4,0),
            setup=field(r,top,"bInitialSetupFinished","BoolProperty","StatusMenuInventoryPanelGFxObject",4,0);
        if(!delegate || !queued || !trigger || !doing || !setup || !doing.extra || (doing.extra&(doing.extra-1)) ||
            !setup.extra || (setup.extra&(setup.extra-1)) || !r.ok()) return fail("input-fields");
        const auto delegateOwner=r.u32(top+delegate.offset),queuedOwner=r.u32(top+queued.offset);
        out.delegateObject=delegateOwner?r.token(delegateOwner):0;out.queuedObject=queuedOwner?r.token(queuedOwner):0;
        out.delegateName=r.name(top+delegate.offset+4);out.queuedName=r.name(top+queued.offset+4);out.triggerName=r.name(top+trigger.offset);
        out.doingEquip=(r.u32(top+doing.offset)&doing.extra)!=0;
        out.setupFinished=(r.u32(top+setup.offset)&setup.extra)!=0;
        if(!r.ok() || r.token(owner)!=out.owner || r.token(top)!=out.top) return fail("input-changed");
        out.valid=true;out.reason="input-state-owned";
    };
    captureTop();
    // A long Top field chain must not spend the outer Movie's independent
    // read budget or turn its unobserved delegate/filter values into defaults.
    UiModelSnapshot movieWitness;Reader movieReader(memory,base,movieWitness);
    const auto movieCounts=[&] {return "/bytes="+std::to_string(movieWitness.bytesRead)+"/top-bytes="+std::to_string(witness.bytesRead);};
    const auto rejectMovie=[&](const char* reason) {
        const auto failure=movieReader.readBudgetExceeded()?"read-budget":
            movieWitness.reason=="unreadable-or-budget"?"unreadable":movieWitness.reason;
        out.movieReason=std::string(reason)+"/"+failure+movieCounts();return out;
    };
    if(!movieReader.init() || movieReader.className(owner)!="StatusMenuExGFxMovie" ||
        movieReader.token(owner)!=out.owner) return rejectMovie("movie-input-owner");
    // GFxUI312/332/348/349: the Movie delegate is transient (CPF00402000),
    // unlike the Top delegate. Native Movie.InputKey uses this outer delegate
    // only after FGFxEngine's original owner/controller/filter gates.
    // Find these four inherited fields in one walk. Repeating the entire
    // Status/Willow child chain four times exhausted the genuine Movie budget.
    // A same-name field on a different declaring class must still reject.
    constexpr std::array<const char*,4> names{{"__OnInputKey__Delegate","LocalPlayerOwnerIndex","bCaptureInput","bDiscardNonOwnerInput"}};
    constexpr std::array<const char*,4> kinds{{"DelegateProperty","IntProperty","BoolProperty","BoolProperty"}};
    constexpr std::array<std::uint32_t,4> sizes{{12,4,4,4}},flags{{0x00402000,0x2000,0,0}};
    std::array<Property,4> movieFields{};
    std::array<std::uint32_t,32> parents{};
    auto type=movieReader.u32(owner+0x34);
    for(std::size_t depth=0;type && depth<parents.size() && movieReader.ok();++depth) {
        if(std::find(parents.begin(),parents.begin()+depth,type)!=parents.begin()+depth) {
            movieReader.fail("super-cycle");break;
        }
        parents[depth]=type;
        const auto typeKind=movieReader.className(type);
        if(typeKind!="Class" && typeKind!="ScriptStruct" && typeKind!="Function") {movieReader.fail("not-a-struct");break;}
        const auto structSize=movieReader.u32(type+0x48)&0x00FFFFFFu;
        if(!structSize || structSize>1048576) {movieReader.fail("struct-size");break;}
        auto item=movieReader.u32(type+0x44);
        std::array<std::uint32_t,512> visited{};std::size_t count=0;
        while(item && movieReader.ok()) {
            if(count==visited.size() || std::find(visited.begin(),visited.begin()+count,item)!=visited.begin()+count) {
                movieReader.fail("field-cycle-or-limit");break;
            }
            visited[count++]=item;
            if(!movieReader.token(item) || movieReader.u32(item+0x28)!=type) {movieReader.fail("field-owner");break;}
            const auto name=movieReader.objectName(item);
            for(std::size_t i=0;i<names.size();++i) if(!movieFields[i] && name==names[i]) {
                movieFields[i]=field(movieReader,owner,names[i],kinds[i],"GFxMoviePlayer",sizes[i],flags[i],type);
                if(!movieFields[i] || movieFields[i].address!=item) movieReader.fail("movie-field-schema");
            }
            if(std::all_of(movieFields.begin(),movieFields.end(),[](const Property& p){return bool(p);})) break;
            item=movieReader.u32(item+0x3C);
        }
        if(std::all_of(movieFields.begin(),movieFields.end(),[](const Property& p){return bool(p);})) break;
        type=movieReader.u32(type+0x40);
    }
    if(!std::all_of(movieFields.begin(),movieFields.end(),[](const Property& p){return bool(p);})) movieReader.fail("missing-property");
    const auto movieDelegate=movieFields[0],ownerIndex=movieFields[1],capture=movieFields[2],discard=movieFields[3];
    if(!movieDelegate || !ownerIndex || !capture || !discard || !capture.extra || (capture.extra&(capture.extra-1)) ||
        !discard.extra || (discard.extra&(discard.extra-1)) || !movieReader.ok()) return rejectMovie("movie-input-fields");
    const auto movieDelegateOwner=movieReader.u32(owner+movieDelegate.offset);
    out.movieDelegateObject=movieDelegateOwner?movieReader.token(movieDelegateOwner):0;
    out.movieDelegateName=movieReader.name(owner+movieDelegate.offset+4);
    out.movieDelegateClass=movieDelegateOwner?movieReader.className(movieDelegateOwner):"None";
    out.localPlayerOwnerIndex=static_cast<std::int32_t>(movieReader.u32(owner+ownerIndex.offset));
    out.captureInput=(movieReader.u32(owner+capture.offset)&capture.extra)!=0;
    out.discardNonOwnerInput=(movieReader.u32(owner+discard.offset)&discard.extra)!=0;
    if(!movieReader.ok() || movieReader.token(owner)!=out.owner ||
        (out.valid && movieReader.token(top)!=out.top)) return rejectMovie("movie-input-changed");
    out.movieValid=true;out.movieReason="movie-input-owned"+movieCounts();return out;
}
// Read-only witness for the original Status PanelOnInputKey controller gate
// and WillowGFxMovie3D.FilterButtonInput. It never calls either handler.
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
std::string CaptureBl2StatusInventoryGate(const UiModelMemory& memory,std::uint32_t base,
    std::uint32_t owner,std::uint32_t controller,std::uint32_t controllerId) {
    UiModelSnapshot topWitness,movieWitness,playerWitness;
    Reader topReader(memory,base,topWitness,4*kReadBudget),movieReader(memory,base,movieWitness,4*kReadBudget),
        playerReader(memory,base,playerWitness,4*kReadBudget);
    const auto counts=[&] {return " bytes=("+std::to_string(topWitness.bytesRead)+","+
        std::to_string(movieWitness.bytesRead)+","+std::to_string(playerWitness.bytesRead)+")";};
    const auto reject=[&](const char* stage) {
        const auto reason=[](const Reader& r,const UiModelSnapshot& w) {
            return r.readBudgetExceeded()?std::string("read-budget"):
                w.reason=="unreadable-or-budget"?std::string("unreadable"):w.reason;
        };
        return std::string("known=0 reason=")+stage+"/("+reason(topReader,topWitness)+","+
            reason(movieReader,movieWitness)+","+reason(playerReader,playerWitness)+")"+counts();
    };
    struct Spec {const char* name;const char* kind;const char* declaring;std::uint32_t flags;const char* reference;};
    // Each diagnostic reader has its own 512 KiB budget. The cooked Willow
    // controller has 2,387 direct children, above the default reader's512
    // fields. Existing functional readers retain their128 KiB/512 limits.
    const auto fields=[](Reader& r,std::uint32_t object,std::initializer_list<Spec> specs) {
        std::array<Property,2> found{};
        if(!r.token(object) || specs.size()>found.size()) {r.fail("gate-object");return found;}
        auto type=r.u32(object+0x34);std::array<std::uint32_t,32> parents{};
        for(std::size_t depth=0;type && depth<parents.size() && r.ok();++depth) {
            if(std::find(parents.begin(),parents.begin()+depth,type)!=parents.begin()+depth) {r.fail("super-cycle");break;}
            parents[depth]=type;
            if(r.className(type)!="Class") {r.fail("not-a-class");break;}
            const auto size=r.u32(type+0x48)&0x00FFFFFFu;
            if(!size || size>1048576) {r.fail("struct-size");break;}
            auto item=r.u32(type+0x44);std::array<std::uint32_t,4096> visited{};std::size_t count=0;
            while(item && r.ok()) {
                if(count==visited.size() || std::find(visited.begin(),visited.begin()+count,item)!=visited.begin()+count) {
                    r.fail("field-cycle-or-limit");break;
                }
                visited[count++]=item;
                if(!r.token(item) || r.u32(item+0x28)!=type) {r.fail("field-owner");break;}
                const auto name=r.objectName(item);std::size_t i=0;
                for(const auto& spec:specs) {
                    if(!found[i] && name==spec.name) {
                        const auto kind=r.u32(item+0x34),offset=r.u32(item+0x54);
                        if(r.objectName(type)!=spec.declaring || r.className(item)!=spec.kind ||
                            !(r.u32(kind+0xC4)&0x8000u) || r.u32(item+0x40)!=1 || r.u32(item+0x44)!=4 ||
                            offset<0x38 || (offset&3) || offset>size || 4>size-offset ||
                            r.u32(item+0x48)!=spec.flags || r.u32(item+0x4C)) {r.fail("gate-field-schema");break;}
                        const auto extra=std::strcmp(spec.kind,"IntProperty")==0?0:r.u32(item+0x74);
                        if(spec.reference && (r.className(extra)!="Class" || r.objectName(extra)!=spec.reference)) {
                            r.fail("gate-property-class");break;
                        }
                        if(std::strcmp(spec.kind,"BoolProperty")==0 && (!extra || (extra&(extra-1)))) {
                            r.fail("gate-bool-mask");break;
                        }
                        found[i]={item,offset,4,extra};
                    }
                    ++i;
                }
                if(std::all_of(found.begin(),found.begin()+specs.size(),[](const Property& p){return bool(p);})) return found;
                item=r.u32(item+0x3C);
            }
            type=r.u32(type+0x40);
        }
        r.fail("missing-gate-property");return found;
    };
    if(!topReader.init() || topReader.className(owner)!="StatusMenuExGFxMovie" ||
        !movieReader.init() || !movieReader.isA(owner,"WillowGFxMovie3D") ||
        !playerReader.init() || !playerReader.isA(controller,"WillowPlayerController")) return reject("gate-owner");
    const auto ownerToken=topReader.token(owner),controllerToken=playerReader.token(controller);
    const auto ownerClass=topReader.u32(owner+0x34),controllerClass=playerReader.u32(controller+0x34);
    const auto panelField=fields(topReader,owner,{{"InventoryPanel","ObjectProperty","StatusMenuExGFxMovie",0,"StatusMenuInventoryPanelGFxObject"}})[0];
    const auto top=panelField?topReader.u32(owner+panelField.offset):0;
    if(!panelField || !topReader.isA(top,"StatusMenuInventoryPanelGFxObject")) return reject("gate-panel");
    const auto topToken=topReader.token(top);
    const auto topClass=topReader.u32(top+0x34);
    const auto parentField=fields(topReader,top,{{"ParentMovie","ObjectProperty","BaseTopLevelPanelGFxObject",0,"WillowInventoryGFxMovie"}})[0];
    const auto movieFields=fields(movieReader,owner,{
        {"WPCOwner","ObjectProperty","WillowGFxMovie",0x2000,"WillowPlayerController"},
        {"bAlwaysHideInCinematicMode","BoolProperty","WillowGFxMovie3D",0x2000,nullptr}});
    const auto playerFields=fields(playerReader,controller,{
        {"Player","ObjectProperty","PlayerController",2,"Player"},
        {"bCinematicMode","BoolProperty","PlayerController",0,nullptr}});
    if(!parentField || !movieFields[0] || !movieFields[1] || !playerFields[0] || !playerFields[1] ||
        !topReader.ok() || !movieReader.ok() || !playerReader.ok()) return reject("gate-fields");
    const auto parent=topReader.u32(top+parentField.offset),wpc=movieReader.u32(owner+movieFields[0].offset),
        player=playerReader.u32(controller+playerFields[0].offset);
    if(!topReader.isA(parent,"WillowInventoryGFxMovie") || (wpc && !movieReader.isA(wpc,"WillowPlayerController")) ||
        !playerReader.isA(player,"LocalPlayer")) return reject("gate-links");
    if(parent!=owner) return reject("gate-foreign-parent");
    // A mismatched movie/controller is reported, not interpreted as the
    // selected controller's FilterButtonInput result.
    if(wpc && wpc!=controller) return reject("gate-foreign-wpc");
    const auto parentToken=topReader.token(parent),wpcToken=wpc?movieReader.token(wpc):0,playerToken=playerReader.token(player);
    const auto playerClass=playerReader.u32(player+0x34);
    const auto idField=fields(playerReader,player,{{"ControllerId","IntProperty","LocalPlayer",0,nullptr}})[0];
    if(!idField || !playerReader.ok()) return reject("gate-controller-id");
    const auto localId=playerReader.u32(player+idField.offset),hideWord=movieReader.u32(owner+movieFields[1].offset),
        cinematicWord=playerReader.u32(controller+playerFields[1].offset);
    const bool hide=(hideWord&movieFields[1].extra)!=0,cinematic=(cinematicWord&playerFields[1].extra)!=0;
    // Native GetMyControllerId reads ControllerId's low byte after checking
    // Player is LocalPlayer. FGFxEngine's player lookup uses the full integer.
    if(playerReader.u32(base+0x01B427B0)!=0xE590566C || playerReader.u32(base+0x01B427F4)!=0xE5D50064 ||
        playerFields[0].offset!=0x66C || idField.offset!=0x64) return reject("gate-native-id-abi");
    const auto stable=[](Reader& r,const Property& p,const char* kind,const char* declaring,std::uint32_t flags) {
        return r.token(p.address) && r.className(p.address)==kind && r.objectName(r.u32(p.address+0x28))==declaring &&
            r.u32(p.address+0x40)==1 && r.u32(p.address+0x44)==p.size &&
            r.u32(p.address+0x54)==p.offset && r.u32(p.address+0x48)==flags && !r.u32(p.address+0x4C) &&
            (!p.extra || r.u32(p.address+0x74)==p.extra);
    };
    if(topReader.token(owner)!=ownerToken || topReader.u32(owner+0x34)!=ownerClass ||
        topReader.token(top)!=topToken || topReader.u32(top+0x34)!=topClass || topReader.token(parent)!=parentToken ||
        topReader.u32(owner+panelField.offset)!=top || topReader.u32(top+parentField.offset)!=parent ||
        movieReader.token(owner)!=ownerToken || movieReader.u32(owner+0x34)!=ownerClass ||
        (wpc && movieReader.token(wpc)!=wpcToken) || movieReader.u32(owner+movieFields[0].offset)!=wpc ||
        movieReader.u32(owner+movieFields[1].offset)!=hideWord || playerReader.token(controller)!=controllerToken ||
        playerReader.u32(controller+0x34)!=controllerClass || playerReader.token(player)!=playerToken ||
        playerReader.u32(player+0x34)!=playerClass || playerReader.u32(controller+playerFields[0].offset)!=player ||
        playerReader.u32(controller+playerFields[1].offset)!=cinematicWord || playerReader.u32(player+idField.offset)!=localId ||
        !stable(topReader,panelField,"ObjectProperty","StatusMenuExGFxMovie",0) ||
        !stable(topReader,parentField,"ObjectProperty","BaseTopLevelPanelGFxObject",0) ||
        !stable(movieReader,movieFields[0],"ObjectProperty","WillowGFxMovie",0x2000) ||
        !stable(movieReader,movieFields[1],"BoolProperty","WillowGFxMovie3D",0x2000) ||
        !stable(playerReader,playerFields[0],"ObjectProperty","PlayerController",2) ||
        !stable(playerReader,playerFields[1],"BoolProperty","PlayerController",0) ||
        !stable(playerReader,idField,"IntProperty","LocalPlayer",0) ||
        !topReader.ok() || !movieReader.ok() || !playerReader.ok()) return reject("gate-changed");
    return "known=1 reason=gate-owned parent="+std::to_string(parent)+" parentSame="+std::to_string(parent==owner)+
        " wpc="+std::to_string(wpc)+" wpcSame="+std::to_string(wpc==controller)+" player="+std::to_string(player)+
        " localId="+std::to_string(localId)+" getMyId="+std::to_string(localId&255u)+
        " controllerMatch="+std::to_string(controllerId==(localId&255u))+" cinematic="+std::to_string(cinematic)+
        " alwaysHide="+std::to_string(hide)+" derivedFilter="+std::to_string(wpc && cinematic && hide)+counts();
}
#endif
UiAgeGateModel CaptureBl2AgeGate(const UiModelMemory& memory,std::uint32_t base,std::uint32_t owner) {
    UiAgeGateModel out;UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto reject=[&](const char* reason) {out.reason=std::string(reason)+"/"+witness.reason;return out;};
    if(!r.init() || r.className(owner)!="GearboxAccountGFxMovie") return reject("age-owner");
    out.owner=r.token(owner);
    const auto cls=r.u32(owner+0x34);
    const auto stack=r.objectProperty(owner,"ScreenStack","ArrayProperty");
    const auto inner=r.inner(stack,"StrProperty");
    if(!stack || stack.size!=12 || r.u32(stack.address+0x28)!=cls ||
        r.u32(stack.address+0x48)!=0x00402000 || r.u32(stack.address+0x4C) ||
        !inner || inner.size!=12 || r.u32(inner.address+0x48)!=0x00400000 || r.u32(inner.address+0x4C))
        return reject("age-stack-schema");
    const auto screens=r.array(owner+stack.offset,12,64,4096);
    if(!screens.count || !r.ok()) return reject("age-stack-empty");
    // This ELF's FString uses bytes, including its terminator. Read only the
    // active original screen, never infer readiness from the movie class.
    const auto top=r.array(screens.data+(screens.count-1)*12,1,8,4096);
    std::array<char,8> text{};
    if(top.count!=text.size() || !r.copy(top.data,text.data(),text.size()) ||
        std::memcmp(text.data(),"ageGate",text.size())) return reject("age-stack-top");
    const auto object=r.objectProperty(owner,"GearboxAccountObj","ObjectProperty");
    const auto objectClass=object?r.u32(object.address+0x74):0;
    if(!object || object.size!=4 || r.u32(object.address+0x28)!=cls ||
        r.u32(object.address+0x48)!=0x2000 || r.u32(object.address+0x4C) ||
        r.className(objectClass)!="Class" || r.objectName(objectClass)!="GearboxAccountGFxObject")
        return reject("age-object-schema");
    const auto clip=r.u32(owner+object.offset);
    if(r.className(clip)!="GearboxAccountGFxObject") return reject("age-object");
    out.clip=r.token(clip);
    out.input=keyHandler(r,owner,"HandleInputKey","GearboxAccountGFxMovie");
    if(!out.input.function || !r.ok()) return reject("age-input");
    const auto event=r.property(out.input.function,"uevent","ByteProperty");
    const auto eventEnum=event?r.u32(event.address+0x74):0;
    if(r.className(eventEnum)!="Enum" || r.objectName(eventEnum)!="EInputEvent") return reject("age-input-enum");
    out.structure=mix(mix(mix(out.owner,out.clip),r.token(stack.address)),r.token(inner.address));
    for(const auto value:{std::uint64_t(stack.offset),std::uint64_t(screens.data),std::uint64_t(screens.count),
                         std::uint64_t(top.data),r.token(object.address),out.input.token,r.token(eventEnum)}) out.structure=mix(out.structure,value);
    out.valid=r.ok();out.reason=out.valid?"age-gate-owned":witness.reason;return out;
}
UiTextChatModel CaptureBl2TextChat(const UiModelMemory& memory,std::uint32_t base,std::uint32_t owner) {
    UiTextChatModel out;UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto reject=[&](const char* reason) {out.reason=std::string(reason)+"/"+witness.reason;return out;};
    if(!r.init() || r.className(owner)!="TextChatGFxMovie") return reject("chat-owner");
    out.owner=r.token(owner);
    const auto chat=r.objectProperty(owner,"ChatObj","ObjectProperty");
    const auto cls=chat?r.u32(chat.address+0x74):0;
    if(!chat || chat.size!=4 || r.u32(chat.address+0x48)!=0x2000 || r.u32(chat.address+0x4C) ||
        r.className(cls)!="Class" || r.objectName(cls)!="GFxObject") return reject("chat-property");
    const auto object=r.u32(owner+chat.offset);
    if(!r.isA(object,"GFxObject")) return reject("chat-object");
    out.chat=r.token(object);
    out.input=keyHandler(r,owner,"HandleTextChatInput","TextChatGFxMovie");
    if(!out.input.function || !r.ok()) return reject("chat-input");
    out.valid=true;out.reason="chat-input-owned";return out;
}
bool Bl2TouchTextChatStopped(const UiModelMemory& memory,std::uint32_t base,
    std::uint64_t ownerToken,std::uint64_t chatToken) {
    UiModelSnapshot witness;Reader r(memory,base,witness);
    const auto owner=static_cast<std::uint32_t>(ownerToken>>32),chat=static_cast<std::uint32_t>(chatToken>>32);
    if(!ownerToken || !chatToken || !r.init() || r.token(owner)!=ownerToken || r.token(chat)!=chatToken ||
        r.className(owner)!="TextChatGFxMovie" || !r.isA(chat,"GFxObject")) return false;
    const auto object=r.objectProperty(owner,"ChatObj","ObjectProperty");
    const auto objectClass=object?r.u32(object.address+0x74):0;
    if(!object || object.size!=4 || r.u32(object.address+0x48)!=0x2000 || r.u32(object.address+0x4C) ||
        r.className(objectClass)!="Class" || r.objectName(objectClass)!="GFxObject" || r.u32(owner+object.offset)!=chat) return false;
    auto cls=r.u32(owner+0x34);
    for(unsigned depth=0;cls && depth<32 && r.ok();++depth) {
        if(r.className(cls)!="Class") return false;
        if(r.objectName(cls)=="GFxMoviePlayer") {
            // GFxUI export348, Outer44, CPF0. The same-named action property
            // export424 belongs to GFxAction_OpenMovie and is not this field.
            const auto p=r.property(cls,"bCaptureInput","BoolProperty");
            if(!p || r.u32(p.address+0x28)!=cls || p.size!=4 || !p.extra || (p.extra&(p.extra-1)) ||
                r.u32(p.address+0x48) || r.u32(p.address+0x4C) || (r.u32(owner+p.offset)&p.extra)) return false;
            return r.token(owner)==ownerToken && r.token(chat)==chatToken && r.u32(owner+object.offset)==chat && r.ok();
        }
        cls=r.u32(cls+0x40);
    }
    return false;
}
std::uint64_t Bl2TouchInventoryToken(const UiModelMemory& memory,std::uint32_t base,std::uint32_t inventory) {
    UiModelSnapshot witness;Reader r(memory,base,witness);
    return r.init() && r.isA(inventory,"WillowInventory") && r.ok()?r.token(inventory):0;
}
std::string Bl2TouchMovieSource(const UiModelMemory& memory,std::uint32_t base,std::uint32_t owner) {
    UiModelSnapshot evidence; Reader r(memory,base,evidence);
    if (!r.init() || !r.token(owner)) return {};
    auto object = r.valueObject(owner,"MovieInfo");
    if (!r.ok() || !r.isA(object,"SwfMovie")) return {};
    std::string result;
    std::array<std::uint32_t,8> seen{};
    for (unsigned i = 0; object && i < seen.size(); ++i) {
        if (std::find(seen.begin(),seen.begin()+i,object) != seen.begin()+i || !r.token(object)) return {};
        seen[i] = object;
        const auto name = r.objectName(object);
        if (name.empty()) return {};
        result = name + (result.empty() ? std::string{} : "."+result);
        object = r.u32(object + 0x28);
    }
    return r.ok() && !object ? result : std::string{};
}

bool Bl2TouchDirectUiCurrent(const UiModelMemory& memory,std::uint32_t base,const UiDirectModel& model,std::int32_t* selected) {
    UiModelSnapshot evidence; Reader r(memory,base,evidence);
    const auto owner = static_cast<std::uint32_t>(model.owner >> 32);
    if (!model.valid || !r.init() || r.token(owner) != model.owner || r.className(owner) != model.movieClass) return false;
    if (model.screenField) {
        if (!r.token(model.screenField) || r.u32(model.screenField + 0x40) != model.screenDimension ||
            r.u32(model.screenField + 0x44) != model.screenSize || r.u32(model.screenField + 0x54) != model.screenOffset) return false;
        std::uint32_t value = 0;
        if (!r.copy(owner + model.screenOffset + model.player * model.screenSize,&value,model.screenSize) ||
            static_cast<std::int32_t>(value) != model.screen) return false;
    }
    if (model.clipField && (!r.token(model.clipField) || r.u32(model.clipField + 0x54) != model.clipOffset ||
        r.token(r.u32(owner + model.clipOffset)) != model.clip)) return false;
    if (selected && model.selectionField) {
        if (!r.token(model.selectionField) || r.u32(model.selectionField + 0x40) != 2 ||
            r.u32(model.selectionField + 0x44) != 4 || r.u32(model.selectionField + 0x54) != model.selectionOffset) return false;
        *selected = static_cast<std::int32_t>(r.u32(owner + model.selectionOffset + model.player * 4));
    }
    return r.ok();
}
bool Bl2TouchTrainingPromptReady(const UiModelMemory& memory,std::uint32_t base,const UiDirectModel& model) {
    if(!model.trainingPrompt || model.screen<0 || model.screen>5 || !Bl2TouchDirectUiCurrent(memory,base,model)) return false;
    const auto fresh=CaptureBl2DirectUi(memory,base,static_cast<std::uint32_t>(model.owner>>32),model.player);
    if(!fresh.valid || !fresh.trainingPrompt || fresh.owner!=model.owner || fresh.screen!=model.screen ||
        fresh.click.token!=model.click.token || fresh.click.bytes!=20) return false;
    UiModelSnapshot witness;Reader r(memory,base,witness);float seconds=0;
    const auto owner=static_cast<std::uint32_t>(model.owner>>32);
    if(!r.init()) return false;
    const auto delay=r.objectProperty(owner,"DelayUntilShowOk","FloatProperty");
    return delay && delay.size==4 && r.copy(owner+delay.offset,&seconds,4) &&
        std::isfinite(seconds) && seconds<0 && r.ok();
}

UiMapState CaptureBl2MapState(const UiModelMemory& memory,std::uint32_t base,const UiDirectModel& model) {
    UiMapState out;
    UiModelSnapshot evidence; Reader r(memory,base,evidence);
    if (model.movieClass != "StatusMenuExGFxMovie" || model.screen != 2 ||
        !Bl2TouchDirectUiCurrent(memory,base,model) || !r.init()) { out.reason="map-not-current";return out; }
    const auto map = static_cast<std::uint32_t>(model.clip >> 32);
    if (r.token(map) != model.clip || !r.isA(map,"StatusMenuMapGFxObject")) { out.reason="map-owner";return out; }
    const auto number = [&](std::uint32_t object,const char* field,float& value) {
        const auto p = r.objectProperty(object,field,"FloatProperty");
        return p && p.size == 4 && r.copy(object+p.offset,&value,4) && std::isfinite(value);
    };
    const auto vector = [&](const char* field,float& x,float& y,Property& p) {
        p = r.objectProperty(map,field,"StructProperty");
        if (!p || p.size != 8 || r.className(p.extra) != "ScriptStruct" || r.objectName(p.extra) != "Vector2D") return false;
        const auto xp = r.property(p.extra,"X","FloatProperty"),yp = r.property(p.extra,"Y","FloatProperty");
        return xp && yp && xp.offset == 0 && yp.offset == 4 && xp.size == 4 && yp.size == 4 &&
            r.copy(map+p.offset,&x,4) && r.copy(map+p.offset+4,&y,4) && std::isfinite(x) && std::isfinite(y);
    };
    const auto controller = r.valueObject(map,"PlayerOwner");
    const auto world = r.inheritedObject(controller,"Actor","WorldInfo");
    Property cursor,clip;
    if (!r.isA(controller,"PlayerController") || !r.isA(world,"WorldInfo") ||
        !number(map,"ZoomSpeedFactor",out.zoomSpeed) || out.zoomSpeed <= 0 || out.zoomSpeed > 10 ||
        !number(world,"DeltaSeconds",out.deltaSeconds) || out.deltaSeconds <= 0 || out.deltaSeconds > 1 ||
        !vector("CursorPos",out.cursorX,out.cursorY,cursor) || !vector("ClipSize",out.clipWidth,out.clipHeight,clip) ||
        out.clipWidth <= 0 || out.clipHeight <= 0 || out.clipWidth > 100000 || out.clipHeight > 100000 || !r.ok()) {
        out.reason=evidence.reason.empty() ? "map-property-shape" : evidence.reason;return out;
    }
    out.map=model.clip;out.world=r.token(world);out.cursorProperty=cursor.address;out.cursorOffset=cursor.offset;
    out.valid=r.ok();out.reason=out.valid ? "map-properties-observed" : evidence.reason;return out;
}

UiModelSnapshot CaptureBl2UiModel(const UiModelMemory& memory, std::uint32_t base,
                                 std::uint32_t owner, bool decorateSettings) {
    UiModelSnapshot out;
    Reader reader(memory,base,out);
    if (!reader.init() || !(out.owner = reader.token(owner))) return out;
    out.movieClass = reader.className(owner);
    if (out.movieClass == "OptionsGFxMovie") out.options = reader.token(reader.valueObject(owner,"OptionsObj"));
    if (out.movieClass == "FrontendGFxMovie" || out.movieClass == "PauseGFxMovie" ||
        out.movieClass == "OptionsGFxMovie") captureList(reader,owner,out);
    else if (Bl2TouchNativeDialogMovie(out.movieClass)) captureDialog(reader,owner,out);
    else if (out.movieClass == "WillowGameViewportClient") {
        const auto ui = reader.valueObject(owner,"ViewportUI");
        if (reader.ok() && reader.isA(ui,"WillowUIInteraction")) {
            const auto opened = reader.objectProperty(ui,"bOpened","BoolProperty");
            if (!opened || opened.size != 4 || !opened.extra || (opened.extra & (opened.extra - 1)))
                reader.fail("native-menu-mask");
            else {
                out.nativeMenuOpen = (reader.u32(ui + opened.offset) & opened.extra) != 0;
                out.structure = mix(reader.token(ui),out.nativeMenuOpen);
            }
        }
    } else reader.fail("unreviewed-movie-class");
    out.valid = reader.ok();
    if (out.valid && out.options) out.structure = mix(out.structure,out.options);
    if (out.valid) out.reason = "model-observed";
    else out.structure = 0;
    if (decorateSettings) DecorateBl2TouchSettingsModel(out);
    return out;
}

std::uint32_t Bl2TouchGameViewportClient(const UiModelMemory& memory, std::uint32_t base) {
    UiModelSnapshot witness;
    Reader reader(memory,base,witness);
    if (!reader.init()) return 0;
    const auto engine = reader.u32(base + 0x03008D20);
    if (!reader.isA(engine,"Engine")) return 0;
    const auto client = reader.valueObject(engine,"GameViewport");
    return reader.ok() && reader.className(client) == "WillowGameViewportClient" && reader.ok() ? client : 0;
}

bool Bl2TouchPausedWorld(const UiModelMemory& memory, std::uint32_t base,
                         std::uint32_t controller, std::uint32_t pauseMovieOwner, std::string* reason,
                         UiPauseWorldIdentity* identity) {
    UiModelSnapshot witness;
    Reader reader(memory,base,witness);
    UiPauseWorldIdentity captured;
    const auto result = [&](bool value, const char* step) {
        if (identity) *identity = value ? captured : UiPauseWorldIdentity{};
        if (reason) *reason = std::string(step) + "/" + witness.reason + " bytes=" + std::to_string(witness.bytesRead);
        return value;
    };
    if (!reader.init() || !reader.isA(controller,"PlayerController")) return result(false,"controller");
    // Pawn/WorldInfo are native Controller/Actor properties. Walking the long
    // WillowPlayerController child/function chain before inherited fields can
    // exhaust the bounded metadata reader during a perfectly live game.
    const auto pawn = reader.inheritedObject(controller,"Controller","Pawn");
    if (!reader.ok() || !reader.isA(pawn,"Pawn")) return result(false,"pawn");
    const auto world = reader.inheritedObject(controller,"Actor","WorldInfo");
    if (!reader.ok() || !reader.isA(world,"WorldInfo")) return result(false,"world-info");
    if (identity) {
        captured = {reader.token(controller),reader.token(pawn),reader.token(world)};
        if (!reader.ok() || !captured.controller || !captured.pawn || !captured.world) return result(false,"world-identity");
    }
    if (pauseMovieOwner && reader.className(pauseMovieOwner) == "PauseGFxMovie" && reader.ok()) return result(true,"owned-pause-movie");
    // SetGFxPauseMenuOpenState updates this original engine flag even when a
    // LAN world keeps simulating. It remains set through nested Options and
    // dialogs. Validate its native producer rather than inventing a UI latch.
    constexpr std::array<std::uint32_t,9> pauseWords{
        0xE59F3018,0xE79F3003,0xE5933000,0xE3530000,0x15D32294,
        0x17C02011,0x15C32294,0xE12FFF1E,0x00BA9C80};
    bool producer = true;
    for (std::size_t i = 0; i < pauseWords.size(); ++i) {
        std::uint32_t word = 0;
        if (!memory.copy || !memory.copy(memory.context,base + 0x0236A188 + static_cast<std::uint32_t>(i * 4),&word,4) ||
            word != pauseWords[i]) { producer = false; break; }
    }
    if (producer) {
        const auto engine = reader.u32(base + 0x03008D20);
        if (reader.isA(engine,"Engine") && (reader.u32(engine + 0x294) & 1u) && reader.ok()) return result(true,"engine-pause-flag");
    }
    const auto pauser = reader.objectProperty(world,"Pauser","ObjectProperty");
    if (!pauser || pauser.size != 4) return result(false,"pauser-property");
    const auto value = reader.u32(world + pauser.offset);
    return result(value && reader.isA(value,"PlayerReplicationInfo") && reader.ok(),"world-pauser");
}

bool Bl2TouchAtStartTitle(const UiModelMemory& memory, std::uint32_t base,
                          std::uint32_t owner) {
    UiModelSnapshot witness;
    Reader reader(memory,base,witness);
    if (!reader.init() || reader.className(owner) != "WillowGFxMoviePressStart") return false;
    // UObject::ProcessDelegate reads Object at +0, followed by the two FName
    // words at +4/+8. The title installs this delegate only after loading.
    if (reader.u32(base + 0x00F97834) != 0xE5933004 ||
        reader.u32(base + 0x00F9785C) != 0xE5964000 ||
        reader.u32(base + 0x00F9788C) != 0xE9960006) return false;
    const auto delegate = reader.objectProperty(owner,"__OnInputKey__Delegate","DelegateProperty");
    if (!delegate || delegate.size != 12) return false;
    const auto target = reader.u32(owner + delegate.offset);
    return (!target || target == owner) &&
        reader.name(owner + delegate.offset + 4) == "HandleInputKey_AtTitle" && reader.ok();
}
} // namespace bl2::touch
