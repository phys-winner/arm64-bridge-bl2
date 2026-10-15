#include "bl2_shader_former.h"

#include "legacy_glsl_compat.h"

#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr const char* kTag = "BL2ShaderCache";
constexpr const char* kFirstAsset = "bl2_shader_first.bin";
constexpr const char* kOrderAsset = "bl2_shader_order.bin";
constexpr const char* kCatalogueAsset = "bl2_shader_stages.bin";
constexpr std::array<uint8_t, 8> kOrderMagic{'B','L','2','S','O','R','D','1'};
// Keeps the producer a few stages ahead of the workers, bounded in memory.
constexpr size_t kQueueLimit = 48;
constexpr unsigned kMaxThreads = 6;
constexpr uint32_t kProgressStep = 4096;
// After the renderer compiled a stage itself, the game is creating stages:
// catalogue work waits this long so the two never contend for the compiler.
constexpr uint64_t kDemandQuietNs = 3'000'000'000ull;
// Only stages held by at most this many level packages say where the player is.
constexpr uint32_t kLocalPackages = 4;
// A stage in n packages gives each 12/n votes; a level pass needs two
// unique stages' worth of evidence.
constexpr uint32_t kVoteUnit = 12, kMinVotes = 2 * kVoteUnit;

using Digest = ObbSha256::Digest;
using Key = std::pair<GLenum, Digest>;
struct KeyHash {
    size_t operator()(const Key& key) const {
        size_t value = 0;
        std::memcpy(&value, key.second.data(), sizeof(value));
        return value ^ key.first;
    }
};

struct Job {
    GLenum target = 0;
    Digest digest{};
    std::string padded;
    bool first_use = false;  // Shipped first-use list rather than the catalogue.
};

// bl2_shader_order.bin: a default pass per catalogue index (main menu map,
// then by how many packages carry the stage) and, for packages with
// level-specific stages, their indices.
struct Order {
    std::vector<uint8_t> pass;
    uint8_t last_pass = 0;
    std::vector<std::string> names;
    std::unordered_map<std::string, uint16_t> by_name;
    std::vector<std::vector<uint16_t>> members;
    std::vector<uint32_t> first_package;  // Index -> range in packages_of (CSR).
    std::vector<uint16_t> packages_of;
};

struct Former {
    const Bl2ShaderStageCache* cache = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    Bl2ShaderFormerOpen open = nullptr;
    unsigned threads = 0;
    std::mutex mutex;
    std::condition_variable work;  // Workers: a job, a resume or the end of production.
    std::condition_variable room;  // Producer: queue space, a resume or no workers left.
    std::deque<Job> queue;
    bool producing = true;
    bool paused = false;
    unsigned workers_failed = 0;
    std::atomic<bool> incomplete{false};  // A stage without a binary: no marker this run.
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint32_t> formed{0}, present{0}, rejected{0}, processed{0};
    uint64_t started_ns = 0;
    std::atomic<bool> serialized{true};  // See ParallelCompiler.
    // Main-menu progress: binaries on disk when formation started, then the
    // ones the workers formed or the renderer compiled on demand since.
    uint32_t total = 0;
    std::atomic<uint32_t> initial_files{0}, demand_compiles{0};
    std::atomic<bool> counted{false};
    // Renderer misses, mapped by the producer to the packages that hold them,
    // and packages the game started reading (bl2_obb_package_watch).
    std::mutex demand_mutex;
    std::vector<Key> demand;
    std::vector<std::string> hints;
    std::atomic<bool> demand_pending{false};
};

std::mutex g_mutex;
Former* g_former = nullptr;  // Lives for the rest of the process once started.
std::atomic<bool> g_active{false};
std::atomic<uint64_t> g_last_demand_ns{0};
std::atomic<Bl2ShaderFormerGameplayProbe> g_gameplay_probe{nullptr};

