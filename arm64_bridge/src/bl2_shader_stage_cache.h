#pragma once

#include "legacy_glsl_compat.h"
#include "obb_sha256.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

// Disk-only storage. Caller keeps a GL context current and serializes access.
// Every returned program is new and caller-owned, even in an unshared context.
// Private GL errors belong to the caller's existing guest-error preservation
// scope. This helper neither consumes errors nor binds a program/pipeline.
class Bl2ShaderStageCache {
public:
    using Digest = ObbSha256::Digest;
    struct Reader {
        void* opaque = nullptr;
        uint64_t bytes = 0;
        size_t (*read)(void*, uint8_t*, size_t) = nullptr;
        bool (*rewind)(void*) = nullptr;
    };
    struct Statistics {
        uint32_t stageCount = 0, compiled = 0, binaryHits = 0, failed = 0;
        uint64_t binaryBytes = 0;
        bool preparedFromMarker = false;
    };
    using Progress = void (*)(void*, uint32_t completed, uint32_t total);
    static constexpr uint32_t kMaxStages = 50000;
    static constexpr uint64_t kMaxTableBytes = 256u * 1024u * 1024u;
    static constexpr size_t kMaxStageBytes = 128u * 1024u;
    static constexpr size_t kMaxBinaryBytes = 8u * 1024u * 1024u;
    static constexpr uint64_t kMaxCacheBytes = uint64_t{1024} * 1024 * 1024;
    static constexpr size_t kHeaderBytes = 56, kRecordBytes = 40;
    static constexpr std::array<uint8_t, 8> kTableMagic{'B','L','2','S','S','O','1',0};

    const Statistics& Stats() const { return stats_; }

    // Header: magic8/version32/count32/payloadBytes64/payloadSHA256.
    // Records: GLES target32/sourceBytes32/sourceSHA256/original normalized GLSL.
    // Records are strictly ordered by (target, source digest), without duplicates.
    // First preparation validates the complete stream before creating GL objects.
    // A committed, identity-bound marker permits header-only subsequent startup.
    bool PrepareStream(const Reader& reader, std::string_view directory,
                       Progress progress = nullptr, void* progressOpaque = nullptr) {
        ready_ = false;
        stats_ = {};
        durable_exports_ = true;
        const auto fail = [&] {
            if (!stats_.failed) ++stats_.failed;
            if (progress) progress(progressOpaque, 0, 0);
            return false;
        };
        try {
            if (!reader.read || !reader.rewind || reader.bytes <= kHeaderBytes ||
                reader.bytes > kMaxTableBytes || !reader.rewind(reader.opaque) ||
                !ReadExact(reader, header_.data(), header_.size()) || !HeaderValid(reader.bytes))
                return fail();
            stats_.stageCount = Read32(header_.data() + 12);
            if (!Open(directory)) return fail();
            if (ReadMarker()) {
                stats_.preparedFromMarker = true;
                ready_ = true;
                if (progress) progress(progressOpaque, stats_.stageCount, stats_.stageCount);
                return true;
            }
            if (!ValidateStream(reader) || !MakeDirectory()) return fail();
            if (!reader.rewind(reader.opaque) || !SkipHeader(reader)) return fail();
            ready_ = true; // Valid individual files remain useful after interruption.
            if (progress) progress(progressOpaque, 0, stats_.stageCount);
            uint32_t reportedPercent = 0;
            uint64_t offset = kHeaderBytes;
            GLenum target = 0;
            Digest original{};
            std::string source, padded;
            ObbSha256 committedManifest;
            for (uint32_t index = 0; index < stats_.stageCount; ++index) {
                if (!ReadRecord(reader, offset, target, original, source, &committedManifest) ||
                    !legacy_glsl::CompleteSsoInterface(source, target == GL_VERTEX_SHADER, &padded))
                    return fail();
                bool persisted = false;
                uint64_t fileBytes = 0;
                OwnedProgram program{Load(target, original, Hash(padded), padded, persisted, fileBytes)};
                if (!program.name || !persisted || fileBytes > kMaxCacheBytes - stats_.binaryBytes)
                    return fail();
                stats_.binaryBytes += fileBytes;
                const uint32_t percent = (index + 1) * 100 / stats_.stageCount;
                if (progress && index + 1 < stats_.stageCount && percent > reportedPercent) {
                    reportedPercent = percent;
                    progress(progressOpaque, index + 1, stats_.stageCount);
                }
            }
            const Digest committed = committedManifest.Finalize();
            if (offset != reader.bytes || !std::equal(committed.begin(), committed.end(), header_.begin() + 24) ||
                !WriteMarker()) return fail();
            if (progress) progress(progressOpaque, stats_.stageCount, stats_.stageCount);
            return true;
        } catch (...) {
            return fail();
        }
    }

