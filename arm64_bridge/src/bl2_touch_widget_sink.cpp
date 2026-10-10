#include "bl2_touch_widget_sink.h"
#include "bl2_touch_widget_router.h"
#include "bl2_touch_gfx_transport.h"
#include "bl2_touch_ui_sink.h"
#include "bl2_touch_physical.h"
#include "bl2_touch_settings.h"
#include "bl2_pause_debug_menu.h"
#include "bl2_locale.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <array>
#include <cstring>
#include <optional>
#include <utility>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace bl2::touch {
namespace {
bool copyMemory(const void* p, std::uint32_t at, void* out, std::size_t bytes) {
    const auto& host = *static_cast<const DynarmicHost*>(p);
#if defined(BL2_NATIVE_ARM32)
    if (!out || at < 0x10000 || at > host.GetGuestMemorySize() ||
        bytes > host.GetGuestMemorySize() - at) return false;
    std::memcpy(out,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(at)),bytes);
#else
    if (!out || !host.GetGuestMemoryBase() || at < 0x10000 || at > host.GetGuestMemorySize() ||
        bytes > host.GetGuestMemorySize() - at) return false;
    std::memcpy(out,host.GetGuestMemoryBase() + at,bytes);
#endif
    return true;
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
    bool debug = false;
};
// Visual ownership is independent of input focus. Only an actually registered
// original Pause movie may extend its scrim. Resume separately validates the
// fresh focused Options/Pause because Options can remove the previous Pause.
class PauseBackdropUi {
public:
    struct Observation {
        const char* prepare="unobserved", *current="unobserved";
        std::string model,world;
        std::uint64_t actual=0,expected=0,lifetime=0,ownerToken=0,listToken=0,engineToken=0,localToken=0,controllerToken=0;
        std::uint32_t movieCount=0,players=0,liveMovies=0,owner=0,movie=0,listOffset=0,interface=0;
        std::array<std::uint32_t,4> candidates{},owners{};
        std::array<std::string,4> classes{};
    };
    static bool diagnosticsEnabled() {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING && defined(__ANDROID__)
        static const bool enabled=[] {
            char property[PROP_VALUE_MAX]{};__system_property_get("debug.bl2bridge.touch_diagnostics",property);
            return property[0]=='1' && !property[1];
        }();
        return enabled;
#else
        return false;
#endif
    }
    UiPauseResumeIdentity resumeIdentity(const Prepared& focused,Observation* observation=nullptr) const {
        if(observation) { *observation={};observation->prepare=preparing_.prepare; }
        const auto reject=[&](const char* why) { if(observation) observation->current=why;return UiPauseResumeIdentity{}; };
        const auto lifetime=generation_?generation_():0;
        const auto engine=read_.read(read_.base+0x03008D20),gfx=read_.read(read_.base+0x0301F840),
            nativeWorld=read_.read(read_.base+0x0300D940);
        const auto engineToken=read_.token(engine),controllerToken=read_.token(focused.controller),ownerToken=read_.token(focused.owner);
        if(!lifetime || !engineToken || !controllerToken || !ownerToken || !nativeWorld ||
            !focused.viewport || !focused.movie || read_.read(gfx+0x44)!=focused.viewport+4) return reject("focused-game-source");
        std::uint32_t data=0,count=0;
        if(!read_.array(engine+0x48C,4,data,count) || count!=1) return reject("focused-player-array");
        const auto local=read_.read(data);const auto localToken=read_.token(local);
        if(!localToken || read_.read(local+0x40)!=focused.controller) return reject("focused-local-controller");
        if(!read_.array(gfx+0x28,4,data,count) || !count || read_.read(data)!=focused.movie) return reject("focused-movie-array");
        const auto name=read_.className(focused.owner);
        if(name!="OptionsGFxMovie" && name!="PauseGFxMovie") return reject("focused-movie-class");
        const auto interface=read_.read(focused.movie+0x34),width=read_.read(focused.viewport+0x3C),height=read_.read(focused.viewport+0x40);
        if(!interface || !width || !height || width>16384 || height>16384 ||
            read_.read(focused.owner+0x3C)!=focused.movie || read_.read(focused.movie+0x64)!=focused.owner ||
            !read_.read(focused.movie+0x4C) || !read_.read(focused.movie+0x5C)) return reject("focused-movie-backlinks");
        const auto model=CaptureBl2UiModel({&read_.host,copyMemory},read_.base,focused.owner,false);
        if(observation) observation->model=model.reason;
        if(!model.valid || model.owner!=ownerToken || model.movieClass!=name || !model.list) return reject("focused-native-model");
        std::uint32_t offset=0;
        for(const auto& property:model.properties) if(property.name=="TheList" && property.elementSize==4) offset=property.offset;
        if(!offset || offset>0x10000 || read_.read(focused.owner+offset)!=static_cast<std::uint32_t>(model.list>>32) ||
            read_.token(static_cast<std::uint32_t>(model.list>>32))!=model.list) return reject("focused-native-list");
        UiPauseWorldIdentity world;
        if(!Bl2TouchPausedWorld({&read_.host,copyMemory},read_.base,focused.controller,
            name=="PauseGFxMovie"?focused.owner:0,observation?&observation->world:nullptr,&world)) return reject("focused-paused-world");
        if(generation_()!=lifetime || read_.token(focused.owner)!=ownerToken || read_.token(engine)!=engineToken ||
            read_.token(local)!=localToken || read_.token(focused.controller)!=controllerToken ||
            read_.read(read_.base+0x03008D20)!=engine || read_.read(read_.base+0x0300D940)!=nativeWorld ||
            read_.read(read_.base+0x0301F840)!=gfx) return reject("focused-recheck");
        if(observation) {
            observation->current="focused-current";observation->lifetime=lifetime;observation->movieCount=count;
            observation->ownerToken=ownerToken;observation->listToken=model.list;observation->engineToken=engineToken;
            observation->localToken=localToken;observation->controllerToken=controllerToken;observation->owner=focused.owner;
            observation->movie=focused.movie;observation->listOffset=offset;observation->interface=interface;
        }
        return {engineToken,localToken,world.controller,world.pawn,world.world,ownerToken,model.list,
            focused.viewport,focused.movie,interface,nativeWorld,width,height};
    }
    PauseBackdropUi(DynarmicHost& host, Ue3WidgetSink::LifetimeGeneration generation)
        : read_(host), generation_(generation) {}
    void prepare(std::uint32_t viewport, std::uint32_t controller) {
        Observation* observation=diagnosticsEnabled()?&preparing_:nullptr;
        if(observation) *observation={};
        const auto stage=[&](const char* reason) { if(observation) observation->prepare=reason; };
        const auto gfx = read_.read(read_.base + 0x0301F840);
        std::uint32_t movies = 0, count = 0, movie = 0, owner = 0;
        if (!generation_ || !viewport || !controller || read_.read(gfx + 0x44) != viewport + 4 ||
            !read_.array(gfx + 0x28,4,movies,count)) { stage("viewport-movie-array");fitted_ = false; return; }
        if(observation) observation->movieCount=count;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto candidate = read_.read(movies + i * 4), object = read_.read(candidate + 0x64);
            const auto name=read_.className(object);
            if(observation) { observation->candidates[i]=candidate;observation->owners[i]=object;observation->classes[i]=name; }
            if (name != "PauseGFxMovie") continue;
            if (movie) { stage("duplicate-pause");fitted_ = false; return; } // Ambiguous duplicate owners.
            movie = candidate; owner = object;
        }
        const auto lifetime = generation_(), token = read_.token(owner);
        const auto width = read_.read(viewport + 0x3C), height = read_.read(viewport + 0x40);
        if (!movie || !token || !lifetime || !width || !height || width > 16384 || height > 16384) {
            stage(movie?"owner-lifetime-dimensions":"no-pause-movie");fitted_ = false; return;
        }
        const auto focus = count ? read_.read(movies) : 0;
        const bool same = source_.movie == movie && ownerToken_ == token && focusedMovie_ == focus && source_.controller == controller &&
            source_.viewport == viewport && lifetime_ == lifetime && width_ == width && height_ == height;
        if (same && (fitted_ || monotonicMilliseconds() < nextAttempt_)) { stage(fitted_?"same-fitted":"same-throttled");return; }
        nextAttempt_ = monotonicMilliseconds() + 100; fitted_ = false;
        source_ = {viewport,controller,movie,owner,0,false}; lifetime_ = lifetime; ownerToken_ = token;
        width_ = width; height_ = height; gfx_ = gfx; focusedMovie_ = focus;
        engine_ = read_.read(read_.base + 0x03008D20); engineToken_ = read_.token(engine_);
        world_ = read_.read(read_.base + 0x0300D940); controllerToken_ = read_.token(controller);
        ownerClass_ = read_.read(owner + 0x34); interface_ = read_.read(movie + 0x34);
        std::uint32_t players = 0, playerCount = 0;
        local_ = read_.array(engine_ + 0x48C,4,players,playerCount) && playerCount == 1 ? read_.read(players) : 0;
        localToken_ = read_.token(local_);
        const auto model = CaptureBl2UiModel({&read_.host,copyMemory},read_.base,owner,false);
        if(observation) modelReason_=model.reason;
        if (!model.valid || model.movieClass != "PauseGFxMovie" || !model.list || model.owner != token) { stage("native-model");return; }
        listToken_ = model.list; listOffset_ = 0;
        for (const auto& property : model.properties)
            if (property.name == "TheList" && property.elementSize == 4) listOffset_ = property.offset;
        if (!current(true,observation)) { stage("visual-current");return; }
        stage("fit-scrim");
        GfxHostTransport transport(read_.host,[this] { return current(); });
        GfxSession session(transport,read_.base,interface_,transport.scratch(),256,true);
        const auto list = session.pin(static_cast<std::uint32_t>(listToken_ >> 32) + 0x3C);
        GfxRect visible, before, after;
        fitted_ = list && FitBl2PauseBackdrop(session,list,visible,before,after);
#if HORN_ENABLE_LOGGING
        if (logs_ < 16) {
            ++logs_;
            HORN_LOGI("BL2TouchDiagnostics","pause native scrim fit=%u movie=%08X visible=(%.2f,%.2f,%.2f,%.2f) "
                "before=(%.2f,%.2f,%.2f,%.2f) after=(%.2f,%.2f,%.2f,%.2f) calls=%u reason=%s",
                fitted_ ? 1u : 0u,movie,visible.left,visible.top,visible.right,visible.bottom,
                before.left,before.top,before.right,before.bottom,after.left,after.top,after.right,after.bottom,
                session.calls(),session.reason() ? session.reason() : "okay");
        }
#endif
        session.release(list); session.close();
        if (!session.cleanupOkay()) fitted_ = false;
    }