uint64_t ClockNs(clockid_t clock) {
    timespec now{};
    clock_gettime(clock, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(now.tv_nsec);
}
uint64_t NowNs() { return ClockNs(CLOCK_MONOTONIC); }

// While the renderer could need the compiler, only one worker may compile on
// a driver whose compiles share one lock: a renderer miss then waits for the
// background compiles (Mali-G76 r26p0: 1.7x longer beside three workers,
// unchanged beside one). Its waiters spin, so thread CPU time does not show
// it; Mali drivers from r40 on compiled in parallel here (Mali-G715 r54p3:
// unaffected by three). Any other driver is treated as serialized.
bool ParallelCompiler() {
    const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (!renderer || !version || !std::strstr(renderer, "Mali")) return false;
    const char* release = std::strstr(version, "v1.r");
    return release && std::atoi(release + 4) >= 40;
}

uint32_t Read32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t Read64(const uint8_t* p) { return Read32(p) | uint64_t(Read32(p + 4)) << 32; }

void Background(const char* name) {
    pthread_setname_np(pthread_self(), name);
    // Never compete with the game, render or audio threads.
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
}

// During play a catalogue stage is not needed this instant: the worker runs
// only on the lowest-capacity cluster at the lowest priority, so it takes no
// time from the game's threads on the big cores (and adds little heat).
void LowerToLittleCores() {
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
    unsigned minimum = ~0u, maximum = 0;
    unsigned capacity[CPU_SETSIZE] = {};
    const long cpus = std::min<long>(sysconf(_SC_NPROCESSORS_CONF), CPU_SETSIZE);
    for (long cpu = 0; cpu < cpus; ++cpu) {
        char path[96];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", cpu);
        FILE* file = std::fopen(path, "r");
        if (!file) return;
        const bool read = std::fscanf(file, "%u", &capacity[cpu]) == 1;
        std::fclose(file);
        if (!read || !capacity[cpu]) return;
        minimum = std::min(minimum, capacity[cpu]);
        maximum = std::max(maximum, capacity[cpu]);
    }
    if (!maximum || minimum * 10 > maximum * 6) return;  // No distinct little cluster.
    cpu_set_t little;
    CPU_ZERO(&little);
    for (long cpu = 0; cpu < cpus; ++cpu)
        if (capacity[cpu] == minimum) CPU_SET(cpu, &little);
    sched_setaffinity(0, sizeof(little), &little);
}

// Menus, loading screens and dialogs leave the CPU idle: every core again.
void RaiseToAllCores() {
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 10);
    cpu_set_t all;
    CPU_ZERO(&all);
    const long cpus = std::min<long>(sysconf(_SC_NPROCESSORS_CONF), CPU_SETSIZE);
    for (long cpu = 0; cpu < cpus; ++cpu) CPU_SET(cpu, &all);
    sched_setaffinity(0, sizeof(all), &all);
}

bool InGameplay() {
    const auto probe = g_gameplay_probe.load(std::memory_order_acquire);
    return probe && probe();
}

void Progress(Former& f) {
    const uint32_t done = f.processed.fetch_add(1, std::memory_order_relaxed) + 1;
    if (done % kProgressStep == 0) {
        __android_log_print(ANDROID_LOG_INFO, kTag,
            "former progress formed=%u present=%u rejected=%u elapsed=%.1fs",
            f.formed.load(), f.present.load(), f.rejected.load(), (NowNs() - f.started_ns) / 1e9);
    }
}

struct AssetStream {
    Bl2ShaderFormerAsset asset{};
    bool opened = false;
    uint64_t offset = 0;
    ~AssetStream() { if (opened && asset.close) asset.close(asset.opaque); }
    bool Open(Former& f, const char* name) {
        opened = f.open(name, &asset);
        return opened && asset.read;
    }
    bool Read(uint8_t* output, size_t bytes) {
        while (bytes) {
            const size_t count = asset.read(asset.opaque, output, bytes);
            if (!count || count > bytes) return false;
            output += count;
            bytes -= count;
            offset += count;
        }
        return true;
    }
};

// Header of a stage-record asset (the catalogue or the first-use list).
bool ReadStageHeader(AssetStream& stream, const std::array<uint8_t, 8>& magic,
                     std::array<uint8_t, Bl2ShaderStageCache::kHeaderBytes>& header, uint32_t& count) {
    if (stream.asset.bytes <= header.size() || stream.asset.bytes > Bl2ShaderStageCache::kMaxTableBytes ||
        !stream.Read(header.data(), header.size())) return false;
    count = Read32(header.data() + 12);
    return std::equal(magic.begin(), magic.end(), header.begin()) && Read32(header.data() + 8) == 1 &&
           count && count <= Bl2ShaderStageCache::kMaxStages &&
           Read64(header.data() + 16) == stream.asset.bytes - header.size();
}

