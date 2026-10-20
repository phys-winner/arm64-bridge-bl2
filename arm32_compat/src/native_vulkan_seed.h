#pragma once
// UNAPPLIED PROPOSAL. No game asset or approved effect coverage accompanies it.
#include "native_vulkan_shader_pack.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace bl2vk::seed {
using Digest = bl2::vk::Digest;
constexpr std::size_t kHeaderBytes = 232, kRecipeBytes = 460;
constexpr std::size_t kMaxMandatory = 512, kMaxCandidates = 16384;
constexpr std::size_t kPostDrawHeaderBytes = 168, kPostDrawRecordBytes = 500;
constexpr std::uint32_t kRecipeRevision = 3;
constexpr std::uint64_t kDepthClamp = 1, kDepthBounds = 2;
struct Recipe {
    Digest vertex{}, fragment{};
    std::array<std::uint32_t,4> colors{};
    std::uint32_t colorCount{}, depthFormat{}, topology{}, restart{}, attributeCount{};
    std::array<std::array<std::uint32_t,4>,16> attributes{};
    std::array<std::uint32_t,26> state{};
};
static_assert(sizeof(Recipe) == kRecipeBytes);
struct Request {
    std::span<const std::uint8_t> bytes;
    Digest content{}, scope{}, receipt{};
};
struct Identity {
    Digest backend{}, pack{}, content{}, scope{}, receipt{};
    std::uint64_t availableFeatures{};
};
struct PostDraw {
    Recipe recipe{};
    std::uint64_t firstDraw{}, lastDraw{}, uses{}, firstNs{}, lastNs{};
};
struct PostDrawRun {
    Digest backend{}, pack{};
    std::uint32_t pid{};
    bool overflow{};
    std::uint64_t startNs{}, rejectedDraws{}, lateMandatoryCreates{}, runtimeCreates{}, startupCreates{};
};
Digest ParseDigest(const char* hex);
std::array<std::uint8_t,kRecipeBytes> EncodeRecipe(const Recipe& recipe);
Recipe DecodeRecipe(std::span<const std::uint8_t,kRecipeBytes> bytes);
bool CanonicalRecipe(const Recipe& recipe);
std::uint64_t RequiredFeatures(const Recipe& recipe);
// Checks the complete envelope, identities, hash, ordering and structural state.
// Device formats + exact live stage-input coverage must also pass before modules.
std::vector<Recipe> Decode(std::span<const std::uint8_t> bytes, const Identity& expected);
std::vector<std::uint8_t> EncodePostDraw(std::span<const PostDraw> records, const PostDrawRun& run);
} // namespace bl2vk::seed
