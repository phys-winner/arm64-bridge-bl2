#include "../src/bl2_coalesced_dlc.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

namespace {
using Bytes = std::vector<uint8_t>;
const std::u16string prefix = u"..\\..\\WillowGame\\Localization\\INT\\";
int checks = 0;
void Check(bool ok, const char* label) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
    ++checks;
}
void U32(Bytes& b, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(uint8_t(n >> (i * 8)));
}
void Wide(Bytes& b, const std::u16string& s) {
    U32(b, 0u - uint32_t(s.size() + 1));
    for (auto c : s) { b.push_back(uint8_t(c)); b.push_back(uint8_t(c >> 8)); }
    b.push_back(0); b.push_back(0);
}
void Narrow(Bytes& b, const std::string& s) {
    U32(b, uint32_t(s.size() + 1)); b.insert(b.end(), s.begin(), s.end()); b.push_back(0);
}
struct Section { std::u16string name; std::vector<std::pair<std::u16string, std::u16string>> pairs; };
Bytes Record(const std::u16string& name, const std::vector<Section>& sections) {
    Bytes b; Wide(b, name); U32(b, uint32_t(sections.size()));
    for (const auto& section : sections) {
        Wide(b, section.name); U32(b, uint32_t(section.pairs.size()));
        for (const auto& pair : section.pairs) { Wide(b, pair.first); Wide(b, pair.second); }
    }
    return b;
}
Bytes Map(const std::vector<Bytes>& records) {
    Bytes b; U32(b, uint32_t(records.size()));
    for (const auto& r : records) b.insert(b.end(), r.begin(), r.end());
    return b;
}
void Reject(const Bytes& base, const Bytes& delta, const char* label) {
    const Bytes saved_base = base, saved_delta = delta, sentinel = {4, 3, 2, 1};
    Bytes out = sentinel;
    Check(!bl2::coalesced::Merge(base, delta, out), label);
    Check(out == sentinel && base == saved_base && delta == saved_delta, "rejection leaves all caller bytes unchanged");
}
Bytes Read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    Check(bool(f), "fixture input opened");
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