// One record whose source hashes to its digest; the manifest covers it.
bool ReadStageRecord(AssetStream& stream, ObbSha256& manifest, GLenum& target, Digest& digest,
                     std::string& source) {
    std::array<uint8_t, Bl2ShaderStageCache::kRecordBytes> record{};
    if (!stream.Read(record.data(), record.size())) return false;
    target = Read32(record.data());
    const uint32_t length = Read32(record.data() + 4);
    if ((target != GL_VERTEX_SHADER && target != GL_FRAGMENT_SHADER) || !length ||
        length > Bl2ShaderStageCache::kMaxStageBytes) return false;
    std::copy_n(record.data() + 8, digest.size(), digest.begin());
    source.resize(length);
    if (!stream.Read(reinterpret_cast<uint8_t*>(source.data()), source.size())) return false;
    ObbSha256 hash;
    hash.Update(reinterpret_cast<const uint8_t*>(source.data()), source.size());
    if (hash.Finalize() != digest || source.find('\0') != std::string::npos) return false;
    manifest.Update(record.data(), record.size());
    manifest.Update(reinterpret_cast<const uint8_t*>(source.data()), source.size());
    return true;
}

// Queues one stage; false once no worker can take it.
bool Push(Former& f, Job job) {
    std::unique_lock lock(f.mutex);
    f.room.wait(lock, [&] {
        return f.workers_failed == f.threads || (!f.paused && f.queue.size() < kQueueLimit);
    });
    if (f.workers_failed == f.threads) return false;
    f.queue.push_back(std::move(job));
    lock.unlock();
    f.work.notify_one();
    return true;
}

// A stage that already has a binary counts as present; false: queue it.
bool AlreadyPresent(Former& f, GLenum target, const Digest& digest) {
    uint64_t fileBytes = 0;
    if (!f.cache->StageFilePresent(target, digest, &fileBytes)) return false;
    f.present.fetch_add(1, std::memory_order_relaxed);
    f.bytes.fetch_add(fileBytes, std::memory_order_relaxed);
    Progress(f);
    return true;
}

// The first-use list: stages the game creates first, formed before anything else.
bool StreamFirstUse(Former& f, std::unordered_set<Key, KeyHash>& first) {
    AssetStream stream;
    std::array<uint8_t, Bl2ShaderStageCache::kHeaderBytes> header{};
    uint32_t count = 0;
    if (!stream.Open(f, kFirstAsset) || !ReadStageHeader(stream, kBl2ShaderFirstMagic, header, count))
        return false;
    ObbSha256 manifest;
    std::string source, padded;
    for (uint32_t index = 0; index < count; ++index) {
        GLenum target = 0;
        Digest digest{};
        if (!ReadStageRecord(stream, manifest, target, digest, source)) return false;
        const Key key{target, digest};
        if (!first.insert(key).second || AlreadyPresent(f, target, digest)) continue;
        if (!legacy_glsl::CompleteSsoInterface(source, target == GL_VERTEX_SHADER, &padded) ||
            padded.size() > Bl2ShaderStageCache::kMaxStageBytes) {
            first.erase(key);  // The catalogue passes decide this one.
            continue;
        }
        if (!Push(f, Job{target, digest, std::move(padded), true})) return false;
        padded.clear();
    }
    const Digest payload = manifest.Finalize();
    return stream.offset == stream.asset.bytes && std::equal(payload.begin(), payload.end(), header.begin() + 24);
}

