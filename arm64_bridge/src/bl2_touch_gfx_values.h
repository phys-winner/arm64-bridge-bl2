#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace bl2::touch {

// All methods run synchronously at the verified guest input boundary. current()
// must recheck scene, focus, generations, physical ownership and object tokens.
// call() returns false only if it did NOT execute. Cleanup does not require the
// old scene to remain current: a session pins the actual Scaleform Movie first.
class GfxTransport {
public:
    virtual ~GfxTransport() = default;
    virtual bool read(std::uint32_t, void*, std::size_t) const = 0;
    virtual bool write(std::uint32_t, const void*, std::size_t) = 0;
    virtual bool executable(std::uint32_t) const = 0;
    virtual bool current() const = 0;
    virtual bool call(std::uint32_t, const std::vector<std::uint32_t>&, std::uint32_t&) = 0;
    // On irrecoverable ownership corruption, never free/reuse scratch that may
    // still be linked into a guest managed-value list. Disable this transport.
    virtual void abandonScratch() = 0;
};

struct GfxHandle { std::uint64_t id = 0; explicit operator bool() const { return id != 0; } };
struct GfxIdentity {
    std::uint32_t interface = 0, data = 0, extra = 0, type = 0;
    bool operator==(const GfxIdentity& o) const {
        return interface == o.interface && data == o.data && extra == o.extra && type == o.type;
    }
    bool operator!=(const GfxIdentity& o) const { return !(*this == o); }
};
struct GfxRect { double left = 0, top = 0, right = 0, bottom = 0; };
struct GfxViewport {
    // Native Scaleform::Render::Viewport; exact caller stores prove these first
    // six fields. Remaining words are retained for the observer, not interpreted.
    std::array<std::uint32_t,13> words{};
};

// GFx::Value is NOT a POD. Its first two words are intrusive list links. Never
// memcpy a live value to obtain ownership. The session rebuilds a clean value,
// calls ObjectInterface::ObjectAddRef, and calls ObjectRelease exactly once.
// Handles are session-owned views, invalid after release or session destruction.
// No pointer/handle from this type is a Java-visible target ID.
class GfxSession final {
public:
    static constexpr std::size_t kSlots = 32, kScratchBytes = 1536 + kSlots * 256;
    static constexpr std::uint32_t kMovieAddRef = 0x027A5198;
    static constexpr std::uint32_t kMovieRelease = 0x0276293C;
    static constexpr std::uint32_t kCreateObject = 0x027628B4;
    static constexpr std::uint32_t kGetVariable = 0x02762738;

    // Caller supplies exclusive, 16-byte-aligned GuestAlloc scratch and the exact
    // stage1 SHA-bound image base. maxCalls excludes mandatory cleanup calls.
    GfxSession(GfxTransport&, std::uint32_t base, std::uint32_t movieInterface,
               std::uint32_t scratch, std::uint32_t maxCalls = 384,
               bool allowMutation = false);
    ~GfxSession();
    GfxSession(const GfxSession&) = delete;
    GfxSession& operator=(const GfxSession&) = delete;

    bool good() const;
    const char* reason() const { return reason_; }
    std::uint32_t calls() const { return calls_; }
    // Persistent settings retain values, but guest work is bounded separately
    // at each serialized input boundary, never inside a callback/gesture.
    void beginInputBoundary() { if (!reason_) { calls_ = 0; bytesRead_ = 0; } }
    bool hasCallBudget(std::uint32_t count) const { return good() && calls_<=maxCalls_ && count<=maxCalls_-calls_; }
    std::uint32_t cleanupCalls() const { return cleanupCalls_; }
    std::size_t bytesRead() const { return bytesRead_; }
    bool cleanupOkay() const { return cleanupOkay_; }
    void close();

