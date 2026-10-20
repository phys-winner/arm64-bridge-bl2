#include "native_vulkan_shader_pack.h"
#include "obb_sha256.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#if defined(_WIN32)
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

using bl2::vk::Digest;
using bl2::vk::ShaderPack;
using bl2::vk::Stage;
using Bytes = std::vector<std::uint8_t>;
namespace {
constexpr std::string_view kRules = "bl2-vulkan-1:std140-private-stage-parameters;explicit-sampler-bindings;gl-depth-to-vk;original-image-row-order";
Digest Hash(const void* bytes, std::size_t size) {
    ObbSha256 hash;
    assert(hash.Update(static_cast<const std::uint8_t*>(bytes), size));
    return hash.Finalize();
}
void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned n = 0; n < 4; ++n) bytes.at(at + n) = static_cast<std::uint8_t>(value >> (8 * n));
}
void Put64(Bytes& bytes, std::size_t at, std::uint64_t value) {
    for (unsigned n = 0; n < 8; ++n) bytes.at(at + n) = static_cast<std::uint8_t>(value >> (8 * n));
}
std::uint32_t U32(const std::uint8_t* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}
void PutDigest(Bytes& bytes, std::size_t at, const Digest& digest) {
    std::copy(digest.begin(), digest.end(), bytes.begin() + at);
}
void Reindex(Bytes& bytes) { PutDigest(bytes, 184, Hash(bytes.data() + 256, 240)); }
void Restage(Bytes& bytes, bool vertex) {
    const std::size_t start = vertex ? 628 : 496, record = vertex ? 416 : 336;
    PutDigest(bytes, record + 48, Hash(bytes.data() + start, 132));
    Reindex(bytes);
}
constexpr std::string_view kVertexRaw = "//ShaderFileName:vertex\nvoid main(){}";
constexpr std::string_view kFragmentRaw = "//ShaderFileName:fragment\nvoid main(){}";
Digest VertexDigest() { return Hash("normalized vertex", 17); }
Digest FragmentDigest() { return Hash("normalized fragment", 19); }
Bytes Fixture() {
    Bytes bytes(760);
    std::memcpy(bytes.data(), "BL2VSP1\0", 8);
    Put32(bytes, 8, 1); Put32(bytes, 12, 256); Put32(bytes, 16, 2); Put32(bytes, 20, 2);
    Put64(bytes, 24, bytes.size()); Put64(bytes, 32, 256); Put64(bytes, 40, 336); Put64(bytes, 48, 496);
    constexpr std::string_view catalogue = "d26172dac51eb373d7590ab76ea7b5bae59b74cbb72684f62c64e3f99d37ce91";
    const auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (std::size_t i = 0; i < 32; ++i) bytes[56 + i] = static_cast<std::uint8_t>((hex(catalogue[i * 2]) << 4) | hex(catalogue[i * 2 + 1]));
    PutDigest(bytes, 88, Hash(kRules.data(), kRules.size()));
    PutDigest(bytes, 120, Hash("tools", 5)); PutDigest(bytes, 152, Hash("raw mapping", 11)); PutDigest(bytes, 216, Hash("manifest", 8));
    Put32(bytes, 256, 0x8620); PutDigest(bytes, 260, Hash(kVertexRaw.data(), kVertexRaw.size())); Put32(bytes, 292, 1);
    Put32(bytes, 296, 0x8804); PutDigest(bytes, 300, Hash(kFragmentRaw.data(), kFragmentRaw.size())); Put32(bytes, 332, 0);
    for (const bool vertex : {false, true}) {
        const std::size_t record = vertex ? 416 : 336, body = vertex ? 628 : 496;
        Put32(bytes, record, vertex ? 0x8b31U : 0x8b30U); Put32(bytes, record + 4, 132); Put64(bytes, record + 8, body);
        PutDigest(bytes, record + 16, vertex ? VertexDigest() : FragmentDigest());
        Put32(bytes, body, 1); Put32(bytes, body + 4, 2); Put32(bytes, body + 8, 1); Put32(bytes, body + 12, vertex ? 1U : 0U);
        Put32(bytes, body + 16, vertex ? 0U : 1U); Put32(bytes, body + 20, vertex ? 80U : 64U);
        Put32(bytes, body + 24, vertex ? 52U : 56U); Put32(bytes, body + 28, vertex ? 80U : 76U);
        Put32(bytes, body + 32, vertex ? 4U : 6U); Put32(bytes, body + 36, vertex ? 3U : 1U);
        Put32(bytes, body + 40, vertex ? 2U : 3U); Put32(bytes, body + 44, 0);
        Put32(bytes, body + 48, vertex ? 1U : 8U); Put32(bytes, body + 52, vertex ? 1U : 3U);
        Put32(bytes, body + 56, vertex ? 3U : 1U); Put32(bytes, body + 60, vertex ? 32U : 48U);
        Put32(bytes, body + 64, 3); Put32(bytes, body + 68, vertex ? 2U : 3U); Put32(bytes, body + 72, vertex ? 5U : 21U);
        if (vertex) Put32(bytes, body + 76, 2);
        const std::size_t spirv = body + (vertex ? 80 : 76);
        Put32(bytes, spirv, 0x07230203); Put32(bytes, spirv + 4, 0x10300); Put32(bytes, spirv + 12, 2);
        Restage(bytes, vertex);
    }
    return bytes;
}
struct Memory {
    Bytes bytes;
    std::size_t maximumRead = 0, calls = 0;
    ShaderPack::Reader Reader() {
        return {this, bytes.size(), [](void* opaque, std::uint64_t at, void* output, std::size_t size) {
            auto& memory = *static_cast<Memory*>(opaque);
            ++memory.calls;
            memory.maximumRead = std::max(memory.maximumRead, size);
            if (at > memory.bytes.size() || size > memory.bytes.size() - static_cast<std::size_t>(at)) return false;
            std::memcpy(output, memory.bytes.data() + at, size);
            return true;
        }, {}};
    }
};
void Rejected(Bytes bytes, bool indexFailure = true, bool vertex = false) {
    Memory memory{std::move(bytes)};
    ShaderPack pack;
    Stage stage;
    std::string error;
    if (indexFailure) assert(!pack.Open(memory.Reader(), &error) && !pack.Ready());
    else {
        assert(pack.Open(memory.Reader(), &error));
        assert(!pack.LookupDigest(vertex ? 0x8620U : 0x8804U, vertex ? VertexDigest() : FragmentDigest(), stage, &error));
        assert(stage.spirv.empty() && stage.fields.empty());
    }
    assert(!error.empty());
}
} // namespace

