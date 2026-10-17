#pragma once

#include "obb_sha256.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

// Caller serializes access and keeps the original GL share group current.
// Abandon before entering a new unshared group: GL names alone cannot identify
// a share group, and glIs* can succeed for a reused name belonging to that group.
// Take transfers each object once; ordinary stages/pipelines then own deletion.
class Bl2ShaderWarmup {
public:
    using Digest = ObbSha256::Digest;
    static constexpr size_t kMaxBytes = 8u * 1024u * 1024u;
    static constexpr size_t kMaxPairs = 512;
    static constexpr size_t kMaxStageBytes = 128u * 1024u;
    static constexpr size_t kMaxBinaryBytes = 8u * 1024u * 1024u;
    static constexpr size_t kMaxBinaryCacheBytes = 32u * 1024u * 1024u;

    Bl2ShaderWarmup() = default;
    ~Bl2ShaderWarmup() { Clear(); }
    Bl2ShaderWarmup(const Bl2ShaderWarmup&) = delete;
    Bl2ShaderWarmup& operator=(const Bl2ShaderWarmup&) = delete;
    size_t BinaryHitCount() const { return binary_hit_count_; }

    static Digest Hash(std::string_view source) {
        ObbSha256 hash;
        hash.Update(reinterpret_cast<const uint8_t*>(source.data()), source.size());
        return hash.Finalize();
    }

    // Views retain the input's lifetime; malformed tables leave output intact.
    static bool ParsePairs(std::span<const uint8_t> bytes,
                           std::vector<std::pair<std::string_view, std::string_view>>& output) {
        if (bytes.empty() || bytes.size() > kMaxBytes) return false;
        std::vector<std::pair<std::string_view, std::string_view>> pairs;
        size_t offset = 0, pair_count = 0;
        try {
            while (offset < bytes.size()) {
                if (++pair_count > kMaxPairs || bytes.size() - offset < 8) return false;
                const uint32_t vertex_bytes = Read32(bytes.data() + offset);
                const uint32_t fragment_bytes = Read32(bytes.data() + offset + 4);
                offset += 8;
                if (!vertex_bytes || !fragment_bytes ||
                    vertex_bytes > kMaxStageBytes || fragment_bytes > kMaxStageBytes ||
                    vertex_bytes > bytes.size() - offset) return false;
                const std::string_view vertex(
                    reinterpret_cast<const char*>(bytes.data() + offset), vertex_bytes);
                offset += vertex_bytes;
                if (fragment_bytes > bytes.size() - offset) return false;
                const std::string_view fragment(
                    reinterpret_cast<const char*>(bytes.data() + offset), fragment_bytes);
                offset += fragment_bytes;
                if (vertex.find('\0') != vertex.npos || fragment.find('\0') != fragment.npos)
                    return false;
                pairs.emplace_back(vertex, fragment);
            }
        } catch (...) {
            return false;
        }
        output.swap(pairs);
        return true;
    }