bool LoadOrder(Former& f, uint32_t count, Order& order) {
    AssetStream stream;
    if (!stream.Open(f, kOrderAsset) || stream.asset.bytes > 16u * 1024 * 1024) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(stream.asset.bytes));
    if (!stream.Read(bytes.data(), bytes.size())) return false;
    const auto& header = f.cache->Header();
    size_t at = 48 + count;
    if (bytes.size() < at + 4 || !std::equal(kOrderMagic.begin(), kOrderMagic.end(), bytes.begin()) ||
        Read32(bytes.data() + 8) != 1 || Read32(bytes.data() + 12) != count ||
        !std::equal(header.begin() + 24, header.end(), bytes.begin() + 16)) return false;
    order.pass.assign(bytes.begin() + 48, bytes.begin() + 48 + count);
    order.last_pass = *std::max_element(order.pass.begin(), order.pass.end());
    const uint32_t packages = Read32(bytes.data() + at);
    at += 4;
    std::vector<uint32_t> per_index(count, 0);
    for (uint32_t package = 0; package < packages; ++package) {
        if (at >= bytes.size()) return false;
        const size_t name_bytes = bytes[at++];
        if (bytes.size() - at < name_bytes + 4) return false;
        order.names.emplace_back(reinterpret_cast<const char*>(bytes.data() + at), name_bytes);
        order.by_name.emplace(order.names.back(), static_cast<uint16_t>(package));
        at += name_bytes;
        const uint32_t members = Read32(bytes.data() + at);
        at += 4;
        if ((bytes.size() - at) / 2 < members) return false;
        auto& list = order.members.emplace_back(members);
        for (uint32_t i = 0; i < members; ++i, at += 2) {
            list[i] = static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
            if (list[i] >= count) return false;
            ++per_index[list[i]];
        }
    }
    if (at != bytes.size()) return false;
    order.first_package.assign(count + 1, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t voters = per_index[i] <= kLocalPackages ? per_index[i] : 0;
        order.first_package[i + 1] = order.first_package[i] + voters;
    }
    order.packages_of.resize(order.first_package[count]);
    std::vector<uint32_t> fill(order.first_package.begin(), order.first_package.end() - 1);
    for (uint16_t package = 0; package < order.members.size(); ++package)
        for (uint16_t index : order.members[package])
            if (per_index[index] <= kLocalPackages) order.packages_of[fill[index]++] = package;
    return true;
}

struct Schedule {
    std::vector<uint8_t> done;                      // Index decided this run.
    std::unordered_map<Key, uint16_t, KeyHash> index_of;
    bool indexed = false;                           // index_of covers the catalogue.
    std::vector<uint8_t> package_queued;
    std::vector<uint32_t> votes;                    // Per package, from renderer misses.
    std::deque<uint16_t> packages;                  // The level pass to run next.
};

// Renderer misses vote for the level packages holding them; the best-supported
// package not formed yet gets the next pass, one level pass at a time. A miss
// on a stage many levels share says nothing about where the player is.
// True: a level pass was queued.
bool DrainDemand(Former& f, const Order& order, Schedule& schedule) {
    if (!schedule.indexed) return false;
    bool queued = false;
    if (f.demand_pending.load(std::memory_order_acquire)) {
        std::vector<Key> misses;
        std::vector<std::string> hints;
        {
            std::lock_guard lock(f.demand_mutex);
            misses.swap(f.demand);
            hints.swap(f.hints);
            f.demand_pending.store(false, std::memory_order_release);
        }
        // A level package being read is loading now: form it before anything
        // else, the newest first (a running level pass resumes after it).
        for (const auto& name : hints) {
            const auto found = order.by_name.find(name);
            if (found == order.by_name.end() || schedule.package_queued[found->second]) continue;
            schedule.package_queued[found->second] = 1;
            schedule.packages.push_front(found->second);
            queued = true;
            __android_log_print(ANDROID_LOG_INFO, kTag, "former level pass for loading package=%s stages=%zu",
                                name.c_str(), order.members[found->second].size());
        }
        for (const Key& key : misses) {
            const auto found = schedule.index_of.find(key);
            if (found == schedule.index_of.end()) continue;
            const uint32_t begin = order.first_package[found->second];
            const uint32_t end = order.first_package[found->second + 1];
            for (uint32_t i = begin; i < end; ++i)
                schedule.votes[order.packages_of[i]] += kVoteUnit / (end - begin);
        }
    }
    if (!schedule.packages.empty()) return queued;
    int best = -1;
    for (size_t package = 0; package < schedule.votes.size(); ++package) {
        if (!schedule.package_queued[package] && schedule.votes[package] >= kMinVotes &&
            (best < 0 || schedule.votes[package] > schedule.votes[best])) best = static_cast<int>(package);
    }
    if (best < 0) return queued;
    schedule.package_queued[best] = 1;
    schedule.packages.push_back(static_cast<uint16_t>(best));
    __android_log_print(ANDROID_LOG_INFO, kTag, "former level pass queued package=%s stages=%zu votes=%u",
                        order.names[best].c_str(), order.members[best].size(), schedule.votes[best]);
    return true;
}