int main(int argc, char** argv) {
    Memory memory{Fixture()};
    ShaderPack pack;
    std::string error;
    Stage vertex, fragment;
    assert(pack.Open(memory.Reader(), &error) && pack.StageCount() == 2 && pack.RawCount() == 2 && memory.calls == 2);
    assert(pack.LookupRaw(0x8620, kVertexRaw, vertex, &error));
    assert(pack.LookupRaw(0x8804, kFragmentRaw, fragment, &error));
    assert(vertex.normalizedDigest == VertexDigest() && fragment.normalizedDigest == FragmentDigest());
    assert(vertex.parameterBinding == 0 && fragment.parameterBinding == 1);
    assert(vertex.fields[0].groupIndex == 4 && vertex.fields[1].offset == 32 && fragment.fields[1].offset == 48);
    assert(vertex.samplers[0].binding == 5 && fragment.samplers[0].binding == 21);
    assert(vertex.attribute_locations == std::vector<std::uint32_t>{2} && fragment.attribute_locations.empty());
    assert(memory.maximumRead == 256);
    Stage failed;
    assert(!pack.LookupRaw(0x8804, kVertexRaw, failed, &error));
    assert(!pack.LookupRaw(0x8620, "void main(){}", failed, &error));
    assert(!pack.LookupRaw(0x8620, std::string_view("a\0b", 3), failed, &error));
    assert(!pack.LookupDigest(0x8b31, VertexDigest(), failed, &error));
    assert(pack.LookupDigest(0x8620, VertexDigest(), failed, &error));
    for (const auto at : {0U, 8U, 12U, 16U, 20U, 24U, 32U, 40U, 48U, 56U, 88U, 120U, 152U, 184U, 216U, 248U}) {
        auto bytes = Fixture();
        if (at == 120 || at == 152 || at == 216) std::fill_n(bytes.begin() + at, 32, std::uint8_t{0});
        else bytes[at] ^= 0x80;
        Rejected(std::move(bytes));
    }
    auto bytes = Fixture(); bytes.pop_back(); Rejected(bytes);
    bytes = Fixture(); bytes.push_back(0); Put64(bytes, 24, bytes.size()); Rejected(bytes);
    bytes = Fixture(); Put32(bytes, 256, 0x8b31); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); Put32(bytes, 292, 0); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); Put32(bytes, 332, 2); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); std::copy_n(bytes.begin() + 256, 40, bytes.begin() + 296); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); std::copy_n(bytes.begin() + 336, 80, bytes.begin() + 416); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); Put64(bytes, 344, 497); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); Put32(bytes, 340, std::numeric_limits<std::uint32_t>::max()); Reindex(bytes); Rejected(bytes);
    bytes = Fixture(); bytes[bytes.size() - 1] ^= 1; Rejected(bytes, false, true);
    for (const auto& [at, value] : {std::pair{500U, 0U}, {504U, 17U}, {508U, 1U}, {512U, 0U}, {516U, 65537U},
                                     {520U, 19U}, {524U, 999U}, {528U, 1U}, {532U, 2U}, {536U, 4097U},
                                     {540U, 16U}, {560U, 16U}, {564U, 0U}, {568U, 5U}, {572U, 0U}}) {
        bytes = Fixture(); Put32(bytes, at, value); Restage(bytes, false); Rejected(bytes, false);
    }
    auto owner = std::make_shared<Memory>(Memory{Fixture()});
    const std::weak_ptr<Memory> weak = owner;
    auto reader = owner->Reader(); reader.owner = owner;
    ShaderPack retained;
    assert(retained.Open(reader)); reader = {}; owner.reset();
    assert(!weak.expired() && retained.LookupRaw(0x8620, kVertexRaw, vertex));
    retained = {}; assert(weak.expired());
    std::puts("PASS: native loader exact raw/normalized target identities, private field layouts, bounded reads, retained asset ownership and malformed/corrupt/truncated assets");
    if (argc == 2) {
        std::ifstream file(argv[1], std::ios::binary);
        assert(file.good());
        Bytes header(256); file.read(reinterpret_cast<char*>(header.data()), 256); assert(file.good());
        const auto rawCount = U32(header.data() + 16), stageCount = U32(header.data() + 20);
        assert(rawCount <= 100000 && stageCount <= 50000);
        file.seekg(256 + std::uint64_t(rawCount) * 40);
        Bytes index(std::size_t(stageCount) * 80); file.read(reinterpret_cast<char*>(index.data()), static_cast<std::streamsize>(index.size())); assert(file.good());
        const auto size = std::uint64_t(std::filesystem::file_size(argv[1]));
        ShaderPack actual;
#if defined(_WIN32)
        int fd = -1;
        assert(::_sopen_s(&fd, argv[1], _O_RDONLY | _O_BINARY, _SH_DENYNO, 0) == 0);
#else
        const auto fd = ::open(argv[1], O_RDONLY);
#endif
        assert(fd >= 0 && actual.OpenFileDescriptor(fd, 0, size, &error));
#if defined(_WIN32)
        ::_close(fd);
#else
        ::close(fd);
#endif
        std::size_t spirvBytes = 0;
        for (std::uint32_t n = 0; n < stageCount; ++n) {
            const auto* record = index.data() + std::size_t(n) * 80;
            Digest digest{}; std::copy_n(record + 16, 32, digest.begin());
            Stage stage;
            assert(actual.LookupDigest(U32(record) == 0x8b31 ? 0x8620 : 0x8804, digest, stage, &error));
            assert(!stage.spirv.empty()); spirvBytes += stage.spirv.size() * 4;
        }
        std::printf("PASS: actual immutable offline asset, every %u normalized stage loaded and verified, %zu SPIR-V bytes; no GPU modules created\n", stageCount, spirvBytes);
    }
}
