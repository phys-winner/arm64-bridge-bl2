#include "obb_sha256.h"

#include <string_view>

constexpr auto kEmpty = [] {
    ObbSha256 sha;
    return ObbSha256::Hex(sha.Finalize());
}();
static_assert(std::string_view(kEmpty.data()) ==
              "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855");

constexpr auto kAbc = [] {
    constexpr uint8_t bytes[] = {'a', 'b', 'c'};
    ObbSha256 sha;
    sha.Update(bytes, 1);
    sha.Update(bytes + 1, 2);
    return ObbSha256::Hex(sha.Finalize());
}();
static_assert(std::string_view(kAbc.data()) ==
              "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");

constexpr auto kMultiBlock = [] {
    constexpr uint8_t bytes[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ObbSha256 sha;
    sha.Update(bytes, 13);
    sha.Update(bytes + 13, sizeof(bytes) - 14);
    return ObbSha256::Hex(sha.Finalize());
}();
static_assert(std::string_view(kMultiBlock.data()) ==
              "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1");

template <size_t Length>
constexpr auto HashRepeatedA() {
    std::array<uint8_t, Length> bytes{};
    bytes.fill('a');
    ObbSha256 sha;
    sha.Update(bytes.data(), 17);
    sha.Update(bytes.data() + 17, Length - 17);
    return ObbSha256::Hex(sha.Finalize());
}

constexpr auto kA55 = HashRepeatedA<55>();
constexpr auto kA56 = HashRepeatedA<56>();
constexpr auto kA64 = HashRepeatedA<64>();
static_assert(std::string_view(kA55.data()) ==
              "9F4390F8D30C2DD92EC9F095B65E2B9AE9B0A925A5258E241C9F1E910F734318");
static_assert(std::string_view(kA56.data()) ==
              "B35439A4AC6F0948B6D6F9E3C6AF0F5F590CE20F1BDE7090EF7970686EC6738A");
static_assert(std::string_view(kA64.data()) ==
              "FFE054FE7AE0CB6DC65C3AF9B61D5209F439851DB43D0BA5997337DF154668EB");

int main() { return 0; }