enum class Pass { Complete, Interrupted, Invalid };

// One sequential pass over the catalogue, queuing the selected stages without
// a binary. The catalogue must carry the opened cache's exact header, stay in
// canonical order and match its manifest: only a complete, valid pass vouches
// for every stage. A default pass yields to a level pass queued meanwhile.
template <typename Select>
Pass StreamCatalogue(Former& f, const Order& order, Schedule& schedule,
                     const std::unordered_set<Key, KeyHash>& first, Select select, bool yields) {
    AssetStream stream;
    std::array<uint8_t, Bl2ShaderStageCache::kHeaderBytes> header{};
    uint32_t count = 0;
    if (!stream.Open(f, kCatalogueAsset) ||
        !ReadStageHeader(stream, Bl2ShaderStageCache::kTableMagic, header, count) ||
        header != f.cache->Header() || count != schedule.done.size()) return Pass::Invalid;
    ObbSha256 manifest;
    std::string source, padded;
    GLenum previous_target = 0;
    Digest previous{};
    for (uint32_t index = 0; index < count; ++index) {
        GLenum target = 0;
        Digest digest{};
        if (!ReadStageRecord(stream, manifest, target, digest, source) ||
            (index && (target < previous_target || (target == previous_target && digest <= previous))))
            return Pass::Invalid;
        previous_target = target;
        previous = digest;
        const Key key{target, digest};
        if (!schedule.indexed) schedule.index_of.emplace(key, static_cast<uint16_t>(index));
        // Yield even while nothing is queued (e.g. a level pass over formed stages).
        if (yields && (index & 255) == 255 && DrainDemand(f, order, schedule)) return Pass::Interrupted;
        if (schedule.done[index] || !select(index)) continue;
        schedule.done[index] = 1;
        if (first.contains(key) || AlreadyPresent(f, target, digest)) continue;
        if (!legacy_glsl::CompleteSsoInterface(source, target == GL_VERTEX_SHADER, &padded) ||
            padded.size() > Bl2ShaderStageCache::kMaxStageBytes) return Pass::Invalid;
        if (!Push(f, Job{target, digest, std::move(padded), false})) return Pass::Invalid;
        padded.clear();
        if (yields && schedule.indexed && DrainDemand(f, order, schedule)) return Pass::Interrupted;
    }
    const Digest payload = manifest.Finalize();
    if (stream.offset != stream.asset.bytes || !std::equal(payload.begin(), payload.end(), header.begin() + 24))
        return Pass::Invalid;
    schedule.indexed = true;
    return Pass::Complete;
}

bool CreateContext(EGLDisplay display, EGLContext& context, EGLSurface& surface) {
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
    if (display == EGL_NO_DISPLAY || !eglBindAPI(EGL_OPENGL_ES_API)) return false;
    const EGLint wanted[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                             EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, wanted, &config, 1, &count) || count < 1) return false;
    // No share group: the compile never waits on a lock held by the renderer.
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attributes);
    if (context == EGL_NO_CONTEXT) return false;
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    const bool surfaceless = extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context");
    if (!surfaceless) {
        const EGLint pbuffer[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, pbuffer);
    }
    if ((surfaceless || surface != EGL_NO_SURFACE) && eglMakeCurrent(display, surface, surface, context))
        return true;
    if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
    eglDestroyContext(display, context);
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;
    return false;
}

