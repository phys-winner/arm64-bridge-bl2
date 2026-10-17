#include "../src/bl2_shader_warmup.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

namespace {
struct Shader { GLenum target; std::string source; bool compiled = false; };
struct Program {
    std::vector<GLuint> attached;
    std::string vertex, fragment;
    bool linked = false;
    bool retrievable = false;
};
std::map<GLuint, Shader> shaders;
std::map<GLuint, Program> programs;
GLuint next_shader = 1, next_program = 1000;
unsigned gl_calls = 0, deleted_shaders = 0, deleted_programs = 0;
unsigned invalid_deletes = 0, compile_calls = 0, link_calls = 0;
unsigned create_shader_calls = 0, create_program_calls = 0;
unsigned fail_shader_creation = 0, fail_program_creation = 0;
constexpr GLenum kMockBinaryFormat = 0x91f1;
std::vector<GLint> supported_formats{static_cast<GLint>(kMockBinaryFormat)};
std::string driver_version = "OpenGL ES 3.2 mock-driver-1";
unsigned binary_load_calls = 0, binary_export_calls = 0, hint_calls = 0, error_reads = 0;
unsigned rejected_loads = 0;
GLenum pending_error = GL_NO_ERROR;
GLint forced_binary_length = -1;
bool invalid_export_format = false;
EGLContext current_context = reinterpret_cast<EGLContext>(uintptr_t{1});
unsigned checks = 0, failures = 0;

void Check(bool passed, const char* label) {
    ++checks;
    if (!passed) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
void U32(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<uint8_t>(value >> shift));
}
void Pair(std::vector<uint8_t>& output, std::string_view vertex, std::string_view fragment) {
    U32(output, static_cast<uint32_t>(vertex.size()));
    U32(output, static_cast<uint32_t>(fragment.size()));
    output.insert(output.end(), vertex.begin(), vertex.end());
    output.insert(output.end(), fragment.begin(), fragment.end());
}
uint32_t ReadU32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
std::vector<uint8_t> ProgramBytes(const Program& program) {
    std::vector<uint8_t> bytes{'M','C','K','1'};
    Pair(bytes, program.vertex, program.fragment);
    return bytes;
}
std::vector<uint8_t> ReadFile(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::rewind(file);
    std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    const bool good = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return good ? bytes : std::vector<uint8_t>{};
}
bool WriteFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool good = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose(file) == 0 && good;
}
void Invalid(std::span<const uint8_t> bytes, const char* label) {
    const unsigned before = gl_calls;
    std::vector<std::pair<std::string_view, std::string_view>> pairs{{"keep-v", "keep-p"}};
    Check(!Bl2ShaderWarmup::ParsePairs(bytes, pairs) && pairs.size() == 1 &&
          pairs.front().first == "keep-v" && pairs.front().second == "keep-p",
          "malformed pair parser preserves caller output");
    Bl2ShaderWarmup cache;
    Check(!cache.Warm(bytes), label);
    Check(gl_calls == before, "invalid whole table makes no GL calls");
}
}  // namespace

