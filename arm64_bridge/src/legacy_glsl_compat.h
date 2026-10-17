#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace legacy_glsl {

inline bool IsIdentifier(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// All 38,980 shipped normalized stages use this ten-location vec4 interface,
// highp floats and the disabled UBO/multidraw path. SSO requires identical
// declared interfaces, including unused outputs. Add only missing declarations
// without changing the original shader body. Unrecognized sources use the
// existing monolithic compiler instead; this is not a general GLSL parser.
inline bool CompleteSsoInterface(std::string_view source, bool vertex,
                                 std::string* result) {
    if (!result || source.size() > 131072 ||
        !source.starts_with("#version 320 es\n") ||
        source.find('\0') != std::string_view::npos) return false;
    constexpr unsigned locations[] = {0, 1, 2, 3, 4, 5, 6, 7, 12, 13};
    constexpr unsigned required = 0x30ff;
    unsigned declared = 0, conditional = 0;
    unsigned versions = 0, precision = 0, ubo = 0, multidraw = 0;
    bool perVertex = false;
    size_t insertion = std::string_view::npos;
    for (size_t begin = 0; begin < source.size();) {
        size_t end = source.find('\n', begin);
        if (end == std::string_view::npos) end = source.size();
        auto line = source.substr(begin, end - begin);
        while (!line.empty() && IsSpace(line.front())) line.remove_prefix(1);
        while (!line.empty() && IsSpace(line.back())) line.remove_suffix(1);
        if (line.starts_with("#version")) ++versions;
        if (line.starts_with("#if")) ++conditional;
        else if (line.starts_with("#endif")) {
            if (!conditional) return false;
            --conditional;
        }
        if (line.starts_with("precision ") && line.ends_with("float;")) {
            if (line != "precision highp float;") return false;
            ++precision;
        }
        if (line.starts_with("#define ENABLE_UNIFORM_BUFFER_OBJECTS")) {
            if (line != "#define ENABLE_UNIFORM_BUFFER_OBJECTS 0" || conditional) return false;
            ++ubo;
        }
        if (line.starts_with("#define ENABLE_MULTIDRAW_INDIRECT")) {
            if (line != "#define ENABLE_MULTIDRAW_INDIRECT 0" || conditional) return false;
            ++multidraw;
        }
        if (line == "out gl_PerVertex {") perVertex = true;
        if (line.starts_with("layout(location=")) {
            constexpr size_t prefix = sizeof("layout(location=") - 1;
            size_t cursor = prefix;
            unsigned location = 0;
            if (cursor == line.size() || line[cursor] < '0' || line[cursor] > '9') return false;
            while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') {
                location = location * 10 + static_cast<unsigned>(line[cursor++] - '0');
                if (location > 31) return false;
            }
            if (line.substr(cursor, 2) != ") ") return false;
            const auto declaration = line.substr(cursor + 2);
            const auto direction = vertex ? std::string_view("out ") : std::string_view("in ");
            if (declaration.starts_with(direction)) {
                const unsigned bit = 1u << location;
                const std::string expected = std::string(direction) + "vec4 UnTexcoord" +
                                             std::to_string(location) + ";";
                if (conditional || !(required & bit) || (declared & bit) || declaration != expected) return false;
                declared |= bit;
            } else if (!declaration.starts_with(vertex ? "in " : "out ")) return false;
        } else if (line.starts_with("layout") && line.find("location") != std::string_view::npos) return false;
        if (line.starts_with("void main(")) {
            if (conditional || insertion != std::string_view::npos) return false;
            insertion = begin;
        }
        begin = end == source.size() ? end : end + 1;
    }
    if (conditional || versions != 1 || precision != 1 || ubo != 1 || multidraw != 1 ||
        (vertex && !perVertex) || insertion == std::string_view::npos) return false;
    std::string padded(source.substr(0, insertion));
    for (const unsigned location : locations) {
        if (!(declared & (1u << location))) {
            padded += "layout(location=" + std::to_string(location) + ") ";
            padded += vertex ? "out" : "in";
            padded += " vec4 UnTexcoord" + std::to_string(location) + ";\n";
        }
    }
    padded.append(source.substr(insertion));
    *result = std::move(padded);
    return true;
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
