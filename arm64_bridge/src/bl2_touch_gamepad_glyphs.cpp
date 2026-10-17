// HIDE GAMEPAD BUTTONS: the game's own gamepad button images.
//
// BL2 never stores button images in its texts. Captions carry markup such as
// <StringAliasMap:GFx_Accept>, which UIDataStore_StringAliasMap resolves on
// every lookup through MenuInputMapArray(index).MappedText of the active
// platform set (Android-WillowGame.ini: Set="ANDROID",
// MappedText="<IMG src='android_A' vspace='-3'>"). WillowUIDataStore_StringAliasMap
// only adds FakePlatform and its GetStringWithFieldName; it keeps no cache.
//
// Rewriting those MappedText strings therefore changes every caption resolved
// afterwards: menu footers, HUD prompts and training windows, in all sets. The
// strings are reassigned through the game's own FString::operator=(const char*),
// so the engine keeps owning their memory; the originals stay on the host and
// are assigned back when the setting is switched off. Every offset comes from
// live UProperty metadata; only GObjObjects, FName::Names and the assignment
// function are fixed addresses of this exact image, and they are checked.
#include "bl2_touch_gamepad_glyphs.h"

#if !defined(BL2_GAMEPAD_GLYPHS_HOST_TEST)
#include "bl2_touch_settings.h"
#include "bl2_touch_guest_call.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
#include <sys/system_properties.h>
#endif
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bl2::touch {
namespace {
std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
bool gamepadSource(const std::string& source) {
    for (const char* prefix : {"android_", "xbox360_", "ps3_"})
        if (source.size() > std::strlen(prefix) && source.compare(0, std::strlen(prefix), prefix) == 0) return true;
    return false;
}
} // namespace

bool Bl2HideGamepadMarkup(const std::string& text, GamepadGlyphHiding mode, std::string& hidden) {
    std::string out;
    bool changed = false;
    for (std::size_t at = 0; at < text.size();) {
        const auto open = text.find('<', at);
        if (open == std::string::npos) { out.append(text, at, std::string::npos); break; }
        out.append(text, at, open - at);
        const auto close = text.find('>', open + 1);
        if (close == std::string::npos) { out.append(text, open, std::string::npos); break; }
        const auto tag = text.substr(open, close - open + 1), low = lower(tag);
        std::string source;
        if (low.compare(0, 5, "<img ") == 0) {
            auto src = low.find("src", 5);
            while (src != std::string::npos && src + 3 < low.size() && low[src + 3] != '=' && low[src + 3] != ' ')
                src = low.find("src", src + 3);
            if (src != std::string::npos) {
                auto value = src + 3;
                while (value < tag.size() && (tag[value] == ' ' || tag[value] == '=')) ++value;
                if (value < tag.size() && (tag[value] == '\'' || tag[value] == '"')) {
                    const auto end = tag.find(tag[value], value + 1);
                    if (end != std::string::npos) source = tag.substr(value + 1, end - value - 1);
                }
            }
        }
        if (!gamepadSource(source)) { out += tag; at = close + 1; continue; }
        changed = true;
        if (mode == GamepadGlyphHiding::Shrink) out += "<IMG src='" + source + "' width='1' height='1'>";
        else if (mode == GamepadGlyphHiding::Unresolved) out += "<IMG src='bl2hidden_" + source + "'>";
        at = close + 1;
    }
    if (changed) hidden = out;
    return changed;
}

std::string Bl2VisibleGlyphSource(const std::string& source) {
    constexpr const char* prefix = "bl2hidden_";
    return source.compare(0, std::strlen(prefix), prefix) == 0 ? source.substr(std::strlen(prefix)) : source;
}

} // namespace bl2::touch