private:
    bool current(bool requireVisible = true,Observation* observation=nullptr) const {
        const auto list = static_cast<std::uint32_t>(listToken_ >> 32);
        const auto reject=[&](const char* reason) { if(observation) observation->current=reason;return false; };
        const auto equal=[&](std::uint64_t actual,std::uint64_t expected,const char* reason) {
            if(actual==expected) return true;
            if(observation) { observation->current=reason;observation->actual=actual;observation->expected=expected; }
            return false;
        };
        if (!generation_) return reject("generation-callback");
        if (!equal(generation_(),lifetime_,"generation")) return false;
        if (!engineToken_ || !localToken_ || !controllerToken_ ||
            !ownerToken_ || !listToken_ || !listOffset_ || listOffset_ > 0x10000 || !interface_) return reject("cached-source");
        if (!equal(read_.token(engine_),engineToken_,"engine-token") || !equal(read_.token(local_),localToken_,"local-token") ||
            !equal(read_.token(source_.controller),controllerToken_,"controller-token") || !equal(read_.token(source_.owner),ownerToken_,"owner-token") ||
            !equal(read_.token(list),listToken_,"list-token") || !equal(read_.read(read_.base + 0x03008D20),engine_,"engine-global") ||
            !equal(read_.read(read_.base + 0x0300D940),world_,"world-global") || !equal(read_.read(read_.base + 0x0301F840),gfx_,"gfx-global") ||
            !equal(read_.read(local_ + 0x40),source_.controller,"local-controller") || !equal(read_.read(source_.owner + 0x34),ownerClass_,"owner-class") ||
            !equal(read_.read(source_.owner + listOffset_),list,"owner-list") || !equal(read_.read(source_.owner + 0x3C),source_.movie,"owner-movie") ||
            !equal(read_.read(source_.movie + 0x64),source_.owner,"movie-owner") || !equal(read_.read(source_.movie + 0x34),interface_,"movie-interface") ||
            (requireVisible && (!read_.read(source_.movie + 0x4C) || !read_.read(source_.movie + 0x5C))) ||
            !equal(read_.read(gfx_ + 0x44),source_.viewport + 4,"gfx-viewport") || !equal(read_.read(source_.viewport + 0x3C),width_,"viewport-width") ||
            !equal(read_.read(source_.viewport + 0x40),height_,"viewport-height")) {
            if(observation && std::strcmp(observation->current,"unobserved")==0) observation->current="movie-visible";
            return false;
        }
        std::uint32_t data = 0, count = 0;
        const bool players=read_.array(engine_ + 0x48C,4,data,count);
        if(observation) observation->players=count;
        if (!players || count != 1 || !equal(read_.read(data),local_,"player-local")) return reject("player-array");
        const bool movies=read_.array(gfx_ + 0x28,4,data,count);
        if(observation) observation->liveMovies=count;
        if(!movies) return reject("live-movie-array");
        bool member = false;
        for (std::uint32_t i = 0; i < count; ++i) if (read_.read(data + i * 4) == source_.movie) member = true;
        if(!member) return reject("pause-not-in-movie-array");
        if(!Bl2TouchPausedWorld({&read_.host,copyMemory},read_.base,source_.controller,source_.owner,
                               observation?&observation->world:nullptr)) return reject("paused-world");
        if(observation) observation->current="current";
        return true;
    }
    Reader read_; Ue3WidgetSink::LifetimeGeneration generation_;
    Prepared source_;
    std::uint64_t lifetime_ = 0, ownerToken_ = 0, listToken_ = 0, engineToken_ = 0, localToken_ = 0, controllerToken_ = 0;
    std::uint64_t nextAttempt_ = 0;
    std::uint32_t width_ = 0, height_ = 0, gfx_ = 0, focusedMovie_ = 0, engine_ = 0, local_ = 0, world_ = 0, ownerClass_ = 0, interface_ = 0, listOffset_ = 0;
    bool fitted_ = false;
    Observation preparing_;
    std::string modelReason_;
