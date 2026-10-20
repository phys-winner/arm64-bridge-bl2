#pragma once

// Cold-start, host-side localization composition. No guest calls or file I/O.
// Failure leaves out unchanged. Aliasing either input as out is rejected.
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace bl2::coalesced {
namespace detail {
constexpr size_t kBaseBytes = 8u << 20;
constexpr size_t kDeltaBytes = 1u << 20;
constexpr size_t kOutputBytes = 10u << 20;
constexpr size_t kFiles = 1280;
constexpr size_t kSections = 32768;
constexpr size_t kPairs = 131072;
constexpr char16_t kPrefix[] = u"..\\..\\WillowGame\\Localization\\INT\\";

struct Pair { std::u16string key, value; };
struct Section { std::u16string name; std::vector<Pair> pairs; };
struct File {
    std::u16string name;
    std::vector<Section> sections;
    size_t begin = 0, end = 0;
};

inline std::u16string Fold(std::u16string text) {
    for (auto& c : text) if (c >= u'A' && c <= u'Z') c += u'a' - u'A';
    return text;
}

inline bool Ascii(const std::u16string& text) {
    for (const auto c : text) if (c > 127) return false;
    return true;
}

inline bool ValidUtf16(const std::u16string& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = text[i];
        if (!c) return false;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i == text.size() || text[i] < 0xDC00 || text[i] > 0xDFFF) return false;
        } else if (c >= 0xDC00 && c <= 0xDFFF) return false;
    }
    return true;
}

struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t pos = 0, sections = 0, pairs = 0;

    bool U32(uint32_t& value) {
        if (bytes.size() - pos < 4) return false;
        value = uint32_t(bytes[pos]) | uint32_t(bytes[pos + 1]) << 8 |
                uint32_t(bytes[pos + 2]) << 16 | uint32_t(bytes[pos + 3]) << 24;
        pos += 4;
        return true;
    }

    bool Count(size_t& value, size_t maximum, size_t minimum_bytes) {
        uint32_t count;
        if (!U32(count) || count > maximum || count > (bytes.size() - pos) / minimum_bytes) return false;
        value = count;
        return true;
    }

    bool String(std::u16string& text) {
        uint32_t encoded;
        if (!U32(encoded) || encoded == 0x80000000u) return false;
        text.clear();
        if (!encoded) return true;
        const bool wide = (encoded & 0x80000000u) != 0;
        const size_t count = wide ? uint32_t(0u - encoded) : encoded;
        const size_t width = wide ? 2 : 1;
        if (count > (bytes.size() - pos) / width) return false;
        const size_t end = pos + count * width;
        if (bytes[end - width] || (wide && bytes[end - 1])) return false;
        text.reserve(count - 1);
        for (size_t i = 0; i + 1 < count; ++i) {
            char16_t c = bytes[pos++];
            if (wide) c |= char16_t(bytes[pos++]) << 8;
            text.push_back(c);
        }
        pos = end;
        return ValidUtf16(text);
    }

    bool Parse(std::vector<File>& files) {
        size_t count = 0;
        if (!Count(count, kFiles, 8)) return false;
        files.reserve(count);
        std::set<std::u16string> names;
        for (size_t i = 0; i < count; ++i) {
            File file;
            file.begin = pos;
            size_t section_count = 0;
            if (!String(file.name) || file.name.empty() || !names.insert(Fold(file.name)).second ||
                !Count(section_count, kSections - sections, 8)) return false;
            sections += section_count;
            file.sections.reserve(section_count);
            for (size_t j = 0; j < section_count; ++j) {
                Section section;
                size_t pair_count = 0;
                if (!String(section.name) || !Count(pair_count, kPairs - pairs, 8)) return false;
                pairs += pair_count;
                section.pairs.reserve(pair_count);
                for (size_t k = 0; k < pair_count; ++k) {
                    Pair pair;
                    if (!String(pair.key) || !String(pair.value)) return false;
                    section.pairs.push_back(std::move(pair));
                }
                file.sections.push_back(std::move(section));
            }
            file.end = pos;
            files.push_back(std::move(file));
        }
        return pos == bytes.size();
    }
};

inline bool ValidateDelta(const std::vector<File>& files) {
    const std::u16string prefix(kPrefix);
    for (const auto& file : files) {
        if (file.name.compare(0, prefix.size(), prefix) != 0 || !Ascii(file.name)) return false;
        const auto basename = file.name.substr(prefix.size());
        if (basename.size() < 5 || basename.compare(basename.size() - 4, 4, u".INT") != 0 ||
            basename == u"..INT") return false;
        for (const auto c : basename) {
            if (!((c >= u'A' && c <= u'Z') || (c >= u'a' && c <= u'z') ||
                  (c >= u'0' && c <= u'9') || c == u'_' || c == u'-' || c == u'.' || c == u' ')) return false;
        }
        std::set<std::u16string> sections;
        for (const auto& section : file.sections) {
            if (section.name.empty() || !Ascii(section.name) || !sections.insert(Fold(section.name)).second) return false;
            std::set<std::u16string> keys;
            for (const auto& pair : section.pairs) {
                if (pair.key.empty() || !Ascii(pair.key) || !keys.insert(Fold(pair.key)).second) return false;
            }
        }
    }
    return true;
}

