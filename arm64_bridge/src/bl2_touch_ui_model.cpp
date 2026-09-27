#include "bl2_touch_ui_model.h"

#include <algorithm>
#include <array>
#include <cstring>
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
    Reader(const UiModelMemory& memory, std::uint32_t base, UiModelSnapshot& output)
        : memory_(memory), base_(base), output_(output) {}
    bool copy(std::uint32_t p, void* out, std::size_t n) {
        if (!ok_ || !memory_.copy || p < 0x10000 || n > kReadBudget - output_.bytesRead ||
            n > std::uint64_t(std::numeric_limits<std::uint32_t>::max()) + 1 - p ||
            !memory_.copy(memory_.context,p,out,n)) {
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
    Property property(std::uint32_t type, const char* wanted, const char* wantedKind) {
        std::array<std::uint32_t,32> parents{};
        for (std::size_t depth = 0; type && depth < parents.size() && ok_; ++depth) {
            if (std::find(parents.begin(),parents.begin() + depth,type) != parents.begin() + depth) {
                fail("super-cycle"); return {};
            }
            parents[depth] = type;
            const auto typeKind = className(type);
            if (typeKind != "Class" && typeKind != "ScriptStruct") { fail("not-a-struct"); return {}; }
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
                    if (u32(field + 0x40) != 1 || !p.size || p.size > 4096 || p.offset > size ||
                        p.size > size - p.offset || (typeKind == "Class" && p.offset < 0x38)) {
                        fail("property-shape"); return {};
                    }
                    if (std::strcmp(wantedKind,"ArrayProperty") == 0 || std::strcmp(wantedKind,"BoolProperty") == 0)
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

private:
    UiModelMemory memory_;
    std::uint32_t base_;
    UiModelSnapshot& output_;
    bool ok_ = true;
    Array objects_{}, names_{};
};

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

UiModelSnapshot CaptureBl2UiModel(const UiModelMemory& memory, std::uint32_t base,
                                 std::uint32_t owner) {
    UiModelSnapshot out;
    Reader reader(memory,base,out);
    if (!reader.init() || !(out.owner = reader.token(owner))) return out;
    out.movieClass = reader.className(owner);
    if (out.movieClass == "FrontendGFxMovie" || out.movieClass == "PauseGFxMovie" ||
        out.movieClass == "OptionsGFxMovie") captureList(reader,owner,out);
    else if (out.movieClass == "WillowGFxDialogBox") captureDialog(reader,owner,out);
    else reader.fail("unreviewed-movie-class");
    out.valid = reader.ok();
    if (out.valid) out.reason = "model-observed";
    else out.structure = 0;
    return out;
}
} // namespace bl2::touch
