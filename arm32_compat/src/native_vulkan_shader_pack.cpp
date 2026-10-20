#include "native_vulkan_shader_pack.h"
#include "obb_sha256.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#if defined(_WIN32)
#include <io.h>
#include <mutex>
#else
#include <unistd.h>
#endif

namespace bl2::vk {
namespace {
constexpr std::size_t kHeaderBytes = 256, kRawBytes = 40, kIndexBytes = 80;
constexpr std::uint32_t kMaxRaw = 100000, kMaxStages = 50000, kMaxSpirv = 1024 * 1024;
constexpr std::uint64_t kMaxPack = 1024ULL * 1024 * 1024;
constexpr std::string_view kRules = "bl2-vulkan-1:std140-private-stage-parameters;explicit-sampler-bindings;gl-depth-to-vk;original-image-row-order";
constexpr std::string_view kCatalogueHex = "d26172dac51eb373d7590ab76ea7b5bae59b74cbb72684f62c64e3f99d37ce91";

std::uint32_t U32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
           (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}
std::uint64_t U64(const std::uint8_t* data) {
    return U32(data) | (std::uint64_t(U32(data + 4)) << 32);
}
Digest Hash(const void* data, std::size_t bytes) {
    ObbSha256 hash;
    hash.Update(static_cast<const std::uint8_t*>(data), bytes);
    return hash.Finalize();
}
bool Fail(std::string* error, const char* text) {
    if (error) *error = text;
    return false;
}
bool Same(const std::uint8_t* bytes, const Digest& digest) {
    return std::equal(digest.begin(), digest.end(), bytes);
}
bool Nonzero(const std::uint8_t* bytes) {
    return std::any_of(bytes, bytes + 32, [](auto b) { return b != 0; });
}
std::uint32_t GlTarget(std::uint32_t arb) {
    return arb == ShaderPack::kVertexTarget ? 0x8b31 : arb == ShaderPack::kFragmentTarget ? 0x8b30 : 0;
}
int Compare(const std::uint8_t* record, std::size_t digestOffset,
            std::uint32_t target, const std::uint8_t* digest) {
    const auto recordedTarget = U32(record);
    if (recordedTarget != target) return recordedTarget < target ? -1 : 1;
    return std::memcmp(record + digestOffset, digest, 32);
}
bool Read(const ShaderPack::Reader& reader, std::uint64_t offset, void* output, std::size_t bytes) {
    return reader.read && offset <= reader.bytes && bytes <= reader.bytes - offset &&
           reader.read(reader.opaque, offset, output, bytes);
}
struct Descriptor {
    int fd = -1;
    std::uint64_t base = 0;
#if defined(_WIN32)
    std::mutex mutex;
#endif
    ~Descriptor() {
#if defined(_WIN32)
        if (fd >= 0) ::_close(fd);
#else
        if (fd >= 0) ::close(fd);
#endif
    }
};
} // namespace

bool ShaderPack::Open(const Reader& reader, std::string* error) {
    reader_ = {};
    index_.clear();
    stageCount_ = rawCount_ = 0;
    stagesOffset_ = 0;
    identity_ = {};
    if (error) error->clear();
    try {
        std::array<std::uint8_t, kHeaderBytes> header{};
        if (reader.bytes <= kHeaderBytes || reader.bytes > kMaxPack || !Read(reader, 0, header.data(), header.size()))
            return Fail(error, "Vulkan shader pack header cannot be read within asset bounds");
        if (std::memcmp(header.data(), "BL2VSP1\0", 8) || U32(header.data() + 8) != 1 ||
            U32(header.data() + 12) != kHeaderBytes || U64(header.data() + 24) != reader.bytes ||
            U64(header.data() + 248) != 0)
            return Fail(error, "Vulkan shader pack header/version/length is invalid");
        const auto rawCount = U32(header.data() + 16), stageCount = U32(header.data() + 20);
        if (!rawCount || rawCount > kMaxRaw || !stageCount || stageCount > kMaxStages || rawCount < stageCount)
            return Fail(error, "Vulkan shader pack index counts exceed bounds");
        const std::size_t rawSize = std::size_t(rawCount) * kRawBytes;
        const std::size_t indexBytes = rawSize + std::size_t(stageCount) * kIndexBytes;
        const std::uint64_t dataStart = kHeaderBytes + indexBytes;
        if (dataStart >= reader.bytes || U64(header.data() + 32) != kHeaderBytes ||
            U64(header.data() + 40) != kHeaderBytes + rawSize || U64(header.data() + 48) != dataStart)
            return Fail(error, "Vulkan shader pack index offsets are invalid");
        Digest catalogue{};
        for (std::size_t i = 0; i < catalogue.size(); ++i) {
            const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
            catalogue[i] = static_cast<std::uint8_t>((digit(kCatalogueHex[i * 2]) << 4) | digit(kCatalogueHex[i * 2 + 1]));
        }
        if (!Same(header.data() + 56, catalogue) || !Same(header.data() + 88, Hash(kRules.data(), kRules.size())) ||
            !Nonzero(header.data() + 120) || !Nonzero(header.data() + 152) || !Nonzero(header.data() + 216))
            return Fail(error, "Vulkan shader pack catalogue/rules/source identity is invalid");
        std::vector<std::uint8_t> index(indexBytes);
        if (!Read(reader, kHeaderBytes, index.data(), index.size()) || !Same(header.data() + 184, Hash(index.data(), index.size())))
            return Fail(error, "Vulkan shader pack index digest is invalid");
        std::uint64_t offset = dataStart;
        for (std::uint32_t i = 0; i < stageCount; ++i) {
            const auto* record = index.data() + rawSize + std::size_t(i) * kIndexBytes;
            const auto target = U32(record), bytes = U32(record + 4);
            if ((target != 0x8b30 && target != 0x8b31) || bytes < 68 || bytes > kMaxSpirv + 512 ||
                U64(record + 8) != offset || bytes > reader.bytes - offset || !Nonzero(record + 16) ||
                !Nonzero(record + 48) || (i && Compare(record - kIndexBytes, 16, target, record + 16) >= 0))
                return Fail(error, "Vulkan shader pack stage index/target/bounds/order is invalid");
            offset += bytes;
        }
        if (offset != reader.bytes)
            return Fail(error, "Vulkan shader pack contains trailing or missing payload bytes");
        std::vector<bool> covered(stageCount, false);
        for (std::uint32_t i = 0; i < rawCount; ++i) {
            const auto* record = index.data() + std::size_t(i) * kRawBytes;
            const auto target = GlTarget(U32(record)), stage = U32(record + 36);
            if (!target || stage >= stageCount || !Nonzero(record + 4) ||
                U32(index.data() + rawSize + std::size_t(stage) * kIndexBytes) != target ||
                (i && Compare(record - kRawBytes, 4, U32(record), record + 4) >= 0))
                return Fail(error, "Vulkan shader pack raw source mapping/target/order is invalid");
            covered[stage] = true;
        }
        if (std::find(covered.begin(), covered.end(), false) != covered.end())
            return Fail(error, "Vulkan shader pack source identity coverage is incomplete");
        reader_ = reader;
        index_ = std::move(index);
        rawCount_ = rawCount;
        stageCount_ = stageCount;
        stagesOffset_ = rawSize;
        identity_ = Hash(header.data(), header.size());
        return true;
    } catch (...) {
        return Fail(error, "Vulkan shader pack index allocation or reader failed");
    }
}

bool ShaderPack::OpenFileDescriptor(int fd, std::uint64_t base, std::uint64_t bytes, std::string* error) {
    if (fd < 0 || base > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
        bytes > std::uint64_t(std::numeric_limits<std::int64_t>::max()) - base)
        return Fail(error, "Vulkan shader pack descriptor bounds are invalid");
    auto descriptor = std::make_shared<Descriptor>();
#if defined(_WIN32)
    descriptor->fd = ::_dup(fd);
#else
    descriptor->fd = ::dup(fd);
#endif
    descriptor->base = base;
    if (descriptor->fd < 0) return Fail(error, "Vulkan shader pack descriptor cannot be duplicated");
    Reader reader{descriptor.get(), bytes,
        [](void* opaque, std::uint64_t offset, void* output, std::size_t size) {
            auto& descriptor = *static_cast<Descriptor*>(opaque);
#if defined(_WIN32)
            std::lock_guard lock(descriptor.mutex);
            if (::_lseeki64(descriptor.fd, static_cast<std::int64_t>(descriptor.base + offset), SEEK_SET) < 0) return false;
#endif
            auto* bytes = static_cast<std::uint8_t*>(output);
            std::size_t used = 0;
            while (used < size) {
#if defined(_WIN32)
                const auto n = ::_read(descriptor.fd, bytes + used, static_cast<unsigned int>(std::min<std::size_t>(size - used, INT_MAX)));
#else
                const auto n = ::pread64(descriptor.fd, bytes + used, size - used,
                                         static_cast<off64_t>(descriptor.base + offset + used));
#endif
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return false;
                used += static_cast<std::size_t>(n);
            }
            return true;
        }, descriptor};
    return Open(reader, error);
}

bool ShaderPack::Load(std::uint32_t number, Stage& stage, std::string* error) const {
    stage = {};
    if (error) error->clear();
    if (!Ready() || number >= stageCount_) return Fail(error, "Vulkan shader pack is not ready");
    try {
        const auto* record = index_.data() + stagesOffset_ + std::size_t(number) * kIndexBytes;
        std::vector<std::uint8_t> bytes(U32(record + 4));
        if (!Read(reader_, U64(record + 8), bytes.data(), bytes.size()) || !Same(record + 48, Hash(bytes.data(), bytes.size())))
            return Fail(error, "Vulkan shader stage payload is truncated or its digest differs");
        const auto* header = bytes.data();
        const bool vertex = U32(record) == 0x8b31;
        const auto fields = U32(header + 4), samplers = U32(header + 8), attrs = U32(header + 12);
        const auto binding = U32(header + 16), parameters = U32(header + 20);
        const auto spirvBytes = U32(header + 24), layoutBytes = U32(header + 28);
        if (U32(header) != 1 || !fields || fields > (vertex ? 5U : 4U) || samplers > 16 || attrs > 16 ||
            (!vertex && attrs) || binding != (vertex ? 0U : 1U) || !parameters || parameters > 65536 ||
            layoutBytes != 32 + fields * 16 + samplers * 12 + attrs * 4 || spirvBytes < 20 ||
            spirvBytes > kMaxSpirv || (spirvBytes & 3) || layoutBytes > bytes.size() || spirvBytes != bytes.size() - layoutBytes)
            return Fail(error, "Vulkan shader stage layout counts or byte ranges are invalid");
        Stage result;
        result.target = vertex ? kVertexTarget : kFragmentTarget;
        result.parameterBinding = binding;
        result.parameterBytes = parameters;
        std::copy_n(record + 16, 32, result.normalizedDigest.begin());
        std::uint32_t groupMask = 0, expectedOffset = 0;
        const auto* cursor = header + 32;
        for (std::uint32_t i = 0; i < fields; ++i, cursor += 16) {
            const auto group = U32(cursor), type = U32(cursor + 4), count = U32(cursor + 8), offset = U32(cursor + 12);
            if (group >= 9 || ((group < 5) != vertex) || (groupMask & (1U << group)) || !count || count > 4096 ||
                offset != expectedOffset || count * 16 > parameters - expectedOffset ||
                type != (group == 3 || group == 7 ? 2U : group == 4 || group == 8 ? 3U : 1U))
                return Fail(error, "Vulkan shader stage private uniform field identity/count/offset is invalid");
            result.fields.push_back({group, static_cast<FieldType>(type), count, offset});
            groupMask |= 1U << group;
            expectedOffset += count * 16;
        }
        if (expectedOffset != parameters) return Fail(error, "Vulkan shader stage uniform range is incomplete");
        std::uint32_t unitMask = 0;
        for (std::uint32_t i = 0; i < samplers; ++i, cursor += 12) {
            const auto unit = U32(cursor), type = U32(cursor + 4), samplerBinding = U32(cursor + 8);
            if (unit >= 16 || (unitMask & (1U << unit)) || !type || type > 3 || samplerBinding != unit + (vertex ? 2U : 18U))
                return Fail(error, "Vulkan shader stage sampler resource identity is invalid");
            result.samplers.push_back({unit, samplerBinding, static_cast<SamplerType>(type)});
            unitMask |= 1U << unit;
        }
        std::uint32_t attributeMask = 0;
        for (std::uint32_t i = 0; i < attrs; ++i, cursor += 4) {
            const auto location = U32(cursor);
            if (location >= 16 || (attributeMask & (1U << location)))
                return Fail(error, "Vulkan shader stage attribute identity is invalid");
            result.attribute_locations.push_back(location);
            attributeMask |= 1U << location;
        }
        if (U32(cursor) != 0x07230203 || U32(cursor + 4) < 0x10000 || U32(cursor + 4) > 0x10300 ||
            !U32(cursor + 12) || U32(cursor + 12) > 1048576 || U32(cursor + 16) != 0)
            return Fail(error, "Vulkan shader stage SPIR-V header is invalid");
        result.spirv.resize(spirvBytes / 4);
        for (std::size_t i = 0; i < result.spirv.size(); ++i) result.spirv[i] = U32(cursor + i * 4);
        stage = std::move(result);
        return true;
    } catch (...) {
        return Fail(error, "Vulkan shader stage allocation or reader failed");
    }
}

bool ShaderPack::LookupRaw(std::uint32_t target, std::string_view source, Stage& stage, std::string* error) const {
    stage = {};
    if (!Ready() || !GlTarget(target) || source.empty() || source.size() > kMaxSourceBytes || source.find('\0') != source.npos)
        return Fail(error, "Vulkan raw shader target/source bounds are invalid");
    const auto digest = Hash(source.data(), source.size());
    std::uint32_t low = 0, high = rawCount_;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        const auto* record = index_.data() + std::size_t(middle) * kRawBytes;
        const auto comparison = Compare(record, 4, target, digest.data());
        if (comparison < 0) low = middle + 1;
        else if (comparison > 0) high = middle;
        else return Load(U32(record + 36), stage, error);
    }
    return Fail(error, "Exact raw Vulkan shader source identity is absent from the complete offline pack");
}

bool ShaderPack::LookupDigest(std::uint32_t target, const Digest& digest, Stage& stage, std::string* error) const {
    stage = {};
    const auto gl = GlTarget(target);
    if (!Ready() || !gl) return Fail(error, "Vulkan normalized shader target is invalid");
    std::uint32_t low = 0, high = stageCount_;
    while (low < high) {
        const auto middle = low + (high - low) / 2;
        const auto* record = index_.data() + stagesOffset_ + std::size_t(middle) * kIndexBytes;
        const auto comparison = Compare(record, 16, gl, digest.data());
        if (comparison < 0) low = middle + 1;
        else if (comparison > 0) high = middle;
        else return Load(middle, stage, error);
    }
    return Fail(error, "Normalized Vulkan shader identity is absent from the complete offline pack");
}

} // namespace bl2::vk
