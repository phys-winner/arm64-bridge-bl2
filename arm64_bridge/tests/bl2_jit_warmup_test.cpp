#include "bl2_jit_warmup.h"
#include <cstdio>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool result, const char* label) {
    ++checks;
    if (!result) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
void Set32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
}
std::vector<uint8_t> Valid() {
    std::vector<uint8_t> bytes(72);
    std::memcpy(bytes.data(), "BL2JIT01", 8);
    Set32(bytes, 8, Bl2JitWarmup::kGuestBytes);
    std::copy(Bl2JitWarmup::kGuestSha.begin(), Bl2JitWarmup::kGuestSha.end(), bytes.begin() + 12);
    Set32(bytes, 44, 2); Set32(bytes, 48, 1); Set32(bytes, 52, 1);
    Set32(bytes, 56, 0x40001000); Set32(bytes, 60, 0x40001004);
    Set32(bytes, 64, 0x40002000); Set32(bytes, 68, 0x40003000);
    return bytes;
}
bool ReadOnly(uint32_t pc) { return pc >= 0x40001000 && pc < 0x40004000; }
}
int main() {
    Bl2JitWarmup catalog;
    const auto valid = Valid();
    Check(catalog.Parse(valid, ReadOnly), "valid role catalog");
    Check(catalog.PCs(Bl2JitWarmup::Main) == std::vector<uint32_t>{0x40001000, 0x40001004} &&
          catalog.PCs(Bl2JitWarmup::Callback) == std::vector<uint32_t>{0x40002000} &&
          catalog.PCs(Bl2JitWarmup::Renderer) == std::vector<uint32_t>{0x40003000}, "exact owner lists");
    auto reject = [&](std::vector<uint8_t> bytes, const char* label) {
        Check(!catalog.Parse(bytes, ReadOnly) && catalog.PCs(Bl2JitWarmup::Main).size() == 2 &&
              catalog.PCs(Bl2JitWarmup::Main)[0] == 0x40001000, label);
    };
    auto bytes = valid; bytes[0] ^= 1; reject(bytes, "wrong magic preserves catalog");
    bytes = valid; bytes[12] ^= 1; reject(bytes, "wrong guest SHA");
    bytes = valid; Set32(bytes, 8, 1); reject(bytes, "wrong guest size");
    bytes = valid; Set32(bytes, 44, 0); reject(bytes, "empty required role");
    bytes = valid; Set32(bytes, 44, 32769); reject(bytes, "role cap");
    bytes = valid; Set32(bytes, 44, 0xFFFFFFFF); reject(bytes, "count overflow rejected");
    bytes = valid; Set32(bytes, 60, 0x40001000); reject(bytes, "duplicate PC");
    bytes = valid; Set32(bytes, 60, 0x40000FFC); reject(bytes, "unsorted PC");
    bytes = valid; Set32(bytes, 56, 0x40001002); reject(bytes, "ARM alignment");
    bytes = valid; Set32(bytes, 68, 0x40004000); reject(bytes, "outside readonly code");
    bytes = valid; bytes.pop_back(); reject(bytes, "truncated data");
    bytes = valid; bytes.push_back(0); reject(bytes, "trailing data");
    reject({}, "empty asset");
    reject(std::vector<uint8_t>(Bl2JitWarmup::kMaxBytes + 1), "asset cap");
    Check(!catalog.Parse(valid, [](uint32_t) { return false; }), "loader permission rejection");
    std::printf("BL2_JIT_WARMUP_TEST %s checks=%u failures=%u\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