#if !defined(BL2_GAMEPAD_GLYPHS_HOST_TEST)
namespace {
using bl2::touch::GamepadGlyphHiding;
[[maybe_unused]] constexpr const char* kTag = "BL2GamepadGlyphs";
constexpr std::uint32_t kImageBytes = 0x03089000;
constexpr std::uint32_t kGObjObjects = 0x02FBA268;  // UObject::GObjObjects
constexpr std::uint32_t kNames = 0x02FB00C8;        // FName::Names
constexpr std::uint32_t kStringAssign = 0x00F27A68; // FString::operator=(const char*)
constexpr std::uint32_t kAssignWords[] = {0xE92D4070, 0xE1A05001, 0xE5901000, 0xE24DD008};
constexpr std::uint32_t kScratchBytes = 512;
// The markup mode of release builds; diagnostics may override it with
// debug.bl2bridge.glyph_mode (0 remove, 1 shrink, 2 unresolved) before launch.
// The shipped Scaleform (ProcessImageTags 0x028BA3DC) looks a plain IMG source
// up only among the movie's exports: an unknown one gets no image and keeps the
// 0x0 size the HTML parser (0x02867F60) stored, while the IMG and its SRC stay
// in the text for the footer parser. WIDTH/HEIGHT 0 would mean natural size.
constexpr GamepadGlyphHiding kDefaultMode = GamepadGlyphHiding::Unresolved;

struct Entry {
    std::uint32_t object = 0, string = 0;
    std::uint64_t token = 0;
    std::string original, hidden;
};
// Game thread only.
struct State {
    enum class Status { Unchecked, Off, Ready } status = Status::Unchecked;
    std::uint32_t base = 0, scratch = 0;
    GamepadGlyphHiding mode = kDefaultMode;
    std::vector<Entry> entries;
    bool scanned = false, hidden = false;
    std::uint64_t checkedAt = 0, scannedAt = 0;
} g;

struct Reader {
    DynarmicHost& host;
    std::uint32_t base;
    bool span(std::uint32_t p, std::size_t n) const {
        return p >= 0x10000 && p <= host.GetGuestMemorySize() && n <= host.GetGuestMemorySize() - p;
    }
    std::uint32_t u32(std::uint32_t p) const { return span(p, 4) ? host.MemoryRead32(p) : 0; }
    bool array(std::uint32_t p, std::uint32_t limit, std::uint32_t stride, std::uint32_t& data, std::uint32_t& count) const {
        if (!span(p, 12)) return false;
        data = u32(p); count = u32(p + 4);
        const auto capacity = u32(p + 8);
        return count <= capacity && capacity <= limit && (!count || span(data, std::size_t(count) * stride));
    }
    std::uint64_t token(std::uint32_t object) const {
        std::uint32_t data = 0, count = 0;
        if (!span(object, 0x48) || (u32(object + 0x0C) & 0x20000002u) ||
            !array(base + kGObjObjects, 2000000, 4, data, count)) return 0;
        const auto index = u32(object + 0x20);
        if (index >= count || u32(data + index * 4) != object) return 0;
        return (std::uint64_t(object) << 32) | index;
    }
    std::string name(std::uint32_t address) const {
        std::uint32_t data = 0, count = 0;
        if (!span(address, 8) || !array(base + kNames, 2000000, 4, data, count)) return {};
        const auto index = u32(address);
        if (index >= count || u32(address + 4)) return {};
        const auto entry = u32(data + index * 4);
        if (!span(entry, 8 + 64)) return {};
        std::string value;
        for (std::uint32_t i = 0; i != 64; ++i) {
            const auto c = host.MemoryRead8(entry + 8 + i);
            if (!c) return value;
            if (c < 32 || c > 126) return {};
            value += static_cast<char>(c);
        }
        return {};
    }
    std::string objectName(std::uint32_t object) const { return token(object) ? name(object + 0x2C) : std::string{}; }
    std::string className(std::uint32_t object) const { return objectName(u32(object + 0x34)); }
    // The class chain through UStruct::SuperField, as the touch sinks walk it.
    bool derives(std::uint32_t type, std::uint32_t wanted) const {
        for (unsigned depth = 0; type && depth != 48; ++depth) {
            if (type == wanted) return true;
            if (!token(type)) return false;
            type = u32(type + 0x40);
        }
        return false;
    }
    struct Property { std::uint32_t field = 0, offset = 0, size = 0, extra = 0; };
    Property property(std::uint32_t type, const char* wanted, const char* kind) const {
        for (unsigned depth = 0; type && depth != 48; ++depth) {
            if (!token(type)) return {};
            auto field = u32(type + 0x44);
            for (unsigned i = 0; field && i != 4096; ++i) {
                if (!token(field) || u32(field + 0x28) != type) return {};
                if (objectName(field) == wanted) {
                    Property p{field, u32(field + 0x54), u32(field + 0x44), u32(field + 0x74)};
                    if (className(field) != kind || u32(field + 0x40) != 1 || !p.size || p.size > 4096) return {};
                    return p;
                }
                field = u32(field + 0x3C);
            }
            type = u32(type + 0x40);
        }
        return {};
    }
    // A one-byte FString (this image's TCHAR), terminator included in Num.
    bool text(std::uint32_t string, std::string& out) const {
        std::uint32_t data = 0, count = 0;
        if (!array(string, kScratchBytes, 1, data, count)) return false;
        out.clear();
        if (!count) return true;
        if (host.MemoryRead8(data + count - 1)) return false;
        for (std::uint32_t i = 0; i + 1 < count; ++i) out += static_cast<char>(host.MemoryRead8(data + i));
        return true;
    }
};

bool Check(DynarmicHost& host) {
    auto& elf = host.GetElfLoader();
    g.base = elf.GetBaseAddress();
    if (elf.GetImageSize() != kImageBytes || host.FindSymbol("_ZN7FStringaSEPKc") != g.base + kStringAssign ||
        host.FindSymbol("_ZN7UObject11GObjObjectsE") != g.base + kGObjObjects ||
        host.FindSymbol("_ZN5FName5NamesE") != g.base + kNames ||
        !elf.IsCodeRange(g.base + kStringAssign, sizeof(kAssignWords))) return false;
    for (std::size_t i = 0; i < sizeof(kAssignWords) / sizeof(kAssignWords[0]); ++i)
        if (host.MemoryRead32(g.base + kStringAssign + static_cast<std::uint32_t>(i * 4)) != kAssignWords[i]) return false;
    g.scratch = host.GuestAlloc(kScratchBytes, 8);
    if (!g.scratch) return false;
#if HORN_ENABLE_LOGGING && defined(__ANDROID__)
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.bl2bridge.glyph_mode", value) > 0 && value[0] >= '0' && value[0] <= '2' && !value[1])
        g.mode = static_cast<GamepadGlyphHiding>(value[0] - '0');
#endif
    return true;
}

// The FName index of an exact ASCII name, or -1: one pass over FName::Names,
// so the object pass below compares two words per object instead of strings.
std::int64_t NameIndex(const Reader& read, const char* wanted) {
    std::uint32_t data = 0, count = 0;
    if (!read.array(g.base + kNames, 2000000, 4, data, count)) return -1;
    const auto length = std::strlen(wanted);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto entry = read.u32(data + i * 4);
        if (!entry || !read.span(entry, 8 + length + 1)) continue;
        std::size_t k = 0;
        while (k < length && read.host.MemoryRead8(entry + 8 + static_cast<std::uint32_t>(k)) == static_cast<std::uint8_t>(wanted[k])) ++k;
        if (k == length && !read.host.MemoryRead8(entry + 8 + static_cast<std::uint32_t>(length))) return i;
    }
    return -1;
}

