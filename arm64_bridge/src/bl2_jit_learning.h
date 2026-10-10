#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// Blocks the Dynarmic cores compiled during play, per warmup role, persisted
// across runs. A later session precompiles every guest path the player has
// already reached (any weapon, enemy, effect or menu), not one fixed capture:
// first contact with combat code no longer pays its translation mid-frame.
namespace bl2::jit {

enum Role : uint8_t { kMain = 0, kCallback = 1, kRenderer = 2, kRoleCount = 3 };

class LearnedProfile {
public:
    // Blocks average ~150 emitted bytes: one role stays near 60 MiB of the
    // 128 MiB cache; warming also stops while less than 32 MiB is free.
    static constexpr size_t kMaxPerRole = 400000;
    static constexpr uint32_t kSingleStepBit = 4;  // In the descriptor's upper word.

    using CodeCheck = std::function<bool(uint32_t pc)>;

    // Identity-bound file; a different guest, size or malformed list loads empty.
    void Load(const std::string& path, uint32_t guest_bytes,
              const std::array<uint8_t, 32>& guest_sha, CodeCheck read_only_code);
    // Thread-safe; called on the emitting thread after each new block.
    void Record(Role role, uint64_t descriptor);
    // Writes the union of loaded and recorded descriptors when it changed.
    bool Save();
    // Descriptors loaded at startup in discovery order (earliest first, i.e.
    // the common paths of earlier runs before situational ones), for warming.
    const std::vector<uint64_t>& Loaded(Role role) const { return loaded_[role]; }
    size_t RecordedThisRun() const;

private:
    mutable std::mutex mutex_;
    std::string path_;
    uint32_t guest_bytes_ = 0;
    std::array<uint8_t, 32> guest_sha_{};
    CodeCheck read_only_code_;
    std::array<std::vector<uint64_t>, kRoleCount> loaded_;
    std::array<std::unordered_set<uint64_t>, kRoleCount> known_;
    std::array<std::vector<uint64_t>, kRoleCount> order_;  // Discovery order.
    size_t recorded_ = 0;
    bool dirty_ = false;
    bool ready_ = false;
};

// Encoding shared by Load/Save and the host test: magic "BL2JITL2", guest
// bytes, guest SHA-256, three role counts, then unique descriptors per role in
// discovery order.
std::vector<uint8_t> EncodeLearned(uint32_t guest_bytes, const std::array<uint8_t, 32>& sha,
                                   const std::array<std::vector<uint64_t>, kRoleCount>& roles);
bool DecodeLearned(const std::vector<uint8_t>& bytes, uint32_t guest_bytes,
                   const std::array<uint8_t, 32>& sha,
                   const LearnedProfile::CodeCheck& read_only_code,
                   std::array<std::vector<uint64_t>, kRoleCount>& roles);

}  // namespace bl2::jit