void Work(Former* f, unsigned index) {
    char name[16];
    std::snprintf(name, sizeof(name), "BL2StageForm%u", index);
    Background(name);
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    // Binaries are only portable within the identity the cache was opened with.
    if (!CreateContext(f->display, context, surface) || !f->cache->MatchesCurrentDriver()) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "former worker %u unavailable error=0x%04X",
                            index, eglGetError());
        if (context != EGL_NO_CONTEXT) {
            eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(f->display, surface);
            eglDestroyContext(f->display, context);
        }
        eglReleaseThread();
        f->incomplete = true;
        {
            std::lock_guard lock(f->mutex);
            ++f->workers_failed;
        }
        f->room.notify_all();
        return;
    }
    if (index == 0) {
        const bool parallel = ParallelCompiler();
        f->serialized.store(!parallel, std::memory_order_relaxed);
        __android_log_print(ANDROID_LOG_INFO, kTag, "former parallel compiler=%d", parallel);
    }
    bool lowered = false;  // Eco lane: little cores, lowest priority.
    for (;;) {
        // While the player is in the world, or the renderer has just compiled
        // a stage itself, one worker forms: more contend for a driver-wide
        // compiler lock on some GPUs (Mali-G76 r26p0: a renderer compile took
        // 1.7x as long beside three workers, unchanged beside one).
        while (index != 0 && f->serialized.load(std::memory_order_relaxed) &&
               (InGameplay() || NowNs() - g_last_demand_ns.load(std::memory_order_relaxed) < kDemandQuietNs)) {
            {
                std::lock_guard lock(f->mutex);
                if (!f->producing && f->queue.empty()) break;
            }
            usleep(100'000);
        }
        Job job;
        {
            std::unique_lock lock(f->mutex);
            f->work.wait(lock, [&] {
                return (!f->paused && !f->queue.empty()) || (!f->producing && f->queue.empty());
            });
            if (f->queue.empty()) break;
            job = std::move(f->queue.front());
            f->queue.pop_front();
        }
        f->room.notify_one();
        // First-use stages are needed now; the catalogue yields to play.
        const bool eco = !job.first_use && InGameplay();
        if (eco != lowered) {
            if (eco) LowerToLittleCores();
            else RaiseToAllCores();
            lowered = eco;
        }
        if (eco) {
            while (NowNs() - g_last_demand_ns.load(std::memory_order_relaxed) < kDemandQuietNs)
                usleep(100'000);
        }
        uint64_t fileBytes = 0;
        // The renderer may have compiled this stage on demand meanwhile.
        if (f->cache->StageFilePresent(job.target, job.digest, &fileBytes)) {
            f->present.fetch_add(1, std::memory_order_relaxed);
            f->bytes.fetch_add(fileBytes, std::memory_order_relaxed);
        } else if (f->cache->FormStage(job.target, job.digest, job.padded, fileBytes)) {
            f->formed.fetch_add(1, std::memory_order_relaxed);
            f->bytes.fetch_add(fileBytes, std::memory_order_relaxed);
        } else {
            f->rejected.fetch_add(1, std::memory_order_relaxed);
            f->incomplete = true;
        }
        while (glGetError() != GL_NO_ERROR) {}
        Progress(*f);
    }
    eglMakeCurrent(f->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surface != EGL_NO_SURFACE) eglDestroySurface(f->display, surface);
    eglDestroyContext(f->display, context);
    eglReleaseThread();
}

void Produce(Former* f) {
    Background("BL2StageList");
    f->started_ns = NowNs();
    f->total = Read32(f->cache->Header().data() + 12);
    f->initial_files = static_cast<uint32_t>(std::min<size_t>(f->cache->CountStageFiles(), f->total));
    f->counted.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "former start threads=%u on_disk=%u/%u", f->threads,
                        f->initial_files.load(), f->total);
    // Directories of older drivers or catalogues: plain file I/O, lowest priority.
    std::thread([cache = f->cache] {
        pthread_setname_np(pthread_self(), "BL2StageStale");
        setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), 19);
        const size_t removed = cache->RemoveStaleIdentities();
        if (removed) __android_log_print(ANDROID_LOG_INFO, kTag, "former removed stale caches=%zu", removed);
    }).detach();
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < f->threads; ++index) {
        try {
            workers.emplace_back(Work, f, index);
        } catch (...) {
            std::lock_guard lock(f->mutex);
            ++f->workers_failed;
            f->incomplete = true;
        }
    }
    std::unordered_set<Key, KeyHash> first;
    const bool first_valid = StreamFirstUse(*f, first);
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "former first-use list queued valid=%d stages=%zu formed=%u present=%u elapsed=%.1fs",
        first_valid, first.size(), f->formed.load(), f->present.load(), (NowNs() - f->started_ns) / 1e9);
    const uint32_t count = Read32(f->cache->Header().data() + 12);
    Order order;
    const bool ordered = LoadOrder(*f, count, order);
    Schedule schedule;
    schedule.done.assign(count, 0);
    schedule.index_of.reserve(count);
    schedule.package_queued.assign(order.names.size(), 0);
    schedule.votes.assign(order.names.size(), 0);
    unsigned next_pass = ordered ? 0 : 256;
    bool valid = true, verified = false;
    // Keys -> catalogue indices first, so misses steer level passes from the
    // first level load on, before any default pass has finished.
    if (ordered) {
        valid = StreamCatalogue(*f, order, schedule, first, [](uint32_t) { return false; }, false) ==
                Pass::Complete;
        __android_log_print(ANDROID_LOG_INFO, kTag, "former index ready=%d elapsed=%.1fs", valid,
                            (NowNs() - f->started_ns) / 1e9);
    }
    while (valid && !verified) {
        Pass result;
        if (!schedule.packages.empty()) {
            const uint16_t package = schedule.packages.front();
            std::vector<uint8_t> member(count, 0);
            for (uint16_t index : order.members[package]) member[index] = 1;
            // Only a newly loading package interrupts a level pass (votes wait).
            result = StreamCatalogue(*f, order, schedule, first,
                                     [&](uint32_t index) { return member[index] != 0; }, true);
            if (result == Pass::Complete) {
                schedule.packages.pop_front();
                __android_log_print(ANDROID_LOG_INFO, kTag, "former level pass done package=%s elapsed=%.1fs",
                                    order.names[package].c_str(), (NowNs() - f->started_ns) / 1e9);
            }
        } else if (next_pass <= order.last_pass && ordered) {
            const unsigned pass = next_pass;
            result = StreamCatalogue(*f, order, schedule, first,
                                     [&](uint32_t index) { return order.pass[index] == pass; }, true);
            if (result == Pass::Complete) {
                ++next_pass;
                __android_log_print(ANDROID_LOG_INFO, kTag, "former pass %u done elapsed=%.1fs",
                                    pass, (NowNs() - f->started_ns) / 1e9);
            }
        } else {
            // Everything left, and the full-catalogue check behind the marker.
            result = StreamCatalogue(*f, order, schedule, first, [](uint32_t) { return true; }, false);
            verified = result == Pass::Complete;
        }
        valid = result != Pass::Invalid;
        if (schedule.indexed) DrainDemand(*f, order, schedule);
    }
    {
        std::lock_guard lock(f->mutex);
        f->producing = false;
    }
    f->work.notify_all();
    for (auto& worker : workers) worker.join();
    const bool complete = verified && !f->incomplete.load();
    const bool committed = complete && f->cache->CommitFormation(f->bytes.load());
    __android_log_print(ANDROID_LOG_INFO, kTag,
        "former finished committed=%d catalogue=%d ordered=%d formed=%u present=%u rejected=%u "
        "bytes=%.1fMiB elapsed=%.1fs",
        committed, verified, ordered, f->formed.load(), f->present.load(), f->rejected.load(),
        f->bytes.load() / 1048576.0, (NowNs() - f->started_ns) / 1e9);
    g_active = false;
}