    // Repeated little-endian (u32 VSbytes, u32 PSbytes, VSsource, PSsource).
    // Parse the entire table before GL; invalid input preserves the old cache.
    bool Warm(std::span<const uint8_t> bytes, std::string_view cache_directory = {}) {
        std::vector<std::pair<std::string_view, std::string_view>> pairs;
        if (!ParsePairs(bytes, pairs)) return false;
        Bl2ShaderWarmup candidate;
        try {
            for (const auto& [vertex, fragment] : pairs) {
                const size_t vertex_index = candidate.AddStage(GL_VERTEX_SHADER, vertex);
                const size_t fragment_index = candidate.AddStage(GL_FRAGMENT_SHADER, fragment);
                const auto& vertex_digest = candidate.stages_[vertex_index].digest;
                const auto& fragment_digest = candidate.stages_[fragment_index].digest;
                const auto existing = std::find_if(candidate.programs_.begin(), candidate.programs_.end(),
                    [&](const Program& entry) {
                        return entry.vertex == vertex_digest && entry.fragment == fragment_digest;
                    });
                if (existing == candidate.programs_.end()) {
                    candidate.programs_.push_back(
                        {vertex_digest, fragment_digest, vertex_index, fragment_index, 0});
                }
            }
        } catch (...) {
            return false;
        }
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) return false;
        std::string cache_path;
        Digest cache_identity{};
        std::vector<GLint> binary_formats;
        std::vector<Binary> cached_binaries, saved_binaries;
        bool binary_cache = BinaryIdentity(bytes, cache_directory, cache_path,
                                           cache_identity, binary_formats);
        if (binary_cache)
            ReadBinaries(cache_path, cache_identity, binary_formats,
                         candidate.programs_, cached_binaries);
        bool refresh_binaries = false;
        size_t binary_hits = 0;
        size_t saved_bytes = kBinaryHeaderBytes;
        for (auto& stage : candidate.stages_) {
            stage.shader = glCreateShader(stage.target);
            if (!stage.shader) return false;
            const GLchar* source = stage.source.data();
            const GLint length = static_cast<GLint>(stage.source.size());
            glShaderSource(stage.shader, 1, &source, &length);
            glCompileShader(stage.shader);
            GLint compiled = GL_FALSE;
            glGetShaderiv(stage.shader, GL_COMPILE_STATUS, &compiled);
            if (compiled != GL_TRUE) return false;
        }
        for (auto& program : candidate.programs_) {
            program.program = glCreateProgram();
            if (!program.program) return false;
            GLint linked = GL_FALSE;
            auto binary = std::find_if(cached_binaries.begin(), cached_binaries.end(),
                [&](const Binary& entry) {
                    return entry.vertex == program.vertex && entry.fragment == program.fragment;
                });
            if (binary_cache && binary != cached_binaries.end()) {
                glProgramBinary(program.program, binary->format, binary->bytes.data(),
                                static_cast<GLsizei>(binary->bytes.size()));
                glGetProgramiv(program.program, GL_LINK_STATUS, &linked);
            }
            const bool binary_hit = linked == GL_TRUE;
            if (binary_hit) ++binary_hits;
            if (!binary_hit) {
                if (binary_cache)
                    glProgramParameteri(program.program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
                glAttachShader(program.program, candidate.stages_[program.vertex_index].shader);
                glAttachShader(program.program, candidate.stages_[program.fragment_index].shader);
                glLinkProgram(program.program);
                glGetProgramiv(program.program, GL_LINK_STATUS, &linked);
            }
            if (linked != GL_TRUE) return false;
            if (binary_cache) {
                try {
                    Binary saved{program.vertex, program.fragment, 0, {}};
                    if (binary_hit) saved = std::move(*binary);
                    else {
                        refresh_binaries = true;
                        if (!ExportBinary(program.program, binary_formats, saved)) continue;
                    }
                    const size_t record_bytes = kBinaryRecordBytes + saved.bytes.size();
                    if (record_bytes > kMaxBinaryCacheBytes - saved_bytes) {
                        binary_cache = false;
                        saved_binaries.clear();
                        cached_binaries.clear();
                        continue;
                    }
                    saved_bytes += record_bytes;
                    saved_binaries.push_back(std::move(saved));
                } catch (...) {
                    // Disk caching is optional even under memory pressure.
                    binary_cache = false;
                    saved_binaries.clear();
                    cached_binaries.clear();
                }
            }
        }
        if (binary_cache && refresh_binaries && !saved_binaries.empty())
            WriteBinaries(cache_path, cache_identity, saved_binaries);
        stages_.swap(candidate.stages_);
        programs_.swap(candidate.programs_);
        binary_hit_count_ = binary_hits;
        return true;
    }

