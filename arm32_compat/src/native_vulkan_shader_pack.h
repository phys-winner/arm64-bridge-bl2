#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bl2::vk {

using Digest = std::array<std::uint8_t, 32>;
enum class FieldType : std::uint32_t { Vec4 = 1, IVec4 = 2, Bool = 3 };
enum class SamplerType : std::uint32_t { Sampler2D = 1, SamplerCube = 2, Sampler2DShadow = 3 };
struct Field {
    // Indices match the nine legacy constant banks, independently of UBO order.
    std::uint32_t groupIndex = 0;
    FieldType type = FieldType::Vec4;
    std::uint32_t count = 0, offset = 0;
};
struct Sampler {
    std::uint32_t unit = 0, binding = 0;
    SamplerType type = SamplerType::Sampler2D;
};
struct Stage {
    std::uint32_t target = 0, parameterBinding = 0, parameterBytes = 0;
    Digest normalizedDigest{};
    std::vector<Field> fields;
    std::vector<Sampler> samplers;
    std::vector<std::uint32_t> attribute_locations;
    std::vector<std::uint32_t> spirv;
};

class ShaderPack {
public:
    struct Reader {
        void* opaque = nullptr;
        std::uint64_t bytes = 0;
        bool (*read)(void*, std::uint64_t offset, void*, std::size_t bytes) = nullptr;
        // The caller can retain an FD/AAsset here; reads must support absolute offsets.
        std::shared_ptr<void> owner;
    };
    static constexpr std::uint32_t kVertexTarget = 0x8620, kFragmentTarget = 0x8804;
    static constexpr std::size_t kMaxSourceBytes = 128 * 1024;
    bool Open(const Reader& reader, std::string* error = nullptr);
    // Duplicates the descriptor. The retained descriptor closes with this pack.
    bool OpenFileDescriptor(int fd, std::uint64_t base, std::uint64_t bytes,
                            std::string* error = nullptr);
    bool LookupRaw(std::uint32_t target, std::string_view source, Stage& stage,
                   std::string* error = nullptr) const;
    bool LookupDigest(std::uint32_t target, const Digest& normalized, Stage& stage,
                      std::string* error = nullptr) const;
    std::uint32_t StageCount() const { return stageCount_; }
    std::uint32_t RawCount() const { return rawCount_; }
    bool Ready() const { return stageCount_ != 0; }
    Digest Identity() const { return identity_; }

private:
    Reader reader_;
    std::vector<std::uint8_t> index_;
    std::uint32_t rawCount_ = 0, stageCount_ = 0;
    std::size_t stagesOffset_ = 0;
    Digest identity_{};
    bool Load(std::uint32_t index, Stage& stage, std::string* error) const;
};

} // namespace bl2::vk
