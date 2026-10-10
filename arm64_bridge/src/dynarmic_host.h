#pragma once

#if defined(BL2_NATIVE_ARM32)
#include "../../arm32_compat/src/native_host.h"
#else

#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>
#include <string>
#include <optional>
#include <mutex>
#include <atomic>
#include <thread>
#include <map>
#include <unordered_map>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include "bl2_jit_warmup.h"
#include "bl2_jit_learning.h"
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
#include "bl2_physx_cookie_watch.h"
#endif

class ElfLoader;
class ThunkManager;

class DynarmicHost : public Dynarmic::A32::UserCallbacks {
public:
    // The full 32-bit guest address space. Mapping all of it means fastmem can resolve any
    // guest address as base + zext32(vaddr) without ever faulting.
    static constexpr size_t GUEST_ADDRESS_SPACE = 0x100000000ULL; // 4 GiB
    // Fallback if the 4 GiB reservation is refused: no fastmem, bounds-checked callbacks.
    static constexpr size_t GUEST_FALLBACK_SIZE = 0x80000000ULL;  // 2 GiB

    DynarmicHost();
    ~DynarmicHost() override;

    bool Initialize();
    bool LoadGame(const std::string& elf_path);
    bool LoadGameFromMemory(const uint8_t* data, size_t size);
    // Install only after the JNI loader verified the pinned original guest.
    bool InstallJitWarmupTable(std::span<const uint8_t> bytes);
    // Learned warmup (bl2_jit_learning): blocks compiled by the warmed roles
    // are persisted in `directory`; the next run precompiles them at entry
    // (bounded) and in later idle callbacks. debug.bl2.jit_learn=0 disables.
    void LoadLearnedJitProfile(const std::string& directory);
    // Asks the background saver for a write now; never blocks the caller.
    void RequestLearnedJitSave();
    // From a host callback of `jit` on its own thread: spend up to budget_ns
    // translating learned blocks of that core's role. No-op for other cores.
    void WarmLearnedBlocksInCallback(Dynarmic::A32::Jit& jit, uint64_t budget_ns);
    void RecordLearnedBlock(size_t role, uint64_t descriptor);
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    // JNI calls this only after the pinned original guest passed verification.
    bool InstallPhysxCookieWatch(bool verified_original);
    static Bl2PhysxCookieWatch::NativeToken BeginPhysxNativeWrite(uint32_t address, size_t bytes);
    static void EndPhysxNativeWrite(const Bl2PhysxCookieWatch::NativeToken& token);
#endif

    bool LoadLibraries(const std::string& fmodex_path, const std::string& fmodevent_path, const std::string& game_path);
    bool LoadLibrariesFromMemory(const uint8_t* fmodex_data, size_t fmodex_size,
                                 const uint8_t* fmodevent_data, size_t fmodevent_size,
                                 const uint8_t* game_data, size_t game_size);