    // Pin an existing GFx::Value, e.g. a verified UGFxObject at object+0x3C.
    GfxHandle pin(std::uint32_t nativeValue);
    GfxHandle number(double);
    GfxHandle boolean(bool);
    // Primitive strings live in the session's per-slot scratch until release.
    // AS copies them when a setting row is attached to its real data provider.
    GfxHandle stringValue(const std::string&);
    GfxHandle array();
    GfxHandle undefined();
    // Fresh private ActionScript Object with numeric x/y. Its fields are the
    // only SetMember writes permitted when allowMutation=false. Used by native
    // MovieClip.localToGlobal, preserving nested transforms without guesses.
    GfxHandle point(double x, double y);
    void release(GfxHandle);
    GfxHandle member(GfxHandle, const char* name);
    GfxHandle variable(const char* path);
    GfxHandle element(GfxHandle array, std::uint32_t index);
    GfxHandle invoke(GfxHandle, const char* method, std::initializer_list<GfxHandle> args = {},
                     bool mutation = false);
    bool setMember(GfxHandle, const char* name, GfxHandle value);
    bool number(GfxHandle, double&) const;
    bool boolean(GfxHandle, bool&, bool undefinedIsFalse = false) const;
    bool string(GfxHandle, std::string&, std::size_t limit = 256) const;
    bool identity(GfxHandle, GfxIdentity&) const;
    std::uint32_t type(GfxHandle) const;
    bool object(GfxHandle) const;
    bool frameRect(GfxRect&);
    bool viewport(GfxViewport&);
    // Native AS2 localToGlobal supports inherited 3D world/view/projection.
    // Bounds are measured in the clip's own coordinates, then each corner is
    // projected by the original engine into global stage pixels.
    bool projectedBounds(GfxHandle clip, GfxRect& stageBounds);
    // A proven persistent local rectangle may be projected without including
    // invisible prototypes or animated children in getBounds().
    bool projectedRect(GfxHandle clip, const GfxRect& local, GfxRect& stageBounds);

    static bool verifyAbi(GfxTransport&, std::uint32_t base);
private:
    struct Slot {
        std::uint64_t id = 0;
        bool owned = false;
        bool temporary = false;
        bool quarantined = false;
        std::uint32_t releaseFunction = 0;
        std::array<std::uint32_t,8> raw{};
    };
    GfxTransport& vm_;
    std::uint32_t base_, movie_, scratch_, maxCalls_, calls_ = 0, cleanupCalls_ = 0;
    bool allowMutation_, pinned_ = false, cleanupOkay_ = true;
    const char* reason_ = "not-open";
    mutable std::size_t bytesRead_ = 0;
    std::array<Slot,kSlots> slots_{};
    Slot* find(GfxHandle);
    const Slot* find(GfxHandle) const;
    Slot* allocate();
    std::uint32_t address(const Slot&) const;
    bool read(std::uint32_t, void*, std::size_t) const;
    bool word(std::uint32_t, std::uint32_t&) const;
    std::uint32_t method(std::uint32_t interface, std::uint32_t offset) const;
    bool name(const char*);
    bool run(std::uint32_t, std::initializer_list<std::uint32_t>, std::uint32_t&);
    bool adopt(Slot&);
    GfxHandle result(Slot&, bool invoked, std::uint32_t result);
    void retire(Slot&);
    bool set(GfxHandle, const char*, GfxHandle, bool temporary);
    void fail(const char*);
};

enum class HudObjectiveState { Unknown, Empty, Projected };
struct HudObjectiveBounds {
    HudObjectiveState state = HudObjectiveState::Unknown;
    GfxRect stage{};
    const char* reason = "objective-unobserved";
};
// Original UI_HUD p1.MissionNotifications has six named Module*.inner clips.
// Observe their visible caption art, not the full authored six-row corridor or
// the fixed-width text field. No game/AS state is changed.
HudObjectiveBounds ObserveBl2HudObjectives(GfxSession&, GfxHandle mission);
} // namespace bl2::touch
