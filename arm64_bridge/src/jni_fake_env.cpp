#include "jni_fake_env.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "perf_probe.h"
#include "thunk_manager.h"
#include "thunk_types.h"
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <limits>
#include <utility>

#define LOG_TAG "Arm32Bridge_FakeJni"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

// The Tegra 4 system-settings profile asks Java to render at 0.9 scale. Keep
// the profile's high-detail feature/content choices without accepting that
// legacy tablet-era resolution reduction. Values above 1 remain untouched so
// a future supersampling request is still honored.
static constexpr float kMinimumRenderScale = 1.0f;
static std::atomic<bool> s_logged_render_scale_clamp{false};

// JNIEnv table slot count (through GetObjectRefType)
static thread_local JNIEnv* thread_local_env = nullptr;

static const uint32_t kEnvSlots = 233;

static const uint32_t kEnvTableBytes = kEnvSlots * 4;
// JavaVM (JNIInvokeInterface) slots: 0..7
static const uint32_t kVmSlots = 8;

// Byte offsets in a 32-bit JNINativeInterface table. These must not be
// calculated with offsetof(JNINativeInterface, ...): this bridge itself is
// compiled for ARM64, where every function-table pointer is eight bytes wide.
namespace aarch32_jni {
constexpr uint32_t kAllocObject = 27 * 4;              // 108
constexpr uint32_t kNewObject = 28 * 4;                // 112
constexpr uint32_t kNewObjectV = 29 * 4;               // 116
constexpr uint32_t kNewObjectA = 30 * 4;               // 120
constexpr uint32_t kCallObjectMethod = 34 * 4;          // 136
constexpr uint32_t kCallObjectMethodV = 35 * 4;         // 140
constexpr uint32_t kCallObjectMethodA = 36 * 4;         // 144
constexpr uint32_t kCallBooleanMethod = 37 * 4;         // 148
constexpr uint32_t kCallBooleanMethodV = 38 * 4;        // 152
constexpr uint32_t kCallBooleanMethodA = 39 * 4;        // 156
constexpr uint32_t kGetLongField = 101 * 4;             // 404
constexpr uint32_t kSetObjectField = 104 * 4;           // 416
constexpr uint32_t kSetIntField = 109 * 4;              // 436
constexpr uint32_t kGetStaticFieldID = 144 * 4;         // 576
constexpr uint32_t kGetStaticObjectField = 145 * 4;     // 580
constexpr uint32_t kGetStaticIntField = 150 * 4;        // 600
constexpr uint32_t kGetStringUTFLength = 168 * 4;       // 672
constexpr uint32_t kGetArrayLength = 171 * 4;           // 684
constexpr uint32_t kGetObjectArrayElement = 173 * 4;    // 692
constexpr uint32_t kNewByteArray = 176 * 4;             // 704
constexpr uint32_t kNewLongArray = 180 * 4;             // 720
constexpr uint32_t kGetByteArrayElements = 184 * 4;    // 736
constexpr uint32_t kGetLongArrayElements = 188 * 4;    // 752
constexpr uint32_t kReleaseByteArrayElements = 192 * 4; // 768
constexpr uint32_t kReleaseLongArrayElements = 196 * 4; // 784
constexpr uint32_t kGetByteArrayRegion = 200 * 4;      // 800
constexpr uint32_t kGetLongArrayRegion = 204 * 4;      // 816
constexpr uint32_t kSetByteArrayRegion = 208 * 4;       // 832
constexpr uint32_t kSetLongArrayRegion = 212 * 4;      // 848
constexpr uint32_t kRegisterNatives = 215 * 4;          // 860
}  // namespace aarch32_jni

FakeJniEnv& FakeJniEnv::Instance() {
    static FakeJniEnv inst;
    return inst;
}

void FakeJniEnv::Init(DynarmicHost& host) {
    std::lock_guard<std::mutex> update_lock(m_update_mutex);
    // Host was recreated (first load attempt failed before the Activity
    // existed) � old tables pointed into freed guest memory.
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        if (m_host == &host) return;
        m_host = &host;
        m_env_table = 0;
        m_vm_table = 0;
        m_next_obj_handle = 0x00020000;
        m_next_mid_handle = 0x00030000;
        m_next_fid_handle = 0x00040000;
        m_objects.clear();
        m_primitive_array_copies.clear();
        m_objects_by_hash.clear();
        m_lru.clear();
        m_methods.clear();
        m_fields.clear();
        m_method_ids.clear();
        m_field_ids.clear();
        m_direct_bufs.clear();
        m_jstr_utf.clear();
    }
    ThunkManager& tm = host.GetThunkManager();

    // Guest memory for tables
    const uint32_t env_table = host.GuestAlloc(kEnvTableBytes, 4);
    const uint32_t vm_table = host.GuestAlloc(kVmSlots * 4, 4);
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        m_env_table = env_table;
        m_vm_table = vm_table;
    }

    auto register_slot = [&](uint32_t table, uint32_t offset, bool is_vm) {
        uint32_t thunk = tm.RegisterThunk(
            std::string("jni_") + (is_vm ? "vm" : "env") + "_" + std::to_string(offset),
            [this, offset, is_vm](Dynarmic::A32::Jit& jit, DynarmicHost&) {
                if (is_vm) DispatchVmSlot(offset, jit);
                else DispatchEnvSlot(offset, jit);
            });
        host.MemoryWrite32(table + offset, thunk);
    };

    // Register all env slots: known offsets get real handlers (via DispatchEnvSlot
    // switch), everything else logs-and-zeroes on first use.
    for (uint32_t off = 0; off < kEnvSlots; ++off) {
        register_slot(m_env_table, off * 4, false);
    }
    for (uint32_t off = 0; off < kVmSlots; ++off) {
        register_slot(m_vm_table, off * 4, true);
    }

    // JNI ABI: guest code does `ldr r, [env]; ldr r, [r, #off]; blx r` �
    // i.e. [env] must point to the function-pointer table itself. Slot 0 of
    // both tables is reserved by the spec, so make it a self-pointer.
    host.MemoryWrite32(m_env_table, m_env_table);
    host.MemoryWrite32(m_vm_table, m_vm_table);

    LOGI("Fake JNI tables: env=0x%08X (%u bytes), vm=0x%08X", m_env_table, kEnvTableBytes, m_vm_table);
}

void FakeJniEnv::AttachHostEnv(JNIEnv* env) {
    thread_local_env = env;
}

// A thread that attaches itself and then exits still attached leaves the
// runtime's Thread object behind, and a checked build aborts the process over
// it. Guest worker threads (audio, streaming, a JIT's own helpers) come and go
// and none of them knows it is talking to a bridge, so the detach hangs off a
// TLS destructor instead of being left to whoever created the thread. Only
// threads attached here get the key set - detaching one the VM owns is worse
// than leaking it.
static pthread_key_t s_detach_key;
static JavaVM* s_detach_vm = nullptr;

static void DetachOnThreadExit(void*) {
    if (s_detach_vm) s_detach_vm->DetachCurrentThread();
    thread_local_env = nullptr;
}

JNIEnv* FakeJniEnv::GetEnv() const {
    if (thread_local_env) return thread_local_env;
    if (!m_vm) return nullptr;
    JNIEnv* env = nullptr;
    if (m_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK) {
        thread_local_env = env;
        return env;
    }
    if (m_vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        thread_local_env = env;
        static std::once_flag once;
        std::call_once(once, [this] {
            s_detach_vm = m_vm;
            pthread_key_create(&s_detach_key, DetachOnThreadExit);
        });
        pthread_setspecific(s_detach_key, reinterpret_cast<void*>(1));
    }
    return env;
}

// -------------------------------------------------------------
// Handles
// -------------------------------------------------------------

// Cap on live global refs. ART's own limit is 51200 and hitting it kills the
// process outright, so the table has to stay well under it and reclaim rather
// than stop caching - a "cache full, hand out a ref and forget it" branch is the
// original leak with a delay on it.
static const size_t kMaxObjectRefs = 8192;

int32_t FakeJniEnv::IdentityHash(JNIEnv* env, jobject o) {
    static jclass system = nullptr;
    static jmethodID identity_hash = nullptr;
    static std::once_flag identity_once;
    std::call_once(identity_once, [env] {
        jclass local = env->FindClass("java/lang/System");
        if (!local) {
            env->ExceptionClear();
            return;
        }
        system = static_cast<jclass>(env->NewGlobalRef(local));
        env->DeleteLocalRef(local);
        if (!system) return;
        identity_hash = env->GetStaticMethodID(system, "identityHashCode", "(Ljava/lang/Object;)I");
        if (!identity_hash) env->ExceptionClear();
    });
    if (!system || !identity_hash) return 0;
    int32_t h = env->CallStaticIntMethod(system, identity_hash, o);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return h;
}

void FakeJniEnv::TouchRef(uint32_t handle) const {
    auto it = m_objects.find(handle);
    if (it != m_objects.end() && it->second.pins == 0 &&
        it->second.utf_leases == 0) {
        m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
    }
}