extern "C" {
EGLContext EGLAPIENTRY eglGetCurrentContext() { return current_context; }
const GLubyte* GL_APIENTRY glGetString(GLenum name) {
    ++gl_calls;
    const char* value = name == GL_VENDOR ? "actual mock vendor" :
        name == GL_RENDERER ? "actual mock renderer" :
        name == GL_VERSION ? driver_version.c_str() :
        name == GL_SHADING_LANGUAGE_VERSION ? "OpenGL ES GLSL ES 3.20 mock" : nullptr;
    return reinterpret_cast<const GLubyte*>(value);
}
void GL_APIENTRY glGetIntegerv(GLenum name, GLint* value) {
    ++gl_calls;
    if (name == GL_NUM_PROGRAM_BINARY_FORMATS)
        *value = static_cast<GLint>(supported_formats.size());
    else if (name == GL_PROGRAM_BINARY_FORMATS)
        std::copy(supported_formats.begin(), supported_formats.end(), value);
}
GLenum GL_APIENTRY glGetError() {
    ++gl_calls; ++error_reads;
    const GLenum result = pending_error;
    pending_error = GL_NO_ERROR;
    return result;
}
GLuint GL_APIENTRY glCreateShader(GLenum target) {
    ++gl_calls;
    if (++create_shader_calls == fail_shader_creation) return 0;
    const GLuint name = next_shader++;
    shaders.emplace(name, Shader{target, {}});
    return name;
}
void GL_APIENTRY glShaderSource(GLuint name, GLsizei count, const GLchar* const* strings,
                                const GLint* lengths) {
    ++gl_calls;
    auto& source = shaders.at(name).source;
    source.clear();
    for (GLsizei i = 0; i < count; ++i)
        source.append(strings[i], lengths && lengths[i] >= 0
            ? static_cast<size_t>(lengths[i]) : std::strlen(strings[i]));
}
void GL_APIENTRY glCompileShader(GLuint name) {
    ++gl_calls; ++compile_calls;
    auto& shader = shaders.at(name);
    shader.compiled = shader.source.find("bad_compile") == std::string::npos;
}
void GL_APIENTRY glGetShaderiv(GLuint name, GLenum query, GLint* value) {
    ++gl_calls;
    *value = query == GL_COMPILE_STATUS && shaders.at(name).compiled ? GL_TRUE : GL_FALSE;
}
GLboolean GL_APIENTRY glIsShader(GLuint name) { ++gl_calls; return shaders.contains(name); }
void GL_APIENTRY glDeleteShader(GLuint name) {
    ++gl_calls;
    if (shaders.erase(name)) ++deleted_shaders; else ++invalid_deletes;
}
GLuint GL_APIENTRY glCreateProgram() {
    ++gl_calls;
    if (++create_program_calls == fail_program_creation) return 0;
    const GLuint name = next_program++;
    programs.emplace(name, Program{});
    return name;
}
void GL_APIENTRY glAttachShader(GLuint program, GLuint shader) {
    ++gl_calls;
    programs.at(program).attached.push_back(shader);
}
void GL_APIENTRY glLinkProgram(GLuint name) {
    ++gl_calls; ++link_calls;
    auto& program = programs.at(name);
    program.linked = program.attached.size() == 2;
    for (GLuint shader_name : program.attached) {
        const auto& shader = shaders.at(shader_name);
        program.linked &= shader.compiled && shader.source.find("bad_link") == std::string::npos;
        (shader.target == GL_VERTEX_SHADER ? program.vertex : program.fragment) = shader.source;
    }
}
void GL_APIENTRY glGetProgramiv(GLuint name, GLenum query, GLint* value) {
    ++gl_calls;
    const auto& program = programs.at(name);
    if (query == GL_PROGRAM_BINARY_LENGTH)
        *value = forced_binary_length >= 0 ? forced_binary_length :
            program.linked && program.retrievable ? static_cast<GLint>(ProgramBytes(program).size() + 7) : 0;
    else *value = query == GL_LINK_STATUS && program.linked ? GL_TRUE : GL_FALSE;
}
void GL_APIENTRY glProgramParameteri(GLuint name, GLenum pname, GLint value) {
    ++gl_calls; ++hint_calls;
    if (pname == GL_PROGRAM_BINARY_RETRIEVABLE_HINT)
        programs.at(name).retrievable = value == GL_TRUE;
}
void GL_APIENTRY glGetProgramBinary(GLuint name, GLsizei buffer_size, GLsizei* length,
                                    GLenum* format, void* output) {
    ++gl_calls; ++binary_export_calls;
    const auto bytes = ProgramBytes(programs.at(name));
    if (buffer_size < static_cast<GLsizei>(bytes.size())) {
        pending_error = GL_INVALID_OPERATION;
        *length = 0;
        return;
    }
    *length = static_cast<GLsizei>(bytes.size());
    *format = invalid_export_format ? 0xf001u : kMockBinaryFormat;
    std::memcpy(output, bytes.data(), bytes.size());
}
void GL_APIENTRY glProgramBinary(GLuint name, GLenum format, const void* input, GLsizei length) {
    ++gl_calls; ++binary_load_calls;
    auto& program = programs.at(name);
    program.linked = false;
    if (std::find(supported_formats.begin(), supported_formats.end(), static_cast<GLint>(format)) ==
        supported_formats.end()) {
        pending_error = GL_INVALID_ENUM;
        return;
    }
    if (rejected_loads) { --rejected_loads; return; }
    const auto* bytes = static_cast<const uint8_t*>(input);
    if (length < 12 || std::memcmp(bytes, "MCK1", 4) != 0) return;
    const uint32_t vertex_size = ReadU32(bytes + 4), fragment_size = ReadU32(bytes + 8);
    if (uint64_t(vertex_size) + fragment_size + 12 != static_cast<uint32_t>(length)) return;
    program.vertex.assign(reinterpret_cast<const char*>(bytes + 12), vertex_size);
    program.fragment.assign(reinterpret_cast<const char*>(bytes + 12 + vertex_size), fragment_size);
    program.linked = true;
}
GLboolean GL_APIENTRY glIsProgram(GLuint name) { ++gl_calls; return programs.contains(name); }
void GL_APIENTRY glDeleteProgram(GLuint name) {
    ++gl_calls;
    if (programs.erase(name)) ++deleted_programs; else ++invalid_deletes;
}
}  // extern "C"