struct Writer {
    std::vector<uint8_t> bytes;

    bool U32(uint32_t value) {
        if (bytes.size() > kOutputBytes - 4) return false;
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(value >> (8 * i)));
        return true;
    }

    bool String(const std::u16string& text) {
        if (text.size() > (kOutputBytes - 6) / 2 || !U32(0u - uint32_t(text.size() + 1)) ||
            text.size() + 1 > (kOutputBytes - bytes.size()) / 2) return false;
        for (const auto c : text) { bytes.push_back(uint8_t(c)); bytes.push_back(uint8_t(c >> 8)); }
        bytes.push_back(0); bytes.push_back(0);
        return true;
    }

    bool Record(const File& file) {
        if (!String(file.name) || !U32(uint32_t(file.sections.size()))) return false;
        for (const auto& section : file.sections) {
            if (!String(section.name) || !U32(uint32_t(section.pairs.size()))) return false;
            for (const auto& pair : section.pairs) if (!String(pair.key) || !String(pair.value)) return false;
        }
        return true;
    }

    bool Raw(const std::vector<uint8_t>& source, size_t begin, size_t end) {
        if (begin > end || end > source.size() || end - begin > kOutputBytes - bytes.size()) return false;
        bytes.insert(bytes.end(), source.begin() + begin, source.begin() + end);
        return true;
    }
};

inline bool Append(File& target, const File& addition) {
    for (const auto& section : addition.sections) {
        size_t found = target.sections.size();
        const auto name = Fold(section.name);
        for (size_t i = 0; i < target.sections.size(); ++i) {
            if (Fold(target.sections[i].name) != name) continue;
            if (found != target.sections.size()) return false;
            found = i;
        }
        if (found == target.sections.size()) target.sections.push_back({section.name, {}});
        auto& pairs = target.sections[found].pairs;
        std::set<std::u16string> keys;
        for (const auto& pair : pairs) keys.insert(Fold(pair.key));
        for (const auto& pair : section.pairs) {
            if (!keys.insert(Fold(pair.key)).second) return false;
            pairs.push_back(pair);
        }
    }
    return true;
}
} // namespace detail

inline bool Merge(const std::vector<uint8_t>& base, const std::vector<uint8_t>& delta,
                  std::vector<uint8_t>& out) {
    using namespace detail;
    if (&out == &base || &out == &delta || base.size() > kBaseBytes || delta.size() > kDeltaBytes) return false;
    std::vector<File> original, additions;
    Reader base_reader{base}, delta_reader{delta};
    if (!base_reader.Parse(original) || !delta_reader.Parse(additions) || !ValidateDelta(additions)) return false;
    std::map<std::u16string, size_t> names;
    for (size_t i = 0; i < original.size(); ++i) names.emplace(Fold(original[i].name), i);
    std::map<size_t, File> changed;
    std::vector<const File*> appended;
    size_t sections = base_reader.sections, pairs = base_reader.pairs;
    for (const auto& addition : additions) {
        if (addition.sections.size() > kSections - sections) return false;
        sections += addition.sections.size();
        for (const auto& section : addition.sections) {
            if (section.pairs.size() > kPairs - pairs) return false;
            pairs += section.pairs.size();
        }
        const auto found = names.find(Fold(addition.name));
        if (found == names.end()) appended.push_back(&addition);
        else {
            auto& target = changed.emplace(found->second, original[found->second]).first->second;
            if (!Append(target, addition)) return false;
        }
    }
    if (appended.size() > kFiles - original.size()) return false;
    Writer writer;
    if (!writer.U32(uint32_t(original.size() + appended.size()))) return false;
    for (size_t i = 0; i < original.size(); ++i) {
        const auto found = changed.find(i);
        if (found != changed.end()) {
            if (!writer.Record(found->second)) return false;
        } else if (!writer.Raw(base, original[i].begin, original[i].end)) return false;
    }
    for (const auto* file : appended) if (!writer.Record(*file)) return false;
    // Do not publish partially built output, even on a rejected boundary case.
    std::vector<File> verified;
    Reader result{writer.bytes};
    if (!result.Parse(verified)) return false;
    out.swap(writer.bytes);
    return true;
}
} // namespace bl2::coalesced
