#include "../src/bl2_shader_stage_cache.h"

#include <filesystem>
#include <map>
#include <utility>

namespace {
struct Shader { GLenum type; std::string source; bool compiled = false; };
struct Program { GLuint shader = 0; GLenum type = 0; std::string source; bool separable = false, linked = false; };
std::map<GLuint, Shader> shaders;
std::map<GLuint, Program> programs;
GLuint nextName = 1;
unsigned checks = 0, failures = 0, compiles = 0, links = 0, imports = 0;
unsigned peakShaders = 0, peakPrograms = 0, errorReads = 0;
bool exportFails = false, importFails = false;
unsigned failExportAtCompile = 0;
std::string driver = "OpenGL ES 3.2 actual-test-driver";
EGLContext context = reinterpret_cast<EGLContext>(uintptr_t{1});
constexpr GLenum binaryFormat = 0x91f1;
void Check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void Put(std::vector<uint8_t>& out, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(n >> (i * 8)));
}
ObbSha256::Digest Hash(std::span<const uint8_t> bytes) {
    ObbSha256 h; h.Update(bytes.data(), bytes.size()); return h.Finalize();
}
std::string Source(bool vertex, std::string_view marker = {}) {
    return std::string("#version 320 es\nprecision highp float;\n#define ENABLE_UNIFORM_BUFFER_OBJECTS 0\n"
                       "#define ENABLE_MULTIDRAW_INDIRECT 0\n") +
        (vertex ? "out gl_PerVertex {\nvec4 gl_Position;\n};\n" : "") +
        "//" + std::string(marker) + "\nvoid main() {}\n";
}
std::vector<uint8_t> Table(const std::vector<std::pair<GLenum, std::string>>& entries) {
    std::map<std::pair<GLenum, ObbSha256::Digest>, std::string> sorted;
    for (const auto& [type, source] : entries) {
        const auto raw = std::span(reinterpret_cast<const uint8_t*>(source.data()), source.size());
        sorted[{type, Hash(raw)}] = source;
    }
    std::vector<uint8_t> payload;
    for (const auto& [key, source] : sorted) {
        Put(payload, key.first); Put(payload, static_cast<uint32_t>(source.size()));
        payload.insert(payload.end(), key.second.begin(), key.second.end());
        payload.insert(payload.end(), source.begin(), source.end());
    }
    const auto digest = Hash(payload);
    std::vector<uint8_t> out(Bl2ShaderStageCache::kTableMagic.begin(), Bl2ShaderStageCache::kTableMagic.end());
    Put(out, 1); Put(out, static_cast<uint32_t>(sorted.size()));
    Put(out, static_cast<uint32_t>(payload.size())); Put(out, 0);
    out.insert(out.end(), digest.begin(), digest.end()); out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
struct Stream {
    std::vector<uint8_t> bytes;
    size_t at = 0, total = 0, chunk = 7;
    static size_t Read(void* opaque, uint8_t* out, size_t n) {
        auto& s = *static_cast<Stream*>(opaque);
        n = std::min({n, s.chunk, s.bytes.size() - s.at});
        std::copy_n(s.bytes.data() + s.at, n, out); s.at += n; s.total += n; return n;
    }
    static bool Rewind(void* opaque) { static_cast<Stream*>(opaque)->at = 0; return true; }
    Bl2ShaderStageCache::Reader Reader() { return {this, bytes.size(), Read, Rewind}; }
};
struct Progress {
    uint32_t last = 0, total = 0, failures = 0;
    unsigned posts = 0;
    bool sawFinal = false, completedWithoutMarker = false;
    std::string root;
    static void Call(void* p, uint32_t done, uint32_t total) {
        auto& s = *static_cast<Progress*>(p); s.last = done; s.total = total; s.failures += total == 0;
        ++s.posts;
        if (total && done == total) {
            s.sawFinal = true;
            bool marker = false;
            for (const auto& e : std::filesystem::recursive_directory_iterator(s.root))
                marker |= e.is_regular_file() && e.path().filename() == "complete.bin";
            s.completedWithoutMarker |= !marker;
        }
    }
};
struct FileStream {
    FILE* file;
    uint64_t bytes, total = 0;
    static size_t Read(void* p, uint8_t* out, size_t n) {
        auto& s = *static_cast<FileStream*>(p);
        const size_t got = std::fread(out, 1, std::min(n, size_t{4096}), s.file);
        s.total += got; return got;
    }
    static bool Rewind(void* p) { return std::fseek(static_cast<FileStream*>(p)->file, 0, SEEK_SET) == 0; }
    Bl2ShaderStageCache::Reader Reader() { return {this, bytes, Read, Rewind}; }
};
std::string Find(const std::string& root, std::string_view prefix) {
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().filename().string().starts_with(prefix)) return e.path().string();
    return {};
}
void DamageFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r+b");
    if (!f) { Check(false, "test owns existing file"); return; }
    std::fseek(f, -1, SEEK_END); const int value = std::fgetc(f); std::fseek(f, -1, SEEK_END);
    std::fputc(value ^ 1, f); std::fclose(f);
}
}