    uint32_t RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    // ARM32 64-bit scalar returns use R0:R1. This is required by retained JNI
    // methods that return jlong (ZipResourceFile in Horn).
    uint64_t RunFunction64(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    // Synchronous guest callback invoked while the primary JIT is paused in a
    // host thunk (qsort comparator, API callback). Uses a second JIT and stack;
    // unlike RunFunction it must never silently defer the result.
    uint32_t RunGuestCallback(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    uint64_t RunGuestCallback64(uint32_t target_pc, const std::vector<uint32_t>& args = {});
    // Checked synchronous UI call at an owned JIT thunk boundary. Never defers
    // a required result or replaces an interrupted architectural context.
    bool CallGuest(uint32_t target_pc, const std::vector<uint32_t>& args, uint32_t& result);
    // Runs calls that were made from inside a thunk (i.e. from inside Jit::Run, where the JIT
    // is not re-entrant). Call once per frame from a point known to be outside the JIT.
    void DrainDeferred();

    uint8_t* GetGuestMemoryBase() const { return m_memory; }
    size_t GetGuestMemorySize() const { return m_memory_size; }
    ElfLoader& GetElfLoader() { return *m_elf_loader; }
    ElfLoader* GetFmodExLoader() { return m_fmodex_loader.get(); }
    ElfLoader* GetFmodEventLoader() { return m_fmodevent_loader.get(); }
    ThunkManager& GetThunkManager() { return *m_thunk_manager; }
    // Serializes entries only within the calling host thread. Different guest
    // pthreads need independent A32 cores; a process-wide entry mutex deadlocks
    // Horn because NativeCallback_Initialize starts the long-lived UE3GameThread.
    std::recursive_mutex& GetMutex();
    // Reserve/release the calling host thread's A32 cores. pthread_create uses
    // the attach handshake before it reports success; short-lived bridge-owned
    // workers release their lease so processor IDs and stacks can be reused.
    bool AttachCurrentThreadContext(uint32_t primary_stack_size = 0);
    void ReleaseCurrentThreadContext();
    // pthread_exit is thread-scoped, not nesting-scoped: stop both the primary
    // and callback cores so a nested callback cannot return to guest code.
    void HaltCurrentThreadExecution(Dynarmic::HaltReason reason);
    uint32_t FindSymbol(const std::string& name);
    // Drop any cached translation of a guest range that was just rewritten.
    void InvalidateGuestCode(uint32_t addr, size_t size);
    uint32_t GuestAlloc(size_t size, size_t alignment = 8);
    void GuestFree(uint32_t addr);
    // Payload size of a live GuestAlloc block, 0 if addr did not come from here.
    uint32_t GuestAllocSize(uint32_t addr);
    // BL2 only: blocks above the largest size class come from their own range
    // at the top of the guest space, [kLargeBase, kLargeLimit), whose free
    // spans are address-ordered and coalesce like bionic's mmap-backed large
    // allocations. The shared heap kept freed level-load buffers in exact-size
    // and power-of-two lists: 533 MiB free yet no 55 MiB span, so a shader
    // cache malloc returned NULL. File mappings then stop at kLargeBase.
    // Returns false (and leaves the shared heap in charge) on a failed check.
    bool EnableLargeBlockRegion();
    // Anonymous RW guest heap (shared heap or large range, including guest
    // stacks): never file-backed, safe for direct host access by fast paths.
    static constexpr bool IsHeapRange(uint64_t address, uint64_t size) {
        return (address >= kHeapBase && address + size <= kHeapLimit) ||
               (address >= kLargeBase && address + size <= kLargeLimit);
    }

    // Address space for whole-file mappings (the mmap thunk), deliberately outside the
    // malloc heap: one expansion file can be hundreds of megabytes and would otherwise eat
    // the arena permanently -- a 345 MB .obb mapped twice is how "GuestAlloc out of guest
    // heap" showed up right after loading started. Bump-only, never freed: these mappings
    // live for the process and the address is never reused.
    uint32_t GuestAllocMapping(size_t size, size_t alignment);

    // Bottom of the guest space: exit traps and SVC trampolines. Native Android
    // maps nothing there, so a store is a NULL-based write that native code
    // faults on; here one silently rewrote the trampolines and exit traps (a
    // shader-cache copy at 0x10000, game thread spinning). After Initialize the
    // host pages are read-only: guest stores are dropped and reported (tag
    // BL2LowGuard); the bridge writes its runtime trampolines through
    // WriteLowGuardWord.
    static constexpr uint32_t kLowGuardEnd = 0x00020000;
    void WriteLowGuardWord(uint32_t vaddr, uint32_t value);
    void ReportLowGuardWrite(uint32_t vaddr, size_t bytes);

    // Runaway-PC containment. The loaded modules and the trampoline page are
    // covered automatically; register anything else that legitimately holds guest
    // code (hand-built veneers, JIT-ed guest stubs) so it is not trapped.
    void AddExecutableRange(uint32_t base, uint32_t size);

    // Dynarmic UserCallbacks interface
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t vaddr) override;
    void PreCodeTranslationHook(bool is_thumb, std::uint32_t pc,
                                Dynarmic::A32::IREmitter& ir) override;

    std::uint8_t MemoryRead8(std::uint32_t vaddr) override;
    std::uint16_t MemoryRead16(std::uint32_t vaddr) override;
    std::uint32_t MemoryRead32(std::uint32_t vaddr) override;
    std::uint64_t MemoryRead64(std::uint32_t vaddr) override;
    bool IsReadOnlyMemory(std::uint32_t vaddr) override;

    void MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) override;
    void MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) override;
    void MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) override;
    void MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) override;

    // Needed now that global_monitor is set: the base-class defaults return false, which makes
    // every guest STREX report failure and spin its retry loop forever.
    bool MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) override;
    bool MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) override;
    bool MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) override;
    bool MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) override;

