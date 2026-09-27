#include "bl2_touch_gfx_values.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>

namespace bl2::touch {
namespace {
std::atomic<std::uint64_t> nextId{1};
constexpr std::uint32_t kName = 1024, kArgs = 1152, kTemp = 1408;
constexpr std::uint32_t kManaged = 0x40;
constexpr std::size_t kReadBudget = 128 * 1024;
bool objectType(std::uint32_t t) { return t >= 8 && t <= 11; }
bool rectValid(const GfxRect& r) {
    return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) &&
        std::isfinite(r.bottom) && r.right > r.left && r.bottom > r.top &&
        std::abs(r.left) < 1000000 && std::abs(r.right) < 1000000 &&
        std::abs(r.top) < 1000000 && std::abs(r.bottom) < 1000000;
}
}

bool GfxSession::verifyAbi(GfxTransport& vm, std::uint32_t base) {
    if (std::uint64_t(base) + 0x03089000 > 0x100000000ull) return false;
    struct Span { std::uint32_t rva; std::initializer_list<std::uint32_t> words; };
    // Current A0B06E... ELF. Covers value layout, managed ownership, vtable
    // argument order, string indirection and the SPECIAL Movie release path.
    const Span spans[] = {
        {0x027A5198,{0xE52D4004,0xE3A02001,0xE2803004,0xE1931F9F,0xE081C002,0xE1834F9C}},
        {0x0276293C,{0xE92D4038,0xE3E02000,0xE1A04000,0xE2803004,0xE1931F9F,0xE0810002}},
        {0x02762968,{0xE594000C,0xE5903000,0xE59330D8,0xE12FFF33}},
        {0x01CDFBCC,{0xE5806044,0xE595300C,0xE5803048,0xE1C521D0,0xE1C024FC}},
        {0x01CDFC0C,{0xE5950008,0xE594204C,0xE5840044,0xE5903000,0xE5933008,0xE12FFF33}},
        {0x01CE0E28,{0xE59D0018,0xE28D1010,0xE59D2020,0xE5903000,0xE593300C,0xE12FFF33}},
        {0x01CE0E70,{0xE594104C,0xE0BCC003,0xE1A03006,0xE590E000,0xE58DC000,0xE59EC014,0xE12FFF3C}},
        {0x01CE1408,{0xE5940044,0xE28D6008,0xE1A02001,0xE1A03006,0xE594104C,0xE590C000,0xE59CC034,0xE12FFF3C}},
        {0x01CC7A90,{0xE1A02004,0xE590E000,0xE58D5008,0xE51B5054,0xE88D1020,0xE59EC01C,0xE12FFF3C}},
        {0x01CC8A88,{0xE1A03004,0xE1A0000C,0xEDCD0B06,0xE59CC000,0xE58DE000,0xE59CC018,0xE12FFF3C}},
        {0x01CC80DC,{0xE3120040,0xE1A00004,0x159D3018,0x059D1018,0x15931000}},
        {0x01CC53F8,{0xE5901034,0xE1A00004,0xE5913000,0xE593304C,0xE12FFF33}},
        {0x01CC535C,{0xE59E3000,0xE5933038,0xE12FFF33}},
        {0x01CE0AF8,{0xE24B604C,0xE58D3000,0xE1A01006,0xE1A0300A,0xE79FC00C,0xE12FFF3C}},
        {0x027628B4,{0xE590000C,0xE52DE004,0xE24DD00C,0xE590C000,0xE59DE010,0xE58DE000,0xE59CC0A4,0xE12FFF3C}},
    };
    for (const auto& span : spans) {
        auto p = base + span.rva;
        for (const auto expected : span.words) {
            std::uint32_t actual = 0;
            if (!vm.read(p,&actual,4) || actual != expected) return false;
            p += 4;
        }
    }
    return vm.executable(base + kMovieAddRef) && vm.executable(base + kMovieRelease);
}