#if HORN_ENABLE_LOGGING
    unsigned logs_ = 0;
#endif
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

// The shipped options list renders these rows. Native provider arrays and
// UObject ownership stay unchanged; custom rows use host preference handlers.
// Original AS data/listeners remain pinned and are restored on Back/ownership.
class SettingsUi {
public:
    SettingsUi(DynarmicHost& host, Ue3WidgetSink::LifetimeGeneration generation)
        : read_(host), generation_(generation) {}
    ~SettingsUi() { drop(false); }
    bool page() const { return bool(values_) && page_; }
    bool takeEditResume() { const bool requested = editResume_;editResume_ = false;return requested; }
    bool takeNativeBack() { const bool requested = nativeBack_ && current() && !page_;nativeBack_ = false;return requested; }
    void inputCompleted() { if (values_) { scalePolls_ = 4; nextScaleAt_ = 0; } }
    bool current() const {
        if ((!values_ && !opening_) || !generation_ || generation_() != lifetime_ ||
            read_.token(source_.owner) != original_.owner ||
            read_.token(static_cast<std::uint32_t>(original_.list >> 32)) != original_.list ||
            read_.token(static_cast<std::uint32_t>(original_.provider >> 32)) != original_.provider ||
            read_.token(static_cast<std::uint32_t>(original_.options >> 32)) != original_.options ||
            read_.read(source_.owner + 0x3C) != source_.movie ||
            read_.read(source_.movie + 0x64) != source_.owner) return false;
        const auto gfx = read_.read(read_.base + 0x0301F840);
        std::uint32_t data = 0, count = 0;
        return read_.array(gfx + 0x28,4,data,count) && count && read_.read(data) == source_.movie &&
            read_.read(gfx + 0x44) == source_.viewport + 4;
    }
    void prepare(const Prepared& source, bool pointerCaptured, bool scrolling = false) {
        if (values_) {
            values_->beginInputBoundary();
            if (!current() || source.owner != source_.owner || source.movie != source_.movie) drop(false);
            else if (physicalOwnership().ownsInput(monotonicMilliseconds())) { drop(true); return; }
            else {
                auto data = values_->member(list_,"data"); GfxIdentity identity;
                const bool same = data && values_->identity(data,identity) && identity == dataIdentity_;
                values_->release(data);
                if (!same) drop(false);
            }
        }
        const auto movieClass = read_.className(source.owner);
        if (movieClass != "OptionsGFxMovie" && movieClass != "FrontendGFxMovie" && movieClass != "PauseGFxMovie") return;
        if (!values_) {
            const auto model = CaptureBl2UiModel({&read_.host,copyMemory},read_.base,source.owner,false);
            if (!model.valid || model.movieClass != movieClass || !model.list || !model.provider) return;
            if (movieClass == "OptionsGFxMovie") {
                std::lock_guard<std::mutex> lock(touchSettingsState.mutex); if (!touchSettingsState.initialized) return;
            }
            source_ = source; original_ = model; lifetime_ = generation_ ? generation_() : 0;
            transport_ = std::make_unique<GfxHostTransport>(read_.host,[this] { return current(); });
            const auto scratch = transport_->scratch();
            // current() must be available during the session's Movie AddRef.
            opening_ = true;
            values_ = std::make_unique<GfxSession>(*transport_,read_.base,read_.read(source.movie + 0x34),scratch,1024,true);
            opening_ = false;
            if (!values_->good()) { HORN_LOGE("BL2Bridge","native-touch settings session=%s",values_->reason()); drop(false); return; }
            auto& s = *values_;
            list_ = s.pin(static_cast<std::uint32_t>(model.list >> 32) + 0x3C);
            if (movieClass != "OptionsGFxMovie") {
                // The main and pause menus share the shipped CLIK row layout.
                // Enlarge its typography while retaining native row pitch,
                // providers, selection handlers and click listeners.
                auto data = s.member(list_,"data");
                const bool okay = list_ && data && s.identity(data,dataIdentity_); s.release(data);
                if (!okay || !s.good()) { drop(false); return; }
                inputCompleted();
            } else {
                options_ = s.pin(static_cast<std::uint32_t>(model.options >> 32) + 0x3C);
                auto header = s.member(options_,"header"); headerText_ = s.member(header,"headerText"); s.release(header);
                description_ = s.member(options_,"optionsHints");
                auto baseline = CaptureBl2OptionsOriginal(s,list_,headerText_,description_,original_);
                originalData_ = s.member(baseline,"data"); originalHeader_ = s.member(baseline,"header");
                if (s.object(description_)) originalDescription_ = s.member(baseline,"description");
                listeners_ = s.member(baseline,"listeners"); sliderHandler_ = s.member(baseline,"slider");
                spinnerHandler_ = s.member(baseline,"spinner"); s.release(baseline);
                auto prototype = s.member(list_,"__proto__");
                quietSlider_ = s.member(prototype,"onSliderValueChange"); quietSpinner_ = s.member(prototype,"onSpinnerValueChange");
                s.release(prototype); emptyListeners_ = s.point(0,0);
                if (!list_ || !options_ || !headerText_ || !originalHeader_ || !originalData_ || !listeners_ || !sliderHandler_ || !spinnerHandler_ ||
                    !quietSlider_ || !quietSpinner_ || !emptyListeners_ || !show(false)) {
                    HORN_LOGE("BL2Bridge","native-touch settings attach=%s provider=%s",s.reason() ? s.reason() : "missing-options-AS-member",model.providerClass.c_str());
                    drop(true); return;
                }
            }
        }
        const double scale = Bl2InterfaceScale();
        if (scale != observedScale_) { observedScale_ = scale; inputCompleted(); }
        if (original_.movieClass != "OptionsGFxMovie") {
            if (!pointerCaptured) pollScale();
            else if (scrolling) scaleList(false);
            return;
        }
        std::uint32_t request = 0;
        { std::lock_guard<std::mutex> lock(touchSettingsState.mutex); request = touchSettingsState.uiRequest; touchSettingsState.uiRequest = 0; }
        if (request & 8u) editResume_ = true;
        if (request & 16u) nativeBack_ = !page_;
        if ((request & 2u) && page_) { if (!show(false)) drop(true); }
        else if ((request & 1u) && !page_) { if (!show(true)) drop(true); }
        else if ((request & 32u) && !page_) { if (!show(2)) drop(true); }
        else if (request & 4u) {
            double selected = 0; number(list_,"selectedIndex",selected);
            if (!show(page_,static_cast<int>(selected))) drop(true);
        }
        // CLIK initSlider writes an unrounded numeric toString whenever a
        // scrolled renderer is recycled. Refresh only its value text on every
        // input boundary, including captured drags and after layout retries.
        // Geometry remains unchanged until the captured pointer is released.
        if (values_ && values_->good())
            FormatBl2TouchSettingsList(*values_,list_,dataIdentity_,events_);
        // Changing row height retires native renderers. Keep a continuous
        // scale-slider drag bound to its original geometry until UP/cancel.
        if (!pointerCaptured) pollScale();
        else if (scrolling) scaleList(false);
    }
    bool action(const ActionEdge& edge) {
        if (!values_ || original_.movieClass != "OptionsGFxMovie" || !current()) return false;
        // This page replaces only AVM data, not the game's native menu stack.
        // Own both edges even while the selected renderer is being refreshed.
        if (edge.action == Action::Back) {
            if (!page_) return false; // Original pages need both original key edges.
            if (!edge.down) RequestBl2TouchSettingsUi(2u);
            return true;
        }
        const bool navigation = edge.action == Action::UiUp || edge.action == Action::UiDown;
        double selected = -1;
        if (!number(list_,"selectedIndex",selected) || selected < 0 || std::floor(selected) != selected) return page_;
        const auto events = events_;
        const auto index = static_cast<std::size_t>(selected);
        const bool custom = index < events.size() && events[index] >= kTouchSettingsOpen;
        if (!page_ && !navigation && !custom) return false;
        if (edge.down) return true;
        if (navigation) {
            int next = static_cast<int>(selected), direction = edge.action == Action::UiUp ? -1 : 1;
            for (std::size_t tries = 0; tries < events.size(); ++tries) {
                next = std::clamp(next + direction,0,static_cast<int>(events.size()) - 1);
                auto data = values_->member(list_,"data"), item = values_->element(data,static_cast<std::uint32_t>(next));
                auto disabled = values_->member(item,"unselectable"); bool blocked = true;
                const bool valid = disabled && values_->boolean(disabled,blocked);
                values_->release(disabled); values_->release(item); values_->release(data);
                if (valid && !blocked) break;
            }
            const bool quiet = page_ || events[static_cast<std::size_t>(next)] >= kTouchSettingsOpen;
            if (quiet) callbacks(false);
            auto value = values_->number(next); auto result = values_->invoke(list_,"__set__selectedIndex",{value},true);
            values_->release(result); values_->release(value); if (quiet && !page_) callbacks(true);
            return true;
        }
        if (edge.action == Action::Confirm && index < events.size()) {
            if (events[index] == kTouchSettingsOpen) RequestBl2TouchSettingsUi(1);
            else if (events[index] == kTouchSettingsGraphicsOpen) RequestBl2TouchSettingsUi(32);
            else if (events[index] == kTouchSettingsNativeBack) RequestBl2TouchSettingsUi(16);
            else if (events[index] == kTouchSettingsBack) RequestBl2TouchSettingsUi(2);
            else if (events[index] == kTouchSettingsEdit || events[index] == kTouchSettingsReset || events[index] == kTouchSettingsCalibrate) {
                CommandBl2TouchSettings(static_cast<std::uint32_t>(events[index] - kTouchSettingsBack));
                if (events[index] == kTouchSettingsEdit) RequestBl2TouchSettingsUi(2
                    | 8u
                );
            }
        }
        if ((edge.action == Action::Confirm || edge.action == Action::UiLeft || edge.action == Action::UiRight) && index < events.size()) {
            const auto setting = events[index] - kTouchSettingsValue;
            if (setting >= 0 && setting < static_cast<int>(kTouchSettingCount)) {
                const auto id = static_cast<std::size_t>(setting); const auto& spec = touchSettingSpecs[id];
                double value = 0;
                { std::lock_guard<std::mutex> lock(touchSettingsState.mutex); value = TouchSettingNumber(id,touchSettingsState.values[id]); }
                value = spec.toggle ? 1 - value : id == 17 ?
                    (edge.action == Action::Confirm && value >= spec.maximum ? spec.minimum : StepBl2InterfacePercent(value,edge.action != Action::UiLeft)) :
                    edge.action == Action::UiLeft ? std::max(spec.minimum,value - spec.step) :
                    edge.action == Action::Confirm && value >= spec.maximum - .00001 ? spec.minimum : std::min(spec.maximum,value + spec.step);
                if (SetBl2TouchSetting(id,value)) {
                    auto data = values_->member(list_,"data"), item = values_->element(data,static_cast<std::uint32_t>(index));
                    auto number = values_->number(value); values_->setMember(item,"currentValue",number);
                    values_->release(number); values_->release(item); values_->release(data);
                    auto renderers = values_->member(list_,"renderers"); double count = 0;
                    if (this->number(renderers,"length",count) && count >= 0 && count <= 64 && std::floor(count) == count) {
                        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(count); ++i) {
                            auto renderer = values_->element(renderers,i); double rendererIndex = -1;
                            if (this->number(renderer,"index",rendererIndex) && rendererIndex == selected) {
                                auto inner = values_->member(renderer,"inner");
                                auto result = values_->invoke(inner,spec.toggle || id == 31 ? "initSpinner" : "initSlider",{},true);
                                values_->release(result); FormatBl2TouchSettingValue(*values_,inner,id,value); values_->release(inner);
                            }
                            values_->release(renderer);
                        }
                    }
                    values_->release(renderers);
                }
            }
        }
        return true;
    }