    GLuint TakeShader(GLenum target, std::string_view exact_normalized_source) {
        if (stages_.empty() ||
            (target != GL_VERTEX_SHADER && target != GL_FRAGMENT_SHADER) ||
            exact_normalized_source.empty() || exact_normalized_source.size() > kMaxStageBytes)
            return 0;
        const Digest digest = Hash(exact_normalized_source);
        const auto found = std::find_if(stages_.begin(), stages_.end(), [&](const Stage& stage) {
            return stage.target == target && stage.digest == digest &&
                   stage.source == exact_normalized_source;
        });
        if (found == stages_.end() || eglGetCurrentContext() == EGL_NO_CONTEXT ||
            glIsShader(found->shader) != GL_TRUE) return 0;
        const GLuint shader = found->shader;
        stages_.erase(found);
        return shader;
    }

    GLuint TakeProgram(const Digest& vertex, const Digest& fragment) {
        const auto found = std::find_if(programs_.begin(), programs_.end(), [&](const Program& entry) {
            return entry.vertex == vertex && entry.fragment == fragment;
        });
        if (found == programs_.end() || eglGetCurrentContext() == EGL_NO_CONTEXT ||
            glIsProgram(found->program) != GL_TRUE) return 0;
        const GLuint program = found->program;
        programs_.erase(found);
        return program;
    }

    // Clear with this cache's GL share group current. A temporarily unbound
    // context does not imply loss: retain ownership until a valid bind+Clear.
    void Clear() {
        const bool owns_objects =
            std::any_of(programs_.begin(), programs_.end(),
                        [](const Program& entry) { return entry.program != 0; }) ||
            std::any_of(stages_.begin(), stages_.end(),
                        [](const Stage& entry) { return entry.shader != 0; });
        if (!owns_objects) { Abandon(); return; }
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) return;
        for (const auto& entry : programs_) {
            if (entry.program && glIsProgram(entry.program) == GL_TRUE)
                glDeleteProgram(entry.program);
        }
        for (const auto& entry : stages_) {
            if (entry.shader && glIsShader(entry.shader) == GL_TRUE)
                glDeleteShader(entry.shader);
        }
        Abandon();
    }

    // Original share group was destroyed/lost. Do not delete these names in a
    // replacement context where they can already designate unrelated objects.
    void Abandon() {
        programs_.clear();
        stages_.clear();
    }