const char* FormerProperty(char (&value)[PROP_VALUE_MAX]) {
    return __system_property_get("debug.bl2.shader_former", value) > 0 ? value : nullptr;
}

}  // namespace

Bl2ShaderFormerMode Bl2ShaderFormerConfiguredMode() {
    static const Bl2ShaderFormerMode mode = [] {
        char value[PROP_VALUE_MAX] = {};
        const char* setting = FormerProperty(value);
        if (setting && std::strcmp(setting, "0") == 0) return Bl2ShaderFormerMode::OnDemand;
        if (setting && std::strcmp(setting, "full") == 0) return Bl2ShaderFormerMode::Blocking;
        return Bl2ShaderFormerMode::Background;
    }();
    return mode;
}

unsigned Bl2ShaderFormerConfiguredThreads() {
    static const unsigned threads = [] {
        char value[PROP_VALUE_MAX] = {};
        if (const char* setting = FormerProperty(value)) {
            const int count = std::atoi(setting);
            if (count >= 1 && count <= static_cast<int>(kMaxThreads)) return static_cast<unsigned>(count);
        }
        // In menus they use every core, during play the little cluster only:
        // three still fit the four little cores of an eight-core phone.
        const long cpus = sysconf(_SC_NPROCESSORS_CONF);
        return cpus >= 8 ? 3u : cpus >= 6 ? 2u : 1u;
    }();
    return threads;
}