private:
    bool number(GfxHandle object, const char* name, double& value) {
        auto member = values_->member(object,name); const bool okay = member && values_->number(member,value); values_->release(member); return okay;
    }
    bool callbacks(bool original) {
        auto& s = *values_;
        return s.setMember(list_,"_listeners",original ? listeners_ : emptyListeners_) &&
            s.setMember(list_,"onSliderValueChange",original ? sliderHandler_ : quietSlider_) &&
            s.setMember(list_,"onSpinnerValueChange",original ? spinnerHandler_ : quietSpinner_);
    }
    void pollScale() {
        if (!values_ || !scalePolls_) return;
        const auto now = monotonicMilliseconds();
        if (now < nextScaleAt_) return;
        --scalePolls_; nextScaleAt_ = now + 100;
        // CommitListItems invalidates CLIK renderers; initSlider subsequently
        // writes data.currentValue.toString() into the authored TextField.
        // Input boundaries can run before that GFx advance, so retain delayed
        // passes rather than exhausting every retry before renderer publication.
        scaleList();
    }
    void scaleList(bool allowLayoutChange = true) {
        auto& s = *values_; if (!s.good() || !current()) return;
        // Value TextFields and descriptions are independent of optional CLIK
        // typography. Some authored lists do not expose the private _rowHeight.
        if (original_.movieClass == "OptionsGFxMovie") {
            if (!page_ && s.hasCallBudget(4)) {
                double selected = -1;
                if (number(list_,"selectedIndex",selected) && selected >= 0 &&
                    std::floor(selected) == selected && selected < events_.size()) {
                    const auto id = events_[static_cast<std::size_t>(selected)] - kTouchSettingsValue;
                    const auto hint = id >= 0 ? TouchSettingHint(static_cast<std::size_t>(id),Bl2RussianLocaleSelected()) : nullptr;
                    if (hint) {
                        auto text = s.stringValue(hint);
                        auto result = s.invoke(options_,"SetDescription",{text},true);
                        s.release(result); s.release(text);
                    }
                }
            }
        }
        const auto typography = ScaleBl2WidgetTypography(s,list_,dataIdentity_,original_,events_,allowLayoutChange);
#if defined(__ANDROID__)
            static const bool diagnostics = [] {
                char property[PROP_VALUE_MAX] = {};
                __system_property_get("debug.bl2bridge.touch_diagnostics",property);
                return property[0] == '1' && !property[1];
            }();
            if (diagnostics && presentationLogs_++ < 12)
                __android_log_print(ANDROID_LOG_INFO,"BL2TouchDiagnostics",
                    "settings typography movie=%s scale=%.2f originalHeight=%.2f rowHeight=%.2f renderers=%u fonts=%u fitted=%u calls=%u session=%s",
                    original_.movieClass.c_str(),Bl2InterfaceScale(),typography.originalRowHeight,typography.rowHeight,
                    typography.renderers,typography.fields,typography.fittedFields,s.calls(),s.reason() ? s.reason() : "okay");
#else
        (void)typography;
#endif
    }
    bool row(const char* caption, std::int32_t event, int setting = -1) {
        auto& s = *values_; auto label = s.stringValue(caption), disabled = s.boolean(false);
        GfxHandle result;
        if (setting < 0) result = s.invoke(list_,"AddListItem",{label,disabled},true);
        else {
            double currentValue = 0;
            { std::lock_guard<std::mutex> lock(touchSettingsState.mutex);
              currentValue = TouchSettingNumber(static_cast<std::size_t>(setting),touchSettingsState.values[static_cast<std::size_t>(setting)]); }
            const auto& spec = touchSettingSpecs[setting]; auto value = s.number(currentValue);
            if (currentValue < spec.minimum || currentValue > spec.maximum) { s.release(value);s.release(disabled);s.release(label);return false; }
            if (spec.toggle || setting == 31) {
                auto choices = s.array();
                const unsigned count = setting == 31 ? 5u : 2u;
                for (unsigned i = 0; i < count; ++i) {
                    const auto caption = setting == 31 ? TouchSettingValueText(31,i) :
                        std::string(i ? (Bl2RussianLocaleSelected() ? "ВКЛ." : "ON") : (Bl2RussianLocaleSelected() ? "ВЫКЛ." : "OFF"));
                    auto choice = s.stringValue(caption.c_str());auto pushed = s.invoke(choices,"push",{choice},true);
                    s.release(pushed);s.release(choice);
                }
                result = s.invoke(list_,"AddSpinnerListItem",{label,disabled,value,choices},true);
                s.release(choices);
            } else {
                auto minimum = s.number(spec.minimum), maximum = s.number(spec.maximum), step = s.number(spec.step);
                result = s.invoke(list_,"AddSliderListItem",{label,disabled,value,minimum,maximum,step},true);
                s.release(step); s.release(maximum); s.release(minimum);
            }
            s.release(value);
        }
        const bool okay = result && s.good(); s.release(result); s.release(disabled); s.release(label);
        if (okay) events_.push_back(event); return okay;
    }
    bool show(unsigned page, int selectedIndex = 0) {
        auto& s = *values_; if (!s.good() || !callbacks(false)) return false;
        auto data = page ? s.array() : s.invoke(originalData_,"slice");
        GfxHandle nativeBack;
        events_ = page ? std::vector<std::int32_t>{} : original_.eventIds;
        const bool customOriginal = !page && (original_.providerClass == "WillowScrollingListDataProviderTopLevelOptions" ||
            original_.providerClass == "WillowScrollingListDataProviderAudioOptions" ||
            original_.providerClass == "WillowScrollingListDataProviderVideoOptions" ||
            original_.providerClass == "WillowScrollingListDataProviderGameOptions");
        if (customOriginal) {
            // Android TopLevel provides BackCaption/event99; child providers
            // use Refresh's event-1. Both are observed original last rows.
            // Preserve other indices; relocated Back uses the original Back key.
            const auto backEvent = events_.empty() ? 0 : events_.back();
            if ((backEvent != 99 && backEvent != -1) || std::count(events_.begin(),events_.end(),backEvent) != 1) { s.release(data);return false; }
            nativeBack = s.invoke(data,"pop",{},true);events_.pop_back();
            if (!nativeBack || !s.object(nativeBack)) { s.release(nativeBack);s.release(data);return false; }
        }
        if (!data || !s.setMember(list_,"data",data)) { s.release(data); return false; }
        s.release(data);
        if (page == 2) {
            for (std::size_t i = 21; i < kTouchSettingCount; ++i) {
#if defined(BL2_NATIVE_ARM32)
                if(i>=32)continue; // These replacements belong to the ARM64 bridge.
#endif
                if (!row(TouchSettingCaption(i,Bl2RussianLocaleSelected()),kTouchSettingsValue + static_cast<std::int32_t>(i),static_cast<int>(i))) return false;
            }
            if (!row(Bl2RussianLocaleSelected() ? "НАЗАД" : "BACK",kTouchSettingsBack)) return false;
        } else if (page == 1) {
            for (std::size_t i = 1; i < 16; ++i) if (i != 8 &&
                !row(TouchSettingCaption(i,Bl2RussianLocaleSelected()),kTouchSettingsValue + static_cast<std::int32_t>(i),static_cast<int>(i))) return false;
            if (!row(Bl2RussianLocaleSelected() ? "ИЗМЕНИТЬ РАСКЛАДКУ" : "EDIT BUTTON LAYOUT",kTouchSettingsEdit) ||
                !row(Bl2RussianLocaleSelected() ? "СБРОСИТЬ УПРАВЛЕНИЕ" : "RESET TOUCH CONTROLS",kTouchSettingsReset) ||
                !row(Bl2RussianLocaleSelected() ? "КАЛИБРОВКА НАКЛОНА" : "CALIBRATE TILT AIMING",kTouchSettingsCalibrate) ||
                !row(Bl2RussianLocaleSelected() ? "НАЗАД" : "BACK",kTouchSettingsBack)) return false;
        } else if (original_.providerClass == "WillowScrollingListDataProviderTopLevelOptions") {
            if (!row(Bl2RussianLocaleSelected() ? "СЕНСОРНОЕ УПРАВЛЕНИЕ" : "TOUCH CONTROLS",kTouchSettingsOpen)) return false;
            if (!row(TouchSettingCaption(17,Bl2RussianLocaleSelected()),kTouchSettingsValue + 17,17)) return false;
        } else if (original_.providerClass == "WillowScrollingListDataProviderAudioOptions" ||
                   original_.providerClass == "WillowScrollingListDataProviderVideoOptions" ||
                   original_.providerClass == "WillowScrollingListDataProviderGameOptions") {
            const int id = original_.providerClass == "WillowScrollingListDataProviderAudioOptions" ? 8 :
                original_.providerClass == "WillowScrollingListDataProviderVideoOptions" ? 0 : 16;
            if (!row(TouchSettingCaption(id,Bl2RussianLocaleSelected()),kTouchSettingsValue + id,id)) return false;
            if (id == 0 && (!row(TouchSettingCaption(18,Bl2RussianLocaleSelected()),kTouchSettingsValue + 18,18) ||
                !row(TouchSettingCaption(19,Bl2RussianLocaleSelected()),kTouchSettingsValue + 19,19) ||
                !row(TouchSettingCaption(20,Bl2RussianLocaleSelected()),kTouchSettingsValue + 20,20) ||
                !row(Bl2RussianLocaleSelected() ? "ДОПОЛНИТЕЛЬНАЯ ГРАФИКА" : "ADDITIONAL GRAPHICS",kTouchSettingsGraphicsOpen))) return false;
        }
        if (nativeBack) {
            data = s.member(list_,"data");auto pushed = s.invoke(data,"push",{nativeBack},true);
            const bool okay = pushed && s.good();s.release(pushed);s.release(data);s.release(nativeBack);
            if (!okay) return false;events_.push_back(kTouchSettingsNativeBack);
        }
        auto committed = s.invoke(list_,"CommitListItems",{},true); s.release(committed);
        if (page) {
            auto caption = s.stringValue(page == 2 ? (Bl2RussianLocaleSelected() ? "ДОПОЛНИТЕЛЬНАЯ ГРАФИКА" : "ADDITIONAL GRAPHICS") :
                (Bl2RussianLocaleSelected() ? "СЕНСОРНОЕ УПРАВЛЕНИЕ" : "TOUCH CONTROLS"));
            auto result = s.invoke(options_,"SetHeader",{caption},true); s.release(result); s.release(caption);
            if (originalDescription_) {
                auto hint = s.stringValue(page == 2 ? (Bl2RussianLocaleSelected() ?
                    "Здесь сохраняются обычные настройки. Картофельная графика временно перекрывает их." :
                    "Your ordinary settings are saved here. Potato graphics temporarily overrides them.") : Bl2RussianLocaleSelected() ?
                    "Тап: выделить; повторный тап: изменить. Смахивание: прокрутка. Для раскладки продолжите игру." :
                    "Tap to select; tap again to change. Swipe to scroll. Resume the game to edit the button layout.");
                result = s.invoke(options_,"SetDescription",{hint},true); s.release(result); s.release(hint);
            }
        } else {
            if (!s.setMember(headerText_,"htmlText",originalHeader_)) return false;
            if (originalDescription_ && !s.setMember(description_,"htmlText",originalDescription_)) return false;
        }
        if (events_.empty()) return false;
        auto first = s.number(std::clamp(selectedIndex,0,static_cast<int>(events_.size()) - 1));
        auto selected = s.invoke(list_,"__set__selectedIndex",{first},true); s.release(selected); s.release(first);
        auto currentData = s.member(list_,"data"); const bool okay = currentData && s.identity(currentData,dataIdentity_); s.release(currentData);
        if (!page && !callbacks(true)) return false;
        if (!okay || !s.good()) return false;
        page_ = page;
        inputCompleted();
        HORN_LOGI("BL2Bridge","native-touch settings original-page=%u provider=%s rows=%zu",page ? 1u : 0u,original_.providerClass.c_str(),events_.size());
        auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
        state.owner = original_.owner; state.list = original_.list; state.provider = original_.provider;
        state.nativeStructure = original_.structure; state.nativeEvents = original_.eventIds;
        state.events = events_; state.page = page; return true;
    }
    void drop(bool restore) {
        if (values_) {
            values_->beginInputBoundary();
            if (restore && current() && values_->good() && originalData_) {
                callbacks(false); values_->setMember(list_,"data",originalData_);
                if (headerText_ && originalHeader_) values_->setMember(headerText_,"htmlText",originalHeader_);
                if (description_ && originalDescription_) values_->setMember(description_,"htmlText",originalDescription_);
                auto result = values_->invoke(list_,"CommitListItems",{},true); values_->release(result);
                auto zero = values_->number(0); result = values_->invoke(list_,"__set__selectedIndex",{zero},true);
                values_->release(result); values_->release(zero); callbacks(true);
            }
            else if (current() && values_->good() && listeners_) callbacks(true);
            values_->close(); values_.reset(); transport_.reset();
        }
        auto& state = touchSettingsState; std::lock_guard<std::mutex> lock(state.mutex);
        if (state.owner == original_.owner) { state.owner = state.list = state.provider = state.nativeStructure = 0; state.events.clear(); state.nativeEvents.clear(); state.page = false; }
        list_ = options_ = headerText_ = originalHeader_ = description_ = originalDescription_ = originalData_ = listeners_ = sliderHandler_ = spinnerHandler_ = quietSlider_ = quietSpinner_ = emptyListeners_ = {};
        page_ = false;
        observedScale_ = 0;
    }
    Reader read_; Ue3WidgetSink::LifetimeGeneration generation_;
    Prepared source_; UiModelSnapshot original_; std::uint64_t lifetime_ = 0;
    unsigned page_ = 0;
    bool opening_ = false, editResume_ = false, nativeBack_ = false;
    unsigned scalePolls_ = 0;
    [[maybe_unused]] unsigned presentationLogs_ = 0;
    std::uint64_t nextScaleAt_ = 0;
    double observedScale_ = 0;
    std::unique_ptr<GfxHostTransport> transport_; std::unique_ptr<GfxSession> values_;
    GfxHandle list_, options_, headerText_, originalHeader_, description_, originalDescription_, originalData_, listeners_, sliderHandler_, spinnerHandler_, quietSlider_, quietSpinner_, emptyListeners_;
    GfxIdentity dataIdentity_; std::vector<std::int32_t> events_;
};
}
struct Ue3WidgetSink::State {
    DynarmicHost& host;
    const LifetimeGeneration generation;
    Ue3UiSink navigation;
    Ue3DebugMenuSink debug;
    Prepared prepared;
    WidgetRouter router;
    SettingsUi settings;
    PauseBackdropUi backdrop;
    UiEditResume editResume;
    bool signatures = false;
    unsigned logs = 0;
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
    unsigned inputLogs = 0;
#endif
#if HORN_ENABLE_LOGGING
    unsigned editLogs = 0;
#endif
    std::string lastLog;
    State(DynarmicHost& h, LifetimeGeneration g)
        : host(h), generation(g), navigation(h,g,true), debug(h,g), router([this](const Frame& frame) -> std::shared_ptr<WidgetSceneLease> {
            if (!signatures || !prepared.widgets || frame.context.sceneIdentity != prepared.scene) return {};
            auto scene = std::make_shared<NativeScene>(host,generation,prepared,frame);
            return scene->current() ? scene : nullptr;
        }
        , true
        ), settings(h,g), backdrop(h,g) {
        navigation.setFooterActionHandler([this](const ActionEdge& edge) {return settings.action(edge);});
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
    RefreshBl2PauseDebugMenu(s.host);
    const auto caps = s.navigation.prepare(viewport,controller,movie,owner,scene);
    s.prepared = {viewport,controller,movie,owner,scene,false};
    if (s.signatures) s.backdrop.prepare(viewport,controller);
    const auto debugCaps = s.debug.prepare(movie ? 0 : viewport,controller,scene);
    if (debugCaps) { s.prepared.debug = true; s.settings.prepare(s.prepared,s.router.captured()); return debugCaps; }
    if (!caps || !s.signatures || s.router.stopped()) return caps;
    const Reader reader(s.host);
    const auto name = reader.className(owner);
    s.prepared.widgets = (name == "FrontendGFxMovie" || name == "PauseGFxMovie" || name == "OptionsGFxMovie") &&
        reader.read(reader.read(reader.base + 0x0301F840) + 0x44) == viewport + 4;
    s.settings.prepare(s.prepared,s.router.captured(),s.router.scrolling());
    return caps | (s.prepared.widgets ? kUiWidgetsTestCapability : 0u);
}
void Ue3WidgetSink::beginTouchFrame(const Frame& frame) {
#if defined(BL2_NATIVE_ARM32) && HORN_ENABLE_LOGGING
    auto& s = *state_;
    if ((frame.context.capabilities & kUiWidgetsTestCapability) &&
        (frame.resetReasons || frame.actionEdgeCount || frame.pointerEventCount) && s.inputLogs < 24) {
        ++s.inputLogs;
        const auto& pointer = frame.pointerEvents[0];
        HORN_LOGI("BL2Bridge", "native-widgets input frame=%llu epoch=%llu reset=%u actions=%zu pointers=%zu "
            "firstPhase=%u firstEpoch=%llu firstSequence=%llu firstTimeMs=%llu mode=%u movie=%08X",
            static_cast<unsigned long long>(frame.frameSerial),static_cast<unsigned long long>(frame.epoch),
            frame.resetReasons,frame.actionEdgeCount,frame.pointerEventCount,
            frame.pointerEventCount ? static_cast<unsigned>(pointer.phase) : 4u,
            static_cast<unsigned long long>(pointer.sourceEpoch),static_cast<unsigned long long>(pointer.sequence),
            static_cast<unsigned long long>(pointer.timestampMs),static_cast<unsigned>(frame.context.mode),s.prepared.movie);
    }
#endif
    state_->router.beginTouchFrame(frame); state_->navigation.beginTouchFrame(frame); state_->debug.beginTouchFrame(frame);
    auto& edit = *state_;
    if (frame.fresh && edit.settings.takeNativeBack() && (frame.context.capabilities & UiNavigation) &&
        !(frame.context.capabilities & (PhysicalOwnsInput | GameplayInput))) {
        edit.router.action({Action::Back,true,frame.epoch,0});
        edit.navigation.action({Action::Back,true,frame.epoch,0});
        edit.navigation.action({Action::Back,false,frame.epoch,0});
        edit.router.action({Action::Back,false,frame.epoch,0});
    }
    const Reader read(edit.host);
    const bool requested = frame.fresh && edit.settings.takeEditResume();
    if (frame.fresh && (requested || edit.editResume.stage())) {
        std::optional<PauseBackdropUi::Observation> observed;
        if(PauseBackdropUi::diagnosticsEnabled()) observed.emplace();
        const auto identity = edit.backdrop.resumeIdentity(edit.prepared,observed?&*observed:nullptr);
        const auto name = read.className(edit.prepared.owner);
        if (requested) edit.editResume.request(identity,monotonicMilliseconds());
        [[maybe_unused]] const auto stage = edit.editResume.stage();
        const bool allowed = frame.fresh && (frame.context.capabilities & UiNavigation) &&
            !(frame.context.capabilities & (PhysicalOwnsInput | GameplayInput)) &&
            (frame.context.mode == Mode::Menu || frame.context.mode == Mode::Frontend);
        const bool back = edit.editResume.next(identity,name == "OptionsGFxMovie",name == "PauseGFxMovie",allowed,monotonicMilliseconds());
#if HORN_ENABLE_LOGGING
        if ((requested || back || stage != edit.editResume.stage()) && edit.editLogs < 8) {
            ++edit.editLogs;
            HORN_LOGI("BL2TouchDiagnostics","edit resume request=%u stage=%u next=%u owned=%u allowed=%u "
                "back=%u owner=%s pause=%08X world=%08X pawn=%08X generation=%llu",
                requested ? 1u : 0u,stage,edit.editResume.stage(),identity ? 1u : 0u,allowed ? 1u : 0u,
                back ? 1u : 0u,name.c_str(),identity.movie,identity.nativeWorld,static_cast<std::uint32_t>(identity.pawn >> 32),
                static_cast<unsigned long long>(edit.generation()));
            if(observed) {
                const auto& o=*observed;
                HORN_LOGI("BL2TouchDiagnostics","edit ownership prepare=%s current=%s model=%s world=%s "
                    "actual=%llu expected=%llu cachedLifetime=%llu movies=%u players=%u liveMovies=%u "
                    "owner=%08X movie=%08X interface=%08X listOffset=%u tokens=(%llu,%llu,%llu,%llu,%llu)",
                    o.prepare,o.current,o.model.c_str(),o.world.c_str(),
                    static_cast<unsigned long long>(o.actual),static_cast<unsigned long long>(o.expected),static_cast<unsigned long long>(o.lifetime),
                    o.movieCount,o.players,o.liveMovies,o.owner,o.movie,o.interface,o.listOffset,
                    static_cast<unsigned long long>(o.engineToken),static_cast<unsigned long long>(o.localToken),
                    static_cast<unsigned long long>(o.controllerToken),static_cast<unsigned long long>(o.ownerToken),static_cast<unsigned long long>(o.listToken));
                for(unsigned i=0;i<o.movieCount && i<o.candidates.size();++i)
                    HORN_LOGI("BL2TouchDiagnostics","edit movie slot=%u movie=%08X owner=%08X class=%s",
                        i,o.candidates[i],o.owners[i],o.classes[i].c_str());
            }
        }
#endif
        if (back) {
            // Every Cancel uses the freshly admitted native focus. The expected
            // Options close may replace the Pause movie and change generation;
            // a freshly registered Pause in the same live game must survive.
            edit.router.action({Action::Back,true,frame.epoch,0});
            edit.navigation.action({Action::Back,true,frame.epoch,0});
            edit.navigation.action({Action::Back,false,frame.epoch,0});
            edit.router.action({Action::Back,false,frame.epoch,0});
        }
    }
}
void Ue3WidgetSink::action(const ActionEdge& edge) {
    state_->router.action(edge);
    if (state_->prepared.debug) state_->debug.action(edge);
    else {
        if (!state_->settings.action(edge)) state_->navigation.action(edge);
        if (!edge.down) state_->settings.inputCompleted();
    }
}
void Ue3WidgetSink::pointer(const PointerEvent& event) {
    if(state_->prepared.debug) state_->debug.pointer(event);
    else if(state_->prepared.widgets) {
        if(state_->navigation.footerPointer(event,!state_->router.captured())) {
            if(event.phase==PointerPhase::Up) state_->settings.inputCompleted();
            return;
        }
        state_->navigation.hover(event);
        state_->router.pointer(event);
        // Typography can recreate renderers, so postpone layout until UP.
        // Numeric text is refreshed independently during captured scrolling.
        if (event.phase == PointerPhase::Up) state_->settings.inputCompleted();
    }
    else state_->navigation.pointer(event);
}
void Ue3WidgetSink::endTouchFrame(const Frame& frame) {
    auto& s = *state_; s.router.endTouchFrame(frame); s.navigation.endTouchFrame(frame); s.debug.endTouchFrame(frame);
    const auto description = s.router.reason() + '/' + std::to_string(s.router.dispatched()) + '/' +
        std::to_string(s.router.captured()) + '/' + std::to_string(s.router.stopped());
    if (description == s.lastLog || s.logs >= 64) return;
    s.lastLog = description; ++s.logs;
    HORN_LOGI("BL2Bridge","native-widgets reason=%s dispatched=%llu captured=%u stopped=%u movie=%08X epoch=%llu",
        s.router.reason().c_str(),static_cast<unsigned long long>(s.router.dispatched()),s.router.captured() ? 1u : 0u,
        s.router.stopped() ? 1u : 0u,s.prepared.movie,static_cast<unsigned long long>(frame.epoch));
}
}