extern "C" {
EGLContext EGLAPIENTRY eglGetCurrentContext() { return context; }
const GLubyte* GL_APIENTRY glGetString(GLenum name) {
    return reinterpret_cast<const GLubyte*>(name == GL_VERSION ? driver.c_str() :
        name == GL_VENDOR ? "test-vendor" : name == GL_RENDERER ? "test-renderer" : "GLSL ES 3.20 test");
}
void GL_APIENTRY glGetIntegerv(GLenum name, GLint* value) {
    *value = name == GL_MAJOR_VERSION ? 3 : name == GL_MINOR_VERSION ? 2 :
        name == GL_NUM_PROGRAM_BINARY_FORMATS ? 1 : name == GL_PROGRAM_BINARY_FORMATS ? static_cast<GLint>(binaryFormat) : 64;
}
GLenum GL_APIENTRY glGetError() { ++errorReads; return GL_NO_ERROR; }
GLuint GL_APIENTRY glCreateShader(GLenum type) {
    const GLuint name = nextName++; shaders[name] = {type, {}};
    peakShaders = std::max(peakShaders, static_cast<unsigned>(shaders.size())); return name;
}
void GL_APIENTRY glShaderSource(GLuint name, GLsizei count, const GLchar* const* source, const GLint* length) {
    if (count == 1) shaders.at(name).source.assign(source[0], static_cast<size_t>(length[0]));
}
void GL_APIENTRY glCompileShader(GLuint name) {
    ++compiles; shaders.at(name).compiled = shaders.at(name).source.find("FAIL_COMPILE") == std::string::npos;
}
void GL_APIENTRY glGetShaderiv(GLuint name, GLenum, GLint* value) { *value = shaders.at(name).compiled ? GL_TRUE : GL_FALSE; }
void GL_APIENTRY glDeleteShader(GLuint name) { Check(shaders.erase(name) == 1, "shader deleted exactly once"); }
GLuint GL_APIENTRY glCreateProgram() {
    const GLuint name = nextName++; programs[name] = {};
    peakPrograms = std::max(peakPrograms, static_cast<unsigned>(programs.size())); return name;
}
void GL_APIENTRY glProgramParameteri(GLuint name, GLenum pname, GLint value) {
    if (pname == GL_PROGRAM_SEPARABLE) programs.at(name).separable = value == GL_TRUE;
}
void GL_APIENTRY glAttachShader(GLuint name, GLuint shader) { programs.at(name).shader = shader; }
void GL_APIENTRY glLinkProgram(GLuint name) {
    ++links; auto& p = programs.at(name); const auto& s = shaders.at(p.shader);
    p.linked = s.compiled && p.separable; p.source = s.source; p.type = s.type;
}
void GL_APIENTRY glGetProgramiv(GLuint name, GLenum query, GLint* value) {
    const auto& p = programs.at(name);
    *value = query == GL_LINK_STATUS ? p.linked : query == GL_PROGRAM_SEPARABLE ? p.separable :
        query == GL_PROGRAM_BINARY_LENGTH ? (exportFails || (failExportAtCompile && compiles == failExportAtCompile) ? 0 : static_cast<GLint>(p.source.size() + 4)) : 0;
}
void GL_APIENTRY glGetProgramBinary(GLuint name, GLsizei size, GLsizei* written, GLenum* format, void* output) {
    const auto& p = programs.at(name);
    std::vector<uint8_t> bytes; Put(bytes, p.type); bytes.insert(bytes.end(), p.source.begin(), p.source.end());
    *format = binaryFormat; *written = static_cast<GLsizei>(bytes.size());
    if (size >= *written) std::memcpy(output, bytes.data(), bytes.size());
}
void GL_APIENTRY glProgramBinary(GLuint name, GLenum format, const void* input, GLsizei size) {
    ++imports; auto& p = programs.at(name);
    Check(p.separable, "SEPARABLE input mode set before binary import");
    p.linked = !importFails && p.separable && format == binaryFormat && size > 4;
    if (p.linked) {
        const auto* bytes = static_cast<const uint8_t*>(input);
        p.type = U32(bytes); p.source.assign(reinterpret_cast<const char*>(bytes + 4), static_cast<size_t>(size - 4));
    }
}
void GL_APIENTRY glDeleteProgram(GLuint name) { Check(programs.erase(name) == 1, "program deleted exactly once"); }
}