void SelfCheck() {
    const auto base_record = Record(prefix + u"Shared.INT", {{u"A", {{u"Old", u"base"}}}});
    // Narrow bytes deliberately differ from the wide serializer. Duplicate INI
    // properties must survive as raw ordered bytes rather than normalized maps.
    Bytes config;
    Narrow(config, "..\\..\\WillowGame\\Config\\Engine.ini"); U32(config, 1);
    Narrow(config, "Rendering"); U32(config, 2);
    Narrow(config, "Same"); Narrow(config, "one"); Narrow(config, "Same"); Narrow(config, "two");
    const auto base = Map({base_record, config});
    const auto added = Record(prefix + u"Shared.INT", {{u"a", {{u"New", u"\u0420\u0443\u0441\u0441\u043a\u0438\u0439 \U0001F98A"}}}});
    const auto fresh = Record(prefix + u"New.INT", {{u"Dialog", {{u"Subtitles[0]", u"(Text=\u0422\u0435\u0441\u0442,Time=0)"}, {u"Empty", u""}}}});
    const auto delta = Map({added, fresh});
    const auto expected = Map({Record(prefix + u"Shared.INT", {{u"A", {{u"Old", u"base"}, {u"New", u"\u0420\u0443\u0441\u0441\u043a\u0438\u0439 \U0001F98A"}}}}), config, fresh});
    Bytes out;
    Check(bl2::coalesced::Merge(base, delta, out) && out == expected,
          "append shared/new records, UTF-16 surrogate value, exact cue and raw duplicate config");
    Check(bl2::coalesced::Merge(base, Map({}), out) && out == base, "empty delta preserves base bytes");
    const auto empty_base = Map({});
    Check(bl2::coalesced::Merge(empty_base, Map({fresh}), out) && out == Map({fresh}), "new record on empty base");
    Reject(base, Map({Record(prefix + u"Shared.INT", {{u"a", {{u"OLD", u"base"}}}})}), "existing equal key still rejects");
    Reject(base, Map({Record(prefix + u"New.INT", {{u"A", {{u"K", u"one"}, {u"k", u"two"}}}})}), "delta duplicate folded key");
    Reject(base, Map({Record(prefix + u"New.INT", {{u"A", {}}, {u"a", {}}})}), "delta duplicate folded section");
    Reject(base, Map({fresh, fresh}), "delta duplicate file");
    Reject(Map({base_record, base_record}), delta, "base duplicate file");
    Reject(Map({Record(prefix + u"Shared.INT", {{u"A", {}}, {u"a", {}}})}), Map({added}), "ambiguous target base section");
    Reject(base, Map({Record(prefix + u"..\\Engine.INI", {{u"A", {{u"K", u"V"}}}})}), "delta config traversal");
    Reject(base, Map({Record(prefix + u"New.INI", {})}), "delta extension injection");
    Reject(base, Map({Record(u"C:\\New.INT", {})}), "absolute delta path");
    Reject(base, Map({Record(prefix + u"..INT", {})}), "invalid dot basename");
    Reject(base, Map({Record(prefix + u"New.INT", {{u"\u042F", {}}})}), "non-ASCII delta identifier outside receipt");
    for (size_t i = 0; i < delta.size(); ++i)
        Reject(base, Bytes(delta.begin(), delta.begin() + i), "every truncated delta boundary");
    for (size_t i = 0; i < base.size(); ++i)
        Reject(Bytes(base.begin(), base.begin() + i), delta, "every truncated base boundary");
    auto trailing = delta; trailing.push_back(0); Reject(base, trailing, "trailing bytes");
    for (uint32_t bad : {0x80000000u, 0xFFFFFFFFu, 1281u}) {
        Bytes bytes; U32(bytes, bad); Reject(base, bytes, "invalid file count");
    }
    static_assert(bl2::coalesced::detail::kFiles == 1280);
    std::vector<Bytes> records;
    for (unsigned i = 0; i < 1281; ++i) {
        const auto number = std::to_string(i);
        records.push_back(Record(prefix + u"Probe" + std::u16string(number.begin(), number.end()) + u".INT", {}));
    }
    Reject(Map(records), Map({}), "1281 input records exceed file budget");
    const auto last_record = records.back();
    records.pop_back();
    const auto at_limit = Map(records);
    Check(bl2::coalesced::Merge(at_limit, Map({}), out) && out == at_limit, "1280 input records accepted exactly");
    Reject(at_limit, Map({last_record}), "append beyond 1280 leaves output unchanged");
    const auto edge_record = records.back();
    records.pop_back();
    Check(bl2::coalesced::Merge(Map(records), Map({edge_record}), out) && out == at_limit, "1279 plus one merge accepted exactly");
    for (uint32_t bad : {0x80000000u, 0x7FFFFFFFu, 0xFFFFFFFEu}) {
        Bytes bytes; U32(bytes, 1); U32(bytes, bad); bytes.resize(12);
        Reject(base, bytes, "invalid FString size/INT_MIN/truncation");
    }
    for (const std::u16string value : {std::u16string(1, char16_t(0xD800)),
                                      std::u16string(1, char16_t(0xDC00)),
                                      std::u16string({u'a', 0, u'b'})})
        Reject(base, Map({Record(prefix + u"New.INT", {{u"A", {{u"K", value}}}})}), "invalid UTF-16 or embedded NUL");
    auto no_nul = delta; no_nul.back() = 1; Reject(base, no_nul, "missing wide NUL");
    Bytes short_count; U32(short_count, 1); Wide(short_count, prefix + u"New.INT"); U32(short_count, 32769);
    Reject(base, short_count, "section budget exceeds before allocation");
    Bytes pair_count; U32(pair_count, 1); Wide(pair_count, prefix + u"New.INT"); U32(pair_count, 1); Wide(pair_count, u"A"); U32(pair_count, 131073);
    Reject(base, pair_count, "pair budget exceeds before allocation");
    Reject(Bytes((8u << 20) + 1), delta, "base byte budget");
    Reject(base, Bytes((1u << 20) + 1), "delta byte budget");
    auto aliased = base;
    Check(!bl2::coalesced::Merge(aliased, delta, aliased) && aliased == base, "output/base alias rejected unchanged");
    aliased = delta;
    Check(!bl2::coalesced::Merge(base, aliased, aliased) && aliased == delta, "output/delta alias rejected unchanged");
    // Latin-1 positive FString input must widen by byte value, not UTF-8 decode.
    Bytes latin;
    Narrow(latin, "..\\..\\WillowGame\\Localization\\INT\\Latin.INT"); U32(latin, 1);
    Narrow(latin, "A"); U32(latin, 1); Narrow(latin, "Old"); Narrow(latin, std::string("\xE9", 1));
    const auto latin_delta = Map({Record(prefix + u"Latin.INT", {{u"A", {{u"New", u"x"}}}})});
    Check(bl2::coalesced::Merge(Map({latin}), latin_delta, out) &&
          out == Map({Record(prefix + u"Latin.INT", {{u"A", {{u"Old", u"\u00E9"}, {u"New", u"x"}}}})}),
          "positive Latin-1 string widening matches ConfigReader");
    std::printf("PASS: %d checks\n", checks);
}
} // namespace

int main(int argc, char** argv) {
    SelfCheck();
    if (argc == 1) return 0;
    if (argc != 4) { std::fprintf(stderr, "usage: test [base delta output]\n"); return 2; }
    const auto base = Read(argv[1]), delta = Read(argv[2]);
    Bytes out;
    Check(bl2::coalesced::Merge(base, delta, out), "actual prepared merge");
    std::ofstream file(argv[3], std::ios::binary | std::ios::trunc);
    Check(bool(file), "output opened");
    file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    Check(bool(file), "output write completed");
    std::printf("actual merged bytes: %zu\n", out.size());
    return 0;
}