int main(int argc, char** argv) {
    static_assert(Bl2ShaderWarmup::kMaxPairs == 512);
    const std::string vertex = "#version 320 es\nvoid main(){gl_Position=vec4(0);}\n";
    const std::string fragment = "#version 320 es\nprecision highp float;\nout vec4 c;void main(){c=vec4(1);}\n";
    const std::string fragment2 = fragment + "// second pair\n";
    std::vector<uint8_t> table;
    Pair(table, vertex, fragment);
    Pair(table, vertex, fragment);  // Exact duplicate pair and both digests.
    Pair(table, vertex, fragment2); // Shared vertex is compiled once.
    {
        const auto before = gl_calls;
        std::vector<std::pair<std::string_view, std::string_view>> pairs;
        Check(Bl2ShaderWarmup::ParsePairs(table, pairs) && pairs.size() == 3 &&
              pairs[0] == pairs[1] && pairs[0].first == vertex &&
              pairs[2].second == fragment2, "exact source views preserve record order and duplicates");
        Check(gl_calls == before && pairs[0].first.data() ==
              reinterpret_cast<const char*>(table.data() + 8),
              "source parser makes no GL calls and references the input");
    }
    const auto vertex_digest = Bl2ShaderWarmup::Hash(vertex);
    const auto fragment_digest = Bl2ShaderWarmup::Hash(fragment);
    Check(ObbSha256::Hex(Bl2ShaderWarmup::Hash("abc")) ==
          std::array<char, 65>{'B','A','7','8','1','6','B','F','8','F','0','1','C','F','E','A',
              '4','1','4','1','4','0','D','E','5','D','A','E','2','2','2','3',
              'B','0','0','3','6','1','A','3','9','6','1','7','7','A','9','C',
              'B','4','1','0','F','F','6','1','F','2','0','0','1','5','A','D',0},
          "SHA256 uses existing exact implementation");
    {
        Bl2ShaderWarmup cache;
        const auto old_compile = compile_calls, old_link = link_calls;
        Check(cache.Warm(table), "valid table warms");
        Check(cache.BinaryHitCount() == 0, "default source-only API has zero binary hits");
        Check(compile_calls == old_compile + 3 && link_calls == old_link + 2,
              "stage and pair exact duplicates are deduplicated");
        Check(cache.TakeShader(GL_VERTEX_SHADER, vertex + " ") == 0,
              "source differs by one byte: fallback");
        Check(cache.TakeShader(GL_FRAGMENT_SHADER, vertex) == 0,
              "stage target is part of identity");
        Check(cache.TakeProgram(fragment_digest, vertex_digest) == 0,
              "pair digest order is exact");
        const GLuint actual_vertex = cache.TakeShader(GL_VERTEX_SHADER, vertex);
        const GLuint actual_fragment = cache.TakeShader(GL_FRAGMENT_SHADER, fragment);
        const GLuint actual_program = cache.TakeProgram(vertex_digest, fragment_digest);
        Check(actual_vertex && actual_fragment && actual_program, "exact source transfers objects");
        Check(shaders.at(actual_vertex).source == vertex &&
              shaders.at(actual_fragment).source == fragment &&
              programs.at(actual_program).vertex == vertex &&
              programs.at(actual_program).fragment == fragment,
              "actual linked source pair equals requested source bytes");
        Check(cache.TakeShader(GL_VERTEX_SHADER, vertex) == 0 &&
              cache.TakeShader(GL_FRAGMENT_SHADER, fragment) == 0 &&
              cache.TakeProgram(vertex_digest, fragment_digest) == 0,
              "objects cannot be transferred twice");
        std::vector<uint8_t> malformed = table;
        malformed.push_back(0x17);
        const auto before_gl = gl_calls;
        Check(!cache.Warm(malformed) && gl_calls == before_gl,
              "malformed later record neither calls GL nor deletes old cache");
        cache.Clear();
        Check(shaders.size() == 2 && programs.size() == 1 &&
              shaders.contains(actual_vertex) && shaders.contains(actual_fragment) &&
              programs.contains(actual_program), "clear deletes only still-owned objects");
        glDeleteProgram(actual_program);
        glDeleteShader(actual_fragment);
        glDeleteShader(actual_vertex);
    }
    Check(shaders.empty() && programs.empty(), "transferred objects released by caller exactly once");
    {
        Bl2ShaderWarmup cache;
        Check(cache.Warm(table) && cache.Warm(table), "valid replacement succeeds");
        Check(shaders.size() == 3 && programs.size() == 2, "old valid cache released on replacement");
    }
    Check(shaders.empty() && programs.empty(), "destructor cleans remaining valid handles");
    {
        Bl2ShaderWarmup cache;
        Check(cache.Warm(table), "context suspension fixture warms");
        current_context = EGL_NO_CONTEXT;
        const auto before_gl = gl_calls;
        cache.Clear();
        Check(gl_calls == before_gl && shaders.size() == 3 && programs.size() == 2,
              "temporarily unbound context retains cache without GL calls");
        Check(cache.TakeShader(GL_VERTEX_SHADER, vertex) == 0 &&
              cache.TakeProgram(vertex_digest, fragment_digest) == 0,
              "no context cannot transfer names");
        current_context = reinterpret_cast<EGLContext>(uintptr_t{1});
        const GLuint rebound_program = cache.TakeProgram(vertex_digest, fragment_digest);
        Check(rebound_program != 0,
              "rebind preserves owned object availability");
        // Simulate caller releasing the one program already transferred.
        glDeleteProgram(rebound_program);
    }
    Check(shaders.empty() && programs.empty(), "rebound cleanup completes");
    {
        Bl2ShaderWarmup cache;
        Check(cache.Warm(table), "context loss fixture warms");
        const GLuint reused_shader = shaders.begin()->first;
        const GLuint reused_program = programs.begin()->first;
        shaders.clear(); programs.clear(); // GL destroys the original share group.
        const auto before_gl = gl_calls;
        cache.Abandon();
        current_context = reinterpret_cast<EGLContext>(uintptr_t{2});
        shaders.emplace(reused_shader, Shader{GL_VERTEX_SHADER, "foreign source", true});
        programs.emplace(reused_program, Program{});
        Check(cache.TakeShader(GL_VERTEX_SHADER, vertex) == 0 &&
              cache.TakeProgram(vertex_digest, fragment_digest) == 0,
              "abandon prevents reuse of foreign names in replacement group");
        cache.Clear();
        Check(gl_calls == before_gl && shaders.contains(reused_shader) &&
              programs.contains(reused_program), "abandoned cleanup does not delete foreign names");
        shaders.clear(); programs.clear();
        current_context = reinterpret_cast<EGLContext>(uintptr_t{1});
    }
    {
        Bl2ShaderWarmup cache;
        std::vector<uint8_t> owned_input;
        Pair(owned_input, vertex, fragment);
        Check(cache.Warm(owned_input), "owned source fixture warms");
        std::fill(owned_input.begin(), owned_input.end(), uint8_t{0});
        const auto shader = cache.TakeShader(GL_VERTEX_SHADER, vertex);
        Check(shader && shaders.at(shader).source == vertex,
              "cache keeps exact source after input buffer changes");
        glDeleteShader(shader);
    }
    Check(shaders.empty() && programs.empty(), "owned input cleanup completes");
    for (const auto& bad : {std::string("bad_compile"), std::string("bad_link")}) {
        std::vector<uint8_t> bytes;
        Pair(bytes, vertex, fragment);
        Pair(bytes, vertex, bad);
        Bl2ShaderWarmup cache;
        Check(!cache.Warm(bytes), "compile/link failure falls back");
        Check(shaders.empty() && programs.empty(), "partial failure releases all created objects");
    }
    for (bool shader_failure : {true, false}) {
        if (shader_failure) fail_shader_creation = create_shader_calls + 2;
        else fail_program_creation = create_program_calls + 2;
        Bl2ShaderWarmup cache;
        Check(!cache.Warm(table), "GL allocation failure falls back");
        Check(shaders.empty() && programs.empty(), "allocation failure releases partial objects");
        fail_shader_creation = fail_program_creation = 0;
    }
    current_context = EGL_NO_CONTEXT;
    {
        const auto before = gl_calls;
        Bl2ShaderWarmup cache;
        Check(!cache.Warm(table) && gl_calls == before,
              "valid table with no GL context falls back without GL calls");
    }
    current_context = reinterpret_cast<EGLContext>(uintptr_t{1});
    Invalid({}, "empty table");
    for (size_t length = 1; length < 8; ++length)
        Invalid(std::vector<uint8_t>(length), "short header");
    std::vector<uint8_t> truncated;
    Pair(truncated, vertex, fragment);
    truncated.pop_back();
    Invalid(truncated, "truncated fragment");
    truncated.assign(8, 0);
    Invalid(truncated, "zero stage lengths");
    truncated.clear();
    U32(truncated, 0xffffffff); U32(truncated, 1);
    Invalid(truncated, "oversized stage length cannot overflow");
    truncated.clear();
    U32(truncated, Bl2ShaderWarmup::kMaxStageBytes + 1); U32(truncated, 1);
    Invalid(truncated, "per-stage cap");
    truncated.clear();
    U32(truncated, 2); U32(truncated, 1); truncated.push_back('v');
    Invalid(truncated, "truncated vertex");
    truncated.clear();
    Pair(truncated, std::string_view("a\0b", 3), "p");
    Invalid(truncated, "embedded NUL rejected");
    truncated.assign(Bl2ShaderWarmup::kMaxBytes + 1, 0);
    Invalid(truncated, "whole-table cap");
    truncated.clear();
    for (size_t i = 0; i <= Bl2ShaderWarmup::kMaxPairs; ++i) Pair(truncated, "v", "p");
    Invalid(truncated, "513 duplicate records are rejected before GL");
    {
        Bl2ShaderWarmup cache;
        truncated.clear();
        for (size_t i = 0; i < Bl2ShaderWarmup::kMaxPairs; ++i) Pair(truncated, "v", "p");
        Check(cache.Warm(truncated), "512 records at exact pair cap accepted");
        Check(shaders.size() == 2 && programs.size() == 1,
              "pair cap boundary still deduplicates exact identities");
    }
    {
        Bl2ShaderWarmup cache;
        const std::string maximum_stage(Bl2ShaderWarmup::kMaxStageBytes, 'x');
        truncated.clear();
        Pair(truncated, maximum_stage, "p");
        Check(cache.Warm(truncated), "exact per-stage cap accepted");
        const GLuint maximum_shader = cache.TakeShader(GL_VERTEX_SHADER, maximum_stage);
        Check(maximum_shader && shaders.at(maximum_shader).source.size() == maximum_stage.size(),
              "per-stage boundary preserves exact byte length");
        glDeleteShader(maximum_shader);
    }
    {
        Bl2ShaderWarmup cache;
        std::vector<uint8_t> equal_sources;
        Pair(equal_sources, "same", "same");
        Check(cache.Warm(equal_sources), "same digest on different shader targets is legal");
        Check(shaders.size() == 2, "different targets do not share one shader name");
        const GLuint disappeared = shaders.begin()->first;
        shaders.erase(disappeared);  // Simulated caller/context deletion.
        Check(cache.TakeShader(GL_VERTEX_SHADER, "same") == 0,
              "invalid native name cannot be transferred");
    }
    Check(shaders.empty() && programs.empty() && invalid_deletes == 0,
          "cleanup guards invalid handles and never double-deletes");

    std::string directory = std::string(argc > 1 ? argv[1] : ".") + "/bl2-shader-cache-check-XXXXXX";
    if (!::mkdtemp(directory.data())) {
        std::perror("mock cache directory");
        return 1;
    }
    const std::string cache_file = directory + "/bl2-program-cache.bin";
    if (argc > 2) {
        const auto captured_table = ReadFile(argv[2]);
        std::map<std::pair<std::string, std::string>, bool> captured_pairs;
        std::map<std::pair<GLenum, std::string>, bool> captured_stages;
        size_t offset = 0;
        bool valid = !captured_table.empty() && captured_table.size() <= Bl2ShaderWarmup::kMaxBytes;
        while (valid && offset < captured_table.size()) {
            if (captured_table.size() - offset < 8) { valid = false; break; }
            const auto vs = ReadU32(captured_table.data() + offset);
            const auto fs = ReadU32(captured_table.data() + offset + 4);
            offset += 8;
            if (!vs || !fs || vs > Bl2ShaderWarmup::kMaxStageBytes ||
                fs > Bl2ShaderWarmup::kMaxStageBytes ||
                uint64_t(vs) + fs > captured_table.size() - offset) { valid = false; break; }
            const std::string v(reinterpret_cast<const char*>(captured_table.data() + offset), vs);
            offset += vs;
            const std::string f(reinterpret_cast<const char*>(captured_table.data() + offset), fs);
            offset += fs;
            captured_pairs.emplace(std::make_pair(v, f), true);
            captured_stages.emplace(std::make_pair(GL_VERTEX_SHADER, v), true);
            captured_stages.emplace(std::make_pair(GL_FRAGMENT_SHADER, f), true);
        }
        Check(valid && offset == captured_table.size() && !captured_pairs.empty() &&
              captured_pairs.size() <= Bl2ShaderWarmup::kMaxPairs,
              "actual captured table has complete bounded records");
        if (valid) {
            const auto links = link_calls, compiles = compile_calls;
            {
                Bl2ShaderWarmup cache;
                Check(cache.Warm(captured_table, directory), "actual captured source table warms and persists");
                Check(link_calls == links + captured_pairs.size() && compile_calls == compiles + captured_stages.size(),
                      "captured programs compile/link only distinct exact stage sources");
            }
            const auto loads = binary_load_calls, warm_links = link_calls;
            {
                Bl2ShaderWarmup cache;
                Check(cache.Warm(captured_table, directory) && cache.BinaryHitCount() == captured_pairs.size() &&
                      binary_load_calls == loads + captured_pairs.size() && link_calls == warm_links,
                      "actual captured binary cache reload avoids every link");
                for (const auto& [stage, unused] : captured_stages) {
                    (void)unused;
                    const auto shader = cache.TakeShader(stage.first, stage.second);
                    Check(shader && shaders.at(shader).compiled && shaders.at(shader).source == stage.second,
                          "actual stage transfers retain target and exact source");
                    if (shader) glDeleteShader(shader);
                }
                for (const auto& [pair, unused] : captured_pairs) {
                    (void)unused;
                    const auto program = cache.TakeProgram(Bl2ShaderWarmup::Hash(pair.first),
                                                           Bl2ShaderWarmup::Hash(pair.second));
                    Check(program && programs.at(program).vertex == pair.first &&
                          programs.at(program).fragment == pair.second,
                          "actual prelinked program remains usable after stage transfers");
                    if (program) glDeleteProgram(program);
                }
            }
            Check(shaders.empty() && programs.empty(), "actual captured transfers retain exact cleanup ownership");
            Check(std::remove(cache_file.c_str()) == 0, "actual captured cache fixture retires only its own file");
        }
    }
    {
        Bl2ShaderWarmup cache;
        const auto links = link_calls, compiles = compile_calls, loads = binary_load_calls;
        const auto exports = binary_export_calls, hints = hint_calls;
        Check(cache.Warm(table, directory), "empty disk cache source-links successfully");
        Check(cache.BinaryHitCount() == 0, "cold source-linked cache records zero hits");
        Check(link_calls == links + 2 && compile_calls == compiles + 3 &&
              binary_load_calls == loads && binary_export_calls == exports + 2 && hint_calls == hints + 2,
              "cold cache preserves stage compiles and extracts each linked pair");
    }
    const auto good_file = ReadFile(cache_file);
    Check(good_file.size() > 44 + 104 && ReadU32(good_file.data() + 40) == 2,
          "bounded cache file contains only unique source pairs");
    if (good_file.size() <= 148) { ::rmdir(directory.c_str()); return 1; }
    const auto first_payload_size = ReadU32(good_file.data() + 112);
    Check(std::memcmp(good_file.data() + 148, "MCK1", 4) == 0 && first_payload_size > 12,
          "cache records use actual returned binary bytes");
    {
        Bl2ShaderWarmup cache;
        const auto links = link_calls, compiles = compile_calls, loads = binary_load_calls;
        const auto exports = binary_export_calls, reads = error_reads;
        const GLuint prior_next_program = next_program;
        pending_error = GL_INVALID_VALUE;
        Check(cache.Warm(table, directory), "second process-equivalent warm loads cache");
        Check(cache.BinaryHitCount() == 2, "successful warm publishes exact binary hit count");
        auto invalid_table = table;
        invalid_table.push_back(0);
        Check(!cache.Warm(invalid_table, directory) && cache.BinaryHitCount() == 2,
              "failed warm preserves previous binary-hit witness and owned objects");
        Check(link_calls == links && compile_calls == compiles + 3 &&
              binary_load_calls == loads + 2 && binary_export_calls == exports,
              "binary hits skip links/export but preserve compiled stage objects");
        const auto shader = cache.TakeShader(GL_VERTEX_SHADER, vertex);
        const auto program = cache.TakeProgram(vertex_digest, fragment_digest);
        Check(shader && shaders.at(shader).compiled && shaders.at(shader).source == vertex &&
              program >= prior_next_program && programs.at(program).attached.empty() &&
              programs.at(program).vertex == vertex && programs.at(program).fragment == fragment,
              "cache reload owns fresh objects and keeps exact fallback shader/source pair");
        Check(!cache.TakeShader(GL_VERTEX_SHADER, vertex) && !cache.TakeProgram(vertex_digest, fragment_digest),
              "binary reload transfers objects only once");
        Check(pending_error == GL_INVALID_VALUE && error_reads == reads && glGetError() == GL_INVALID_VALUE,
              "cache never reads or conceals pre-existing GL error");
        glDeleteProgram(program); glDeleteShader(shader);
    }
    Check(ReadFile(cache_file) == good_file, "all-hit warm does not rewrite or re-extract cache");
    Check(shaders.empty() && programs.empty(), "binary transfers and owned remainders clean up");

    std::vector<std::vector<uint8_t>> malformed_caches;
    auto damaged = good_file; damaged[0] ^= 1; malformed_caches.push_back(damaged);
    damaged = good_file; damaged.back() ^= 1; malformed_caches.push_back(damaged);
    damaged = good_file; damaged.pop_back(); malformed_caches.push_back(damaged);
    damaged = good_file; damaged[108] ^= 0x80; malformed_caches.push_back(damaged); // Unsupported format.
    damaged = good_file; std::fill(damaged.begin() + 112, damaged.begin() + 116, uint8_t{0xff});
    malformed_caches.push_back(damaged); // Length overflow/cap.
    damaged = good_file; damaged[44] ^= 1; malformed_caches.push_back(damaged); // Wrong pair digest.
    damaged = good_file; damaged.push_back(0); malformed_caches.push_back(damaged);
    damaged.assign(good_file.begin(), good_file.begin() + 44);
    const auto first_record_end = good_file.begin() + 148 + first_payload_size;
    damaged.insert(damaged.end(), good_file.begin() + 44, first_record_end);
    damaged.insert(damaged.end(), good_file.begin() + 44, first_record_end);
    malformed_caches.push_back(damaged); // Duplicate pair.
    damaged = good_file; std::fill(damaged.begin() + 40, damaged.begin() + 44, uint8_t{0xff});
    malformed_caches.push_back(damaged); // Record-count cap.
    for (const auto& invalid : malformed_caches) {
        Check(WriteFile(cache_file, invalid), "invalid-cache fixture written");
        Bl2ShaderWarmup cache;
        const auto loads = binary_load_calls, links = link_calls;
        Check(cache.Warm(table, directory) && binary_load_calls == loads && link_calls == links + 2,
              "invalid whole cache falls back before any binary GL call");
    }
    {
        Check(WriteFile(cache_file, good_file), "driver-rejection fixture restored");
        Bl2ShaderWarmup cache;
        const auto loads = binary_load_calls, links = link_calls, exports = binary_export_calls;
        rejected_loads = 1;
        Check(cache.Warm(table, directory) && binary_load_calls == loads + 2 &&
              link_calls == links + 1 && binary_export_calls == exports + 1 && pending_error == GL_NO_ERROR,
              "driver-rejected binary source-links on its private program without GL error");
        Check(cache.BinaryHitCount() == 1, "driver rejection counts only successful binary loads");
    }
    {
        Check(WriteFile(cache_file, good_file), "driver-change fixture restored");
        driver_version = "OpenGL ES 3.2 mock-driver-2";
        Bl2ShaderWarmup cache;
        const auto loads = binary_load_calls, links = link_calls;
        Check(cache.Warm(table, directory) && binary_load_calls == loads && link_calls == links + 2,
              "real host driver version change invalidates binary identity");
        driver_version = "OpenGL ES 3.2 mock-driver-1";
    }
    {
        Check(WriteFile(cache_file, good_file), "source-change fixture restored");
        std::vector<uint8_t> changed_table;
        Pair(changed_table, vertex + "// normalized source changed\n", fragment);
        Pair(changed_table, vertex + "// normalized source changed\n", fragment2);
        Bl2ShaderWarmup cache;
        const auto loads = binary_load_calls, links = link_calls;
        Check(cache.Warm(changed_table, directory) && binary_load_calls == loads && link_calls == links + 2,
              "exact source change invalidates cached programs");
    }
    {
        Check(WriteFile(cache_file, good_file), "unsupported-driver fixture restored");
        supported_formats.clear();
        Bl2ShaderWarmup cache;
        const auto loads = binary_load_calls, links = link_calls, exports = binary_export_calls;
        Check(cache.Warm(table, directory) && binary_load_calls == loads && link_calls == links + 2 &&
              binary_export_calls == exports && ReadFile(cache_file) == good_file,
              "zero supported formats leaves existing source behavior and disk file unchanged");
        supported_formats.push_back(kMockBinaryFormat);
    }
    {
        Bl2ShaderWarmup cache;
        const auto links = link_calls;
        Check(cache.Warm(table, directory + "/absent") && link_calls == links + 2,
              "unavailable cache directory does not fail warmup");
    }
    for (GLint export_length : {0, static_cast<GLint>(Bl2ShaderWarmup::kMaxBinaryBytes + 1)}) {
        std::remove(cache_file.c_str());
        forced_binary_length = export_length;
        Bl2ShaderWarmup cache;
        const auto exports = binary_export_calls;
        Check(cache.Warm(table, directory) && binary_export_calls == exports && ReadFile(cache_file).empty(),
              "zero/oversized export length disables persistence without failing warmup");
    }
    forced_binary_length = -1;
    {
        invalid_export_format = true;
        Bl2ShaderWarmup cache;
        Check(cache.Warm(table, directory) && ReadFile(cache_file).empty(),
              "unexpected export format never creates a disk record");
        invalid_export_format = false;
    }
    {
        Check(WriteFile(cache_file, good_file), "old-cache preservation fixture restored");
        std::vector<uint8_t> failed_table;
        Pair(failed_table, vertex, "bad_link");
        Bl2ShaderWarmup cache;
        Check(!cache.Warm(failed_table, directory) && ReadFile(cache_file) == good_file,
              "authored source link failure does not publish partial disk cache");
    }
    Check(shaders.empty() && programs.empty() && invalid_deletes == 0 && pending_error == GL_NO_ERROR,
          "all cache hit/miss/failure paths clean owned objects without double delete or GL error");
    Check(std::remove(cache_file.c_str()) == 0 && ::rmdir(directory.c_str()) == 0,
          "atomic cache writes leave no temporary files or test directory");
    std::printf("bl2_shader_warmup: %u checks, %u failures, shaders_deleted=%u programs_deleted=%u\n",
                checks, failures, deleted_shaders, deleted_programs);
    return failures ? 1 : 0;
}