int main(int argc, char** argv) {
    if (argc < 2 || std::filesystem::exists(argv[1])) return 2;
    const std::string root = argv[1]; std::filesystem::create_directories(root);
    const std::string vertex = Source(true), fragment = Source(false);
    Stream source{Table({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}})};
    Progress progress; progress.root = root;
    Bl2ShaderStageCache cache;
    Check(cache.PrepareStream(source.Reader(), root, Progress::Call, &progress), "first preparation succeeds");
    Check(cache.Stats().stageCount == 2 && cache.Stats().compiled == 2 && cache.Stats().binaryHits == 0,
          "first prep actual two source compiles");
    Check(progress.last == 2 && progress.total == 2 && !progress.failures, "completed progress only after persisted stages");
    Check(progress.sawFinal && !progress.completedWithoutMarker && progress.posts <= 102,
          "final UI completion observes already committed marker with bounded posts");
    Check(shaders.empty() && programs.empty() && peakShaders == 1 && peakPrograms == 1, "first prep retains at most one GL stage/program");
    Check(!Find(root, "complete.bin").empty(), "complete marker exists after exports");
    Stream again{source.bytes}; Bl2ShaderStageCache next;
    const unsigned before = compiles, linksBefore = links;
    Check(next.PrepareStream(again.Reader(), root), "committed next preparation succeeds");
    Check(again.total == 56 && next.Stats().preparedFromMarker && next.Stats().compiled == 0 &&
          next.Stats().binaryHits == 0 && compiles == before && links == linksBefore,
          "second startup header-only, zero source compile/link/import");
    std::string padded;
    Check(legacy_glsl::CompleteSsoInterface(vertex, true, &padded), "real common-interface helper used");
    const GLuint a = next.LoadProgram(GL_VERTEX_SHADER, vertex, padded), b = next.LoadProgram(GL_VERTEX_SHADER, vertex, padded);
    Check(a && b && a != b && compiles == before && next.Stats().binaryHits == 2, "binary-first loads return separate caller-owned programs");
    glDeleteProgram(a); glDeleteProgram(b);
    context = reinterpret_cast<EGLContext>(uintptr_t{2});
    const GLuint other = next.LoadProgram(GL_VERTEX_SHADER, vertex, padded);
    Check(other && compiles == before, "unshared context gets fresh binary program, no resident cache names"); glDeleteProgram(other);
    Check(!next.LoadProgram(0x8620, vertex, padded) && !next.LoadProgram(GL_VERTEX_SHADER, vertex, padded + "\n"),
          "ARB enum and noncanonical padded bytes rejected");
    DamageFile(Find(root, "v-"));
    const GLuint repaired = next.LoadProgram(GL_VERTEX_SHADER, vertex, padded);
    Check(repaired && compiles == before + 1, "corrupted file causes genuine lazy source compile"); glDeleteProgram(repaired);
    importFails = true;
    const GLuint rejected = next.LoadProgram(GL_VERTEX_SHADER, vertex, padded);
    Check(rejected && compiles == before + 2, "driver-rejected binary genuinely recompiles"); glDeleteProgram(rejected); importFails = false;
    const std::string unseen = Source(true, "unseen"), interruptedRoot = root + "/interrupted";
    std::filesystem::create_directory(interruptedRoot);
    Progress interruptedProgress; interruptedProgress.root = interruptedRoot;
    Stream interrupted{Table({{GL_VERTEX_SHADER, vertex}, {GL_FRAGMENT_SHADER, fragment}})};
    failExportAtCompile = compiles + 2; Bl2ShaderStageCache partial;
    Check(!partial.PrepareStream(interrupted.Reader(), interruptedRoot, Progress::Call, &interruptedProgress), "export failure rejects preparation");
    Check(Find(interruptedRoot, "complete.bin").empty() && interruptedProgress.total == 0 && !interruptedProgress.sawFinal,
          "no complete marker or success UI after export failure");
    failExportAtCompile = 0; Stream resume{interrupted.bytes}; Bl2ShaderStageCache resumed;
    Check(resumed.PrepareStream(resume.Reader(), interruptedRoot) && resumed.Stats().compiled == 1 &&
          resumed.Stats().binaryHits == 1, "resume imports existing stage and compiles only missing stage");
    std::string unseenPadded; Check(legacy_glsl::CompleteSsoInterface(unseen, true, &unseenPadded), "unseen source canonical padding");
    const GLuint lazy = resumed.LoadProgram(GL_VERTEX_SHADER, unseen, unseenPadded);
    Check(lazy && resumed.Stats().compiled == 2, "supported unforeseen source genuinely compiled/persisted"); glDeleteProgram(lazy);
    const std::string noDisk = Source(true, "disk-full"), noCompile = Source(true, "FAIL_COMPILE");
    std::string noDiskPadded, noCompilePadded;
    Check(legacy_glsl::CompleteSsoInterface(noDisk, true, &noDiskPadded) &&
          legacy_glsl::CompleteSsoInterface(noCompile, true, &noCompilePadded), "runtime failure controls have original valid schema");
    exportFails = true;
    const GLuint transient = resumed.LoadProgram(GL_VERTEX_SHADER, noDisk, noDiskPadded);
    Check(transient && Find(interruptedRoot, "complete.bin").empty(), "export failure keeps real runtime program, retires complete marker");
    glDeleteProgram(transient); exportFails = false;
    Check(!resumed.LoadProgram(GL_VERTEX_SHADER, noCompile, noCompilePadded) && shaders.empty() && programs.empty(),
          "actual failed shader compile returns zero with original fallback and no leaks");
    const std::string blockedRoot = root + "/blocked-marker"; std::filesystem::create_directory(blockedRoot);
    Stream blocked{source.bytes}; Bl2ShaderStageCache blockedCache; exportFails = true;
    Check(!blockedCache.PrepareStream(blocked.Reader(), blockedRoot), "create owned interrupted namespace for marker failure");
    exportFails = false;
    const auto cacheNamespace = std::filesystem::directory_iterator(blockedRoot)->path();
    std::filesystem::create_directory(cacheNamespace / "complete.bin");
    Stream retryBlocked{source.bytes}; Progress markerProgress; markerProgress.root = blockedRoot;
    Check(!blockedCache.PrepareStream(retryBlocked.Reader(), blockedRoot, Progress::Call, &markerProgress) &&
          blockedCache.Stats().compiled == 2 && !Find(blockedRoot, "v-").empty() && !Find(blockedRoot, "f-").empty(),
          "marker rename failure follows successful export of both real stages");
    Check(!markerProgress.sawFinal && markerProgress.total == 0 && markerProgress.posts <= 102,
          "marker commit failure never publishes final total and explicitly clears UI");
    Stream invalid{source.bytes}; invalid.bytes.back() ^= 1; Bl2ShaderStageCache broken;
    const unsigned invalidBefore = compiles;
    const std::string invalidRoot = root + "/invalid"; std::filesystem::create_directory(invalidRoot);
    Check(!broken.PrepareStream(invalid.Reader(), invalidRoot) && compiles == invalidBefore &&
          std::filesystem::is_empty(invalidRoot), "whole-pack corruption rejected before objects/files");
    Stream excessive{source.bytes}; excessive.bytes[12] = 0x51; excessive.bytes[13] = 0xc3; excessive.bytes[14] = excessive.bytes[15] = 0;
    Check(!broken.PrepareStream(excessive.Reader(), invalidRoot) && compiles == invalidBefore, "50001 stage bound rejects before GL objects");
    Stream badHash{source.bytes}; badHash.bytes[24] ^= 1;
    Check(!broken.PrepareStream(badHash.Reader(), invalidRoot) && compiles == invalidBefore, "manifest digest mismatch rejects before objects");
    Stream truncated{source.bytes}; truncated.bytes.pop_back();
    Check(!broken.PrepareStream(truncated.Reader(), invalidRoot) && compiles == invalidBefore, "header size vs truncated stream rejects before objects");
    context = EGL_NO_CONTEXT;
    Check(!next.LoadProgram(GL_VERTEX_SHADER, vertex, padded), "missing context rejects without exposing old GL names");
    context = reinterpret_cast<EGLContext>(uintptr_t{2});
    driver += " changed"; Stream changed{source.bytes}; Bl2ShaderStageCache newDriver;
    Check(newDriver.PrepareStream(changed.Reader(), root) && newDriver.Stats().compiled == 2 &&
          !newDriver.Stats().preparedFromMarker, "driver identity change never reuses old binary/marker");
    Check(shaders.empty() && programs.empty() && errorReads == 0, "no leaked objects or swallowed guest errors");
    if (argc == 3) {
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) return 2;
        std::fseek(f, 0, SEEK_END); const long n = std::ftell(f); std::rewind(f);
        if (n <= 0) { std::fclose(f); return 2; }
        FileStream all{f, static_cast<uint64_t>(n)};
        const std::string allRoot = root + "/full"; std::filesystem::create_directory(allRoot);
        Bl2ShaderStageCache full; exportFails = true; const unsigned start = compiles;
        Check(!full.PrepareStream(all.Reader(), allRoot) && full.Stats().stageCount == 38980 &&
              compiles == start + 1 && all.total >= all.bytes && Find(allRoot, "complete.bin").empty(),
              "actual38980 full stream validated, first export failure leaves no marker");
        exportFails = false; std::fclose(f);
    }
    std::printf("STAGE_CACHE_CHECK checks=%u failures=%u compiles=%u links=%u imports=%u peak_shader=%u peak_program=%u\n",
                checks, failures, compiles, links, imports, peakShaders, peakPrograms);
    return failures ? 1 : 0;
}