private:
    struct Stage {
        GLenum target;
        Digest digest;
        std::string source;
        GLuint shader;
    };
    struct Program {
        Digest vertex, fragment;
        size_t vertex_index, fragment_index;  // Used only before ownership transfers.
        GLuint program;
    };
    struct Binary {
        Digest vertex, fragment;
        GLenum format;
        std::vector<uint8_t> bytes;
    };
    static constexpr size_t kBinaryHeaderBytes = 8 + 32 + 4;
    static constexpr size_t kBinaryRecordBytes = 32 + 32 + 4 + 4 + 32;
    static constexpr std::array<uint8_t, 8> kBinaryMagic{'B','L','2','P','B','C','0','1'};
    std::vector<Stage> stages_;
    std::vector<Program> programs_;
    size_t binary_hit_count_ = 0;  // Last successful Warm; failed input preserves its witness.

    static uint32_t Read32(const uint8_t* bytes) {
        return uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8) |
               (uint32_t{bytes[2]} << 16) | (uint32_t{bytes[3]} << 24);
    }

    static std::array<uint8_t, 4> Encode32(uint32_t value) {
        return {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
    }

    static bool BinaryIdentity(std::span<const uint8_t> source_table,
                               std::string_view directory, std::string& path,
                               Digest& identity, std::vector<GLint>& formats) {
        if (directory.empty() || directory.size() > 4096 ||
            directory.find('\0') != directory.npos) return false;
        try {
            // These are the real host strings, never the guest's virtual Tegra identity.
            const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
            if (!version || !std::string_view(version).starts_with("OpenGL ES 3.")) return false;
            GLint count = 0;
            glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
            if (count <= 0 || count > 64) return false;
            formats.resize(static_cast<size_t>(count));
            glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, formats.data());
            std::sort(formats.begin(), formats.end());
            ObbSha256 hash;
            constexpr std::string_view policy = "BL2PBC01:default-linked-VS-PS:";
            hash.Update(reinterpret_cast<const uint8_t*>(policy.data()), policy.size());
#if defined(__arm__)
            constexpr std::string_view abi = "armeabi-v7a";
#elif defined(__aarch64__)
            constexpr std::string_view abi = "arm64-v8a";
#else
            constexpr std::string_view abi = "mock-host";
#endif
            hash.Update(reinterpret_cast<const uint8_t*>(abi.data()), abi.size());
            const auto pointer_bytes = Encode32(sizeof(void*));
            hash.Update(pointer_bytes.data(), pointer_bytes.size());
            for (GLenum name : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION}) {
                const auto* value = reinterpret_cast<const char*>(glGetString(name));
                if (!value) return false;
                const size_t size = ::strnlen(value, 4097);
                if (!size || size > 4096) return false;
                const auto length = Encode32(static_cast<uint32_t>(size));
                hash.Update(length.data(), length.size());
                hash.Update(reinterpret_cast<const uint8_t*>(value), size);
            }
            for (GLint format : formats) {
                const auto encoded = Encode32(static_cast<uint32_t>(format));
                hash.Update(encoded.data(), encoded.size());
            }
            const Digest table_hash = Hash(std::string_view(
                reinterpret_cast<const char*>(source_table.data()), source_table.size()));
            hash.Update(table_hash.data(), table_hash.size());
            identity = hash.Finalize();
            path = std::string(directory) + "/bl2-program-cache.bin";
            return true;
        } catch (...) {
            return false;
        }
    }

    static void ReadBinaries(const std::string& path, const Digest& identity,
                             const std::vector<GLint>& formats,
                             const std::vector<Program>& programs,
                             std::vector<Binary>& output) {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return;
        std::vector<uint8_t> bytes;
        try {
            if (std::fseek(file, 0, SEEK_END) != 0) { std::fclose(file); return; }
            const long size = std::ftell(file);
            if (size < static_cast<long>(kBinaryHeaderBytes) ||
                static_cast<size_t>(size) > kMaxBinaryCacheBytes ||
                std::fseek(file, 0, SEEK_SET) != 0) { std::fclose(file); return; }
            bytes.resize(static_cast<size_t>(size));
            const bool read = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
            const int closed = std::fclose(file);
            file = nullptr;
            if (!read || closed != 0 ||
                !std::equal(kBinaryMagic.begin(), kBinaryMagic.end(), bytes.begin()) ||
                !std::equal(identity.begin(), identity.end(), bytes.begin() + 8)) return;
            const uint32_t count = Read32(bytes.data() + 40);
            if (!count || count > programs.size() || count > kMaxPairs) return;
            std::vector<Binary> parsed;
            size_t offset = kBinaryHeaderBytes;
            for (uint32_t index = 0; index < count; ++index) {
                if (bytes.size() - offset < kBinaryRecordBytes) return;
                Binary binary{};
                std::memcpy(binary.vertex.data(), bytes.data() + offset, 32);
                std::memcpy(binary.fragment.data(), bytes.data() + offset + 32, 32);
                binary.format = Read32(bytes.data() + offset + 64);
                const uint32_t length = Read32(bytes.data() + offset + 68);
                Digest payload_hash{};
                std::memcpy(payload_hash.data(), bytes.data() + offset + 72, 32);
                offset += kBinaryRecordBytes;
                if (!length || length > kMaxBinaryBytes || length > bytes.size() - offset ||
                    std::find(formats.begin(), formats.end(), static_cast<GLint>(binary.format)) == formats.end())
                    return;
                const bool known_pair = std::any_of(programs.begin(), programs.end(),
                    [&](const Program& entry) {
                        return entry.vertex == binary.vertex && entry.fragment == binary.fragment;
                    });
                const bool duplicate = std::any_of(parsed.begin(), parsed.end(),
                    [&](const Binary& entry) {
                        return entry.vertex == binary.vertex && entry.fragment == binary.fragment;
                    });
                const std::string_view payload(reinterpret_cast<const char*>(bytes.data() + offset), length);
                if (!known_pair || duplicate || Hash(payload) != payload_hash) return;
                binary.bytes.assign(bytes.begin() + offset, bytes.begin() + offset + length);
                parsed.push_back(std::move(binary));
                offset += length;
            }
            if (offset == bytes.size()) output.swap(parsed);
        } catch (...) {
            if (file) std::fclose(file);
        }
    }

    static bool ExportBinary(GLuint program, const std::vector<GLint>& formats, Binary& output) {
        GLint length = 0;
        glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length <= 0 || static_cast<size_t>(length) > kMaxBinaryBytes) return false;
        output.bytes.resize(static_cast<size_t>(length));
        GLsizei written = 0;
        glGetProgramBinary(program, length, &written, &output.format, output.bytes.data());
        if (written <= 0 || written > length ||
            std::find(formats.begin(), formats.end(), static_cast<GLint>(output.format)) == formats.end())
            return false;
        output.bytes.resize(static_cast<size_t>(written));
        return true;
    }

    static void WriteBinaries(const std::string& path, const Digest& identity,
                              const std::vector<Binary>& binaries) {
        std::string temporary;
        FILE* file = nullptr;
        try {
            temporary = path + ".tmp.XXXXXX";
            const int descriptor = ::mkstemp(temporary.data());
            if (descriptor < 0) return;
            file = ::fdopen(descriptor, "wb");
            if (!file) { ::close(descriptor); std::remove(temporary.c_str()); return; }
            const auto write = [&](const auto* data, size_t size) {
                return std::fwrite(data, 1, size, file) == size;
            };
            const auto count = Encode32(static_cast<uint32_t>(binaries.size()));
            bool good = write(kBinaryMagic.data(), kBinaryMagic.size()) &&
                        write(identity.data(), identity.size()) && write(count.data(), count.size());
            for (const auto& binary : binaries) {
                const auto format = Encode32(binary.format);
                const auto length = Encode32(static_cast<uint32_t>(binary.bytes.size()));
                const auto payload_hash = Hash(std::string_view(
                    reinterpret_cast<const char*>(binary.bytes.data()), binary.bytes.size()));
                good = good && write(binary.vertex.data(), binary.vertex.size()) &&
                       write(binary.fragment.data(), binary.fragment.size()) &&
                       write(format.data(), format.size()) && write(length.data(), length.size()) &&
                       write(payload_hash.data(), payload_hash.size()) &&
                       write(binary.bytes.data(), binary.bytes.size());
            }
            good = good && std::fflush(file) == 0 && ::fsync(::fileno(file)) == 0;
            const int closed = std::fclose(file);
            file = nullptr;
            if (!good || closed != 0 || std::rename(temporary.c_str(), path.c_str()) != 0)
                std::remove(temporary.c_str());
        } catch (...) {
            if (file) std::fclose(file);
            if (!temporary.empty()) std::remove(temporary.c_str());
        }
    }

    size_t AddStage(GLenum target, std::string_view source) {
        const Digest digest = Hash(source);
        for (size_t index = 0; index < stages_.size(); ++index) {
            const auto& entry = stages_[index];
            if (entry.target != target || entry.digest != digest) continue;
            if (entry.source != source) throw 0;  // Reject conflicting digest identities.
            return index;
        }
        stages_.push_back({target, digest, std::string(source), 0});
        return stages_.size() - 1;
    }
};