// Every live UIDataStore_StringAliasMap (the instance and the class default
// objects): the MappedText strings that show a gamepad button image.
void Scan(DynarmicHost& host) {
    Reader read{host, g.base};
    g.entries.clear();
    std::uint32_t data = 0, count = 0;
    const auto storeName = NameIndex(read, "UIDataStore_StringAliasMap");
    if (storeName < 0 || !read.array(g.base + kGObjObjects, 2000000, 4, data, count)) return;
    std::uint32_t storeClass = 0;
    for (std::uint32_t i = 0; i < count && !storeClass; ++i) {
        const auto object = read.u32(data + i * 4);
        if (object && read.span(object, 0x48) && read.u32(object + 0x2C) == static_cast<std::uint32_t>(storeName) &&
            !read.u32(object + 0x30) && read.className(object) == "Class") storeClass = object;
    }
    if (!storeClass) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto object = read.u32(data + i * 4);
        if (!object || object == storeClass || !read.span(object, 0x48)) continue;
        const auto type = read.u32(object + 0x34);
        // The store class itself or a direct subclass (BL2's own
        // WillowUIDataStore_StringAliasMap): two words per object.
        if (!read.span(type, 0x48) || (type != storeClass && read.u32(type + 0x40) != storeClass) ||
            !read.derives(type, storeClass)) continue;
        const auto maps = read.property(type, "MenuInputMapArray", "ArrayProperty");
        if (!maps.field || maps.size != 12 || !read.token(maps.extra) || read.className(maps.extra) != "StructProperty" ||
            read.u32(maps.extra + 0x28) != maps.field) continue;
        const auto stride = read.u32(maps.extra + 0x44), structure = read.u32(maps.extra + 0x74);
        const auto mapped = read.property(structure, "MappedText", "StrProperty");
        if (read.className(structure) != "ScriptStruct" || read.objectName(structure) != "UIMenuInputMap" ||
            !mapped.field || mapped.size != 12 || !stride || stride > 256 || mapped.offset + 12 > stride) continue;
        std::uint32_t rows = 0, entries = 0;
        if (!read.array(object + maps.offset, 8192, stride, rows, entries)) continue;
        const auto token = read.token(object);
        for (std::uint32_t row = 0; row < entries; ++row) {
            Entry entry{object, rows + row * stride + mapped.offset, token, {}, {}};
            if (read.text(entry.string, entry.original) &&
                bl2::touch::Bl2HideGamepadMarkup(entry.original, g.mode, entry.hidden) &&
                entry.hidden.size() + 1 <= kScratchBytes) g.entries.push_back(entry);
        }
    }
}

