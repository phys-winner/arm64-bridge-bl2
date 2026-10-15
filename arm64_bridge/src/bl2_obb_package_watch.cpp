#include "bl2_obb_package_watch.h"

#include "bl2_shader_former.h"

#include <android/log.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Span {
    uint64_t begin = 0, end = 0;
    std::string name;
};
struct Index {
    bool parsed = false, valid = false;
    std::vector<Span> spans;  // Cooked packages by payload offset.
};
struct Reader {
    int obb = 0;
    int read_fd = -1;  // Where the index can be read (the guest's own fd or its APK entry).
    off_t base = 0;
    size_t last = std::numeric_limits<size_t>::max();  // Span of the previous read.
};

std::mutex g_mutex;
std::array<Index, 2> g_index;
std::unordered_map<int, Reader> g_readers;

uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool ReadAt(int fd, off_t offset, uint8_t* output, size_t bytes) {
    while (bytes) {
        const ssize_t count = pread(fd, output, bytes, offset);
        if (count <= 0) return false;
        output += count;
        bytes -= static_cast<size_t>(count);
        offset += count;
    }
    return true;
}

// The OBB begins with its own index: the first payload offset, the name
// section length, NUL-separated names padded with 0xCD, then one 60-byte entry
// per name (type, size, offset; type 0 is a file).
bool Parse(int fd, off_t base, std::vector<Span>& spans) {
    uint8_t prefix[8];
    if (!ReadAt(fd, base, prefix, sizeof(prefix))) return false;
    const uint32_t payload = Read32(prefix), names_bytes = Read32(prefix + 4);
    if (payload < 12 || payload > 16u * 1024 * 1024 || uint64_t{8} + names_bytes + 4 > payload) return false;
    std::vector<uint8_t> index(payload);
    if (!ReadAt(fd, base, index.data(), index.size())) return false;
    size_t names_end = 8 + names_bytes;
    while (names_end > 8 && index[names_end - 1] == 0xCD) --names_end;
    std::vector<std::string> names;
    for (size_t at = 8; at < names_end;) {
        const char* name = reinterpret_cast<const char*>(index.data() + at);
        const size_t length = strnlen(name, names_end - at);
        if (length) names.emplace_back(name, length);
        at += length + 1;
    }
    const size_t metadata = 8 + names_bytes;
    const uint32_t metadata_bytes = Read32(index.data() + metadata);
    if (metadata_bytes % 60 || metadata + 4 + metadata_bytes > payload || names.size() != metadata_bytes / 60)
        return false;
    for (size_t i = 0; i < names.size(); ++i) {
        const uint8_t* entry = index.data() + metadata + 4 + 60 * i;
        const uint32_t type = Read32(entry), size = Read32(entry + 4), offset = Read32(entry + 8);
        if (type == 0 && size && names[i].ends_with(".xxx"))
            spans.push_back({offset, uint64_t{offset} + size, names[i]});
    }
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) { return a.begin < b.begin; });
    return !spans.empty();
}

}  // namespace

// The game opens both OBBs once, early, before any formation starts: every
// open is recorded and the index is parsed at the first read that matters.
void Bl2ObbPackageWatchOpen(int guest_fd, int obb, int read_fd, off_t base_offset) {
    if (obb < 0 || obb > 1 || read_fd < 0) return;
    std::lock_guard lock(g_mutex);
    g_readers[guest_fd] = Reader{obb, read_fd, base_offset};
}

void Bl2ObbPackageWatchRead(int guest_fd, uint64_t offset) {
    if (!Bl2ShaderFormerActive()) return;
    std::string name;
    {
        std::lock_guard lock(g_mutex);
        const auto reader = g_readers.find(guest_fd);
        if (reader == g_readers.end()) return;
        Index& index = g_index[reader->second.obb];
        if (!index.parsed) {
            index.parsed = true;
            try {
                index.valid = Parse(reader->second.read_fd, reader->second.base, index.spans);
            } catch (...) {
                index.valid = false;
            }
            __android_log_print(ANDROID_LOG_INFO, "BL2ShaderCache", "obb package index obb=%d valid=%d packages=%zu",
                                reader->second.obb, index.valid, index.spans.size());
        }
        if (!index.valid) return;
        const auto& spans = index.spans;
        auto span = std::upper_bound(spans.begin(), spans.end(), offset,
                                     [](uint64_t value, const Span& entry) { return value < entry.begin; });
        if (span == spans.begin()) return;
        --span;
        const size_t position = static_cast<size_t>(span - spans.begin());
        if (offset >= span->end || position == reader->second.last) return;
        reader->second.last = position;
        name = span->name;
    }
    Bl2ShaderFormerHintPackage(name.c_str());
}

void Bl2ObbPackageWatchClose(int guest_fd) {
    std::lock_guard lock(g_mutex);
    g_readers.erase(guest_fd);
}

bool Bl2ObbPackageWatched(int guest_fd) {
    if (!Bl2ShaderFormerActive()) return false;  // No lseek per read otherwise.
    std::lock_guard lock(g_mutex);
    return g_readers.contains(guest_fd);
}
