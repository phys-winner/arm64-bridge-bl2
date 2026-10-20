#include "bl2_jit_learning.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace bl2::jit {
namespace {

constexpr char kMagic[8] = {'B', 'L', '2', 'J', 'I', 'T', 'L', '2'};
constexpr size_t kHeaderBytes = 56;  // magic8 bytes4 sha32 counts3x4.
constexpr size_t kMaxFileBytes = kHeaderBytes + 8 * LearnedProfile::kMaxPerRole * kRoleCount;

uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Read64(const uint8_t* p) { return Read32(p) | uint64_t(Read32(p + 4)) << 32; }
void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
void Put64(std::vector<uint8_t>& out, uint64_t value) {
    Put32(out, static_cast<uint32_t>(value));
    Put32(out, static_cast<uint32_t>(value >> 32));
}

bool Eligible(uint64_t descriptor, const LearnedProfile::CodeCheck& read_only_code) {
    const uint32_t upper = static_cast<uint32_t>(descriptor >> 32);
    const uint32_t pc = static_cast<uint32_t>(descriptor);
    const bool thumb = upper & 1u;
    return !(upper & LearnedProfile::kSingleStepBit) && (pc & (thumb ? 1u : 3u)) == 0 &&
           read_only_code && read_only_code(pc);
}

bool ReadFile(const std::string& path, std::vector<uint8_t>& bytes) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    bool good = std::fseek(file, 0, SEEK_END) == 0;
    const long size = good ? std::ftell(file) : -1;
    good = good && size >= static_cast<long>(kHeaderBytes) &&
           static_cast<size_t>(size) <= kMaxFileBytes && std::fseek(file, 0, SEEK_SET) == 0;
    if (good) {
        bytes.resize(static_cast<size_t>(size));
        good = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    }
    std::fclose(file);
    return good;
}

bool AtomicWrite(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::string temporary = path + ".tmp.XXXXXX";
    const int fd = ::mkstemp(temporary.data());
    if (fd < 0) return false;
    // Same access as the saves beside it: adb can collect a profile for QA.
    ::fchmod(fd, 0660);
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count <= 0) break;
        written += static_cast<size_t>(count);
    }
    bool good = written == bytes.size() && ::fsync(fd) == 0;
    good = ::close(fd) == 0 && good;
    good = good && std::rename(temporary.c_str(), path.c_str()) == 0;
    if (!good) std::remove(temporary.c_str());
    return good;
}

}  // namespace

std::vector<uint8_t> EncodeLearned(uint32_t guest_bytes, const std::array<uint8_t, 32>& sha,
                                   const std::array<std::vector<uint64_t>, kRoleCount>& roles) {
    std::vector<uint8_t> out(kMagic, kMagic + sizeof(kMagic));
    Put32(out, guest_bytes);
    out.insert(out.end(), sha.begin(), sha.end());
    for (const auto& role : roles) Put32(out, static_cast<uint32_t>(role.size()));
    for (const auto& role : roles)
        for (const uint64_t descriptor : role) Put64(out, descriptor);
    return out;
}

bool DecodeLearned(const std::vector<uint8_t>& bytes, uint32_t guest_bytes,
                   const std::array<uint8_t, 32>& sha,
                   const LearnedProfile::CodeCheck& read_only_code,
                   std::array<std::vector<uint64_t>, kRoleCount>& roles) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kMaxFileBytes ||
        std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0 ||
        Read32(bytes.data() + 8) != guest_bytes ||
        !std::equal(sha.begin(), sha.end(), bytes.begin() + 12)) return false;
    std::array<size_t, kRoleCount> counts{};
    size_t total = 0;
    for (size_t role = 0; role < kRoleCount; ++role) {
        counts[role] = Read32(bytes.data() + 44 + 4 * role);
        if (counts[role] > LearnedProfile::kMaxPerRole) return false;
        total += counts[role];
    }
    if (bytes.size() != kHeaderBytes + 8 * total) return false;
    std::array<std::vector<uint64_t>, kRoleCount> decoded;
    size_t offset = kHeaderBytes;
    for (size_t role = 0; role < kRoleCount; ++role) {
        decoded[role].reserve(counts[role]);
        std::unordered_set<uint64_t> seen;
        seen.reserve(counts[role]);
        for (size_t i = 0; i < counts[role]; ++i, offset += 8) {
            const uint64_t descriptor = Read64(bytes.data() + offset);
            if (!Eligible(descriptor, read_only_code) || !seen.insert(descriptor).second)
                return false;
            decoded[role].push_back(descriptor);
        }
    }
    roles = std::move(decoded);
    return true;
}

void LearnedProfile::Load(const std::string& path, uint32_t guest_bytes,
                          const std::array<uint8_t, 32>& guest_sha, CodeCheck read_only_code) {
    std::array<std::vector<uint64_t>, kRoleCount> roles;
    std::vector<uint8_t> bytes;
    try {
        if (!ReadFile(path, bytes) ||
            !DecodeLearned(bytes, guest_bytes, guest_sha, read_only_code, roles)) roles = {};
    } catch (...) {
        roles = {};
    }
    std::lock_guard lock(mutex_);
    path_ = path;
    guest_bytes_ = guest_bytes;
    guest_sha_ = guest_sha;
    read_only_code_ = std::move(read_only_code);
    for (size_t role = 0; role < kRoleCount; ++role) {
        known_[role].clear();
        order_[role].clear();
        try {
            known_[role].reserve(roles[role].size() + 4096);
            known_[role].insert(roles[role].begin(), roles[role].end());
            order_[role] = roles[role];
        } catch (...) {
            known_[role].clear();
            order_[role].clear();
            roles[role].clear();
        }
        loaded_[role] = std::move(roles[role]);
    }
    recorded_ = 0;
    dirty_ = false;
    ready_ = !path_.empty();
}

void LearnedProfile::Record(Role role, uint64_t descriptor) {
    if (role >= kRoleCount) return;
    std::lock_guard lock(mutex_);
    if (!ready_ || known_[role].size() >= kMaxPerRole || !Eligible(descriptor, read_only_code_))
        return;
    try {
        if (!known_[role].insert(descriptor).second) return;
        order_[role].push_back(descriptor);
    } catch (...) {
        known_[role].erase(descriptor);
        return;
    }
    ++recorded_;
    dirty_ = true;
}

bool LearnedProfile::Save() {
    std::array<std::vector<uint64_t>, kRoleCount> roles;
    std::string path;
    uint32_t guest_bytes = 0;
    std::array<uint8_t, 32> sha{};
    {
        std::lock_guard lock(mutex_);
        if (!ready_ || !dirty_) return true;
        try {
            roles = order_;
        } catch (...) {
            return false;
        }
        path = path_;
        guest_bytes = guest_bytes_;
        sha = guest_sha_;
        dirty_ = false;
    }
    bool good = false;
    try {
        good = AtomicWrite(path, EncodeLearned(guest_bytes, sha, roles));
    } catch (...) {
    }
    if (!good) {
        std::lock_guard lock(mutex_);
        dirty_ = true;
    }
    return good;
}

bool LearnedProfile::SaveNow() {
    {
        std::lock_guard lock(mutex_);
        if (!ready_) return false;
        dirty_ = true;
    }
    return Save();
}

size_t LearnedProfile::RecordedThisRun() const {
    std::lock_guard lock(mutex_);
    return recorded_;
}

}  // namespace bl2::jit
