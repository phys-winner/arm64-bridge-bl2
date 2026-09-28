#pragma once

#include <jni.h>
#include <cstdint>
#include <list>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace Dynarmic::A32 { class Jit; }
class DynarmicHost;

// Fake JNIEnv/JavaVM tables built in guest memory. Guest 32-bit code calls
// env->XXX via its fixed JNI function-table offsets; each slot is an SVC
// trampoline dispatched here. Opaque jobject/jmethodID/jfieldID values passed
// to/from the guest are 32-bit handles backed by a host-side table.
class FakeJniEnv {
public:
    static FakeJniEnv& Instance();

    // Must be called after DynarmicHost::Initialize() (needs guest memory + thunk manager)
    void Init(DynarmicHost& host);
    // Host JNIEnv used to call the real 64-bit JVM (set on first JNI entry)
    void AttachHostEnv(JNIEnv* env);
    void SetJavaVM(JavaVM* vm) { m_vm = vm; }
    // Per-host-thread JNIEnv (AttachCurrentThread for guest worker threads)
    JNIEnv* GetEnv() const;

    uint32_t EnvTableGuestAddr() const { return m_env_table; }
    uint32_t VmTableGuestAddr() const { return m_vm_table; }

    // ---- handles ----
    // One handle per *object*, not per call: the same jobject arriving twice gets
    // the same handle back. Without that, a guest that calls into Java in a loop
    // walks into ART's hard limit of 51200 global references and the process is
    // killed at a reproducible number of seconds. Interning alone is not enough
    // either - a guest that produces a *new* object every time (NewStringUTF,
    // GetObjectField, an event object) has nothing to intern, so the table is
    // capped and evicts. See PinObjectRef/DemoteObjectRef for what decides the
    // eviction order.
    uint32_t AddObjectRef(jobject o);              // NewGlobalRef + handle
    // Returns a caller-owned local ref, stable even if another guest thread
    // evicts the backing global ref immediately afterwards.
    jobject  ResolveObject(uint32_t handle) const;
    uint32_t AddMethod(jmethodID mid, bool isStatic, uint32_t clsHandle, const std::string& sig,
                       const std::string& name = {});
    uint32_t AddField(jfieldID fid);

    // The guest's own NewGlobalRef/DeleteGlobalRef, counted: an entry the guest
    // says it is keeping is never evicted. delta is +1 or -1.
    bool PinObjectRef(uint32_t handle, int delta);
    // The guest's DeleteLocalRef: not a free (the handle is shared), but it does
    // make this entry the next one reclaimed.
    void DemoteObjectRef(uint32_t handle);

    // ---- dispatch (called from thunk handlers) ----
    void DispatchEnvSlot(uint32_t offset, Dynarmic::A32::Jit& jit);
    void DispatchVmSlot(uint32_t offset, Dynarmic::A32::Jit& jit);

    // ---- direct ByteBuffer bridging (FMOD fmodProcess) ----
    // Registers a Java direct ByteBuffer, mirrors its content into guest
    // scratch memory and returns the guest handle.
    uint32_t RegisterDirectBuffer(jobject buf);
    // Copies guest scratch back into the host direct buffer.
    void CommitDirectBuffer(uint32_t handle);
    jlong DirectBufferCapacity(uint32_t handle) const;

private:
    struct MethodInfo {
        jmethodID mid = nullptr;
        bool isStatic = false;
        uint32_t clsHandle = 0;
        std::string sig;
        std::string name;                  // kept so named-call hooks can match
    };

    struct ObjRef {
        jobject global = nullptr;
        int32_t hash = 0;                  // System.identityHashCode, the lookup key
        int pins = 0;                      // guest NewGlobalRef minus DeleteGlobalRef
        uint32_t utf_leases = 0;           // outstanding GetStringUTFChars calls
        std::list<uint32_t>::iterator lru; // linked only while there are no holds
    };

    struct DirectBuf {
        jobject jobj = nullptr;   // global ref
        void* hostPtr = nullptr;
        jlong capacity = 0;
        uint32_t scratch = 0;     // guest addr
    };

    struct PrimitiveArrayCopy {
        uint32_t array_handle = 0;
        size_t bytes = 0;
        char type = 0;
    };

    struct EvictedRef {
        jobject global = nullptr;
        uint32_t utf_copy = 0;
    };

    struct GuestArg {
        char type = 0;      // Z B C S I J F D L
        uint64_t v = 0;
        jobject obj = nullptr;
    };

    JavaVM* m_vm = nullptr;
    DynarmicHost* m_host = nullptr;
    // Serializes composite object/direct-buffer publication. Host JNI and guest
    // allocator calls may run while this is held, but never while m_mutex is held.
    mutable std::mutex m_update_mutex;
    mutable std::mutex m_mutex;
    uint32_t m_env_table = 0;
    uint32_t m_vm_table = 0;

    uint32_t m_next_obj_handle = 0x00020000;
    uint32_t m_next_mid_handle = 0x00030000;
    uint32_t m_next_fid_handle = 0x00040000;

    std::unordered_map<uint32_t, ObjRef> m_objects;
    // identityHashCode -> handles with that hash. Usually one, so a lookup is a
    // single IsSameObject call.
    std::unordered_map<int32_t, std::vector<uint32_t>> m_objects_by_hash;
    // Least-recently-used last. Mutable because resolving a handle counts as
    // using it, and ResolveObject is const.
    mutable std::list<uint32_t> m_lru;
    // One synthetic id per jmethodID/jfieldID. Reflection-heavy guests look the
    // same method up on every call, and without this both tables - and the
    // signature string in each entry - grow for the life of the process.
    std::unordered_map<jmethodID, uint32_t> m_method_ids;
    std::unordered_map<jfieldID, uint32_t> m_field_ids;
    std::unordered_map<uint32_t, MethodInfo> m_methods;
    std::unordered_map<uint32_t, jfieldID> m_fields;
    std::unordered_map<uint32_t, DirectBuf> m_direct_bufs;
    std::unordered_map<uint32_t, PrimitiveArrayCopy> m_primitive_array_copies;
    std::unordered_map<uint32_t, uint32_t> m_jstr_utf; // jstring handle -> guest copy

    // Held by callers that already own m_mutex.
    void TouchRef(uint32_t handle) const;
    EvictedRef EvictOneRefLocked();
    int32_t IdentityHash(JNIEnv* env, jobject o);
    uint32_t AddLocalObjectRef(JNIEnv* env, jobject local);
    bool AcquireUtfLease(uint32_t handle);
    void CancelUtfLease(uint32_t handle);
    void ReleaseUtfLease(uint32_t handle, uint32_t guest_copy);
    std::vector<char> ParamsForMethod(uint32_t handle) const;
    jfieldID ResolveField(uint32_t handle) const;
    bool ResolveDirectBuffer(uint32_t handle, DirectBuf& out) const;

    std::vector<char> ParseParams(const std::string& sig) const;
    std::vector<GuestArg> ReadArgsA(Dynarmic::A32::Jit& jit, const std::vector<char>& params, uint32_t jvalue_guest);
    std::vector<GuestArg> ReadArgsV(Dynarmic::A32::Jit& jit, const std::vector<char>& params, uint32_t va_list_guest);
    std::vector<GuestArg> ReadArgsVar(Dynarmic::A32::Jit& jit, const std::vector<char>& params);

    void CallMethod(Dynarmic::A32::Jit& jit, uint32_t mid_handle, uint32_t obj_handle,
                    char retType, const std::vector<GuestArg>& args, bool isStatic);
};