GfxSession::GfxSession(GfxTransport& vm, std::uint32_t base, std::uint32_t movie,
                     std::uint32_t scratch, std::uint32_t maxCalls, bool mutations)
    : vm_(vm), base_(base), movie_(movie), scratch_(scratch),
      maxCalls_(std::min(maxCalls,1024u)), allowMutation_(mutations) {
    if (!scratch || (scratch & 15) || std::uint64_t(scratch) + kScratchBytes > 0x100000000ull ||
        !movie || !maxCalls_ || !vm.current() || !verifyAbi(vm,base)) return;
    std::uint32_t refs = 0;
    if (!word(movie + 4,refs) || !refs || refs > 1000000 || !method(movie,4)) return;
    const std::array<std::uint8_t,kScratchBytes> zero{};
    if (!vm.write(scratch,zero.data(),zero.size())) return;
    std::uint32_t result = 0;
    if (!vm.call(base + kMovieAddRef,{movie},result)) return;
    ++calls_; pinned_ = true; reason_ = nullptr;
    if (!vm.current()) fail("scene-changed-after-movie-pin");
}
GfxSession::~GfxSession() { close(); }
bool GfxSession::good() const { return pinned_ && cleanupOkay_ && !reason_ && vm_.current(); }
void GfxSession::fail(const char* s) { if (!reason_) reason_ = s; }
bool GfxSession::read(std::uint32_t p, void* out, std::size_t size) const {
    if (p < 0x10000 || size > kReadBudget - std::min(bytesRead_,kReadBudget) ||
        std::uint64_t(p) + size > 0x100000000ull) return false;
    bytesRead_ += size;
    return vm_.read(p,out,size);
}
bool GfxSession::word(std::uint32_t p, std::uint32_t& out) const { return read(p,&out,4); }
std::uint32_t GfxSession::method(std::uint32_t interface, std::uint32_t offset) const {
    std::uint32_t table = 0, fn = 0;
    if (!interface || !word(interface,table) || !table ||
        std::uint64_t(table) + offset > 0xFFFFFFFCull || !word(table + offset,fn) ||
        !vm_.executable(fn)) return 0;
    return fn;
}
GfxSession::Slot* GfxSession::find(GfxHandle h) {
    for (auto& s : slots_) if (h.id && s.id == h.id && !s.quarantined) return &s;
    return nullptr;
}
const GfxSession::Slot* GfxSession::find(GfxHandle h) const {
    for (const auto& s : slots_) if (h.id && s.id == h.id && !s.quarantined) return &s;
    return nullptr;
}
std::uint32_t GfxSession::address(const Slot& s) const {
    return scratch_ + static_cast<std::uint32_t>(&s - slots_.data()) * 32;
}
GfxSession::Slot* GfxSession::allocate() {
    if (!good()) return nullptr;
    for (auto& s : slots_) if (!s.id) {
        s = {}; s.id = nextId.fetch_add(1,std::memory_order_relaxed);
        if (!s.id) s.id = nextId.fetch_add(1,std::memory_order_relaxed);
        if (!vm_.write(address(s),s.raw.data(),32)) { s = {}; fail("scratch-write"); return nullptr; }
        return &s;
    }
    fail("value-slot-budget"); return nullptr;
}
bool GfxSession::name(const char* value) {
    if (!value) return false;
    std::size_t length = 0;
    while (length < 127 && value[length]) {
        const auto c = static_cast<unsigned char>(value[length]);
        if (c < 32 || c > 126) return false;
        ++length;
    }
    return length && length < 127 && vm_.write(scratch_ + kName,value,length + 1);
}
bool GfxSession::run(std::uint32_t fn, std::initializer_list<std::uint32_t> args, std::uint32_t& out) {
    if (!good() || !fn || !vm_.executable(fn)) return false;
    if (calls_ >= maxCalls_) { fail("guest-call-budget"); return false; }
    if (!vm_.call(fn,args,out)) { fail("guest-call-rejected"); return false; }
    ++calls_;
    // Do not discard a managed output when a getter changes scene. result()
    // first adopts that output, and close() then releases it while Movie is held.
    if (!vm_.current()) fail("scene-changed-in-callback");
    return true;
}
bool GfxSession::adopt(Slot& s) {
    // Native result memory remains ours even after a scene change. No UObject
    // pointer is consulted here. Read directly so cleanup cannot lose a managed
    // reference merely because the diagnostic read budget was exhausted.
    if (!vm_.read(address(s),s.raw.data(),32)) {
        fail("result-read"); s.quarantined = true; cleanupOkay_ = false; vm_.abandonScratch(); return false;
    }
    bytesRead_ += 32;
    if (s.raw[3] & kManaged) {
        s.owned = true;
        std::uint32_t table = 0, fn = 0;
        if (vm_.read(s.raw[2],&table,4) && table && table <= 0xFFFFFFF3u &&
            vm_.read(table + 12,&fn,4) && vm_.executable(fn)) s.releaseFunction = fn;
        if (!s.releaseFunction) {
            fail("managed-release-abi"); s.quarantined = true; cleanupOkay_ = false; vm_.abandonScratch(); return false;
        }
    }
    if ((s.raw[3] & ~0x4Fu) || (s.raw[3] & 15) > 11) { fail("value-type"); return false; }
    if (bytesRead_ > kReadBudget) { fail("read-budget"); return false; }
    return true;
}
GfxHandle GfxSession::result(Slot& s, bool invoked, std::uint32_t success) {
    const bool adopted = invoked && adopt(s);
    if (adopted && success && good()) return {s.id};
    retire(s); return {};
}
GfxHandle GfxSession::pin(std::uint32_t p) {
    auto* s = allocate();
    if (!s) return {};
    std::array<std::uint32_t,8> source{};
    if (!read(p,source.data(),32) || (source[3] & ~0x4Fu) || (source[3] & 15) > 11) {
        retire(*s); return {};
    }
    // Constructor semantics proven by CreateValueAddRef. Intrusive links at 0/4
    // and trailing padding are zero, while the closure extra word is retained.
    for (unsigned i = 2; i <= 6; ++i) s->raw[i] = source[i];
    if (!vm_.write(address(*s),s->raw.data(),32)) { retire(*s); return {}; }
    if (source[3] & kManaged) {
        const auto add = method(source[2],8), release = method(source[2],12);
        if (!add || !release) { retire(*s); return {}; }
        std::uint32_t ignored = 0;
        const bool executed = run(add,{source[2],address(*s),source[4]},ignored);
        // Once AddRef ran, this slot must be released even if current() changed.
        if (executed) { s->owned = true; s->releaseFunction = release; }
        if (!executed || !good()) { retire(*s); return {}; }
    }
    return {s->id};
}
GfxHandle GfxSession::number(double value) {
    if (!std::isfinite(value)) return {};
    auto* s = allocate(); if (!s) return {};
    s->raw[3] = 5; std::memcpy(&s->raw[4],&value,8);
    if (!vm_.write(address(*s),s->raw.data(),32)) { retire(*s); return {}; }
    return {s->id};
}
GfxHandle GfxSession::undefined() { auto* s = allocate(); return s ? GfxHandle{s->id} : GfxHandle{}; }
GfxHandle GfxSession::point(double x, double y) {
    if (!good() || !std::isfinite(x) || !std::isfinite(y) || !name("Object")) return {};
    auto* s = allocate(); if (!s) return {};
    std::uint32_t ignored = 0;
    const bool ran = run(base_ + kCreateObject,{movie_,address(*s),scratch_ + kName,0,0},ignored);
    // This native API is void. The output's real type is the success criterion.
    const auto h = result(*s,ran,1);
    if (!h || type(h) != 8) { release(h); return {}; }
    s->temporary = true;
    const auto a = number(x), b = number(y);
    const bool okay = a && b && set(h,"x",a,true) && set(h,"y",b,true);
    release(a); release(b);
    if (!okay) { release(h); return {}; }
    return h;
}
void GfxSession::retire(Slot& s) {
    if (!s.id || s.quarantined) return;
    const bool owned = s.owned;
    s.owned = false; s.id = 0; // Reentrant cleanup cannot release this twice.
    if (owned) {
        std::uint32_t ignored = 0;
        // Cached ABI pointer belongs to the pinned Movie's ObjectInterface.
        // No scene check and no ordinary call/read budget on required cleanup.
        if (!s.releaseFunction || !vm_.executable(s.releaseFunction) ||
            !vm_.call(s.releaseFunction,{s.raw[2],address(s),s.raw[4]},ignored)) {
            // This is still an owned, intrusive guest value. Quarantine the slot
            // immediately, not only at close(): no ordinary operation may zero
            // or reuse it. Other live values may still undergo required cleanup.
            s.owned = true; s.quarantined = true; cleanupOkay_ = false;
            fail("managed-release-failed"); vm_.abandonScratch(); return;
        }
        ++cleanupCalls_;
    }
    s = {};
}
void GfxSession::release(GfxHandle h) { if (auto* s = find(h)) retire(*s); }
void GfxSession::close() {
    for (auto i = slots_.rbegin(); i != slots_.rend(); ++i) retire(*i);
    if (!cleanupOkay_) vm_.abandonScratch();
    if (pinned_) {
        pinned_ = false;
        std::uint32_t ignored = 0;
        // If an impossible/corrupt managed result prevented safe release, retain
        // the Movie pin as well. A reported bounded leak is safer than destroying
        // its ObjectInterface underneath a still-owned native Value.
        if (!cleanupOkay_ || !vm_.executable(base_ + kMovieRelease) ||
            !vm_.call(base_ + kMovieRelease,{movie_},ignored)) {
            cleanupOkay_ = false; vm_.abandonScratch();
        }
        else ++cleanupCalls_;
    }
}
GfxHandle GfxSession::member(GfxHandle h, const char* key) {
    const auto* source = find(h);
    if (!good() || !source || !object(h) || !name(key)) return {};
    const auto interface = source->raw[2], data = source->raw[4], display = type(h) == 10 ? 1u : 0u;
    auto* out = allocate(); if (!out) return {};
    std::uint32_t success = 0;
    const bool ran = run(method(interface,20),{interface,data,scratch_ + kName,address(*out),display},success);
    return result(*out,ran,success);
}
GfxHandle GfxSession::element(GfxHandle h, std::uint32_t index) {
    const auto* source = find(h);
    if (!source || type(h) != 9 || index > 4095) return {};
    const auto interface = source->raw[2], data = source->raw[4];
    auto* out = allocate(); if (!out) return {};
    std::uint32_t success = 0;
    const bool ran = run(method(interface,52),{interface,data,index,address(*out)},success);
    return result(*out,ran,success);
}
GfxHandle GfxSession::invoke(GfxHandle h, const char* key, std::initializer_list<GfxHandle> args, bool mutation) {
    const auto* source = find(h);
    if (!good() || !source || !object(h) || args.size() > 8 || (mutation && !allowMutation_) || !name(key)) return {};
    const auto interface = source->raw[2], data = source->raw[4], display = type(h) == 10 ? 1u : 0u;
    std::uint32_t argument = scratch_ + kArgs;
    for (auto value : args) {
        const auto* entry = find(value); if (!entry) return {};
        auto copy = entry->raw; copy[0] = copy[1] = copy[7] = 0;
        // Borrowed const argument array. Original owned slots remain alive for
        // the synchronous call; these copies never undergo ObjectRelease.
        if (!vm_.write(argument,copy.data(),32)) return {};
        argument += 32;
    }
    auto* out = allocate(); if (!out) return {};
    std::uint32_t success = 0;
    const bool ran = run(method(interface,28),{interface,data,address(*out),scratch_ + kName,
        args.size() ? scratch_ + kArgs : 0,static_cast<std::uint32_t>(args.size()),display},success);
    return result(*out,ran,success);
}
bool GfxSession::setMember(GfxHandle h, const char* key, GfxHandle value) {
    return set(h,key,value,false);
}
bool GfxSession::set(GfxHandle h, const char* key, GfxHandle value, bool temporary) {
    const auto* source = find(h); const auto* v = find(value);
    if (!source || !v || !object(h) || !good() || !name(key)) return false;
    if (temporary) {
        if (!source->temporary || (std::strcmp(key,"x") && std::strcmp(key,"y")) || type(value) != 5) return false;
    } else if (!allowMutation_) return false;
    std::uint32_t success = 0;
    return run(method(source->raw[2],24),{source->raw[2],source->raw[4],scratch_ + kName,
        address(*v),type(h) == 10 ? 1u : 0u},success) && success && good();
}
std::uint32_t GfxSession::type(GfxHandle h) const { const auto* s = find(h); return s ? s->raw[3] & 0x8F : 0xFFFFFFFFu; }
bool GfxSession::object(GfxHandle h) const { return objectType(type(h)); }
bool GfxSession::number(GfxHandle h, double& out) const {
    const auto* s = find(h); if (!s || !good()) return false;
    switch (type(h)) {
    case 3: out = static_cast<std::int32_t>(s->raw[4]); break;
    case 4: out = s->raw[4]; break;
    case 5: std::memcpy(&out,&s->raw[4],8); break;
    default: return false;
    }
    return std::isfinite(out);
}
bool GfxSession::boolean(GfxHandle h, bool& out) const {
    const auto* s = find(h); if (!s || !good() || type(h) != 2 || (s->raw[4] & 255) > 1) return false;
    out = (s->raw[4] & 255) != 0; return true;
}
bool GfxSession::string(GfxHandle h, std::string& out, std::size_t limit) const {
    out.clear(); const auto* s = find(h);
    if (!s || !good() || type(h) != 6 || !limit || limit > 1024) return false;
    std::uint32_t p = s->raw[4];
    if ((s->raw[3] & kManaged) && !word(p,p)) return false;
    for (std::size_t i = 0; i <= limit; ++i) {
        char c = 0;
        if (std::uint64_t(p) + i > 0xFFFFFFFFull || !read(p + static_cast<std::uint32_t>(i),&c,1)) break;
        if (!c) return true;
        if (i == limit) break;
        out += c; // UTF-8 preserved byte-for-byte, never used as a target ID.
    }
    out.clear(); return false;
}
bool GfxSession::identity(GfxHandle h, GfxIdentity& out) const {
    const auto* s = find(h); if (!s || !good() || !object(h)) return false;
    out = {s->raw[2],s->raw[4],type(h) == 11 ? s->raw[6] : 0,type(h)};
    return out.interface && out.data;
}
bool GfxSession::frameRect(GfxRect& out) {
    std::array<float,4> rect{};
    rect.fill(std::numeric_limits<float>::quiet_NaN());
    if (!good() || !vm_.write(scratch_ + kTemp,rect.data(),16)) return false;
    std::uint32_t ignored = 0;
    if (!run(method(movie_,0x4C),{scratch_ + kTemp,movie_},ignored) || !good() ||
        !read(scratch_ + kTemp,rect.data(),16)) return false;
    out = {rect[0],rect[1],rect[2],rect[3]}; return rectValid(out);
}
bool GfxSession::viewport(GfxViewport& out) {
    out = {};
    if (!good() || !vm_.write(scratch_ + kTemp,out.words.data(),52)) return false;
    std::uint32_t ignored = 0;
    return run(method(movie_,0x38),{movie_,scratch_ + kTemp},ignored) && good() &&
        read(scratch_ + kTemp,out.words.data(),52);
}
} // namespace bl2::touch