#if defined(HORN_DYNARMIC_HAS_INTERPRETER_FALLBACK)
    void InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) override;
#endif
    void CallSVC(std::uint32_t swi) override;
    void ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) override;

    void AddTicks(std::uint64_t ticks) override;
    std::uint64_t GetTicksRemaining() override;

    bool IsJitRunning() const;

private:
#if defined(HORN_PHYSX_COOKIE_TEST_ACCESS)
    friend struct PhysxCookieIntegrationAccess;
#endif
    struct ThreadContext {
        size_t slot = 0;
        size_t primary_processor_id = 0;
        size_t callback_processor_id = 0;
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
        // Declared before Jits: reverse destruction destroys them before their table.
        std::unique_ptr<Bl2PhysxPageTable> physx_table;
        Bl2PhysxCookieWatch physx_cookie;
#endif
        std::unique_ptr<Dynarmic::A32::Jit> jit;
        std::unique_ptr<Dynarmic::A32::Jit> callback_jit;
        std::unique_ptr<Dynarmic::A32::Jit> sync_jit;
        std::recursive_mutex mutex;
        bool is_running_jit = false;
        bool is_running_callback_jit = false;
        bool is_running_sync_jit = false;
        uint8_t warmed_roles = 0;
        uint32_t stack_base = 0;
        uint32_t primary_stack_capacity = 0;
        uint32_t stack_top = 0;
        uint32_t callback_stack_top = 0;
        // Allocated once with sync_jit, retained for context reuse. All cores
        // are destroyed before the host releases this guest arena.
        uint32_t sync_stack_top = 0;
        std::vector<std::pair<uint32_t, std::vector<uint32_t>>> deferred;
    };

    uint8_t* m_memory = nullptr;
    size_t m_memory_size = 0;
    std::unique_ptr<Dynarmic::ExclusiveMonitor> m_monitor;
    std::unique_ptr<ElfLoader> m_fmodex_loader;
    std::unique_ptr<ElfLoader> m_fmodevent_loader;
    std::unique_ptr<ElfLoader> m_elf_loader;
    std::unique_ptr<ThunkManager> m_thunk_manager;
    Bl2JitWarmup m_jit_warmup;
    // Leaked with the process: the background saver may outlive teardown.
    bl2::jit::LearnedProfile* m_learned_jit = nullptr;
    void WarmLearnedAtEntry(Dynarmic::A32::Jit& jit, size_t role);
    uint32_t m_warm_native_render = 0;
    uint32_t m_warm_thread_proc = 0;
    uint32_t m_warm_render_vtable = 0;
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    bool m_physx_cookie_installed = false;
    void PreparePhysxCookieCore(ThreadContext& context, uint32_t target_pc);
    void RetirePhysxCookieCore(ThreadContext& context); // caller holds collection lock; owner idle
    void PhysxCookieEntry(Dynarmic::A32::Jit& jit);
    void PhysxCookieExit(Dynarmic::A32::Jit& jit);
    void ReportPhysxCookie(Dynarmic::A32::Jit& jit);