FakeJniEnv::EvictedRef FakeJniEnv::EvictOneRefLocked() {
    EvictedRef evicted;
    if (m_lru.empty()) return evicted;  // everything pinned/leased; grow past the cap
    uint32_t handle = m_lru.back();
    m_lru.pop_back();

    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return evicted;

    auto bucket = m_objects_by_hash.find(it->second.hash);
    if (bucket != m_objects_by_hash.end()) {
        auto& v = bucket->second;
        v.erase(std::remove(v.begin(), v.end(), handle), v.end());
        if (v.empty()) m_objects_by_hash.erase(bucket);
    }
    // The UTF-8 copy of a jstring lives as long as the jstring's handle does;
    // leaving it behind strands the arena block and, worse, keeps a dead handle
    // mapped to it.
    auto utf = m_jstr_utf.find(handle);
    if (utf != m_jstr_utf.end()) {
        evicted.utf_copy = utf->second;
        m_jstr_utf.erase(utf);
    }
    evicted.global = it->second.global;
    m_objects.erase(it);
    return evicted;
}

uint32_t FakeJniEnv::AddObjectRef(jobject o) {
    if (!o) return 0;
    JNIEnv* env = GetEnv();
    if (!env) return 0;
    // Only one publisher may compare/evict/insert at a time. This keeps copied
    // global refs stable while IsSameObject runs, without holding m_mutex across
    // any host JNI or guest allocator call.
    std::lock_guard<std::mutex> update_lock(m_update_mutex);
    int32_t hash = IdentityHash(env, o);

    std::vector<std::pair<uint32_t, jobject>> candidates;
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        auto bucket = m_objects_by_hash.find(hash);
        if (bucket != m_objects_by_hash.end()) {
            candidates.reserve(bucket->second.size());
            for (uint32_t handle : bucket->second) {
                auto object = m_objects.find(handle);
                if (object != m_objects.end()) {
                    candidates.emplace_back(handle, object->second.global);
                }
            }
        }
    }
    for (const auto& [handle, global] : candidates) {
        if (env->IsSameObject(global, o)) {
            std::lock_guard<std::mutex> state_lock(m_mutex);
            if (m_objects.find(handle) != m_objects.end()) TouchRef(handle);
            return handle;
        }
    }

    jobject global = env->NewGlobalRef(o);
    if (!global) return 0;

    EvictedRef evicted;
    uint32_t handle = 0;
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        // Removal and publication are state-only operations. JNI/global-ref and
        // guest-memory cleanup happens after releasing m_mutex below.
        if (m_objects.size() >= kMaxObjectRefs) evicted = EvictOneRefLocked();

        handle = m_next_obj_handle;
        m_next_obj_handle += 4;
        m_lru.push_front(handle);
        ObjRef ref;
        ref.global = global;
        ref.hash = hash;
        ref.pins = 0;
        ref.lru = m_lru.begin();
        m_objects.emplace(handle, std::move(ref));
        m_objects_by_hash.try_emplace(hash).first->second.push_back(handle);
    }
    if (evicted.utf_copy && m_host) m_host->GuestFree(evicted.utf_copy);
    if (evicted.global) env->DeleteGlobalRef(evicted.global);
    return handle;
}

uint32_t FakeJniEnv::AddLocalObjectRef(JNIEnv* env, jobject local) {
    if (!local) return 0;
    const uint32_t handle = AddObjectRef(local);
    env->DeleteLocalRef(local);
    return handle;
}

jobject FakeJniEnv::ResolveObject(uint32_t handle) const {
    if (!handle) return nullptr;
    JNIEnv* env = GetEnv();
    if (!env) return nullptr;

    // AddObjectRef owns m_update_mutex while it may delete an evicted global
    // reference. Keep that lock until ART has made a local reference: the local
    // then remains valid even if the backing global is evicted immediately.
    std::lock_guard<std::mutex> update_lock(m_update_mutex);
    jobject global = nullptr;
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        auto it = m_objects.find(handle);
        if (it == m_objects.end()) return nullptr;
        TouchRef(handle);
        global = it->second.global;
    }
    return env->NewLocalRef(global);
}

bool FakeJniEnv::PinObjectRef(uint32_t handle, int delta) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return false;
    ObjRef& ref = it->second;
    const bool was = ref.pins > 0 || ref.utf_leases > 0;
    ref.pins += delta;
    if (ref.pins < 0) ref.pins = 0;
    const bool now = ref.pins > 0 || ref.utf_leases > 0;
    if (now == was) return true;
    if (now) {
        m_lru.erase(ref.lru);
    } else {
        m_lru.push_front(handle);
        ref.lru = m_lru.begin();
    }
    return true;
}

bool FakeJniEnv::AcquireUtfLease(uint32_t handle) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end()) return false;
    ObjRef& ref = it->second;
    if (ref.pins == 0 && ref.utf_leases == 0) m_lru.erase(ref.lru);
    ++ref.utf_leases;
    return true;
}

void FakeJniEnv::CancelUtfLease(uint32_t handle) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end() || it->second.utf_leases == 0) return;
    ObjRef& ref = it->second;
    --ref.utf_leases;
    if (ref.pins == 0 && ref.utf_leases == 0) {
        m_lru.push_front(handle);
        ref.lru = m_lru.begin();
    }
}

void FakeJniEnv::ReleaseUtfLease(uint32_t handle, uint32_t guest_copy) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it == m_objects.end() || it->second.utf_leases == 0) return;
    auto cached = m_jstr_utf.find(handle);
    if (!guest_copy || cached == m_jstr_utf.end() || cached->second != guest_copy) return;
    ObjRef& ref = it->second;
    --ref.utf_leases;
    if (ref.pins == 0 && ref.utf_leases == 0) {
        m_lru.push_front(handle);
        ref.lru = m_lru.begin();
    }
}

void FakeJniEnv::DemoteObjectRef(uint32_t handle) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_objects.find(handle);
    if (it != m_objects.end() && it->second.pins == 0 &&
        it->second.utf_leases == 0) {
        m_lru.splice(m_lru.end(), m_lru, it->second.lru);
    }
}

// One id per jmethodID. Guests that reach Java through reflection ask for the
// same method on every call; appending unconditionally grew this table, and the
// signature string in each entry, for the life of the process.
//
// Dedup by mid alone is only correct because handles are interned: clsHandle for
// a given class is now stable, so the first registration's copy stays right.
uint32_t FakeJniEnv::AddMethod(jmethodID mid, bool isStatic, uint32_t clsHandle, const std::string& sig,
                               const std::string& name) {
    if (!mid) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto known = m_method_ids.find(mid);
    if (known != m_method_ids.end()) return known->second;

    uint32_t h = m_next_mid_handle;
    m_next_mid_handle += 4;
    MethodInfo mi;
    mi.mid = mid;
    mi.isStatic = isStatic;
    mi.clsHandle = clsHandle;
    mi.sig = sig;
    mi.name = name;
    m_methods.emplace(h, std::move(mi));
    m_method_ids.emplace(mid, h);
    return h;
}

uint32_t FakeJniEnv::AddField(jfieldID fid) {
    if (!fid) return 0;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto known = m_field_ids.find(fid);
    if (known != m_field_ids.end()) return known->second;

    uint32_t h = m_next_fid_handle;
    m_next_fid_handle += 4;
    m_fields.emplace(h, fid);
    m_field_ids.emplace(fid, h);
    return h;
}

std::vector<char> FakeJniEnv::ParamsForMethod(uint32_t handle) const {
    std::string signature;
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        auto method = m_methods.find(handle);
        if (method == m_methods.end()) return {};
        signature = method->second.sig;
    }
    return ParseParams(signature);
}

jfieldID FakeJniEnv::ResolveField(uint32_t handle) const {
    std::lock_guard<std::mutex> state_lock(m_mutex);
    auto field = m_fields.find(handle);
    return field != m_fields.end() ? field->second : nullptr;
}

bool FakeJniEnv::ResolveDirectBuffer(uint32_t handle, DirectBuf& out) const {
    std::lock_guard<std::mutex> state_lock(m_mutex);
    auto buffer = m_direct_bufs.find(handle);
    if (buffer == m_direct_bufs.end()) return false;
    out = buffer->second;
    return true;
}

// -------------------------------------------------------------
// Signature parsing & guest arg reading
// -------------------------------------------------------------

