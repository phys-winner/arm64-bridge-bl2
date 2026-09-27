#include "bl2_touch_widget_sink.h"
#include "bl2_touch_widget_router.h"
#include "bl2_touch_gfx_transport.h"
#include "bl2_touch_ui_sink.h"
#include "bl2_touch_physical.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <cstring>
#include <utility>

namespace bl2::touch {
namespace {
bool copyMemory(const void* p, std::uint32_t at, void* out, std::size_t bytes) {
    const auto& host = *static_cast<const DynarmicHost*>(p);
    if (!out || !host.GetGuestMemoryBase() || at < 0x10000 || at > host.GetGuestMemorySize() ||
        bytes > host.GetGuestMemorySize() - at) return false;
    std::memcpy(out,host.GetGuestMemoryBase() + at,bytes); return true;
}
struct Reader {
    DynarmicHost& host;
    const std::uint32_t base;
    explicit Reader(DynarmicHost& h) : host(h), base(h.GetElfLoader().GetBaseAddress()) {}
    std::uint32_t read(std::uint32_t p) const {
        std::uint32_t value = 0; return copyMemory(&host,p,&value,4) ? value : 0;
    }
    bool array(std::uint32_t p, std::uint32_t limit, std::uint32_t& data, std::uint32_t& count) const {
        const auto capacity = read(p + 8); data = read(p); count = read(p + 4);
        return count <= capacity && capacity <= limit && (!count || (data >= 0x10000 &&
            std::uint64_t(data) + std::uint64_t(count) * 4 <= host.GetGuestMemorySize()));
    }
    std::uint64_t token(std::uint32_t p) const {
        if (p < 0x10000 || std::uint64_t(p) + 0x38 > host.GetGuestMemorySize()) return 0;
        std::uint32_t data = 0, count = 0;
        if (!array(base + 0x02FBA268,2000000,data,count)) return 0;
        const auto index = read(p + 0x20);
        if (index >= count || read(data + index * 4) != p || (read(p + 0xC) & 0x20000002u)) return 0;
        return (std::uint64_t(p) << 32) | index;
    }
    std::string className(std::uint32_t p) const {
        if (!token(p)) return {};
        const auto cls = read(p + 0x34);
        if (!token(cls) || read(cls + 0x30)) return {};
        std::uint32_t data = 0, count = 0;
        if (!array(base + 0x02FB00C8,2000000,data,count)) return {};
        const auto index = read(cls + 0x2C);
        if (index >= count) return {};
        const auto entry = read(data + index * 4);
        std::string name;
        for (std::uint32_t i = 0; i < 128; ++i) {
            unsigned char c = 0;
            if (std::uint64_t(entry) + 8 + i > 0xFFFFFFFFull || !copyMemory(&host,entry + 8 + i,&c,1)) return {};
            if (!c) return name;
            if (c < 32 || c > 126) return {};
            name += static_cast<char>(c);
        }
        return {};
    }
};
struct Prepared {
    std::uint32_t viewport = 0, controller = 0, movie = 0, owner = 0;
    std::uint64_t scene = 0;
    bool widgets = false;
};
// Native reads here are the same exact-image seams proved by stage1/stage3.
// TheList offset alone is resolved from the live UProperty graph, never guessed.
class NativeScene final : public WidgetSceneLease, public std::enable_shared_from_this<NativeScene> {
public:
    NativeScene(DynarmicHost& host, Ue3WidgetSink::LifetimeGeneration generation,
                const Prepared& source, const Frame& frame)
        : read_(host), generation_(generation), source_(source), model_(CaptureBl2UiModel({&host,copyMemory},read_.base,source.owner)) {
        captured_.epoch = frame.epoch; captured_.scene = frame.context.sceneIdentity;
        captured_.width = read_.read(source.viewport + 0x3C); captured_.height = read_.read(source.viewport + 0x40);
        lifetime_ = generation_ ? generation_() : 0;
        engine_ = read_.read(read_.base + 0x03008D20); engineToken_ = read_.token(engine_);
        world_ = read_.read(read_.base + 0x0300D940); gfx_ = read_.read(read_.base + 0x0301F840);
        std::uint32_t players = 0, count = 0;
        if (engineToken_ && read_.array(engine_ + 0x48C,4,players,count) && count == 1) local_ = read_.read(players);
        localToken_ = read_.token(local_); controllerToken_ = read_.token(source.controller);
        captured_.controllerId = read_.read(local_ + 0x64);
        ownerClass_ = read_.read(source.owner + 0x34);
        interface_ = read_.read(source.movie + 0x34);
        for (const auto& property : model_.properties)
            if (property.name == "TheList" && property.elementSize == 4) listOffset_ = property.offset;
    }
    const WidgetSceneInfo& info() const override { return captured_; }
    bool current() const override {
        if (!source_.widgets || !generation_ || generation_() != lifetime_ || !lifetime_ || !model_.valid ||
            !model_.owner || !model_.list || !model_.provider || !listOffset_ || listOffset_ > 0x10000 ||
            !engineToken_ || !localToken_ || !controllerToken_ || captured_.controllerId > 3 ||
            !captured_.width || !captured_.height || captured_.width > 16384 || captured_.height > 16384 ||
            physicalOwnership().ownsInput(monotonicMilliseconds())) return false;
        const auto snapshot = sharedMailbox().snapshot();
        if (snapshot.epoch != captured_.epoch || snapshot.context.sceneIdentity != captured_.scene ||
            !(snapshot.context.capabilities & kUiWidgetsTestCapability) ||
            (snapshot.context.capabilities & (PhysicalOwnsInput | GameplayInput)) ||
            (snapshot.context.mode != Mode::Frontend && snapshot.context.mode != Mode::Menu)) return false;
        const auto list = static_cast<std::uint32_t>(model_.list >> 32);
        const auto provider = static_cast<std::uint32_t>(model_.provider >> 32);
        if (read_.token(source_.owner) != model_.owner || read_.token(list) != model_.list ||
            read_.token(provider) != model_.provider || read_.token(source_.controller) != controllerToken_ ||
            read_.token(engine_) != engineToken_ || read_.token(local_) != localToken_ ||
            read_.read(read_.base + 0x03008D20) != engine_ || read_.read(read_.base + 0x0300D940) != world_ ||
            read_.read(read_.base + 0x0301F840) != gfx_ || read_.read(source_.owner + 0x34) != ownerClass_ ||
            read_.read(source_.owner + listOffset_) != list ||
            read_.read(local_ + 0x40) != source_.controller || read_.read(local_ + 0x64) != captured_.controllerId ||
            read_.read(source_.owner + 0x3C) != source_.movie || read_.read(source_.movie + 0x64) != source_.owner ||
            read_.read(source_.movie + 0x34) != interface_ || !read_.read(source_.movie + 0x4C) ||
            !read_.read(source_.movie + 0x5C) || read_.read(gfx_ + 0x44) != source_.viewport + 4 ||
            read_.read(source_.viewport + 0x3C) != captured_.width || read_.read(source_.viewport + 0x40) != captured_.height)
            return false;
        std::uint32_t data = 0, count = 0;
        if (!read_.array(engine_ + 0x48C,4,data,count) || count != 1 || read_.read(data) != local_) return false;
        return read_.array(gfx_ + 0x28,4,data,count) && count >= 1 && read_.read(data) == source_.movie;
    }
    UiModelSnapshot readModel() const override {
        if (!current()) return {};
        auto model = CaptureBl2UiModel({&read_.host,copyMemory},read_.base,source_.owner);
        if (!current() || !SameWidgetNativeModel(model_,model)) return {};
        return model;
    }
    std::unique_ptr<WidgetSessionScope> open(std::uint32_t calls, bool mutation) override {
        if (!current()) return {};
        auto transport = std::make_unique<GfxHostTransport>(read_.host,[self = shared_from_this()] { return self->current(); });
        const auto scratch = transport->scratch();
        return std::make_unique<WidgetSessionScope>(std::move(transport),read_.base,interface_,scratch,calls,mutation);
    }
private:
    const Reader read_;
    const Ue3WidgetSink::LifetimeGeneration generation_;
    const Prepared source_;
    const UiModelSnapshot model_;
    WidgetSceneInfo captured_;
    std::uint64_t lifetime_ = 0, engineToken_ = 0, localToken_ = 0, controllerToken_ = 0;
    std::uint32_t engine_ = 0, local_ = 0, world_ = 0, gfx_ = 0, ownerClass_ = 0, interface_ = 0, listOffset_ = 0;
};
}
struct Ue3WidgetSink::State {
    DynarmicHost& host;
    const LifetimeGeneration generation;
    Ue3UiSink navigation;
    Prepared prepared;
    WidgetRouter router;
    bool signatures = false;
    unsigned logs = 0;
    std::string lastLog;
    State(DynarmicHost& h, LifetimeGeneration g)
        : host(h), generation(g), navigation(h,g,false), router([this](const Frame& frame) -> std::shared_ptr<WidgetSceneLease> {
            if (!signatures || !prepared.widgets || frame.context.sceneIdentity != prepared.scene) return {};
            auto scene = std::make_shared<NativeScene>(host,generation,prepared,frame);
            return scene->current() ? scene : nullptr;
        }) {
        GfxHostTransport verification(host,[] { return false; }); // No allocation or callbacks.
        signatures = navigation.verified() && GfxSession::verifyAbi(verification,verification.base());
    }
};
Ue3WidgetSink::Ue3WidgetSink(DynarmicHost& host, LifetimeGeneration generation)
    : state_(std::make_unique<State>(host,generation)) {}
Ue3WidgetSink::~Ue3WidgetSink() = default;
bool Ue3WidgetSink::verified() const { return state_->signatures; }
std::uint32_t Ue3WidgetSink::prepare(std::uint32_t viewport, std::uint32_t controller,
    std::uint32_t movie, std::uint32_t owner, std::uint64_t scene) {
    auto& s = *state_;
    const auto caps = s.navigation.prepare(viewport,controller,movie,owner,scene);
    s.prepared = {viewport,controller,movie,owner,scene,false};
    if (!caps || !s.signatures || s.router.stopped()) return caps;
    const Reader reader(s.host);
    const auto name = reader.className(owner);
    s.prepared.widgets = (name == "FrontendGFxMovie" || name == "PauseGFxMovie" || name == "OptionsGFxMovie") &&
        reader.read(reader.read(reader.base + 0x0301F840) + 0x44) == viewport + 4;
    return caps | (s.prepared.widgets ? kUiWidgetsTestCapability : 0u);
}
void Ue3WidgetSink::beginTouchFrame(const Frame& frame) {
    state_->router.beginTouchFrame(frame); state_->navigation.beginTouchFrame(frame);
}
void Ue3WidgetSink::action(const ActionEdge& edge) {
    state_->router.action(edge); state_->navigation.action(edge);
}
void Ue3WidgetSink::pointer(const PointerEvent& event) { state_->router.pointer(event); }
void Ue3WidgetSink::endTouchFrame(const Frame& frame) {
    auto& s = *state_; s.router.endTouchFrame(frame); s.navigation.endTouchFrame(frame);
    const auto description = s.router.reason() + '/' + std::to_string(s.router.dispatched()) + '/' +
        std::to_string(s.router.captured()) + '/' + std::to_string(s.router.stopped());
    if (description == s.lastLog || s.logs >= 64) return;
    s.lastLog = description; ++s.logs;
    HORN_LOGI("BL2Bridge","native-widgets reason=%s dispatched=%llu captured=%u stopped=%u movie=%08X epoch=%llu",
        s.router.reason().c_str(),static_cast<unsigned long long>(s.router.dispatched()),s.router.captured() ? 1u : 0u,
        s.router.stopped() ? 1u : 0u,s.prepared.movie,static_cast<unsigned long long>(frame.epoch));
}
}
