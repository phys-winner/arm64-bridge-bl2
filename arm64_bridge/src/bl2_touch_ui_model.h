#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bl2::touch {

// A read-only bridge. The implementation never stores this view or guest pointers
// after capture and never calls guest code. Only use at the input-thread boundary.
struct UiModelMemory {
    const void* context = nullptr;
    bool (*copy)(const void*, std::uint32_t, void*, std::size_t) = nullptr;
};

struct UiPropertyWitness {
    std::string name;
    std::uint32_t property = 0, offset = 0, elementSize = 0, extra = 0;
};

struct UiDialogButtonModel {
    std::string tag;
    std::vector<std::string> keys;
};

struct UiModelSnapshot {
    bool valid = false;
    std::string reason, movieClass, providerClass;
    std::uint64_t owner = 0, list = 0, provider = 0;
    // A diagnostic structural fingerprint, NOT a complete widget/content token.
    // Labels, enabled state, GFx renderers and asynchronous refresh are not covered.
    std::uint64_t structure = 0;
    std::int32_t selectedIndex = -1;
    std::uint32_t providerDepth = 0;
    bool noCancel = false;
    std::vector<std::int32_t> eventIds;
    std::vector<UiDialogButtonModel> buttons;
    std::vector<UiPropertyWitness> properties;
    std::size_t bytesRead = 0;
};

// base and owner belong to the exact SHA-bound ELF guarded by stage1 Install.
// Runtime layout comes from live UProperty metadata, not guessed game offsets.
UiModelSnapshot CaptureBl2UiModel(const UiModelMemory&, std::uint32_t base,
                                 std::uint32_t owner);

} // namespace bl2::touch
