#include "bl2_touch_widget_probe.h"
#include "bl2_touch_gfx_transport.h"
#include "bl2_touch_widget_registry.h"
#include "bl2_touch_physical.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <cstring>
#include <string>

namespace bl2::touch {
namespace {
bool copyMemory(const void* p, std::uint32_t at, void* out, std::size_t bytes) {
    const auto& host = *static_cast<const DynarmicHost*>(p);
    if (!host.GetGuestMemoryBase() || at < 0x10000 || at > host.GetGuestMemorySize() ||
        bytes > host.GetGuestMemorySize() - at) return false;
    std::memcpy(out,host.GetGuestMemoryBase() + at,bytes); return true;
}
[[maybe_unused]] const char* kindName(WidgetKind kind) {
    switch (kind) { case WidgetKind::Button: return "button"; case WidgetKind::Slider: return "slider";
        case WidgetKind::Spinner: return "spinner"; default: return "unsupported"; }
}
std::string singleLine(std::string text) {
    for (auto& c : text) if (static_cast<unsigned char>(c) < 32) c = ' ';
    return text;
}
}
struct Ue3WidgetProbe::State {
    DynarmicHost& host;
    LifetimeGeneration generation;
    const std::uint32_t base;
    GfxHostTransport transport;
    bool busy = false, active = false, stopped = false;
    std::uint32_t owner = 0, ownerClass = 0, list = 0, provider = 0, movie = 0, interface = 0;
    std::uint32_t viewport = 0, width = 0, height = 0, world = 0, gfx = 0, listOffset = 0;
    std::uint64_t ownerToken = 0, listToken = 0, providerToken = 0, epoch = 0, lifetime = 0;
    std::uint64_t nextRead = 0, nextCapture = 0, previousScene = 0;
    unsigned attempts = 0, logs = 0;
    std::string previousModel, previousResult;

