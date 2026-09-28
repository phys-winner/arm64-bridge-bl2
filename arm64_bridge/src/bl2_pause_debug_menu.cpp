#include "bl2_pause_debug_menu.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "obb_sha256.h"
#include "thunk_manager.h"
#include <cstring>
#include <string>

namespace {
constexpr char kTag[] = "Arm32Bridge_PauseDebug";
constexpr char kSha[] = "A0B06EF59F4C89C0889654196951D2DEB966CFEAC7180699B5724F0E26938814";
constexpr std::size_t kFileBytes = 49885152;
constexpr std::uint32_t kImageBytes = 0x03089000;
constexpr std::uint32_t kSerialize = 0x00F6A190;
constexpr std::uint32_t kReturn = 0x00F6A2E0;
constexpr std::uint32_t kObjects = 0x02FBA268;
constexpr std::uint32_t kNames = 0x02FB00C8;
bool verified = false;
std::uint32_t installedVeneer = 0;
std::uint32_t installedSvc = 0;

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
    DynarmicHost& host;
    std::uint32_t base;
};

void afterSerialize(Dynarmic::A32::Jit& jit, DynarmicHost& host) {
    Reader r(host);
    // This exact normal epilogue still holds this in R4 and archive in R5.
    // No guest callback or processor/register change is needed.
    const auto function = jit.Regs()[4], archive = jit.Regs()[5];
    if (!r.span(archive, 0x18) || !r.u32(archive + 0x14) ||
        !r.span(function, 0xA0) || r.u32(function + 0x50) != 1091) return;
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
    if (jump == 0xE6) {
        host.MemoryWrite8(gate + 1, 0x8B);
        HORN_LOGI(kTag, "pause Debug event3 visible: scriptBytes=1091 jump03E6->038B; manager untouched");
    }
}
} // namespace

bool VerifyBl2PauseDebugMenuInput(const std::uint8_t* bytes, std::size_t size) {
    verified = false;
    if (!bytes || size != kFileBytes) return false;
    ObbSha256 sha;
    if (!sha.Update(bytes, size)) return false;
    const auto hex = ObbSha256::Hex(sha.Finalize());
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
        host.MemoryRead32(installedVeneer) == installedSvc) return true;
    constexpr std::uint32_t prologue[] = {0xE92D4070,0xE1A05001,0xE5916014,0xE24DD008,0xE1A04000};
    for (std::uint32_t i = 0; i != 5; ++i)
        if (host.MemoryRead32(base + kSerialize + i * 4) != prologue[i]) return false;
    if (host.MemoryRead32(site) != 0xE28DD008 || host.MemoryRead32(site + 4) != 0xE8BD8070) return false;
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
    host.MemoryWrite32(site, 0xE51FF004);
    host.MemoryWrite32(site + 4, veneer);
    host.InvalidateGuestCode(site, 8);
    installedVeneer = veneer;
    installedSvc = svc;
    HORN_LOGI(kTag, "pause visibility hook installed at UFunction::Serialize normal epilogue; no cheat actions");
    return true;
}
