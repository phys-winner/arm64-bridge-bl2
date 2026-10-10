#include "bl2_touch_gfx_values.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace bl2::touch;
namespace {
constexpr std::uint32_t base = 0x10000000, movie = 0x21000000, iface = 0x21001000;
constexpr std::uint32_t movieTable = 0x21002000, valueTable = 0x21003000;
constexpr std::uint32_t scratch = 0x21004000, native = 0x21008000, data = 0x21009000;
constexpr std::uint32_t add = base + 0x02010000, release = add + 4, member = add + 8;
constexpr std::uint32_t invoke = add + 12, set = add + 16, element = add + 20;
constexpr std::uint32_t frame = add + 24, viewport = add + 28;
struct Mock final : GfxTransport {
    std::unordered_map<std::uint32_t,std::uint8_t> mem;
    std::vector<std::uint32_t> calls;
    std::vector<std::uint32_t> held;
    std::array<std::uint32_t,8> returned{};
    bool valid = true, staleInMember = false, falseMember = false, staleInAdd = false;
    bool rejectedRelease = false, abandoned = false;
    int movieRefs = 1, valueRefs = 0;
    explicit Mock(const char* fixture) {
        std::ifstream in(fixture); assert(in.good());
        std::uint32_t address = 0, word = 0;
        while (in >> std::hex >> address >> word) put(base + address,word);
        put(movie,movieTable); put(movie + 4,1u); put(movieTable + 4,add + 32);
        put(iface,valueTable);
        for (auto p : std::vector<std::pair<std::uint32_t,std::uint32_t>>{
            {8,add},{12,release},{20,member},{24,set},{28,invoke},{52,element}}) put(valueTable + p.first,p.second);
        put(movieTable + 0x4C,frame); put(movieTable + 0x38,viewport);
        std::array<std::uint32_t,8> object{0xDEAD0001,0xDEAD0002,iface,0x4A,data,0,0xEEEEEEEE,0xDDDDDDDD};
        write(native,object.data(),32);
        returned = {0,0,iface,0x48,data + 4,0,0,0};
    }
    template<class T> void put(std::uint32_t p, T value) { write(p,&value,sizeof(value)); }
    std::uint32_t get(std::uint32_t p) const { std::uint32_t v = 0; assert(read(p,&v,4)); return v; }
    bool read(std::uint32_t p, void* out, std::size_t count) const override {
        auto* bytes = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < count; ++i) {
            const auto it = mem.find(p + static_cast<std::uint32_t>(i));
            if (it == mem.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    bool write(std::uint32_t p, const void* in, std::size_t count) override {
        const auto* bytes = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < count; ++i) mem[p + static_cast<std::uint32_t>(i)] = bytes[i];
        return true;
    }
    bool executable(std::uint32_t p) const override { return p >= base + 0xE823B0 && p < base + 0x2BB91EC; }
    bool current() const override { return valid && !abandoned; }
    void abandonScratch() override { abandoned = true; }
    void registerValue(std::uint32_t p) {
        assert(get(p) == 0 && get(p + 4) == 0);
        put(p,0xA1100001u); put(p + 4,0xA1100002u);
        held.push_back(p); ++valueRefs;
    }
    bool call(std::uint32_t p, const std::vector<std::uint32_t>& a, std::uint32_t& result) override {
        calls.push_back(p); result = 1;
        if (p == base + GfxSession::kMovieAddRef) { assert(a == std::vector<std::uint32_t>{movie}); ++movieRefs; }
        else if (p == base + GfxSession::kMovieRelease) {
            assert(valueRefs == 0); assert(a == std::vector<std::uint32_t>{movie}); --movieRefs;
        } else if (p == add) {
            assert(a.size() == 3 && a[0] == iface && a[2] == data); registerValue(a[1]);
            if (staleInAdd) valid = false;
        } else if (p == release) {
            if (rejectedRelease) return false;
            assert(movieRefs == 2 && a.size() == 3 && a[0] == iface);
            assert(get(a[1]) == 0xA1100001 && get(a[1] + 4) == 0xA1100002);
            assert(a[2] == get(a[1] + 16));
            auto it = std::find(held.begin(),held.end(),a[1]); assert(it != held.end());
            held.erase(it); --valueRefs;
        } else if (p == base + GfxSession::kCreateObject) {
            assert(a.size() == 5 && a[0] == movie && a[3] == 0 && a[4] == 0);
            const std::array<std::uint32_t,8> point{0,0,iface,0x48,data + 0x400,0,0,0};
            write(a[1],point.data(),32); registerValue(a[1]); result = 0;
        } else if (p == member || p == element || p == invoke) {
            assert(movieRefs == 2 && a[0] == iface);
            assert(a.size() == (p == member ? 5u : p == invoke ? 7u : 4u));
            if (p == member) assert(a[4] == 1);
            if (p == invoke && a[5]) {
                assert(a[5] == 1 && get(a[4]) == 0 && get(a[4] + 4) == 0);
                assert(get(a[4] + 12) == 0x4A && get(a[4] + 16) == data);
            }
            const auto out = p == invoke ? a[2] : a[3];
            write(out,returned.data(),32);
            if (returned[3] & 0x40) registerValue(out);
            if (staleInMember) valid = false;
            if (falseMember) result = 0;
        } else if (p == set) { assert(a.size() == 5 && a[4] == (a[1] == data ? 1u : 0u)); }
        else if (p == frame) { const float rect[]{10,20,970,560}; write(a[0],rect,16); }
        else if (p == viewport) { std::array<std::uint32_t,13> v{2424,1080,0,0,2424,1080}; write(a[1],v.data(),52); }
        else assert(false);
        return true;
    }
};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    {
        Mock vm(argv[1]);
        GfxSession session(vm,base,movie,scratch);
        assert(session.good() && vm.movieRefs == 2);
        const auto root = session.pin(native); assert(root && vm.valueRefs == 1);
        GfxIdentity id; assert(session.identity(root,id) && id.extra == 0 && id.type == 10);
        auto child = session.member(root,"child"); assert(child && vm.valueRefs == 2);
        session.release(child); session.release(child); assert(vm.valueRefs == 1);
        assert(session.type(child) == 0xFFFFFFFFu);
        child = session.invoke(root,"getBounds",{root}); assert(child && vm.valueRefs == 2);
        GfxRect r; GfxViewport v;
        assert(session.frameRect(r) && r.left == 10 && r.bottom == 560);
        assert(session.viewport(v) && v.words[0] == 2424 && v.words[5] == 1080);
        const auto n = session.number(12.5); double d = 0;
        assert(session.number(n,d) && d == 12.5 && !session.setMember(root,"value",n));
        assert(!session.invoke(root,"mutate",{},true));
        const auto point = session.point(3,4); assert(point && session.type(point) == 8);
        session.close(); assert(vm.valueRefs == 0 && vm.movieRefs == 1 && session.cleanupOkay());
        assert(vm.calls.back() == base + GfxSession::kMovieRelease);
    }
    for (const bool getter : {false,true}) {
        Mock vm(argv[1]); GfxSession session(vm,base,movie,scratch);
        if (!getter) vm.staleInAdd = true;
        const auto root = session.pin(native);
        if (getter) { assert(root); vm.staleInMember = true; assert(!session.member(root,"transition")); }
        else assert(!root);
        assert(!session.good()); session.close();
        assert(session.cleanupOkay() && vm.valueRefs == 0 && vm.movieRefs == 1);
    }
    {
        Mock vm(argv[1]); GfxSession session(vm,base,movie,scratch);
        const auto root = session.pin(native); vm.falseMember = true;
        assert(!session.member(root,"missing") && vm.valueRefs == 1);
        assert(session.good());
    }
    {
        Mock vm(argv[1]); GfxSession session(vm,base,movie,scratch,3);
        const auto root = session.pin(native);
        assert(session.member(root,"last")); assert(!session.member(root,"budget"));
        session.close(); assert(vm.valueRefs == 0 && vm.movieRefs == 1 && session.cleanupOkay());
    }
    {
        Mock vm(argv[1]); GfxSession session(vm,base,movie,scratch);
        const auto root = session.pin(native);
        std::string text; const char utf8[] = "\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82";
        vm.put(data + 0x100,data + 0x200); vm.write(data + 0x200,utf8,sizeof(utf8));
        vm.returned = {0,0,iface,0x46,data + 0x100,0,0,0};
        const auto s = session.member(root,"label"); assert(session.string(s,text) && text == utf8);
        assert(!session.string(s,text,2) && text.empty());
    }
    {
        Mock vm(argv[1]); GfxSession session(vm,base,movie,scratch);
        const auto root = session.pin(native);
        const auto child = session.member(root,"child"); assert(child && vm.valueRefs == 2);
        vm.rejectedRelease = true;
        session.release(root);
        assert(!session.good() && !session.cleanupOkay() && vm.abandoned);
        const auto calls = vm.calls.size();
        assert(!session.number(42) && !session.undefined() && !session.member(child,"blocked"));
        assert(!session.invoke(child,"blocked") && session.type(root) == 0xFFFFFFFFu);
        assert(vm.calls.size() == calls && vm.get(scratch) == 0xA1100001 && vm.get(scratch + 4) == 0xA1100002);
        vm.rejectedRelease = false; // Other live slots still receive required cleanup.
        session.close();
        assert(!session.cleanupOkay() && vm.abandoned && vm.movieRefs == 2 && vm.valueRefs == 1);
        assert(vm.get(scratch) == 0xA1100001 && vm.get(scratch + 4) == 0xA1100002);
        const auto afterClose = vm.calls.size(); session.close();
        assert(vm.calls.size() == afterClose); // Quarantined release is not retried.
    }
    {
        Mock vm(argv[1]); vm.put(base + GfxSession::kMovieAddRef,0u);
        GfxSession session(vm,base,movie,scratch); assert(!session.good() && vm.calls.empty());
    }
    {
        Mock vm(argv[1]); GfxHandle previous;
        { GfxSession session(vm,base,movie,scratch); previous = session.pin(native); }
        { GfxSession session(vm,base,movie,scratch); assert(session.pin(native));
          assert(!session.member(previous,"stale")); }
        assert(vm.movieRefs == 1 && vm.valueRefs == 0);
    }
    {
        Mock vm(argv[1]); GfxSession session(vm,base,movie,scratch);
        const auto root = session.pin(native); assert(root);
        for (int boundary = 0; boundary < 5000; ++boundary) {
            session.beginInputBoundary();
            const auto child = session.member(root,"child");
            assert(child && session.good() && session.bytesRead() < 128 * 1024);
            session.release(child); assert(vm.valueRefs == 1);
        }
        session.close(); assert(session.cleanupOkay() && vm.movieRefs == 1 && vm.valueRefs == 0);
    }
    std::cout << "PASS: GFx ownership, exact release order, stale scene cleanup, budgets, UTF-8 and handles\n";
}