// Assigns hidden or original markup through the game's FString::operator=.
// An entry the engine changed meanwhile adopts that value as its original.
unsigned Apply(DynarmicHost& host, bool hide, unsigned& stale) {
    Reader read{host, g.base};
    unsigned assigned = 0;
    stale = 0;
    for (auto& entry : g.entries) {
        std::string current;
        if (read.token(entry.object) != entry.token || !read.text(entry.string, current)) { ++stale; continue; }
        const auto& wanted = hide ? entry.hidden : entry.original;
        if (current == wanted) continue;
        if (current != entry.original && current != entry.hidden) { ++stale; continue; }
        for (std::size_t i = 0; i <= wanted.size(); ++i)
            host.MemoryWrite8(g.scratch + static_cast<std::uint32_t>(i), i < wanted.size() ? static_cast<std::uint8_t>(wanted[i]) : 0);
        if (bl2::touch::Bl2TouchCallGuest(host, g.base + kStringAssign, {entry.string, g.scratch})) ++assigned;
    }
    return assigned;
}
} // namespace

void Bl2TouchGamepadGlyphsTick(DynarmicHost& host) {
    if (g.status == State::Status::Off) return;
    const bool hide = bl2::touch::Bl2HideGamepadGlyphs();
    // Nothing to do until the player switches the option on for the first time.
    if (g.status == State::Status::Unchecked && !hide) return;
    if (g.status == State::Status::Unchecked) {
        g.status = Check(host) ? State::Status::Ready : State::Status::Off;
        HORN_LOGI(kTag, "glyph hiding %s mode=%u", g.status == State::Status::Ready ? "ready" : "off: image check failed",
            static_cast<unsigned>(g.mode));
        if (g.status != State::Status::Ready) return;
    }
    const auto now = bl2::touch::monotonicMilliseconds();
    // A changed choice applies at once; otherwise a cheap check every 2 s
    // re-applies after a config reload, and a missing store is looked for again.
    if (hide == g.hidden && now - g.checkedAt < 2000) return;
    g.checkedAt = now;
    if (hide && (!g.scanned || (g.entries.empty() && now - g.scannedAt >= 10000))) {
        Scan(host); g.scanned = true; g.scannedAt = now;
        HORN_LOGI(kTag, "found %zu gamepad image strings", g.entries.size());
    }
    unsigned stale = 0;
    const auto assigned = Apply(host, hide, stale);
    if (assigned) HORN_LOGI(kTag, "%s %u gamepad image strings (stale %u)", hide ? "hid" : "restored", assigned, stale);
    // Stale objects: the store was replaced; look for the new one next time.
    if (stale && hide) g.scanned = false;
    g.hidden = hide;
}
#endif