bool Bl2ShaderFormerStart(const Bl2ShaderStageCache* cache, EGLDisplay display,
                          Bl2ShaderFormerOpen open, unsigned threads) {
    std::lock_guard lock(g_mutex);
    if (g_former || !cache || !open || display == EGL_NO_DISPLAY || !threads ||
        threads > kMaxThreads) return false;
    auto* former = new Former;
    former->cache = cache;
    former->display = display;
    former->open = open;
    former->threads = threads;
    g_former = former;
    g_active = true;
    try {
        std::thread(Produce, former).detach();
    } catch (...) {
        g_active = false;
        return false;
    }
    return true;
}

void Bl2ShaderFormerPause(bool paused) {
    std::lock_guard lock(g_mutex);
    if (!g_former) return;
    {
        std::lock_guard former_lock(g_former->mutex);
        g_former->paused = paused;
    }
    g_former->work.notify_all();
    g_former->room.notify_all();
}

bool Bl2ShaderFormerActive() { return g_active.load(); }

bool Bl2ShaderFormerProgress(uint32_t* ready, uint32_t* total) {
    if (!g_active.load(std::memory_order_acquire)) return false;
    Former* former = nullptr;
    {
        std::lock_guard lock(g_mutex);
        former = g_former;
    }
    if (!former || !former->counted.load(std::memory_order_acquire) || !former->total) return false;
    // A stage both formed and compiled on demand counts twice: rare, clamped.
    const uint64_t on_disk = uint64_t{former->initial_files.load(std::memory_order_relaxed)} +
                             former->formed.load(std::memory_order_relaxed) +
                             former->demand_compiles.load(std::memory_order_relaxed);
    if (ready) *ready = static_cast<uint32_t>(std::min<uint64_t>(on_disk, former->total));
    if (total) *total = former->total;
    return true;
}

void Bl2ShaderFormerSetGameplayProbe(Bl2ShaderFormerGameplayProbe probe) {
    g_gameplay_probe.store(probe, std::memory_order_release);
}

void Bl2ShaderFormerHintPackage(const char* name) {
    if (!g_active.load(std::memory_order_relaxed) || !name) return;
    Former* former = nullptr;
    {
        std::lock_guard lock(g_mutex);
        former = g_former;
    }
    if (!former) return;
    std::lock_guard lock(former->demand_mutex);
    if (former->hints.size() < 256) former->hints.emplace_back(name);
    former->demand_pending.store(true, std::memory_order_release);
}

void Bl2ShaderFormerNoteDemandCompile(GLenum target, const uint8_t* digest) {
    if (!g_active.load(std::memory_order_relaxed)) return;
    g_last_demand_ns.store(NowNs(), std::memory_order_relaxed);
    Former* former = nullptr;
    {
        std::lock_guard lock(g_mutex);
        former = g_former;
    }
    if (!former) return;
    former->demand_compiles.fetch_add(1, std::memory_order_relaxed);
    if (!digest) return;
    Key key{target, {}};
    std::memcpy(key.second.data(), digest, key.second.size());
    std::lock_guard lock(former->demand_mutex);
    if (former->demand.size() < 4096) former->demand.push_back(key);
    former->demand_pending.store(true, std::memory_order_release);
}