std::vector<char> FakeJniEnv::ParseParams(const std::string& sig) const {
    std::vector<char> out;
    size_t i = sig.find('(');
    if (i == std::string::npos) return out;
    for (++i; i < sig.size() && sig[i] != ')'; ++i) {
        char c = sig[i];
        if (c == 'L') {
            while (sig[i] != ';') ++i;
            out.push_back('L');
        } else if (c == '[') {
            while (sig[i] == '[') ++i;
            if (sig[i] == 'L') while (sig[i] != ';') ++i;
            out.push_back('L');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// JNI's plain `...` and va_list entry points obey the C default argument
// promotions. In particular, F is carried as an 8-byte double, while
// Z/B/C/S are carried as 32-bit int. AAPCS32 also aligns J/D and the promoted
// F to an even word / 8-byte address. With the three fixed JNI arguments
// (env, object/class, methodID), a first wide vararg skips r3 and starts at SP.
static constexpr bool IsWidePromotedJniArg(char descriptor_type) {
    return descriptor_type == 'J' || descriptor_type == 'D' || descriptor_type == 'F';
}

class Aapcs32DirectVarArgCursor {
public:
    constexpr uint32_t Take(char descriptor_type) {
        if (IsWidePromotedJniArg(descriptor_type)) m_next_word = (m_next_word + 1u) & ~1u;
        uint32_t location = m_next_word;
        m_next_word += IsWidePromotedJniArg(descriptor_type) ? 2u : 1u;
        return location;
    }

    constexpr uint32_t NextWord() const { return m_next_word; }

private:
    // r0=JNIEnv*, r1=jobject/jclass, r2=jmethodID; r3 is the first possible
    // 32-bit vararg. Logical words >=4 live at SP + (word-4)*4.
    uint32_t m_next_word = 3;
};

class Aapcs32VaListCursor {
public:
    constexpr explicit Aapcs32VaListCursor(uint32_t address) : m_next_address(address) {}

    constexpr uint32_t Take(char descriptor_type) {
        if (IsWidePromotedJniArg(descriptor_type)) {
            m_next_address = (m_next_address + 7u) & ~7u;
        }
        uint32_t location = m_next_address;
        m_next_address += IsWidePromotedJniArg(descriptor_type) ? 8u : 4u;
        return location;
    }

    constexpr uint32_t NextAddress() const { return m_next_address; }

private:
    uint32_t m_next_address;
};

static constexpr bool TestFirstWideDirectLayouts() {
    Aapcs32DirectVarArgCursor j;
    Aapcs32DirectVarArgCursor d;
    Aapcs32DirectVarArgCursor f;
    return j.Take('J') == 4 && j.NextWord() == 6 &&
           d.Take('D') == 4 && d.NextWord() == 6 &&
           f.Take('F') == 4 && f.NextWord() == 6;
}

static constexpr bool TestMixedDirectLayout() {
    // Descriptor fragment IFLJ: I=r3, promoted F=SP..SP+7, L=SP+8,
    // then one stack pad word before J=SP+16..SP+23.
    Aapcs32DirectVarArgCursor cursor;
    return cursor.Take('I') == 3 && cursor.Take('F') == 4 &&
           cursor.Take('L') == 6 && cursor.Take('J') == 8 &&
           cursor.NextWord() == 10;
}

static constexpr bool TestAllSingleWordPromotions() {
    Aapcs32DirectVarArgCursor cursor;
    return cursor.Take('Z') == 3 && cursor.Take('B') == 4 &&
           cursor.Take('C') == 5 && cursor.Take('S') == 6 &&
           cursor.Take('I') == 7 && cursor.Take('L') == 8 &&
           cursor.NextWord() == 9;
}

static constexpr bool TestMixedVaListLayout() {
    // A va_list may initially point at the saved r3 word (4 mod 8).
    Aapcs32VaListCursor cursor(0x1004);
    return cursor.Take('I') == 0x1004 && cursor.Take('F') == 0x1008 &&
           cursor.Take('L') == 0x1010 && cursor.Take('J') == 0x1018 &&
           cursor.NextAddress() == 0x1020;
}

static constexpr bool TestFirstWideVaListLayouts() {
    Aapcs32VaListCursor j(0x1004);
    Aapcs32VaListCursor d(0x1004);
    Aapcs32VaListCursor f(0x1004);
    return j.Take('J') == 0x1008 && j.NextAddress() == 0x1010 &&
           d.Take('D') == 0x1008 && d.NextAddress() == 0x1010 &&
           f.Take('F') == 0x1008 && f.NextAddress() == 0x1010;
}

static constexpr uint64_t CombineGuestLittleEndianWords(uint32_t low, uint32_t high) {
    return static_cast<uint64_t>(low) | (static_cast<uint64_t>(high) << 32);
}

static constexpr uint32_t PromotedDoubleToFloatBits(uint64_t promoted_bits) {
    double promoted = std::bit_cast<double>(promoted_bits);
    return std::bit_cast<uint32_t>(static_cast<float>(promoted));
}

static_assert(TestFirstWideDirectLayouts(),
              "AAPCS32 first J/D/F vararg must skip r3 and start at SP");
static_assert(TestAllSingleWordPromotions(),
              "AAPCS32 Z/B/C/S/I/L arguments must consume one promoted word");
static_assert(TestMixedDirectLayout(), "AAPCS32 mixed direct-vararg layout regression");
static_assert(TestMixedVaListLayout(), "AAPCS32 mixed va_list layout regression");
static_assert(TestFirstWideVaListLayouts(),
              "AAPCS32 va_list J/D/F values must align to the next 8-byte address");
static_assert(CombineGuestLittleEndianWords(0x55667788u, 0x11223344u) ==
                  UINT64_C(0x1122334455667788),
              "ARM32 guest wide arguments use little-endian low/high word order");
static_assert(PromotedDoubleToFloatBits(UINT64_C(0x3ff8000000000000)) == 0x3fc00000u,
              "JNI F vararg must narrow promoted double 1.5 back to float 1.5");

static uint64_t ReadGuestLittleEndian64(DynarmicHost& host, uint32_t address) {
    return CombineGuestLittleEndianWords(host.MemoryRead32(address),
                                         host.MemoryRead32(address + 4u));
}

static uint32_t ReadDirectVarArgWord(DynarmicHost& host, Dynarmic::A32::Jit& jit,
                                     uint32_t logical_word) {
    if (logical_word == 3) return jit.Regs()[3];
    if (logical_word < 4) return 0;
    return host.MemoryRead32(jit.Regs()[13] + (logical_word - 4u) * 4u);
}

static uint64_t ReadDirectVarArgWide(DynarmicHost& host, Dynarmic::A32::Jit& jit,
                                     uint32_t logical_word) {
    return CombineGuestLittleEndianWords(
        ReadDirectVarArgWord(host, jit, logical_word),
        ReadDirectVarArgWord(host, jit, logical_word + 1u));
}

// A-variant: r3 points to an array of jvalue (8 bytes each on ARM32)
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsA(Dynarmic::A32::Jit&,
                                                        const std::vector<char>& params,
                                                        uint32_t jvalue_guest) {
    std::vector<GuestArg> args;
    uint32_t p = jvalue_guest;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        switch (t) {
            case 'L':
                a.obj = ResolveObject(m_host->MemoryRead32(p));
                break;
            case 'J':
            case 'D':
                a.v = ReadGuestLittleEndian64(*m_host, p);
                break;
            case 'Z':
            case 'B':
                a.v = m_host->MemoryRead8(p);
                break;
            case 'C':
            case 'S':
                a.v = m_host->MemoryRead16(p);
                break;
            case 'F':
            case 'I':
            default:
                a.v = m_host->MemoryRead32(p);
                break;
        }
        args.push_back(a);
        p += 8;
    }
    return args;
}

// V-variant: r3 is the ARM32 va_list value. It points at the next promoted
// argument; wide values are aligned to 8 bytes within that memory walk.
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsV(Dynarmic::A32::Jit&,
                                                        const std::vector<char>& params,
                                                        uint32_t va_list_guest) {
    std::vector<GuestArg> args;
    Aapcs32VaListCursor cursor(va_list_guest);
    for (char t : params) {
        GuestArg a;
        a.type = t;
        const uint32_t location = cursor.Take(t);
        switch (t) {
            case 'L':
                a.obj = ResolveObject(m_host->MemoryRead32(location));
                break;
            case 'J':
            case 'D':
                a.v = ReadGuestLittleEndian64(*m_host, location);
                break;
            case 'F':
                a.v = PromotedDoubleToFloatBits(
                    ReadGuestLittleEndian64(*m_host, location));
                break;
            default:
                // Z/B/C/S are promoted to int; I is already one word.
                a.v = m_host->MemoryRead32(location);
                break;
        }
        args.push_back(a);
    }
    return args;
}

// Plain C vararg: logical word 3 is r3 and words >=4 are guest stack words.
std::vector<FakeJniEnv::GuestArg> FakeJniEnv::ReadArgsVar(Dynarmic::A32::Jit& jit,
                                                          const std::vector<char>& params) {
    std::vector<GuestArg> args;
    Aapcs32DirectVarArgCursor cursor;
    for (char t : params) {
        GuestArg a;
        a.type = t;
        const uint32_t location = cursor.Take(t);
        switch (t) {
            case 'L':
                a.obj = ResolveObject(ReadDirectVarArgWord(*m_host, jit, location));
                break;
            case 'J':
            case 'D':
                a.v = ReadDirectVarArgWide(*m_host, jit, location);
                break;
            case 'F':
                a.v = PromotedDoubleToFloatBits(
                    ReadDirectVarArgWide(*m_host, jit, location));
                break;
            default:
                // Z/B/C/S are promoted to int; I is already one word.
                a.v = ReadDirectVarArgWord(*m_host, jit, location);
                break;
        }
        args.push_back(a);
    }
    return args;
}

// -------------------------------------------------------------
// Host-side call marshaling
// -------------------------------------------------------------

void FakeJniEnv::CallMethod(Dynarmic::A32::Jit& jit, uint32_t mid_handle, uint32_t obj_handle,
                            char retType, const std::vector<GuestArg>& args, bool isStatic) {
    MethodInfo mi;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto mi_it = m_methods.find(mid_handle);
        if (mi_it == m_methods.end()) {
            LOGE("CallMethod: unknown method handle 0x%08X", mid_handle);
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            return;
        }
        mi = mi_it->second; // copy; host JNI call below runs without the lock
    }
    jobject obj = ResolveObject(obj_handle);
    if (!obj) {
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
        return;
    }

    std::vector<jvalue> jvs(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        jvalue& jv = jvs[i];
        const GuestArg& a = args[i];
        switch (a.type) {
            case 'Z': jv.z = static_cast<jboolean>(a.v); break;
            case 'B': jv.b = static_cast<jbyte>(a.v); break;
            case 'C': jv.c = static_cast<jchar>(a.v); break;
            case 'S': jv.s = static_cast<jshort>(a.v); break;
            case 'I': jv.i = static_cast<jint>(a.v); break;
            case 'J': jv.j = static_cast<jlong>(a.v); break;
            case 'F': { float f; memcpy(&f, &a.v, 4); jv.f = f; break; }
            case 'D': { double d; memcpy(&d, &a.v, 8); jv.d = d; break; }
            case 'L': jv.l = a.obj; break;
        }
    }

    if (retType == 'V' && !jvs.empty() &&
        mi.name == "JavaCallback_SetFixedSizeScale" && mi.sig == "(F)V" &&
        jvs[0].f < kMinimumRenderScale) {
        const float requested = jvs[0].f;
        jvs[0].f = kMinimumRenderScale;
        bool expected = false;
        if (s_logged_render_scale_clamp.compare_exchange_strong(expected, true)) {
            LOGI("Native-resolution floor: JavaCallback_SetFixedSizeScale %.3f -> %.3f",
                 requested, kMinimumRenderScale);
        }
    }

    JNIEnv* env = GetEnv();
    if (!env || env->ExceptionCheck()) {
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
        return;
    }

    const bool is_frame_swap =
        !isStatic && retType == 'Z' &&
        mi.name == "JavaCallback_swapBuffers" && mi.sig == "()Z";
    const uint64_t java_start_ns =
        HornPerfEnabled() ? HornPerfNowNs() : 0;

    switch (retType) {
        case 'V':
            if (isStatic) env->CallStaticVoidMethodA(static_cast<jclass>(obj), mi.mid, jvs.data());
            else env->CallVoidMethodA(obj, mi.mid, jvs.data());
            break;
        case 'Z': {
            jboolean r = isStatic ? env->CallStaticBooleanMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                  : env->CallBooleanMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = r ? 1 : 0;
            break;
        }
        case 'B': {
            jbyte r = isStatic ? env->CallStaticByteMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                               : env->CallByteMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(static_cast<int32_t>(r));
            break;
        }
        case 'C': {
            jchar r = isStatic ? env->CallStaticCharMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                               : env->CallCharMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r);
            break;
        }
        case 'S': {
            jshort r = isStatic ? env->CallStaticShortMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                : env->CallShortMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(static_cast<int32_t>(r));
            break;
        }
        case 'I': {
            jint r = isStatic ? env->CallStaticIntMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                              : env->CallIntMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r);
            break;
        }
        case 'J': {
            jlong r = isStatic ? env->CallStaticLongMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                               : env->CallLongMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = static_cast<uint32_t>(r & 0xFFFFFFFFu);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
            break;
        }
        case 'F': {
            jfloat r = isStatic ? env->CallStaticFloatMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                : env->CallFloatMethodA(obj, mi.mid, jvs.data());
            WriteSF(jit, 0, r);
            break;
        }
        case 'D': {
            jdouble r = isStatic ? env->CallStaticDoubleMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                 : env->CallDoubleMethodA(obj, mi.mid, jvs.data());
            WriteDF(jit, 0, r);
            break;
        }
        case 'L': {
            jobject r = isStatic ? env->CallStaticObjectMethodA(static_cast<jclass>(obj), mi.mid, jvs.data())
                                 : env->CallObjectMethodA(obj, mi.mid, jvs.data());
            jit.Regs()[0] = AddLocalObjectRef(env, r);
            break;
        }
        default:
            jit.Regs()[0] = 0;
            break;
    }

    if (java_start_ns != 0) {
        const uint64_t elapsed_ns = HornPerfNowNs() - java_start_ns;
        HornPerfRecord(is_frame_swap ? HornPerfCategory::Swap
                                     : HornPerfCategory::JavaCall,
                       elapsed_ns);
        if (!is_frame_swap && elapsed_ns >= 8'000'000) {
            LOGI("Performance: slow Java call %s%s took %.2f ms",
                 mi.name.c_str(), mi.sig.c_str(),
                 static_cast<double>(elapsed_ns) / 1'000'000.0);
        }
        if (is_frame_swap) HornPerfFrameBoundary();
    }
}

