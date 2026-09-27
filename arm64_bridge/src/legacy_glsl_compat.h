#pragma once

#include <string>
#include <string_view>

namespace legacy_glsl {

inline bool IsIdentifier(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The desktop cache replicates a shadow lookup with textureProj(...).rrrr.
// ES returns float for shadow lookups but prohibits scalar swizzles. Wrapping
// this *specific swizzled call* in vec4 preserves both the scalar case and the
// regular textureProj(vec4) case. Unswizzled scalar calls must remain scalar.
// This scans generated GLSL, with comments skipped and nested calls balanced;
// it is not a general GLSL parser or a blanket rewrite of .rrrr expressions.
inline std::string NormalizeProjectedSwizzles(std::string_view source) {
    std::string result;
    result.reserve(source.size() + 64);
    constexpr std::string_view token = "textureProj";
    size_t cursor = 0;
    while (cursor < source.size()) {
        if (source.substr(cursor, 2) == "//") {
            size_t end = source.find('\n', cursor);
            if (end == std::string_view::npos) end = source.size();
            result.append(source.substr(cursor, end - cursor));
            cursor = end;
            continue;
        }
        if (source.substr(cursor, 2) == "/*") {
            size_t end = source.find("*/", cursor + 2);
            end = end == std::string_view::npos ? source.size() : end + 2;
            result.append(source.substr(cursor, end - cursor));
            cursor = end;
            continue;
        }
        if (source.substr(cursor, token.size()) == token &&
            (cursor == 0 || !IsIdentifier(source[cursor - 1]))) {
            size_t open = cursor + token.size();
            while (open < source.size() && IsSpace(source[open])) ++open;
            if (open < source.size() && source[open] == '(') {
                size_t close = open + 1;
                unsigned depth = 1;
                while (close < source.size() && depth != 0) {
                    if (source.substr(close, 2) == "//") {
                        const size_t end = source.find('\n', close);
                        close = end == std::string_view::npos
                            ? source.size() : end;
                        continue;
                    }
                    if (source.substr(close, 2) == "/*") {
                        const size_t end = source.find("*/", close + 2);
                        close = end == std::string_view::npos
                            ? source.size() : end + 2;
                        continue;
                    }
                    if (source[close] == '(') ++depth;
                    if (source[close] == ')') --depth;
                    ++close;
                }
                size_t suffix = close;
                while (suffix < source.size() && IsSpace(source[suffix])) ++suffix;
                if (depth == 0 && source.substr(suffix, 5) == ".rrrr" &&
                    (suffix + 5 == source.size() ||
                     !IsIdentifier(source[suffix + 5]))) {
                    result += "vec4(";
                    result.append(source.substr(cursor, close - cursor));
                    result.push_back(')');
                    cursor = close;
                    continue;
                }
            }
        }
        result.push_back(source[cursor++]);
    }
    return result;
}

}  // namespace legacy_glsl
