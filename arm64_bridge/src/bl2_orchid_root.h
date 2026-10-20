#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace bl2_orchid {
// Decode the original ANSI FString, including its sole final NUL. This is the
// original GAndroidSystemMemory game root, not a path supplied by an archive.
inline bool DecodeDlcRoot(const std::uint8_t* data, std::size_t count, std::string& output) {
    if (!data || count < 3 || count > 512 || data[0] != '/' || data[count - 1]) return false;
    std::string root;
    for (std::size_t i = 0; i + 1 < count; ++i) {
        const auto c = data[i];
        if (c < 32 || c > 126 || c == '\\' || c == ':' || c == '*' || c == '?') return false;
        root += static_cast<char>(c);
    }
    while (!root.empty() && root.back() == '/') root.pop_back();
    if (root.empty()) return false;
    for (std::size_t at = 1; at <= root.size();) {
        const auto end = root.find('/', at);
        const auto part = root.substr(at, end == std::string::npos ? end : end - at);
        if (part.empty() || part == "." || part == ".." || part.size() > 255) return false;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    output = root + "/DLC";
    return true;
}
}  // namespace bl2_orchid