    State(DynarmicHost& h, LifetimeGeneration g)
        : host(h), generation(g), base(h.GetElfLoader().GetBaseAddress()),
          transport(h,[this] { return current(); }) {}
    std::uint32_t read(std::uint32_t p) const {
        std::uint32_t value = 0; return copyMemory(&host,p,&value,4) ? value : 0;
    }
    std::uint64_t token(std::uint32_t p) const {
        const auto array = read(base + 0x02FBA268), count = read(base + 0x02FBA26C);
        const auto capacity = read(base + 0x02FBA270), index = read(p + 0x20);
        if (p < 0x10000 || count > capacity || capacity > 2000000 || index >= count ||
            std::uint64_t(array) + std::uint64_t(index) * 4 > 0xFFFFFFFCull ||
            read(array + index * 4) != p || (read(p + 0xC) & 0x20000002u)) return 0;
        return (std::uint64_t(p) << 32) | index;
    }
    bool current() const {
        if (!active || !generation || generation() != lifetime || !ownerToken || !listToken || !providerToken ||
            sharedMailbox().snapshot().epoch != epoch || physicalOwnership().ownsInput(monotonicMilliseconds()) ||
            token(owner) != ownerToken || token(list) != listToken || token(provider) != providerToken ||
            read(owner + 0x34) != ownerClass || read(owner + listOffset) != list ||
            read(base + 0x0300D940) != world || read(base + 0x0301F840) != gfx ||
            read(owner + 0x3C) != movie || read(movie + 0x64) != owner || read(movie + 0x34) != interface ||
            !read(movie + 0x4C) || !read(movie + 0x5C) ||
            read(viewport + 0x3C) != width || read(viewport + 0x40) != height || read(gfx + 0x44) != viewport + 4)
            return false;
        const auto focus = read(gfx + 0x28), count = read(gfx + 0x2C), capacity = read(gfx + 0x30);
        return count >= 1 && count <= capacity && capacity <= 4 && read(focus) == movie;
    }
    void probe(std::uint32_t completeViewport, std::uint32_t movieOwner, std::uint64_t scene) {
        if (busy || stopped || !generation || !movieOwner || !scene || attempts >= 120 || logs >= 48) return;
        const auto now = monotonicMilliseconds();
        if (now < nextRead) return;
        nextRead = now + 1000;
        if (sharedMailbox().snapshot().context.mode == Mode::TextInput || physicalOwnership().ownsInput(now)) return;
        const auto model = CaptureBl2UiModel({&host,copyMemory},base,movieOwner);
        if (!model.valid || !model.list || !model.provider) return;
        const auto description = std::to_string(scene) + '/' + std::to_string(model.structure) + '/' +
            std::to_string(model.selectedIndex);
        if (description == previousModel && scene == previousScene && now < nextCapture) return;
        previousModel = description; previousScene = scene; nextCapture = now + 10000;
        ++attempts;
        listOffset = 0;
        for (const auto& p : model.properties) if (p.name == "TheList" && p.elementSize == 4) listOffset = p.offset;
        if (!listOffset || listOffset > 0x10000) return;
        owner = movieOwner; ownerToken = model.owner; ownerClass = read(owner + 0x34);
        list = static_cast<std::uint32_t>(model.list >> 32); listToken = model.list;
        provider = static_cast<std::uint32_t>(model.provider >> 32); providerToken = model.provider;
        movie = read(owner + 0x3C); interface = read(movie + 0x34);
        viewport = completeViewport; width = read(viewport + 0x3C); height = read(viewport + 0x40);
        world = read(base + 0x0300D940); gfx = read(base + 0x0301F840);
        lifetime = generation(); epoch = sharedMailbox().snapshot().epoch;
        if (!width || !height || width > 16384 || height > 16384) return;
        busy = active = true;
        struct Guard { State& state; ~Guard() { state.active = state.busy = false; } } guard{*this};
        GfxSession session(transport,base,interface,transport.scratch(),768,false);
        auto widgets = CaptureBl2Widgets(session,model,width,height);
        if (widgets.valid) {
            const auto after = CaptureBl2UiModel({&host,copyMemory},base,owner);
            if (!after.valid || after.structure != model.structure || after.owner != model.owner ||
                after.list != model.list || after.provider != model.provider || !current()) {
                widgets.valid = false; widgets.reason = "native-model-changed-during-capture";
            }
        }
        session.close();
        if (!session.cleanupOkay()) { stopped = true; widgets.valid = false; widgets.reason = "managed-cleanup-fault-disabled"; }
        const auto result = description + '/' + widgets.reason + '/' + std::to_string(widgets.content);
        if (result == previousResult) return;
        previousResult = result; ++logs;
        HORN_LOGI("BL2Bridge", "ui-widgets valid=%u reason=%s session=%s movie=%s owner=%08X scene=%llu "
            "route=%llu content=%llu calls=%u cleanup=%u bytes=%zu listType=%u dataType=%u "
            "frame=(%.3f,%.3f,%.3f,%.3f) viewport=(%u,%u,%u,%u,%u,%u) rows=%zu scroll=%d renderers=%d",
            widgets.valid ? 1u : 0u,widgets.reason.c_str(),session.reason() ? session.reason() : "okay",model.movieClass.c_str(),owner,
            static_cast<unsigned long long>(scene),static_cast<unsigned long long>(widgets.route),
            static_cast<unsigned long long>(widgets.content),session.calls(),session.cleanupCalls(),session.bytesRead(),
            widgets.list.type,widgets.data.type,widgets.frame.left,widgets.frame.top,widgets.frame.right,widgets.frame.bottom,
            widgets.viewport.words[0],widgets.viewport.words[1],widgets.viewport.words[2],widgets.viewport.words[3],
            widgets.viewport.words[4],widgets.viewport.words[5],widgets.rows.size(),widgets.scrollPosition,widgets.totalRenderers);
        for (const auto& row : widgets.rows) {
            const auto caption = singleLine(row.caption);
            HORN_LOGI("BL2Bridge", "ui-widget index=%d event=%d id=%llu kind=%s enabled=%u visible=%u "
                "caption='%s' rect=(%.6f,%.6f,%.6f,%.6f) slider=(%.3f,%.3f,%.3f,%.3f) "
                "axis=(%.6f,%.6f,%.6f,%.6f)",row.index,row.eventId,
                static_cast<unsigned long long>(row.id),kindName(row.kind),row.enabled ? 1u : 0u,row.visible ? 1u : 0u,
                caption.c_str(),row.bounds.left,row.bounds.top,row.bounds.right,row.bounds.bottom,
                row.minimum,row.maximum,row.increment,row.value,row.sliderStart.x,row.sliderStart.y,row.sliderEnd.x,row.sliderEnd.y);
        }
    }
};
Ue3WidgetProbe::Ue3WidgetProbe(DynarmicHost& host, LifetimeGeneration generation)
    : state_(std::make_unique<State>(host,generation)) {}
Ue3WidgetProbe::~Ue3WidgetProbe() = default;
void Ue3WidgetProbe::capture(std::uint32_t viewport, std::uint32_t owner, std::uint64_t scene) {
    state_->probe(viewport,owner,scene);
}
}