    // Background formation (bl2_shader_former): validates only the header,
    // binds the driver identity and directory and reads the marker. Stages are
    // then formed by worker threads while LoadProgram serves the renderer at
    // once: a present binary is imported, a missing stage is genuinely compiled
    // and persisted on the caller's thread. Complete(): nothing left to form.
    bool OpenStream(const Reader& reader, std::string_view directory) {
        ready_ = false;
        stats_ = {};
        durable_exports_ = false;
        try {
            if (!reader.read || !reader.rewind || reader.bytes <= kHeaderBytes ||
                reader.bytes > kMaxTableBytes || !reader.rewind(reader.opaque) ||
                !ReadExact(reader, header_.data(), header_.size()) || !HeaderValid(reader.bytes) ||
                !Open(directory) || !MakeDirectory()) {
                ++stats_.failed;
                return false;
            }
            stats_.stageCount = Read32(header_.data() + 12);
            stats_.preparedFromMarker = ReadMarker();
            ready_ = true;
            return true;
        } catch (...) {
            ++stats_.failed;
            return false;
        }
    }
    bool Complete() const { return ready_ && stats_.preparedFromMarker; }
    const std::array<uint8_t, kHeaderBytes>& Header() const { return header_; }

    // A missing/rejected binary is genuinely compiled and persisted if possible.
    // A successful runtime compile remains usable even if disk export fails.
    // Original source is retained by the renderer for its monolithic fallback.
    // importOnly: return zero instead of compiling a stage without a binary.
    GLuint LoadProgram(GLenum target, std::string_view originalNormalizedSource,
                       std::string_view paddedSsoSource, bool importOnly = false) {
        if (!ready_ || !TargetValid(target) || originalNormalizedSource.empty() ||
            originalNormalizedSource.size() > kMaxStageBytes || paddedSsoSource.empty() ||
            paddedSsoSource.size() > kMaxStageBytes ||
            originalNormalizedSource.find('\0') != originalNormalizedSource.npos ||
            paddedSsoSource.find('\0') != paddedSsoSource.npos ||
            eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
        try {
            std::string expected;
            if (!legacy_glsl::CompleteSsoInterface(originalNormalizedSource, target == GL_VERTEX_SHADER, &expected) ||
                expected != paddedSsoSource) return 0;
            const Digest original = Hash(originalNormalizedSource);
            const Digest padded = Hash(paddedSsoSource);
            uint64_t fileBytes = 0;
            if (importOnly) return Import(target, original, padded, fileBytes);
            bool persisted = false;
            const GLuint program = Load(target, original, padded, paddedSsoSource, persisted, fileBytes);
            if (!persisted) std::remove(MarkerPath().c_str());
            return program;
        } catch (...) {
            ++stats_.failed;
            return 0;
        }
    }

    // Learned stage prewarm (renderer thread, its context current): imports the
    // persisted binary of a stage known only by its digests. Zero when absent,
    // stale or rejected; nothing is ever compiled here.
    GLuint ImportStage(GLenum target, const Digest& original, const Digest& padded) {
        if (!ready_ || !TargetValid(target) || eglGetCurrentContext() == EGL_NO_CONTEXT) return 0;
        try {
            uint64_t fileBytes = 0;
            return Import(target, original, padded, fileBytes);
        } catch (...) {
            ++stats_.failed;
            return 0;
        }
    }

    // Formation helpers. After OpenStream they are safe on any thread with its
    // own current context of the same driver: identity, path and formats are
    // fixed then, and they never touch the renderer's statistics.
    // The current context reports the identity this cache was opened with.
    bool MatchesCurrentDriver() const {
        Digest identity{};
        std::vector<GLint> formats;
        return ready_ && ComputeIdentity(identity, formats) && identity == identity_;
    }
    bool StageFilePresent(GLenum target, const Digest& original, uint64_t* bytes = nullptr) const {
        struct stat info{};
        if (::stat(BinaryPath(target, original).c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_size <= static_cast<off_t>(kBinaryHeaderBytes)) return false;
        if (bytes) *bytes = static_cast<uint64_t>(info.st_size);
        return true;
    }
    // Compiles one canonical padded stage on the calling thread's context and
    // persists its binary without a per-file sync (CommitFormation syncs once).
    bool FormStage(GLenum target, const Digest& original, std::string_view paddedSsoSource,
                   uint64_t& fileBytes) const {
        if (!ready_ || !TargetValid(target) || paddedSsoSource.empty() ||
            paddedSsoSource.size() > kMaxStageBytes) return false;
        OwnedProgram program{CompileProgram(target, paddedSsoSource)};
        return program.name &&
               Export(program.name, target, original, Hash(paddedSsoSource), fileBytes, false);
    }
    // Every catalogue stage is present: one filesystem sync makes the binaries
    // durable before the marker that lets later launches skip formation.
    bool CommitFormation(uint64_t binaryBytes) const {
        if (!ready_ || binaryBytes > kMaxCacheBytes ||
            binaryBytes < uint64_t(Read32(header_.data() + 12)) * (kBinaryHeaderBytes + 1)) return false;
        const int directory = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory < 0) return false;
        const bool synced = ::syscall(SYS_syncfs, directory) == 0;  // libc syncfs is API 28+
        ::close(directory);
        return synced && WriteMarker(binaryBytes);
    }
    // Stage binaries already on disk (v-/f-<sha>.bin): where a resumed
    // formation starts. One directory listing, no file is opened.
    size_t CountStageFiles() const {
        if (!ready_) return 0;
        size_t count = 0;
        if (DIR* listing = ::opendir(path_.c_str())) {
            while (const dirent* entry = ::readdir(listing)) {
                const std::string_view name(entry->d_name);
                if (name.size() == 70 && (name[0] == 'v' || name[0] == 'f') && name[1] == '-' &&
                    name.ends_with(".bin")) ++count;
            }
            ::closedir(listing);
        }
        return count;
    }
    // Directories of other identities (an older driver or catalogue) are never
    // read again; returns how many were removed.
    size_t RemoveStaleIdentities() const {
        if (!ready_) return 0;
        const size_t slash = path_.find_last_of('/');
        if (slash == std::string::npos) return 0;
        const std::string parent = path_.substr(0, slash), own = path_.substr(slash + 1);
        std::vector<std::string> stale;
        if (DIR* listing = ::opendir(parent.c_str())) {
            while (const dirent* entry = ::readdir(listing)) {
                const std::string_view name(entry->d_name);
                if (name.starts_with("bl2-stage-cache-") && name != own)
                    stale.emplace_back(parent + "/" + entry->d_name);
            }
            ::closedir(listing);
        }
        size_t removed = 0;
        for (const auto& directory : stale) {
            if (DIR* listing = ::opendir(directory.c_str())) {
                while (const dirent* entry = ::readdir(listing)) {
                    if (entry->d_name[0] == '.') continue;
                    ::unlink((directory + "/" + entry->d_name).c_str());
                }
                ::closedir(listing);
            }
            if (::rmdir(directory.c_str()) == 0) ++removed;
        }
        return removed;
    }

    // Linked (non-separable) original GLSL of one vertex+fragment pair, kept
    // beside the stage binaries under the same driver identity. Only the
    // identity, path and formats fixed by preparation are read, so a worker
    // thread with its own share-group context may call these concurrently.
    bool PairStoreReady() const { return ready_ && !path_.empty(); }
    GLuint ImportPair(const Digest& vertex, const Digest& fragment) const {
        if (!PairStoreReady()) return 0;
        const auto bytes = ReadFile(PairPath(vertex, fragment), kBinaryHeaderBytes + kMaxBinaryBytes);
        if (bytes.size() < kBinaryHeaderBytes ||
            !std::equal(kPairMagic.begin(), kPairMagic.end(), bytes.begin()) ||
            !std::equal(identity_.begin(), identity_.end(), bytes.begin() + 8) ||
            !std::equal(vertex.begin(), vertex.end(), bytes.begin() + 44) ||
            !std::equal(fragment.begin(), fragment.end(), bytes.begin() + 76)) return 0;
        const GLenum format = Read32(bytes.data() + 108);
        const uint32_t length = Read32(bytes.data() + 112);
        if (!length || length > kMaxBinaryBytes || length != bytes.size() - kBinaryHeaderBytes ||
            !SupportedFormat(format)) return 0;
        const auto payload = std::span(bytes).subspan(kBinaryHeaderBytes);
        const Digest digest = Hash(payload);
        if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 116)) return 0;
        OwnedProgram program{glCreateProgram()};
        if (!program.name) return 0;
        glProgramBinary(program.name, format, payload.data(), static_cast<GLsizei>(payload.size()));
        GLint linked = GL_FALSE;
        glGetProgramiv(program.name, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) return 0;
        const GLuint result = program.name; program.name = 0; return result;
    }
    // Program must have been linked with GL_PROGRAM_BINARY_RETRIEVABLE_HINT.
    bool ExportPair(GLuint program, const Digest& vertex, const Digest& fragment,
                    uint64_t& fileBytes) const {
        if (!PairStoreReady()) return false;
        GLint length = 0; glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length <= 0 || static_cast<size_t>(length) > kMaxBinaryBytes) return false;
        std::vector<uint8_t> bytes(kBinaryHeaderBytes + static_cast<size_t>(length));
        GLsizei written = 0; GLenum format = 0;
        glGetProgramBinary(program, length, &written, &format, bytes.data() + kBinaryHeaderBytes);
        if (written <= 0 || written > length || !SupportedFormat(format)) return false;
        bytes.resize(kBinaryHeaderBytes + static_cast<size_t>(written));
        std::copy(kPairMagic.begin(), kPairMagic.end(), bytes.begin());
        std::copy(identity_.begin(), identity_.end(), bytes.begin() + 8);
        Put32(bytes.data() + 40, 0);
        std::copy(vertex.begin(), vertex.end(), bytes.begin() + 44);
        std::copy(fragment.begin(), fragment.end(), bytes.begin() + 76);
        Put32(bytes.data() + 108, format); Put32(bytes.data() + 112, static_cast<uint32_t>(written));
        const Digest digest = Hash(std::span(bytes).subspan(kBinaryHeaderBytes));
        std::copy(digest.begin(), digest.end(), bytes.begin() + 116);
        if (!AtomicWrite(PairPath(vertex, fragment), bytes)) return false;
        fileBytes = bytes.size(); return true;
    }

