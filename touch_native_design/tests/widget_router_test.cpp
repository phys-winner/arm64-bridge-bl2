#include "gfx_widget_fixture.h"
#include "bl2_touch_widget_router.h"
#include <functional>
#include <iostream>
#include <memory>
using namespace widget_fixture;
namespace {
struct Environment {
    Vm vm;
    std::uint64_t generation = 1;
    bool focused = true, rejectOneRelease = false;
    unsigned opens = 0;
    explicit Environment(const char* f) : vm(f) {}
};
struct Transport final : GfxTransport {
    Environment& e;
    std::function<bool()> live;
    std::uint32_t scratch;
    bool abandoned = false;
    Transport(Environment& environment, std::function<bool()> check, std::uint32_t address)
        : e(environment), live(std::move(check)), scratch(address) {}
    bool read(std::uint32_t p, void* out, std::size_t n) const override { return e.vm.read(p,out,n); }
    bool write(std::uint32_t p, const void* in, std::size_t n) override {
        return p >= scratch && p - scratch <= GfxSession::kScratchBytes &&
            n <= GfxSession::kScratchBytes - (p - scratch) && e.vm.write(p,in,n);
    }
    bool executable(std::uint32_t p) const override { return e.vm.executable(p); }
    bool current() const override { return !abandoned && e.vm.current() && live(); }
    void abandonScratch() override { abandoned = true; }
    bool call(std::uint32_t pc, const std::vector<std::uint32_t>& args, std::uint32_t& out) override {
        if (pc == release && e.rejectOneRelease) { e.rejectOneRelease = false; return false; }
        return e.vm.call(pc,args,out);
    }
};
struct Scene final : WidgetSceneLease, std::enable_shared_from_this<Scene> {
    Environment& e;
    const std::uint64_t generation;
    const UiModelSnapshot model;
    const WidgetSceneInfo captured;
    Scene(Environment& environment, const Frame& f)
        : e(environment), generation(e.generation), model(e.vm.model),
          captured{f.epoch,f.context.sceneIdentity,2560,1440,0} {}
    const WidgetSceneInfo& info() const override { return captured; }
    bool current() const override { return e.focused && e.generation == generation && e.vm.current(); }
    UiModelSnapshot readModel() const override { return current() ? e.vm.model : UiModelSnapshot{}; }
    std::unique_ptr<WidgetSessionScope> open(std::uint32_t budget, bool mutation) override {
        const auto p = 0x25000000u + ++e.opens * 0x1000;
        auto vm = std::make_unique<Transport>(e,[self = shared_from_this()] { return self->current(); },p);
        return std::make_unique<WidgetSessionScope>(std::move(vm),base,movie,p,budget,mutation);
    }
};
Frame inputFrame(unsigned serial) {
    Frame f; f.fresh = true; f.epoch = 2; f.frameSerial = serial;
    f.context.mode = Mode::Menu; f.context.sceneIdentity = 10;
    f.context.capabilities = UiNavigation | kUiWidgetsTestCapability; return f;
}
PointerEvent pointer(PointerPhase phase, float x = .2f, float y = .25f, int id = 4) {
    return {id,phase,x,y,2,0};
}
WidgetRouter::AcquireScene acquire(Environment& e) {
    return [&e](const Frame& f) { return std::make_shared<Scene>(e,f); };
}
void begin(WidgetRouter& r, unsigned n) { r.beginTouchFrame(inputFrame(n)); }
void end(WidgetRouter& r, unsigned n) { r.endTouchFrame(inputFrame(n)); }
void clean(const Environment& e) { assert(e.vm.refs.empty() && e.vm.movieRefs == 1); }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); end(r,1); assert(e.opens == 0); // Idle input has zero GFx calls.
        begin(r,2); r.pointer(pointer(PointerPhase::Down)); assert(r.captured()); end(r,2);
        assert(e.vm.refs.size() >= 4 && e.vm.movieRefs == 2 && !e.vm.clicks);
        begin(r,3); r.pointer(pointer(PointerPhase::Up)); end(r,3);
        assert(e.vm.clicks == 1 && r.dispatched() == 1 && !r.captured()); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down));
        r.action({Action::Back,true,2,2}); r.pointer(pointer(PointerPhase::Up));
        r.action({Action::Back,false,2,3}); end(r,1);
        assert(!e.vm.clicks); clean(e);
        begin(r,2); r.pointer(pointer(PointerPhase::Down)); r.pointer(pointer(PointerPhase::Up)); end(r,2);
        assert(e.vm.clicks == 1); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.action({Action::Back,true,2,1}); r.pointer(pointer(PointerPhase::Down));
        r.action({Action::Back,false,2,3}); r.pointer(pointer(PointerPhase::Up)); end(r,1);
        assert(!e.vm.clicks); clean(e);
        begin(r,2); r.pointer(pointer(PointerPhase::Down)); r.pointer(pointer(PointerPhase::Up)); end(r,2);
        assert(e.vm.clicks == 1); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down)); r.pointer(pointer(PointerPhase::Down,.3f,.25f,5)); end(r,1);
        assert(!r.captured()); clean(e);
        begin(r,2); r.pointer(pointer(PointerPhase::Up)); r.pointer(pointer(PointerPhase::Move,.2f,.25f,5));
        r.pointer(pointer(PointerPhase::Up,.2f,.25f,5)); end(r,2);
        assert(!e.vm.clicks); clean(e);
    }
    for (int interruption = 0; interruption < 4; ++interruption) {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down)); end(r,1);
        auto next = inputFrame(2);
        if (interruption == 0) e.focused = false;
        if (interruption == 1) ++e.generation;
        if (interruption == 2) next.context.capabilities |= PhysicalOwnsInput;
        if (interruption == 3) { next.resetReasons = ContextChanged; ++next.epoch; }
        r.beginTouchFrame(next); r.pointer(pointer(PointerPhase::Up)); r.endTouchFrame(next);
        assert(!e.vm.clicks && !r.captured()); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down,.6f,.5f));
        r.pointer(pointer(PointerPhase::Move,.75f,.5f)); end(r,1);
        assert(e.vm.sliderNotifications == 1 && number(e.vm.object(e.vm.sliderData).properties.at("currentValue")) == 9);
        begin(r,2); r.pointer(pointer(PointerPhase::Up,.7f,.5f)); end(r,2);
        assert(e.vm.sliderNotifications == 2 && number(e.vm.object(e.vm.sliderData).properties.at("currentValue")) == 8);
        assert(!e.vm.clicks); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down,.2f,.25f));
        r.pointer(pointer(PointerPhase::Move,.2f,.05f)); end(r,1);
        assert(e.vm.scrolls == 1 && !e.vm.clicks);
        // Fixture deliberately has not redrawn/recycled renderers after scroll.
        // Fresh capture must reject inconsistent row indices instead of clicking.
        begin(r,2); r.pointer(pointer(PointerPhase::Up,.2f,.05f)); end(r,2);
        assert(!r.captured() && !e.vm.clicks); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down)); end(r,1);
        e.vm.changeInClick = true;
        begin(r,2); r.pointer(pointer(PointerPhase::Up));
        r.pointer(pointer(PointerPhase::Down)); r.pointer(pointer(PointerPhase::Up)); end(r,2);
        assert(e.vm.clicks == 1 && !r.captured()); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down)); end(r,1);
        ++e.vm.model.structure;
        begin(r,2); r.pointer(pointer(PointerPhase::Up)); end(r,2);
        assert(!e.vm.clicks); clean(e);
    }
    {
        Environment e(argv[1]); WidgetRouter r(acquire(e));
        begin(r,1); r.pointer(pointer(PointerPhase::Down)); end(r,1);
        e.rejectOneRelease = true;
        auto cancelled = inputFrame(2); cancelled.resetReasons = ManualReset;
        r.beginTouchFrame(cancelled); r.endTouchFrame(cancelled);
        assert(r.stopped() && e.vm.refs.size() == 1 && e.vm.movieRefs == 2);
        const auto opens = e.opens;
        begin(r,3); r.pointer(pointer(PointerPhase::Down)); r.pointer(pointer(PointerPhase::Up)); end(r,3);
        assert(e.opens == opens && !e.vm.clicks);
    }
    std::cout << "PASS: widget router retention, click/slider/scroll, multi-pointer cancel, lifecycle and cleanup fault\n";
}