#endif

    static constexpr size_t kMaxThreadContexts = 64;
    static constexpr uint32_t kDefaultPrimaryStackSize = 2u * 1024u * 1024u;
    static constexpr uint32_t kMinimumArm32StackSize = 8192;
    static constexpr uint32_t kCallbackStackSize = 512u * 1024u;
    std::mutex m_contexts_mutex;
    std::unordered_map<std::thread::id, ThreadContext*> m_context_by_thread;
    std::vector<std::unique_ptr<ThreadContext>> m_contexts;
    std::vector<size_t> m_free_context_slots;
    std::recursive_mutex m_failed_context_mutex;
    std::atomic<bool> m_low_guard_active{false};
    std::mutex m_low_guard_mutex;
    void ProtectLowGuard();
    std::atomic<bool> m_context_failure_logged{false};

    ThreadContext* GetOrCreateThreadContext(uint32_t requested_primary_stack_size = 0);
    static bool NormalizePrimaryStackSize(uint32_t requested, uint32_t& normalized);
    bool EnsureThreadStackCapacity(ThreadContext& context, uint32_t requested);
    std::unique_ptr<Dynarmic::A32::Jit> CreateJit(size_t processor_id
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
        , Bl2PhysxPageTable* physx_table = nullptr
#endif
    );
    void WarmupForEntry(ThreadContext& context, uint32_t target_pc,
                        const std::vector<uint32_t>& args);
    uint64_t ExecuteFunction(Dynarmic::A32::Jit& jit, uint32_t target_pc,
                             const std::vector<uint32_t>& args, uint32_t stack_top,
                             uint32_t stack_capacity, bool* returned = nullptr);

    std::vector<std::pair<uint32_t, uint32_t>> m_code_ranges;   // [base, end)
    std::mutex m_code_ranges_mutex;

    // Guest heap: bump pointer for fresh memory, free lists for reuse. Requests up
    // to kMaxClassSize round to a power-of-two size class; anything larger (asset
    // payloads, thread stacks) is sized exactly. Exact reuse is preferred; any
    // fitting free span can be split, including tails smaller than 1 MB.
    // BL2 guest image at 0x40000000 is 0x03089000 bytes. Keep a 15 MiB+
    // gap after it, while recovering 192 MiB of ARM32 heap for level loading.
    static constexpr uint32_t kHeapBase  = 0x44000000;
    static constexpr uint32_t kHeapLimit = 0x7E000000;   // guest stacks live above this
    // File mappings sit above the heap/stacks and below the top guard page. Horn
    // carries two STORED OBBs totalling 1,930,064,837 bytes; the old 1.25 GiB
    // range could not represent both even in the 4 GiB fastmem reservation.
    static constexpr uint32_t kMapBase   = 0x80000000;
    static constexpr uint32_t kMapLimit  = 0xFFFF0000;
    static constexpr uint32_t kLargeBase  = 0xC0000000;
    static constexpr uint32_t kLargeLimit = 0xFFF00000;
    static constexpr uint32_t kLargeGranule = 64u * 1024u;
    std::atomic<bool> m_large_enabled{false};
    std::mutex m_large_mutex;
    std::map<uint32_t, uint32_t> m_large_free;  // base -> bytes, coalesced
    uint32_t m_large_top = kLargeBase;          // Bump end of the large range.
    uint32_t m_large_high = kLargeBase;         // Memory below may be dirty.
    bool LargeAlloc(uint64_t need, uint32_t& base, uint32_t& capacity, bool& dirty);
    void LargeFree(uint32_t base, uint32_t capacity);
    static constexpr int kMinClassShift = 5;                        // 32 B smallest block
    static constexpr uint32_t kMaxClassSize = 1u << 20;             // 1 MB largest class
    static constexpr int kNumSizeClasses = 20 - kMinClassShift + 1;
    bool HeapSelfCheck();
    // Small size classes are independent. A single heap mutex made unrelated
    // render/audio/streaming allocations serialize under load.
    std::mutex m_free_small_mutex[kNumSizeClasses];
    std::vector<uint32_t> m_free_small[kNumSizeClasses];             // class -> block bases
    std::mutex m_free_huge_mutex;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_free_huge; // capacity -> large blocks and split free tails
    std::atomic<uint32_t> m_heap_current{kHeapBase};
    std::atomic<uint32_t> m_map_current{kMapBase};

};
#endif