private:
    static constexpr std::array<uint8_t, 8> kPairMagic{'B','L','2','P','B','I','N','1'};
    std::string PairPath(const Digest& vertex, const Digest& fragment) const {
        return path_ + "/p-" + ObbSha256::Hex(vertex).data() + "-" + ObbSha256::Hex(fragment).data() + ".bin";
    }
    struct OwnedProgram {
        GLuint name = 0;
        ~OwnedProgram() { if (name) glDeleteProgram(name); }
    };
    struct OwnedShader {
        GLuint name = 0;
        ~OwnedShader() { if (name) glDeleteShader(name); }
    };
    using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
    static constexpr size_t kBinaryHeaderBytes = 148, kMarkerBytes = 116;
    static constexpr std::array<uint8_t, 8> kBinaryMagic{'B','L','2','S','B','I','N','1'};
    static constexpr std::array<uint8_t, 8> kMarkerMagic{'B','L','2','D','O','N','E','1'};
    std::array<uint8_t, kHeaderBytes> header_{};
    Digest identity_{};
    std::vector<GLint> formats_;
    std::string path_;
    Statistics stats_{};
    bool ready_ = false;
    bool durable_exports_ = true;  // False once opened for background formation.

    static bool TargetValid(GLenum target) { return target == GL_VERTEX_SHADER || target == GL_FRAGMENT_SHADER; }
    static uint32_t Read32(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
    static uint64_t Read64(const uint8_t* p) { return Read32(p) | uint64_t(Read32(p + 4)) << 32; }
    static void Put32(uint8_t* p, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8 * i));
    }
    static void Put64(uint8_t* p, uint64_t value) {
        Put32(p, static_cast<uint32_t>(value)); Put32(p + 4, static_cast<uint32_t>(value >> 32));
    }
    static Digest Hash(std::string_view value) {
        ObbSha256 h;
        h.Update(reinterpret_cast<const uint8_t*>(value.data()), value.size());
        return h.Finalize();
    }
    static Digest Hash(std::span<const uint8_t> value) {
        ObbSha256 h; h.Update(value.data(), value.size()); return h.Finalize();
    }
    static bool ReadExact(const Reader& reader, uint8_t* output, size_t bytes) {
        while (bytes) {
            const size_t count = reader.read(reader.opaque, output, bytes);
            if (!count || count > bytes) return false;
            output += count; bytes -= count;
        }
        return true;
    }
    bool HeaderValid(uint64_t bytes) const {
        const uint32_t count = Read32(header_.data() + 12);
        return std::equal(kTableMagic.begin(), kTableMagic.end(), header_.begin()) &&
               Read32(header_.data() + 8) == 1 && count && count <= kMaxStages &&
               Read64(header_.data() + 16) == bytes - kHeaderBytes &&
               uint64_t(count) * (kRecordBytes + 1) <= bytes - kHeaderBytes;
    }
    bool SkipHeader(const Reader& reader) const {
        std::array<uint8_t, kHeaderBytes> header{};
        return ReadExact(reader, header.data(), header.size()) && header == header_;
    }
    bool ReadRecord(const Reader& reader, uint64_t& offset, GLenum& target, Digest& digest,
                    std::string& source, ObbSha256* manifest) const {
        std::array<uint8_t, kRecordBytes> record{};
        if (offset > reader.bytes || reader.bytes - offset < record.size() ||
            !ReadExact(reader, record.data(), record.size())) return false;
        offset += record.size(); target = Read32(record.data());
        const uint32_t length = Read32(record.data() + 4);
        if (!TargetValid(target) || !length || length > kMaxStageBytes || length > reader.bytes - offset)
            return false;
        std::copy_n(record.data() + 8, digest.size(), digest.data());
        source.resize(length);
        if (!ReadExact(reader, reinterpret_cast<uint8_t*>(source.data()), source.size()) ||
            source.find('\0') != source.npos || Hash(source) != digest) return false;
        offset += length;
        if (manifest) {
            manifest->Update(record.data(), record.size());
            manifest->Update(reinterpret_cast<const uint8_t*>(source.data()), source.size());
        }
        return true;
    }
    bool ValidateStream(const Reader& reader) const {
        uint64_t offset = kHeaderBytes;
        GLenum target = 0, previousTarget = 0;
        Digest digest{}, previousDigest{};
        std::string source, padded;
        ObbSha256 manifest;
        for (uint32_t i = 0; i < Read32(header_.data() + 12); ++i) {
            if (!ReadRecord(reader, offset, target, digest, source, &manifest) ||
                (i && (target < previousTarget || (target == previousTarget && digest <= previousDigest))) ||
                !legacy_glsl::CompleteSsoInterface(source, target == GL_VERTEX_SHADER, &padded) ||
                padded.size() > kMaxStageBytes) return false;
            previousTarget = target; previousDigest = digest;
        }
        const Digest payloadDigest = manifest.Finalize();
        return offset == reader.bytes && std::equal(payloadDigest.begin(), payloadDigest.end(), header_.begin() + 24);
    }
    bool Open(std::string_view directory) {
        if (directory.empty() || directory.size() > 3500 || directory.find('\0') != directory.npos ||
            !ComputeIdentity(identity_, formats_)) return false;
        path_ = std::string(directory) + "/bl2-stage-cache-" + ObbSha256::Hex(identity_).data();
        return true;
    }
    // Driver identity of the current context for this catalogue header.
    bool ComputeIdentity(Digest& identity, std::vector<GLint>& formats) const {
        if (eglGetCurrentContext() == EGL_NO_CONTEXT) return false;
        GLint major = 0, minor = 0, count = 0, vertex = 0, fragment = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major); glGetIntegerv(GL_MINOR_VERSION, &minor);
        glGetIntegerv(GL_MAX_VERTEX_OUTPUT_COMPONENTS, &vertex);
        glGetIntegerv(GL_MAX_FRAGMENT_INPUT_COMPONENTS, &fragment);
        glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
        if (major < 3 || (major == 3 && minor < 2) || vertex < 56 || fragment < 56 || count <= 0 || count > 64)
            return false;
        formats.resize(static_cast<size_t>(count));
        glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, formats.data());
        std::sort(formats.begin(), formats.end());
        ObbSha256 h;
        constexpr std::string_view policy = "BL2SSO1:Normalize320ProjectedSwizzles:UnTexcoord0-7-12-13:highp-smooth-vec4:UBO0:MultiDraw0";
        h.Update(reinterpret_cast<const uint8_t*>(policy.data()), policy.size());