// -------------------------------------------------------------
// Direct ByteBuffer bridging
// -------------------------------------------------------------

uint32_t FakeJniEnv::RegisterDirectBuffer(jobject buf) {
    if (!buf) return 0;
    JNIEnv* env = GetEnv();
    if (!env) return 0;
    std::lock_guard<std::mutex> update_lock(m_update_mutex);
    // fmodProcess hands us the same ByteBuffer on every audio callback (~125/s).
    // A fresh entry per call burned a permanent JNI global ref and a
    // capacity-sized arena block each time. One entry per live buffer, so the
    // scan is over one or two elements.
    std::vector<std::pair<uint32_t, jobject>> candidates;
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        candidates.reserve(m_direct_bufs.size());
        for (const auto& [handle, db] : m_direct_bufs) {
            candidates.emplace_back(handle, db.jobj);
        }
    }
    for (const auto& [handle, global] : candidates) {
        if (env->IsSameObject(global, buf)) return handle;
    }
    void* p = env->GetDirectBufferAddress(buf);
    jlong cap = env->GetDirectBufferCapacity(buf);
    if (!p || cap <= 0) return 0;

    DirectBuf db;
    db.jobj = env->NewGlobalRef(buf);
    if (!db.jobj) return 0;
    db.hostPtr = p;
    db.capacity = cap;
    db.scratch = m_host->GuestAlloc(static_cast<size_t>(cap), 16);
    if (!db.scratch) {
        env->DeleteGlobalRef(db.jobj);
        return 0;
    }

    uint32_t handle = 0;
    {
        std::lock_guard<std::mutex> state_lock(m_mutex);
        handle = m_next_obj_handle;
        m_next_obj_handle += 4;
        m_direct_bufs.emplace(handle, db);
    }
    return handle;
}

void FakeJniEnv::CommitDirectBuffer(uint32_t handle) {
    DirectBuf db;
    if (!ResolveDirectBuffer(handle, db)) return;
    memcpy(db.hostPtr, m_host->GetGuestMemoryBase() + db.scratch, static_cast<size_t>(db.capacity));
}

jlong FakeJniEnv::DirectBufferCapacity(uint32_t handle) const {
    DirectBuf db;
    return ResolveDirectBuffer(handle, db) ? db.capacity : 0;
}

// -------------------------------------------------------------
// Dispatch
// -------------------------------------------------------------

// Helpers
static const char* ReadGuestStr(DynarmicHost& host, uint32_t gaddr) {
    if (!gaddr) return nullptr;
    return reinterpret_cast<const char*>(host.GetGuestMemoryBase() + gaddr);
}

static bool GuestRangeIsValid(const DynarmicHost& host, uint32_t gaddr, size_t size) {
    const uint64_t end = static_cast<uint64_t>(gaddr) + static_cast<uint64_t>(size);
    return gaddr != 0 && end <= host.GetGuestMemorySize();
}

// RegisterNatives is the one guest call where Java-provided data contains raw
// ARM32 code pointers. Read it defensively: malformed pointers must produce
// JNI_ERR, not an out-of-bounds host read. The bounded copy also keeps logging
// from walking through arbitrary guest memory when a terminator is missing.
static bool ReadGuestStringChecked(const DynarmicHost& host, uint32_t gaddr,
                                   size_t max_length, std::string& out) {
    out.clear();
    if (!GuestRangeIsValid(host, gaddr, 1)) return false;

    const size_t remaining = host.GetGuestMemorySize() - static_cast<size_t>(gaddr);
    const size_t scan_length = std::min(remaining, max_length + 1);
    const char* begin = reinterpret_cast<const char*>(host.GetGuestMemoryBase() + gaddr);
    const char* end = static_cast<const char*>(std::memchr(begin, '\0', scan_length));
    if (!end || static_cast<size_t>(end - begin) > max_length) return false;

    out.assign(begin, static_cast<size_t>(end - begin));
    return true;
}

void FakeJniEnv::DispatchVmSlot(uint32_t offset, Dynarmic::A32::Jit& jit) {
    LOGI("[JNI-VM] slot %u (off 0x%X) r1=0x%08X", offset / 4, offset, jit.Regs()[1]);
    switch (offset) {
        case 16: // AttachCurrentThread(vm, penv, args)
        case 24: // GetEnv(vm, penv, version)
        case 28: // AttachCurrentThreadAsDaemon(vm, penv, args)
        {
            uint32_t out = jit.Regs()[1];
            if (out) m_host->MemoryWrite32(out, m_env_table);
            jit.Regs()[0] = 0; // JNI_OK
            break;
        }
        case 20: // DetachCurrentThread(vm)
        {
            jit.Regs()[0] = 0; // JNI_OK
            break;
        }
        default:
            static std::atomic<bool> warned_vm[64]{};
            if (offset / 4 < 64 &&
                !warned_vm[offset / 4].exchange(true, std::memory_order_relaxed)) {
                LOGI("[JNI-VM] unhandled slot %u (off 0x%X) at PC 0x%08X", offset / 4, offset, jit.Regs()[15]);
            }
            jit.Regs()[0] = 0;
            break;
    }
}

