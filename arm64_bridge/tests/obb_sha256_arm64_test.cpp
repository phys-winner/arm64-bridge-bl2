// Device check of the ARMv8 SHA-256 path (build with -DBL2_ARM64_SHA2 and
// src/obb_sha256_arm64.cpp): known answers, random chunking against the same
// stream hashed in one call, and files to compare with toybox sha256sum.
#include "../src/obb_sha256.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
std::string HexOf(const uint8_t* data, size_t size) {
    ObbSha256 hash;
    hash.Update(data, size);
    return ObbSha256::Hex(hash.Finalize()).data();
}
}  // namespace

int main(int argc, char** argv) {
    const std::string abc = "abc";
    Check(HexOf(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()) ==
          "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", "abc");
    Check(HexOf(nullptr, 0) == "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855", "empty");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Check(HexOf(reinterpret_cast<const uint8_t*>(two.data()), two.size()) ==
          "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1", "two blocks");
    std::vector<uint8_t> million(1000000, 'a');
    Check(HexOf(million.data(), million.size()) ==
          "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0", "million a");
    std::mt19937 random(12345);
    for (int round = 0; round < 2000; ++round) {
        std::vector<uint8_t> data(random() % 4096);
        for (auto& byte : data) byte = static_cast<uint8_t>(random());
        const std::string whole = HexOf(data.data(), data.size());
        ObbSha256 chunked;
        for (size_t at = 0; at < data.size();) {
            const size_t step = std::min<size_t>(data.size() - at, 1 + random() % 300);
            chunked.Update(data.data() + at, step);
            at += step;
        }
        if (ObbSha256::Hex(chunked.Finalize()).data() != whole) { Check(false, "chunked equals whole"); break; }
    }
    ObbSha256::Digest scoped{}, direct{};
    {
        ObbSha256::MappingScope scope(million.data(), million.size());
        Check(ObbSha256::OfMapping(million.data(), million.size(), scoped), "scoped digest");
        Check(ObbSha256::OfMapping(million.data(), million.size() - 1, direct) && direct != scoped,
              "other range is hashed itself");
    }
    Check(ObbSha256::OfMapping(million.data(), million.size(), direct) && direct == scoped, "same digest outside scope");
    for (int i = 1; i < argc; ++i) {
        FILE* file = std::fopen(argv[i], "rb");
        if (!file) return 2;
        std::vector<uint8_t> bytes;
        uint8_t buffer[65536];
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + count);
        std::fclose(file);
        std::printf("%s  %s\n", HexOf(bytes.data(), bytes.size()).c_str(), argv[i]);
    }
    std::printf("SHA256_ARM64_CHECK checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