#if defined(__arm__)
        constexpr std::string_view abi = "armeabi-v7a";
#elif defined(__aarch64__)
        constexpr std::string_view abi = "arm64-v8a";
#else
        constexpr std::string_view abi = "mock-host";
#endif
        h.Update(reinterpret_cast<const uint8_t*>(abi.data()), abi.size());
        std::array<uint8_t, 4> word{}; Put32(word.data(), sizeof(void*)); h.Update(word.data(), word.size());
        for (GLenum name : {GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION}) {
            const auto* value = reinterpret_cast<const char*>(glGetString(name));
            if (!value) return false;
            const size_t size = ::strnlen(value, 4097);
            if (!size || size > 4096) return false;
            Put32(word.data(), static_cast<uint32_t>(size)); h.Update(word.data(), word.size());
            h.Update(reinterpret_cast<const uint8_t*>(value), size);
        }
        for (GLint format : formats) {
            Put32(word.data(), static_cast<uint32_t>(format)); h.Update(word.data(), word.size());
        }
        h.Update(header_.data(), header_.size()); identity = h.Finalize();
        return true;
    }
    bool MakeDirectory() const {
        if (::mkdir(path_.c_str(), 0700) != 0 && errno != EEXIST) return false;
        struct stat info{};
        return ::stat(path_.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    }
    std::string MarkerPath() const { return path_ + "/complete.bin"; }
    std::string BinaryPath(GLenum target, const Digest& source) const {
        return path_ + (target == GL_VERTEX_SHADER ? "/v-" : "/f-") + ObbSha256::Hex(source).data() + ".bin";
    }
    static std::vector<uint8_t> ReadFile(const std::string& path, size_t maximum) {
        File file(std::fopen(path.c_str(), "rb"), &std::fclose);
        if (!file || std::fseek(file.get(), 0, SEEK_END) != 0) return {};
        const long size = std::ftell(file.get());
        if (size <= 0 || static_cast<size_t>(size) > maximum || std::fseek(file.get(), 0, SEEK_SET) != 0) return {};
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (std::fread(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) return {};
        return bytes;
    }
    bool ReadMarker() {
        const auto bytes = ReadFile(MarkerPath(), kMarkerBytes);
        if (bytes.size() != kMarkerBytes ||
            !std::equal(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin()) ||
            !std::equal(identity_.begin(), identity_.end(), bytes.begin() + 8) ||
            !std::equal(header_.begin() + 24, header_.end(), bytes.begin() + 40) ||
            Read32(bytes.data() + 72) != stats_.stageCount || Read64(bytes.data() + 76) > kMaxCacheBytes ||
            Read64(bytes.data() + 76) < uint64_t(stats_.stageCount) * (kBinaryHeaderBytes + 1))
            return false;
        const Digest digest = Hash(std::span(bytes).first(84));
        if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 84)) return false;
        stats_.binaryBytes = Read64(bytes.data() + 76);
        return true;
    }
    // durable=false skips both fsyncs: a stage binary torn by a power loss fails
    // its SHA-256 check on import and is compiled again, and the marker is only
    // written after CommitFormation's filesystem sync.
    static bool AtomicWrite(const std::string& path, std::span<const uint8_t> bytes, bool durable = true) {
        std::string temporary = path + ".tmp.XXXXXX";
        const int fd = ::mkstemp(temporary.data());
        if (fd < 0) return false;
        FILE* file = ::fdopen(fd, "wb");
        if (!file) { ::close(fd); std::remove(temporary.c_str()); return false; }
        bool good = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
        good = good && std::fflush(file) == 0 && (!durable || ::fsync(::fileno(file)) == 0);
        const int closed = std::fclose(file);
        good = good && closed == 0 && std::rename(temporary.c_str(), path.c_str()) == 0;
        if (!good) std::remove(temporary.c_str());
        if (good && durable) {
            const std::string parent = path.substr(0, path.find_last_of('/'));
            const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (directory < 0) return false;
            good = ::fsync(directory) == 0;
            if (::close(directory) != 0) good = false;
        }
        return good;
    }
    bool WriteMarker() const { return WriteMarker(stats_.binaryBytes); }
    bool WriteMarker(uint64_t binaryBytes) const {
        std::array<uint8_t, kMarkerBytes> bytes{};
        std::copy(kMarkerMagic.begin(), kMarkerMagic.end(), bytes.begin());
        std::copy(identity_.begin(), identity_.end(), bytes.begin() + 8);
        std::copy(header_.begin() + 24, header_.end(), bytes.begin() + 40);
        Put32(bytes.data() + 72, Read32(header_.data() + 12)); Put64(bytes.data() + 76, binaryBytes);
        const Digest digest = Hash(std::span(bytes).first(84));
        std::copy(digest.begin(), digest.end(), bytes.begin() + 84);
        const bool good = AtomicWrite(MarkerPath(), bytes);
        if (!good) std::remove(MarkerPath().c_str());
        return good;
    }
    bool SupportedFormat(GLenum format) const {
        return std::find(formats_.begin(), formats_.end(), static_cast<GLint>(format)) != formats_.end();
    }
    GLuint Import(GLenum target, const Digest& original, const Digest& padded, uint64_t& fileBytes) {
        const auto bytes = ReadFile(BinaryPath(target, original), kBinaryHeaderBytes + kMaxBinaryBytes);
        if (bytes.size() < kBinaryHeaderBytes ||
            !std::equal(kBinaryMagic.begin(), kBinaryMagic.end(), bytes.begin()) ||
            !std::equal(identity_.begin(), identity_.end(), bytes.begin() + 8) ||
            Read32(bytes.data() + 40) != target ||
            !std::equal(original.begin(), original.end(), bytes.begin() + 44) ||
            !std::equal(padded.begin(), padded.end(), bytes.begin() + 76)) return 0;
        const GLenum format = Read32(bytes.data() + 108);
        const uint32_t length = Read32(bytes.data() + 112);
        if (!length || length > kMaxBinaryBytes || length != bytes.size() - kBinaryHeaderBytes || !SupportedFormat(format))
            return 0;
        const auto payload = std::span(bytes).subspan(kBinaryHeaderBytes);
        const Digest digest = Hash(payload);
        if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 116)) return 0;
        OwnedProgram program{glCreateProgram()};
        if (!program.name) return 0;
        // Some older Adreno drivers omit separable state during binary import.
        glProgramParameteri(program.name, GL_PROGRAM_SEPARABLE, GL_TRUE);
        glProgramBinary(program.name, format, payload.data(), static_cast<GLsizei>(payload.size()));
        GLint linked = GL_FALSE, separable = GL_FALSE;
        glGetProgramiv(program.name, GL_LINK_STATUS, &linked);
        glGetProgramiv(program.name, GL_PROGRAM_SEPARABLE, &separable);
        if (linked != GL_TRUE || separable != GL_TRUE) return 0;
        ++stats_.binaryHits; fileBytes = bytes.size();
        const GLuint result = program.name; program.name = 0; return result;
    }
    GLuint Compile(GLenum target, std::string_view source) {
        ++stats_.compiled;
        return CompileProgram(target, source);
    }
    static GLuint CompileProgram(GLenum target, std::string_view source) {
        OwnedShader shader{glCreateShader(target)};
        if (!shader.name) return 0;
        const GLchar* text = source.data(); const GLint length = static_cast<GLint>(source.size());
        glShaderSource(shader.name, 1, &text, &length);
        glCompileShader(shader.name);
        GLint compiled = GL_FALSE; glGetShaderiv(shader.name, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) return 0;
        OwnedProgram program{glCreateProgram()};
        if (!program.name) return 0;
        glProgramParameteri(program.name, GL_PROGRAM_SEPARABLE, GL_TRUE);
        glProgramParameteri(program.name, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
        glAttachShader(program.name, shader.name); glLinkProgram(program.name);
        GLint linked = GL_FALSE, separable = GL_FALSE;
        glGetProgramiv(program.name, GL_LINK_STATUS, &linked);
        glGetProgramiv(program.name, GL_PROGRAM_SEPARABLE, &separable);
        if (linked != GL_TRUE || separable != GL_TRUE) return 0;
        const GLuint result = program.name; program.name = 0; return result;
    }
    bool Export(GLuint program, GLenum target, const Digest& original, const Digest& padded,
                uint64_t& fileBytes, bool durable = true) const {
        GLint length = 0; glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
        if (length <= 0 || static_cast<size_t>(length) > kMaxBinaryBytes) return false;
        std::vector<uint8_t> bytes(kBinaryHeaderBytes + static_cast<size_t>(length));
        GLsizei written = 0; GLenum format = 0;
        glGetProgramBinary(program, length, &written, &format, bytes.data() + kBinaryHeaderBytes);
        if (written <= 0 || written > length || !SupportedFormat(format)) return false;
        bytes.resize(kBinaryHeaderBytes + static_cast<size_t>(written));
        std::copy(kBinaryMagic.begin(), kBinaryMagic.end(), bytes.begin());
        std::copy(identity_.begin(), identity_.end(), bytes.begin() + 8);
        Put32(bytes.data() + 40, target);
        std::copy(original.begin(), original.end(), bytes.begin() + 44);
        std::copy(padded.begin(), padded.end(), bytes.begin() + 76);
        Put32(bytes.data() + 108, format); Put32(bytes.data() + 112, static_cast<uint32_t>(written));
        const Digest digest = Hash(std::span(bytes).subspan(kBinaryHeaderBytes));
        std::copy(digest.begin(), digest.end(), bytes.begin() + 116);
        if (!AtomicWrite(BinaryPath(target, original), bytes, durable)) return false;
        fileBytes = bytes.size(); return true;
    }
    GLuint Load(GLenum target, const Digest& original, const Digest& padded, std::string_view source,
                bool& persisted, uint64_t& fileBytes) {
        if (const GLuint program = Import(target, original, padded, fileBytes)) {
            persisted = true; return program;
        }
        OwnedProgram program{Compile(target, source)};
        if (!program.name) { ++stats_.failed; return 0; }
        // A blocking preparation keeps durable files; a renderer-side miss must
        // not wait for two fsyncs inside a frame.
        persisted = Export(program.name, target, original, padded, fileBytes, durable_exports_);
        if (!persisted) ++stats_.failed;
        const GLuint result = program.name; program.name = 0; return result;
    }
};