void FakeJniEnv::DispatchEnvSlot(uint32_t offset, Dynarmic::A32::Jit& jit) {
    JNIEnv* e = GetEnv();
    uint8_t* mem = m_host->GetGuestMemoryBase();

    if (!e) {
        LOGE("[JNI] no host env attached (slot %u)", offset / 4);
        jit.Regs()[0] = 0;
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }

    // Guest-created native threads do not naturally return through a Java
    // native frame after every emulated JNI call. Bound all temporary local
    // refs produced by ResolveObject to this one dispatch instead of letting
    // them accumulate until the thread detaches.
    if (e->PushLocalFrame(64) != JNI_OK) {
        LOGE("[JNI] PushLocalFrame failed (slot %u)", offset / 4);
        jit.Regs()[0] = 0;
        jit.Regs()[1] = 0;
        jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    struct LocalFrameGuard {
        JNIEnv* env;
        ~LocalFrameGuard() { env->PopLocalFrame(nullptr); }
    } local_frame{e};

    // JNI permits only a small cleanup/exception subset while an exception is
    // pending. Gate before any handle resolution so NewLocalRef and ordinary
    // Java calls never run in that state (important with CheckJNI enabled).
    if (e->ExceptionCheck()) {
        bool exception_safe = false;
        switch (offset) {
            case 60:   // ExceptionOccurred
            case 64:   // ExceptionDescribe
            case 68:   // ExceptionClear
            case 72:   // FatalError
            case 88:   // DeleteGlobalRef
            case 92:   // DeleteLocalRef
            case 664:  // ReleaseStringChars
            case 680:  // ReleaseStringUTFChars
            case 876:  // GetJavaVM (bridge-local write only)
            case 912:  // ExceptionCheck
                exception_safe = true;
                break;
            default:
                break;
        }
        if (!exception_safe) {
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            return;
        }
    }

    switch (offset) {
        // ---- class / object basics ----
        case 24: { // FindClass
            const char* name = ReadGuestStr(*m_host, jit.Regs()[1]);
            // Preserve the immutable guest ABI after removing the vendor SDK.
            if (name && std::strcmp(name, "com/nvidia/nvsi/NvSecureInterfaceUtil") == 0) {
                name = "com/tt2kgames/Borderlands2/LocalPlatform";
            }
            jclass c = e->FindClass(name);
            if (!c) { e->ExceptionClear(); jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddLocalObjectRef(e, c);
            break;
        }
        case 40: { // GetSuperclass
            jobject o = ResolveObject(jit.Regs()[1]);
            if (!o) { jit.Regs()[0] = 0; break; }
            jclass c = e->GetSuperclass(static_cast<jclass>(o));
            jit.Regs()[0] = AddLocalObjectRef(e, c);
            break;
        }
        // Handles are shared, so dropping the host ref in DeleteGlobalRef/
        // DeleteLocalRef would invalidate any copy the guest still holds. What
        // the three calls do instead is set how willing the table is to reclaim
        // the entry - which is as close to the real lifetime as a shared handle
        // gets, and it is what keeps the table off ART's 51200 limit.
        case 84: { // NewGlobalRef -> same handle, plus a pin
            const uint32_t handle = jit.Regs()[1];
            jit.Regs()[0] = PinObjectRef(handle, +1) ? handle : 0;
            break;
        }
        case 88: // DeleteGlobalRef -> drop the pin
            PinObjectRef(jit.Regs()[1], -1);
            jit.Regs()[0] = 0;
            break;
        case 92: // DeleteLocalRef -> next in line to be evicted
            DemoteObjectRef(jit.Regs()[1]);
            jit.Regs()[0] = 0;
            break;
        case 100: { // NewLocalRef
            jobject o = ResolveObject(jit.Regs()[1]);
            jit.Regs()[0] = AddObjectRef(o);
            break;
        }
        case 124: { // GetObjectClass
            jobject o = ResolveObject(jit.Regs()[1]);
            if (!o) { jit.Regs()[0] = 0; break; } // guest expects null, ART would abort
            jclass c = e->GetObjectClass(o);
            jit.Regs()[0] = AddLocalObjectRef(e, c);
            break;
        }
        case 128: { // IsInstanceOf
            jobject o = ResolveObject(jit.Regs()[1]);
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[2]));
            if (!c) { jit.Regs()[0] = 0; break; }
            jit.Regs()[0] = e->IsInstanceOf(o, c) ? 1 : 0;
            break;
        }

        // ---- method/field IDs ----
        case 132: { // GetMethodID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c) { jit.Regs()[0] = 0; break; }
            jmethodID mid = e->GetMethodID(c, name, sig);
            if (!mid) {
                e->ExceptionClear();
                LOGE("[JNI] GetMethodID failed: %s%s (cls=0x%08X)", name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddMethod(mid, false, jit.Regs()[1], sig, name);
            }
            break;
        }
        case 376: { // GetFieldID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c) { jit.Regs()[0] = 0; break; }
            jfieldID fid = e->GetFieldID(c, name, sig);
            if (!fid) {
                e->ExceptionClear();
                LOGE("[JNI] GetFieldID failed: %s%s", name, sig);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddField(fid);
            }
            break;
        }
        case 452: { // GetStaticMethodID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (sig && std::strcmp(sig, "()Lcom/nvidia/nvsi/NvSecureInterfaceUtil;") == 0) {
                sig = "()Lcom/tt2kgames/Borderlands2/LocalPlatform;";
            }
            if (!c) { jit.Regs()[0] = 0; break; }
            jmethodID mid = e->GetStaticMethodID(c, name, sig);
            if (!mid) {
                e->ExceptionClear();
                LOGE("[JNI] GetStaticMethodID failed: %s%s (cls=0x%08X)", name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddMethod(mid, true, jit.Regs()[1], sig, name);
            }
            break;
        }
        case aarch32_jni::kGetStaticFieldID: { // GetStaticFieldID
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* name = ReadGuestStr(*m_host, jit.Regs()[2]);
            const char* sig = ReadGuestStr(*m_host, jit.Regs()[3]);
            if (!c || !name || !sig) { jit.Regs()[0] = 0; break; }
            jfieldID fid = e->GetStaticFieldID(c, name, sig);
            if (!fid) {
                e->ExceptionClear();
                LOGE("[JNI] GetStaticFieldID failed: %s%s (cls=0x%08X)",
                     name, sig, jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddField(fid);
            }
            break;
        }
        case aarch32_jni::kGetStaticObjectField: { // GetStaticObjectField
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            jfieldID fid = ResolveField(jit.Regs()[2]);
            if (!c || !fid) { jit.Regs()[0] = 0; break; }
            jobject value = e->GetStaticObjectField(c, fid);
            jit.Regs()[0] = AddLocalObjectRef(e, value);
            break;
        }

        // ---- object creation ----
        case aarch32_jni::kAllocObject: { // AllocObject
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            if (!c) { jit.Regs()[0] = 0; break; }
            jobject o = e->AllocObject(c);
            if (!o) {
                if (e->ExceptionCheck()) e->ExceptionClear();
                LOGE("[JNI] AllocObject failed (cls=0x%08X)", jit.Regs()[1]);
                jit.Regs()[0] = 0;
            } else {
                jit.Regs()[0] = AddLocalObjectRef(e, o);
            }
            break;
        }
        case aarch32_jni::kNewObject:  // NewObject (varargs)
        case aarch32_jni::kNewObjectV: // NewObjectV
        case aarch32_jni::kNewObjectA: { // NewObjectA
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            uint32_t mid_handle = jit.Regs()[2];
            MethodInfo mi;
            {
                std::lock_guard<std::mutex> lk(m_mutex);
                auto mi_it = m_methods.find(mid_handle);
                if (mi_it == m_methods.end()) {
                    LOGE("[JNI] NewObject: unknown method handle 0x%08X", mid_handle);
                    jit.Regs()[0] = 0;
                    break;
                }
                mi = mi_it->second;
            }
            if (!c) { jit.Regs()[0] = 0; break; }
            std::vector<char> params = ParseParams(mi.sig);
            std::vector<GuestArg> args =
                (offset == aarch32_jni::kNewObject) ? ReadArgsVar(jit, params) :
                (offset == aarch32_jni::kNewObjectV) ? ReadArgsV(jit, params, jit.Regs()[3]) :
                                                       ReadArgsA(jit, params, jit.Regs()[3]);
            // Build jvalues and call NewObjectA
            std::vector<jvalue> jvs(args.size());
            for (size_t i = 0; i < args.size(); ++i) {
                const GuestArg& a = args[i];
                switch (a.type) {
                    case 'Z': jvs[i].z = static_cast<jboolean>(a.v); break;
                    case 'B': jvs[i].b = static_cast<jbyte>(a.v); break;
                    case 'C': jvs[i].c = static_cast<jchar>(a.v); break;
                    case 'S': jvs[i].s = static_cast<jshort>(a.v); break;
                    case 'I': jvs[i].i = static_cast<jint>(a.v); break;
                    case 'J': jvs[i].j = static_cast<jlong>(a.v); break;
                    case 'F': { float f; memcpy(&f, &a.v, 4); jvs[i].f = f; break; }
                    case 'D': { double d; memcpy(&d, &a.v, 8); jvs[i].d = d; break; }
                    case 'L': jvs[i].l = a.obj; break;
                    default: jvs[i].i = static_cast<jint>(a.v); break;
                }
            }
            jobject o = e->NewObjectA(c, mi.mid, jvs.data());
            if (!o) { e->ExceptionClear(); LOGE("[JNI] NewObject failed"); jit.Regs()[0] = 0; }
            else jit.Regs()[0] = AddLocalObjectRef(e, o);
            break;
        }

        // ---- instance method calls (V and A variants) ----
        case aarch32_jni::kCallObjectMethod: { // CallObjectMethod
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsVar(jit, params), false);
            break;
        }
        case aarch32_jni::kCallObjectMethodV: { // CallObjectMethodV
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsV(jit, params, jit.Regs()[3]), false);
            break;
        }
        case aarch32_jni::kCallObjectMethodA: { // CallObjectMethodA
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', ReadArgsA(jit, params, jit.Regs()[3]), false);
            break;
        }
        case aarch32_jni::kCallBooleanMethod:
        case aarch32_jni::kCallBooleanMethodV:
        case aarch32_jni::kCallBooleanMethodA: { // CallBooleanMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args =
                (offset == aarch32_jni::kCallBooleanMethod) ? ReadArgsVar(jit, params) :
                (offset == aarch32_jni::kCallBooleanMethodV) ? ReadArgsV(jit, params, jit.Regs()[3]) :
                                                               ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'Z', args, false);
            break;
        }
        case 160: case 164: case 168: { // CallByteMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 160) ? ReadArgsVar(jit, params)
                                        : (offset == 164) ? ReadArgsV(jit, params, jit.Regs()[3])
                                                          : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'B', args, false);
            break;
        }
        case 172: case 176: case 180: { // CallCharMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 172) ? ReadArgsVar(jit, params)
                                        : (offset == 176) ? ReadArgsV(jit, params, jit.Regs()[3])
                                                          : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'C', args, false);
            break;
        }
        case 184: case 188: case 192: { // CallShortMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 184) ? ReadArgsVar(jit, params)
                                        : (offset == 188) ? ReadArgsV(jit, params, jit.Regs()[3])
                                                          : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'S', args, false);
            break;
        }
        case 196: case 200: case 204: { // CallIntMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 196) ? ReadArgsVar(jit, params)
                                        : (offset == 200) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'I', args, false);
            break;
        }
        case 208: case 212: case 216: { // CallLongMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 208) ? ReadArgsVar(jit, params)
                                        : (offset == 212) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'J', args, false);
            break;
        }
        case 244: case 248: case 252: { // CallVoidMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 244) ? ReadArgsVar(jit, params)
                                        : (offset == 248) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'V', args, false);
            break;
        }

        // ---- static method calls ----
        case 456: case 460: case 464: { // CallStaticObjectMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 456) ? ReadArgsVar(jit, params)
                                        : (offset == 460) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'L', args, true);
            break;
        }
        case 468: case 472: case 476: { // CallStaticBooleanMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 468) ? ReadArgsVar(jit, params)
                                        : (offset == 472) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'Z', args, true);
            break;
        }
        case 480: case 484: case 488: { // CallStaticByteMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 480) ? ReadArgsVar(jit, params)
                                        : (offset == 484) ? ReadArgsV(jit, params, jit.Regs()[3])
                                                          : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'B', args, true);
            break;
        }
        case 492: case 496: case 500: { // CallStaticCharMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 492) ? ReadArgsVar(jit, params)
                                        : (offset == 496) ? ReadArgsV(jit, params, jit.Regs()[3])
                                                          : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'C', args, true);
            break;
        }
        case 504: case 508: case 512: { // CallStaticShortMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 504) ? ReadArgsVar(jit, params)
                                        : (offset == 508) ? ReadArgsV(jit, params, jit.Regs()[3])
                                                          : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'S', args, true);
            break;
        }
        case 516: case 520: case 524: { // CallStaticIntMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 516) ? ReadArgsVar(jit, params)
                                        : (offset == 520) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'I', args, true);
            break;
        }
        case 528: case 532: case 536: { // CallStaticLongMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 528) ? ReadArgsVar(jit, params)
                                        : (offset == 532) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'J', args, true);
            break;
        }
        case 564: case 568: case 572: { // CallStaticVoidMethod / V / A
            std::vector<char> params = ParamsForMethod(jit.Regs()[2]);
            std::vector<GuestArg> args = (offset == 564) ? ReadArgsVar(jit, params)
                                        : (offset == 568) ? ReadArgsV(jit, params, jit.Regs()[3])
                                        : ReadArgsA(jit, params, jit.Regs()[3]);
            CallMethod(jit, jit.Regs()[2], jit.Regs()[1], 'V', args, true);
            break;
        }

        // ---- field access ----
        case 400: { // GetIntField
            jobject o = ResolveObject(jit.Regs()[1]);
            jfieldID fid = ResolveField(jit.Regs()[2]);
            if (!o || !fid) { jit.Regs()[0] = 0; break; }
            jit.Regs()[0] = static_cast<uint32_t>(e->GetIntField(o, fid));
            break;
        }
        case 380: { // GetObjectField
            jobject o = ResolveObject(jit.Regs()[1]);
            jfieldID fid = ResolveField(jit.Regs()[2]);
            if (!o || !fid) { jit.Regs()[0] = 0; break; }
            jobject r = e->GetObjectField(o, fid);
            jit.Regs()[0] = AddLocalObjectRef(e, r);
            break;
        }
        case aarch32_jni::kGetStaticIntField: { // GetStaticIntField
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            jfieldID fid = ResolveField(jit.Regs()[2]);
            if (!c || !fid) { jit.Regs()[0] = 0; break; }
            jit.Regs()[0] = static_cast<uint32_t>(e->GetStaticIntField(c, fid));
            break;
        }

        // ---- arrays ----
        case aarch32_jni::kGetArrayLength: { // GetArrayLength
            jarray array = static_cast<jarray>(ResolveObject(jit.Regs()[1]));
            jit.Regs()[0] = array ? static_cast<uint32_t>(e->GetArrayLength(array)) : 0;
            break;
        }
        case aarch32_jni::kGetObjectArrayElement: { // GetObjectArrayElement
            jobjectArray array = static_cast<jobjectArray>(ResolveObject(jit.Regs()[1]));
            if (!array) { jit.Regs()[0] = 0; break; }
            jobject value = e->GetObjectArrayElement(array, static_cast<jsize>(jit.Regs()[2]));
            jit.Regs()[0] = AddLocalObjectRef(e, value);
            break;
        }
        case aarch32_jni::kNewByteArray: { // NewByteArray
            const jsize length = static_cast<jsize>(jit.Regs()[1]);
            jbyteArray array = e->NewByteArray(length);
            jit.Regs()[0] = AddLocalObjectRef(e, array);
            break;
        }
        case aarch32_jni::kNewLongArray: { // NewLongArray (save timestamp)
            const jsize length = static_cast<jsize>(jit.Regs()[1]);
            jlongArray array = e->NewLongArray(length);
            jit.Regs()[0] = AddLocalObjectRef(e, array);
            break;
        }
        case aarch32_jni::kGetByteArrayElements:
        case aarch32_jni::kGetLongArrayElements: {
            const bool longs = offset == aarch32_jni::kGetLongArrayElements;
            const uint32_t array_handle = jit.Regs()[1];
            if (jit.Regs()[2] &&
                !GuestRangeIsValid(*m_host, jit.Regs()[2], 1)) {
                LOGE("[JNI] GetArrayElements invalid isCopy address 0x%08X",
                     jit.Regs()[2]);
                jit.Regs()[0] = 0;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
                break;
            }
            jarray array = static_cast<jarray>(ResolveObject(array_handle));
            if (!array) { jit.Regs()[0] = 0; break; }
            const jsize length = e->GetArrayLength(array);
            const size_t bytes = static_cast<size_t>(length) *
                (longs ? sizeof(jlong) : sizeof(jbyte));
            if (bytes > UINT32_MAX) { jit.Regs()[0] = 0; break; }
            const uint32_t guest = m_host->GuestAlloc(std::max<size_t>(bytes, 1),
                                                    longs ? alignof(jlong) : 1);
            if (!guest) { jit.Regs()[0] = 0; break; }
            if (length) {
                if (longs) {
                    e->GetLongArrayRegion(static_cast<jlongArray>(array), 0,
                                          length, reinterpret_cast<jlong*>(mem + guest));
                } else {
                    e->GetByteArrayRegion(static_cast<jbyteArray>(array), 0,
                                          length, reinterpret_cast<jbyte*>(mem + guest));
                }
            }
            if (e->ExceptionCheck()) {
                m_host->GuestFree(guest);
                jit.Regs()[0] = 0;
                break;
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_primitive_array_copies.emplace(guest, PrimitiveArrayCopy{
                    array_handle, bytes, longs ? 'J' : 'B'});
            }
            if (jit.Regs()[2]) {
                m_host->MemoryWrite8(jit.Regs()[2], JNI_TRUE);
            }
            jit.Regs()[0] = guest;
            break;
        }
        case aarch32_jni::kReleaseByteArrayElements:
        case aarch32_jni::kReleaseLongArrayElements: {
            const bool longs = offset == aarch32_jni::kReleaseLongArrayElements;
            const uint32_t array_handle = jit.Regs()[1];
            const uint32_t guest = jit.Regs()[2];
            const jint mode = static_cast<jint>(jit.Regs()[3]);
            PrimitiveArrayCopy copy;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto it = m_primitive_array_copies.find(guest);
                if (it != m_primitive_array_copies.end() &&
                    it->second.array_handle == array_handle &&
                    it->second.type == (longs ? 'J' : 'B')) {
                    copy = it->second;
                    found = true;
                    if (mode != JNI_COMMIT) m_primitive_array_copies.erase(it);
                }
            }
            if (!found || !GuestRangeIsValid(*m_host, guest,
                                             std::max<size_t>(copy.bytes, 1))) {
                LOGE("[JNI] ReleaseArrayElements invalid copy 0x%08X", guest);
                jit.Regs()[0] = 0;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
                break;
            }
            if (mode != JNI_ABORT && copy.bytes) {
                jarray array = static_cast<jarray>(ResolveObject(array_handle));
                if (array) {
                    if (longs) {
                        e->SetLongArrayRegion(static_cast<jlongArray>(array), 0,
                            static_cast<jsize>(copy.bytes / sizeof(jlong)),
                            reinterpret_cast<const jlong*>(mem + guest));
                    } else {
                        e->SetByteArrayRegion(static_cast<jbyteArray>(array), 0,
                            static_cast<jsize>(copy.bytes),
                            reinterpret_cast<const jbyte*>(mem + guest));
                    }
                }
            }
            if (mode != JNI_COMMIT) m_host->GuestFree(guest);
            jit.Regs()[0] = 0;
            break;
        }
        case aarch32_jni::kGetByteArrayRegion:
        case aarch32_jni::kGetLongArrayRegion:
        case aarch32_jni::kSetLongArrayRegion: {
            const bool longs = offset != aarch32_jni::kGetByteArrayRegion;
            const bool write_array = offset == aarch32_jni::kSetLongArrayRegion;
            const jsize start = static_cast<jsize>(jit.Regs()[2]);
            const jsize length = static_cast<jsize>(jit.Regs()[3]);
            const uint32_t stack = jit.Regs()[13];
            if (!GuestRangeIsValid(*m_host, stack, sizeof(uint32_t))) {
                LOGE("[JNI] ArrayRegion invalid guest stack 0x%08X", stack);
                jit.Regs()[0] = 0;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
                break;
            }
            const uint32_t guest = m_host->MemoryRead32(stack);
            const size_t bytes = length > 0 ? static_cast<size_t>(length) *
                (longs ? sizeof(jlong) : sizeof(jbyte)) : 0;
            if (bytes && !GuestRangeIsValid(*m_host, guest, bytes)) {
                LOGE("[JNI] ArrayRegion invalid guest buffer 0x%08X length=%d",
                     guest, length);
                jit.Regs()[0] = 0;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
                break;
            }
            jarray array = static_cast<jarray>(ResolveObject(jit.Regs()[1]));
            if (array) {
                jlong empty_long = 0;
                jbyte empty_byte = 0;
                if (longs) {
                    jlong* data = bytes ? reinterpret_cast<jlong*>(mem + guest) : &empty_long;
                    if (write_array) {
                        e->SetLongArrayRegion(static_cast<jlongArray>(array),
                                              start, length, data);
                    } else {
                        e->GetLongArrayRegion(static_cast<jlongArray>(array),
                                              start, length, data);
                    }
                } else {
                    jbyte* data = bytes ? reinterpret_cast<jbyte*>(mem + guest) : &empty_byte;
                    e->GetByteArrayRegion(static_cast<jbyteArray>(array),
                                          start, length, data);
                }
            }
            jit.Regs()[0] = 0;
            break;
        }
        case aarch32_jni::kSetByteArrayRegion: { // SetByteArrayRegion
            const uint32_t array_handle = jit.Regs()[1];
            const jsize start = static_cast<jsize>(jit.Regs()[2]);
            const jsize length = static_cast<jsize>(jit.Regs()[3]);
            const uint32_t stack = jit.Regs()[13];
            if (!GuestRangeIsValid(*m_host, stack, sizeof(uint32_t))) {
                LOGE("[JNI] SetByteArrayRegion invalid guest stack 0x%08X", stack);
                jit.Regs()[0] = 0;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
                break;
            }
            const uint32_t source = m_host->MemoryRead32(stack);
            if (length > 0 &&
                !GuestRangeIsValid(*m_host, source, static_cast<size_t>(length))) {
                LOGE("[JNI] SetByteArrayRegion invalid source 0x%08X length=%d",
                     source, length);
                jit.Regs()[0] = 0;
                jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
                break;
            }
            jbyteArray array = static_cast<jbyteArray>(ResolveObject(array_handle));
            if (array) {
                e->SetByteArrayRegion(array, start, length,
                    length > 0 ? reinterpret_cast<const jbyte*>(mem + source) : nullptr);
            }
            jit.Regs()[0] = 0;
            break;
        }

        // ---- strings ----
        case 668: { // NewStringUTF
            const char* s = ReadGuestStr(*m_host, jit.Regs()[1]);
            jstring js = e->NewStringUTF(s ? s : "");
            jit.Regs()[0] = AddLocalObjectRef(e, js);
            break;
        }
        case 656: { // GetStringLength
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            jit.Regs()[0] = s ? static_cast<uint32_t>(e->GetStringLength(s)) : 0;
            break;
        }
        case 660: { // GetStringChars (UTF-16)
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            const uint32_t is_copy_out = jit.Regs()[2];
            if (!s) { jit.Regs()[0] = 0; break; }
            const jchar* chars = e->GetStringChars(s, nullptr);
            if (!chars) { jit.Regs()[0] = 0; break; }
            const jsize java_length = e->GetStringLength(s);
            const size_t length = static_cast<size_t>(java_length);
            if (length > (std::numeric_limits<size_t>::max() / sizeof(jchar)) - 1) {
                e->ReleaseStringChars(s, chars);
                jit.Regs()[0] = 0;
                break;
            }
            const size_t bytes = (length + 1) * sizeof(jchar);
            uint32_t guest = m_host->GuestAlloc(bytes, 4);
            if (!guest) {
                e->ReleaseStringChars(s, chars);
                jit.Regs()[0] = 0;
                break;
            }
            memcpy(mem + guest, chars, length * sizeof(jchar));
            memset(mem + guest + length * sizeof(jchar), 0, sizeof(jchar));
            e->ReleaseStringChars(s, chars);
            if (is_copy_out) m_host->MemoryWrite8(is_copy_out, JNI_TRUE);
            jit.Regs()[0] = guest;
            break;
        }
        case 664: // ReleaseStringChars -> the copy is ours, and this is the only
                  // call that says when it stops being needed
            m_host->GuestFree(jit.Regs()[2]);
            jit.Regs()[0] = 0;
            break;
        case aarch32_jni::kGetStringUTFLength: { // GetStringUTFLength
            jstring s = static_cast<jstring>(ResolveObject(jit.Regs()[1]));
            jit.Regs()[0] = s
                ? static_cast<uint32_t>(e->GetStringUTFLength(s))
                : 0;
            break;
        }
        case 676: { // GetStringUTFChars
            const uint32_t string_handle = jit.Regs()[1];
            if (!AcquireUtfLease(string_handle)) {
                jit.Regs()[0] = 0;
                break;
            }
            jstring s = static_cast<jstring>(ResolveObject(string_handle));
            uint32_t isCopyOut = jit.Regs()[2];
            if (!s) {
                CancelUtfLease(string_handle);
                jit.Regs()[0] = 0;
                break;
            }
            uint32_t cached_copy = 0;
            {
                std::lock_guard<std::mutex> state_lock(m_mutex);
                auto cached = m_jstr_utf.find(string_handle);
                if (cached != m_jstr_utf.end()) cached_copy = cached->second;
            }
            if (cached_copy) {
                if (isCopyOut) m_host->MemoryWrite8(isCopyOut, JNI_TRUE);
                jit.Regs()[0] = cached_copy;
                break;
            }
            const char* utf = e->GetStringUTFChars(s, nullptr);
            if (!utf) {
                CancelUtfLease(string_handle);
                jit.Regs()[0] = 0;
                break;
            }
            size_t len = strlen(utf) + 1;
            uint32_t guest = m_host->GuestAlloc(len, 4);
            if (guest) memcpy(mem + guest, utf, len);
            e->ReleaseStringUTFChars(s, utf);
            if (!guest) {
                CancelUtfLease(string_handle);
                jit.Regs()[0] = 0;
                break;
            }

            uint32_t published = guest;
            bool inserted = false;
            {
                std::lock_guard<std::mutex> state_lock(m_mutex);
                auto result = m_jstr_utf.emplace(string_handle, guest);
                published = result.first->second;
                inserted = result.second;
            }
            if (!inserted) m_host->GuestFree(guest);
            if (isCopyOut) m_host->MemoryWrite8(isCopyOut, JNI_TRUE);
            jit.Regs()[0] = published;
            break;
        }
        case 680: // ReleaseStringUTFChars: keep the cached copy for reuse, then
                  // make its handle evictable after the final lease ends
            ReleaseUtfLease(jit.Regs()[1], jit.Regs()[2]);
            jit.Regs()[0] = 0;
            break;

        // ---- exceptions ----
        case 60: { // ExceptionOccurred
            jthrowable t = e->ExceptionOccurred();
            if (!t) { jit.Regs()[0] = 0; break; }
            // AddObjectRef performs ordinary JNI calls to intern the object;
            // those are not legal with a pending exception. Preserve the exact
            // throwable across interning and restore ART's pending state.
            e->ExceptionClear();
            jit.Regs()[0] = AddObjectRef(t);
            if (e->ExceptionCheck()) e->ExceptionClear();
            e->Throw(t);
            e->DeleteLocalRef(t);
            break;
        }
        case 64: // ExceptionDescribe
            e->ExceptionDescribe();
            jit.Regs()[0] = 0;
            break;
        case 68: // ExceptionClear
            e->ExceptionClear();
            jit.Regs()[0] = 0;
            break;
        case 912: { // ExceptionCheck
            jit.Regs()[0] = e->ExceptionCheck() ? 1 : 0;
            break;
        }
        case 56: { // ThrowNew
            jclass c = static_cast<jclass>(ResolveObject(jit.Regs()[1]));
            const char* msg = ReadGuestStr(*m_host, jit.Regs()[2]);
            if (c) e->ThrowNew(c, msg);
            jit.Regs()[0] = 0;
            break;
        }
        case 72: { // FatalError
            const char* msg = ReadGuestStr(*m_host, jit.Regs()[1]);
            LOGE("[JNI] FatalError: %s", msg ? msg : "");
            jit.Regs()[0] = 0;
            break;
        }

        // The guest's JNI_OnLoad registers ARM32 function pointers. ART cannot
        // execute or even safely retain those pointers in this ARM64 process;
        // the host bridge registers its ARM64 wrappers separately. Validate and
        // log the guest table so JNI_OnLoad observes the expected JNI_OK without
        // ever forwarding the raw table to JNIEnv::RegisterNatives.
        case aarch32_jni::kRegisterNatives: { // RegisterNatives
            constexpr uint32_t kGuestNativeMethodSize = 12; // 3 x ARM32 pointer
            constexpr int32_t kMaxGuestNativeMethods = 1024;
            constexpr size_t kMaxMethodNameLength = 512;
            constexpr size_t kMaxMethodSignatureLength = 2048;

            const uint32_t class_handle = jit.Regs()[1];
            const uint32_t table = jit.Regs()[2];
            const int32_t count = static_cast<int32_t>(jit.Regs()[3]);
            if (!ResolveObject(class_handle)) {
                LOGE("[JNI] guest RegisterNatives rejected: invalid class handle 0x%08X",
                     class_handle);
                jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
                break;
            }
            if (count < 0 || count > kMaxGuestNativeMethods) {
                LOGE("[JNI] guest RegisterNatives rejected: invalid count %d", count);
                jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
                break;
            }
            if (count == 0) {
                LOGI("[JNI] guest RegisterNatives: class=0x%08X, empty table accepted",
                     class_handle);
                jit.Regs()[0] = JNI_OK;
                break;
            }

            const size_t table_size = static_cast<size_t>(count) * kGuestNativeMethodSize;
            if ((table & 3u) != 0 || !GuestRangeIsValid(*m_host, table, table_size)) {
                LOGE("[JNI] guest RegisterNatives rejected: table=0x%08X count=%d is outside guest memory",
                     table, count);
                jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
                break;
            }

            bool valid = true;
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t entry = table + static_cast<uint32_t>(i) * kGuestNativeMethodSize;
                const uint32_t name_ptr = m_host->MemoryRead32(entry);
                const uint32_t sig_ptr = m_host->MemoryRead32(entry + 4);
                const uint32_t fn_ptr = m_host->MemoryRead32(entry + 8);
                const uint32_t code_ptr = fn_ptr & ~1u; // permit a Thumb-state bit
                std::string name;
                std::string sig;

                const bool strings_ok =
                    ReadGuestStringChecked(*m_host, name_ptr, kMaxMethodNameLength, name) &&
                    ReadGuestStringChecked(*m_host, sig_ptr, kMaxMethodSignatureLength, sig);
                const size_t close_paren = strings_ok ? sig.find(')') : std::string::npos;
                const bool descriptor_ok = strings_ok && !name.empty() && sig.size() >= 3 &&
                                           sig.front() == '(' && close_paren != std::string::npos &&
                                           close_paren + 1 < sig.size();
                const bool function_ok = fn_ptr != 0 && GuestRangeIsValid(*m_host, code_ptr, 2);
                if (!descriptor_ok || !function_ok) {
                    LOGE("[JNI] guest RegisterNatives rejected entry %d: name=0x%08X sig=0x%08X fn=0x%08X",
                         i, name_ptr, sig_ptr, fn_ptr);
                    valid = false;
                    break;
                }

                LOGI("[JNI] guest native[%d] %s %s -> ARM32 0x%08X",
                     i, name.c_str(), sig.c_str(), fn_ptr);
            }

            if (valid) {
                LOGI("[JNI] guest RegisterNatives validated %d entries on class handle 0x%08X; ART registration intentionally skipped",
                     count, class_handle);
                jit.Regs()[0] = JNI_OK;
            } else {
                jit.Regs()[0] = static_cast<uint32_t>(JNI_ERR);
            }
            break;
        }

        // ---- direct buffers ----
        case 920: { // GetDirectBufferAddress
            uint32_t handle = jit.Regs()[1];
            DirectBuf known;
            if (ResolveDirectBuffer(handle, known)) {
                jit.Regs()[0] = known.scratch;
                break;
            }
            jobject buf = ResolveObject(handle);
            if (!buf) { jit.Regs()[0] = 0; break; }
            void* p = e->GetDirectBufferAddress(buf);
            if (!p) { jit.Regs()[0] = 0; break; }
            // Unknown buffer: mirror host content into guest scratch (read-only view)
            uint32_t newHandle = RegisterDirectBuffer(buf);
            DirectBuf registered;
            jit.Regs()[0] = newHandle && ResolveDirectBuffer(newHandle, registered)
                                ? registered.scratch : 0;
            break;
        }
        case 924: { // GetDirectBufferCapacity
            uint32_t handle = jit.Regs()[1];
            DirectBuf known;
            if (ResolveDirectBuffer(handle, known)) {
                jit.Regs()[0] = static_cast<uint32_t>(known.capacity);
                break;
            }
            jobject buf = ResolveObject(handle);
            jit.Regs()[0] = buf ? static_cast<uint32_t>(e->GetDirectBufferCapacity(buf)) : 0;
            break;
        }

        // ---- misc ----
        case 876: { // GetJavaVM
            uint32_t out = jit.Regs()[1];
            if (out) m_host->MemoryWrite32(out, m_vm_table);
            jit.Regs()[0] = 0;
            break;
        }
        case 96: { // IsSameObject
            jobject a = ResolveObject(jit.Regs()[1]);
            jobject b = ResolveObject(jit.Regs()[2]);
            jit.Regs()[0] = e->IsSameObject(a, b) ? 1 : 0;
            break;
        }
        case 52: { // Throw
            jobject t = ResolveObject(jit.Regs()[1]);
            if (t) e->Throw(static_cast<jthrowable>(t));
            jit.Regs()[0] = 0;
            break;
        }
        case aarch32_jni::kSetObjectField: { // SetObjectField
            jobject o = ResolveObject(jit.Regs()[1]);
            jfieldID fid = ResolveField(jit.Regs()[2]);
            if (o && fid) {
                e->SetObjectField(o, fid, ResolveObject(jit.Regs()[3]));
            }
            jit.Regs()[0] = 0;
            break;
        }
        case aarch32_jni::kSetIntField: { // SetIntField
            jobject o = ResolveObject(jit.Regs()[1]);
            jfieldID fid = ResolveField(jit.Regs()[2]);
            if (o && fid) {
                e->SetIntField(o, fid, static_cast<jint>(jit.Regs()[3]));
            }
            jit.Regs()[0] = 0;
            break;
        }
        case aarch32_jni::kGetLongField: { // GetLongField
            jobject o = ResolveObject(jit.Regs()[1]);
            jfieldID fid = ResolveField(jit.Regs()[2]);
            jlong v = (o && fid) ? e->GetLongField(o, fid) : 0;
            jit.Regs()[0] = static_cast<uint32_t>(v & 0xFFFFFFFFu);
            jit.Regs()[1] = static_cast<uint32_t>(static_cast<uint64_t>(v) >> 32);
            break;
        }

        default: {
            static std::atomic<bool> warned[240]{};
            uint32_t slot = offset / 4;
            if (slot < 240 &&
                !warned[slot].exchange(true, std::memory_order_relaxed)) {
                LOGE("[JNI] unhandled env slot %u (off 0x%X) at PC 0x%08X, r1=0x%08X r2=0x%08X r3=0x%08X",
                     slot, offset, jit.Regs()[15], jit.Regs()[1], jit.Regs()[2], jit.Regs()[3]);
            }
            jit.Regs()[0] = 0;
            jit.Regs()[1] = 0;
            jit.HaltExecution(Dynarmic::HaltReason::UserDefined1);
            break;
        }
    }
}
