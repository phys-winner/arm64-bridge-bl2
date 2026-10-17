#include "bl2_bink_audio.h"
#if defined(BL2_LOCALE_HOST_TEST)
#include "bl2_locale_host_port.h"
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string_view>

namespace {
uint32_t U32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
bool Bounds(uint64_t total, uint64_t offset, uint64_t size) {
    return offset <= total && size <= total - offset;
}
bool ReadExact(int fd, uint64_t offset, void* output, size_t size) {
    auto* data = static_cast<uint8_t*>(output);
    while (size) {
        const auto count = pread(fd, data, size, static_cast<int64_t>(offset));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        size -= static_cast<size_t>(count);
        data += count;
        offset += static_cast<size_t>(count);
    }
    return true;
}
bool FindMovie(int fd, uint64_t base, uint64_t size, std::string_view movie,
               uint64_t& offset, uint64_t& bytes) {
    uint8_t prefix[8];
    if (fd < 0 || size < 8 || !ReadExact(fd, base, prefix, sizeof(prefix))) return false;
    const uint32_t payload = U32(prefix), names_bytes = U32(prefix + 4);
    if (payload > 16 * 1024 * 1024 || !Bounds(size, 0, payload) ||
        !Bounds(payload, 8, uint64_t(names_bytes) + 4)) return false;
    std::vector<uint8_t> index(payload);
    if (!ReadExact(fd, base, index.data(), index.size())) return false;
    const uint64_t metadata = uint64_t{8} + names_bytes;
    const uint32_t metadata_bytes = U32(index.data() + metadata);
    if (metadata_bytes % 60 || !Bounds(payload, metadata + 4, metadata_bytes)) return false;
    size_t name_at = 8, count = 0;
    bool found = false;
    while (name_at < metadata && index[name_at] != 0xCD) {
        const char* name = reinterpret_cast<const char*>(index.data() + name_at);
        const auto* end = static_cast<const char*>(std::memchr(name, 0, metadata - name_at));
        if (!end || end == name || count >= metadata_bytes / 60) return false;
        const uint8_t* record = index.data() + metadata + 4 + count * 60;
        if (std::string_view(name, end - name) == movie && U32(record) == 0) {
            if (found) return false;
            offset = U32(record + 8); bytes = U32(record + 4);
            if (offset < payload || !Bounds(size, offset, bytes)) return false;
            found = true;
        }
        name_at += static_cast<size_t>(end - name) + 1;
        ++count;
    }
    return found && count == metadata_bytes / 60;
}
}  // namespace

bool Bl2BinkAudio::Load(int audio_fd, uint64_t audio_base, uint64_t audio_bytes,
                       int main_fd, uint64_t main_base, uint64_t main_bytes) {
    bytes_ = 0; spans_.clear();
    std::array<uint8_t, 80> header{};
    if (audio_fd < 0 || audio_bytes < header.size() ||
        !ReadExact(audio_fd, audio_base, header.data(), header.size()) ||
        std::memcmp(header.data(), "BL2BAUD1", 8) != 0) return false;
    const uint32_t logical_bytes = U32(header.data() + 8), count = U32(header.data() + 12);
    const char* name = reinterpret_cast<const char*>(header.data() + 16);
    const auto* end = static_cast<const char*>(std::memchr(name, 0, 64));
    if (!end || end == name || count == 0 || count > 1000000 || logical_bytes == 0 ||
        !Bounds(audio_bytes, header.size(), uint64_t(count) * 16)) return false;
    const std::string_view movie(name, end - name);
    if (!movie.ends_with(".bik") || movie.find_first_of("/\\") != std::string_view::npos) return false;
    uint64_t movie_offset = 0, movie_bytes = 0;
    if (!FindMovie(main_fd, main_base, main_bytes, movie, movie_offset, movie_bytes)) return false;
    std::vector<uint8_t> table(size_t(count) * 16);
    if (!ReadExact(audio_fd, audio_base + header.size(), table.data(), table.size())) return false;
    const uint64_t payload = header.size() + table.size();
    uint64_t next = 0;
    spans_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = table.data() + size_t(i) * 16;
        Span span{U32(p), U32(p + 4), U32(p + 8), U32(p + 12)};
        if (span.begin != next || span.bytes == 0 || span.video > 1 ||
            !Bounds(logical_bytes, span.begin, span.bytes) ||
            (span.video ? !Bounds(movie_bytes, span.offset, span.bytes)
                        : span.offset < payload || !Bounds(audio_bytes, span.offset, span.bytes))) {
            spans_.clear(); return false;
        }
        next += span.bytes;
        spans_.push_back(span);
    }
    if (next != logical_bytes || spans_.front().video) { spans_.clear(); return false; }
    bytes_ = logical_bytes; audio_fd_ = audio_fd; main_fd_ = main_fd;
    audio_base_ = audio_base; video_base_ = main_base + movie_offset;
    return true;
}

int64_t Bl2BinkAudio::Read(void* output, size_t bytes, int64_t offset) const {
    if (offset < 0 || (!output && bytes)) { errno = EINVAL; return -1; }
    if (uint64_t(offset) >= bytes_ || !bytes) return 0;
    bytes = static_cast<size_t>(std::min<uint64_t>(bytes, bytes_ - uint64_t(offset)));
    auto span = std::upper_bound(spans_.begin(), spans_.end(), uint64_t(offset),
        [](uint64_t at, const Span& part) { return at < part.begin; });
    if (span == spans_.begin()) { errno = EIO; return -1; }
    --span;
    size_t done = 0;
    while (done < bytes) {
        const uint64_t within = uint64_t(offset) + done - span->begin;
        const size_t size = std::min<uint64_t>(bytes - done, span->bytes - within);
        const int fd = span->video ? main_fd_ : audio_fd_;
        const uint64_t base = span->video ? video_base_ : audio_base_;
        if (!ReadExact(fd, base + span->offset + within, static_cast<uint8_t*>(output) + done, size)) {
            errno = EIO; return done ? static_cast<int64_t>(done) : -1;
        }
        done += size;
        ++span;
    }
    return static_cast<int64_t>(done);
}
