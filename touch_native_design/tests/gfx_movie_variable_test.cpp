#include "bl2_touch_gfx_values.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <unordered_map>
using namespace bl2::touch;
namespace {
constexpr std::uint32_t base = 0x10000000, movie = 0x23000000, movieTable = 0x23001000;
constexpr std::uint32_t iface = 0x23002000, valueTable = 0x23003000, scratch = 0x23004000;
constexpr std::uint32_t release = base + 0x02000000;
struct Vm final : GfxTransport {
    std::unordered_map<std::uint32_t,unsigned char> memory;
    bool active = true, abandoned = false, returnFalse = false, changeScene = false;
    unsigned refs = 0, movieRefs = 1, variables = 0;
    std::string path;
    explicit Vm(const char* fixture) {
        std::ifstream in(fixture); assert(in.good());
        std::uint32_t address = 0, value = 0;
        while (in >> std::hex >> address >> value) put(base + address,value);
        put(movie,movieTable); put(movie + 4,1u); put(movieTable + 4,release + 4);
        put(iface,valueTable); put(valueTable + 12,release);
    }
    template<class T> void put(std::uint32_t p, const T& v) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
        for (std::size_t i = 0; i < sizeof(v); ++i) memory[p + static_cast<std::uint32_t>(i)] = bytes[i];
    }
    bool read(std::uint32_t p, void* out, std::size_t n) const override {
        auto* bytes = static_cast<unsigned char*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            auto it = memory.find(p + static_cast<std::uint32_t>(i)); if (it == memory.end()) return false;
            bytes[i] = it->second;
        }
        return true;
    }
    bool write(std::uint32_t p, const void* value, std::size_t n) override {
        if (p < scratch || p - scratch > GfxSession::kScratchBytes || n > GfxSession::kScratchBytes - (p - scratch)) return false;
        const auto* bytes = static_cast<const unsigned char*>(value);
        for (std::size_t i = 0; i < n; ++i) memory[p + static_cast<std::uint32_t>(i)] = bytes[i];
        return true;
    }
    bool executable(std::uint32_t p) const override { return p >= base + 0xE823B0 && p < base + 0x2BB91EC; }
    bool current() const override { return active && !abandoned; }
    void abandonScratch() override { abandoned = true; }
    bool call(std::uint32_t pc, const std::vector<std::uint32_t>& args, std::uint32_t& out) override {
        out = 1;
        if (pc == base + GfxSession::kMovieAddRef) { assert(args[0] == movie); ++movieRefs; }
        else if (pc == base + GfxSession::kMovieRelease) { assert(!refs && movieRefs == 2); --movieRefs; }
        else if (pc == base + GfxSession::kGetVariable) {
            assert(args.size() == 3 && args[0] == movie && movieRefs == 2);
            ++variables; path.clear(); char c = 0;
            for (auto p = args[2]; read(p,&c,1) && c; ++p) path += c;
            const std::array<std::uint32_t,8> value{0xA1100001,0xA1100002,iface,0x4A,0x23010000,0,0,0};
            assert(write(args[1],value.data(),32)); ++refs;
            if (returnFalse) out = 0;
            if (changeScene) active = false;
        } else if (pc == release) {
            assert(args.size() == 3 && args[0] == iface && args[2] == 0x23010000 && refs == 1 && movieRefs == 2);
            std::uint32_t link = 0; assert(read(args[1],&link,4) && link == 0xA1100001); --refs;
        } else assert(false);
        return true;
    }
};
}
int main(int argc, char** argv) {
    assert(argc == 2);
    for (int mode = 0; mode < 3; ++mode) {
        Vm vm(argv[1]); vm.returnFalse = mode == 1; vm.changeScene = mode == 2;
        {
            GfxSession session(vm,base,movie,scratch);
            const auto value = session.variable("_root.button0");
            assert(vm.path == "_root.button0" && vm.variables == 1);
            assert(bool(value) == (mode == 0));
            if (mode == 0) {
                assert(session.type(value) == 10 && vm.refs == 1);
                session.release(value); assert(!vm.refs);
            }
        }
        assert(!vm.refs && vm.movieRefs == 1 && !vm.abandoned);
    }
    {
        Vm vm(argv[1]); GfxSession session(vm,base,movie,scratch);
        const std::string tooLong(128,'x');
        assert(!session.variable("") && !session.variable(tooLong.c_str()) && !vm.variables);
        vm.active = false; assert(!session.variable("_root.button0") && !vm.variables);
    }
    std::cout << "PASS: direct Movie variable ABI, owned outputs on false/stale results and bounded paths\n";
}
