#include "guest_software_write_watch.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include "perf_probe.h"
#include "thunk_manager.h"
#include "bridge_log.h"
#include "bl2_hitch_report.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <chrono>
#include <condition_variable>
#include <pthread.h>
#include <time.h>
#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define LOG_TAG "Arm32Bridge_Host"
#define LOGI(...) HORN_LOGI(LOG_TAG, __VA_ARGS__)
#define LOGE(...) HORN_LOGE(LOG_TAG, __VA_ARGS__)

#define EXIT_TRAP_ARM   0x00000004
#define EXIT_TRAP_THUMB 0x00000008

void ResetGuestMappedDirtyTracker();

// Linux exposes a tiny ARM-mode helper page at the top of every AArch32
// process. Old Bionic/libgcc code calls these addresses directly instead of
// importing functions. An ARM64 host process has no compat kuser page of its
// own, so provide the two helpers Horn actually references as guest code.
//
// These words are the ARMv7 SMP sequences from Linux entry-armv.S. Keeping
// them as guest instructions is important: Dynarmic's shared exclusive
// monitor and MemoryWriteExclusive32 implement the atomic compare/exchange,
// while DMB preserves the helper's required ordering and CPSR flags.
static std::optional<uint32_t> ReadArmKuserHelperCode(uint32_t vaddr) {
    switch (vaddr) {
        // __kuser_memory_barrier, 0xffff0fa0
        case 0xFFFF0FA0u: return 0xF57FF05Bu; // dmb ish
        case 0xFFFF0FA4u: return 0xE12FFF1Eu; // bx lr

        // __kuser_cmpxchg, 0xffff0fc0
        case 0xFFFF0FC0u: return 0xF57FF05Bu; // dmb ish
        case 0xFFFF0FC4u: return 0xE1923F9Fu; // ldrex r3, [r2]
        case 0xFFFF0FC8u: return 0xE0533000u; // subs r3, r3, r0
        case 0xFFFF0FCCu: return 0x01823F91u; // strexeq r3, r1, [r2]
        case 0xFFFF0FD0u: return 0x03330001u; // teqeq r3, #1
        case 0xFFFF0FD4u: return 0x0AFFFFFAu; // beq 0xffff0fc4
        case 0xFFFF0FD8u: return 0xE2730000u; // rsbs r0, r3, #0
        case 0xFFFF0FDCu: return 0xEAFFFFEFu; // b 0xffff0fa0
        default: return std::nullopt;
    }
}

static thread_local Dynarmic::A32::Jit* s_active_jit = nullptr;
static thread_local DynarmicHost* s_active_host = nullptr;
static thread_local std::uint64_t s_ticks_remaining = 0;
constexpr std::uint64_t kGuestSampleTickBudget = 200'000;
constexpr std::uint64_t kGuestExitTraceTickBudget = 10'000;
constexpr size_t kGuestExitTracePcCount = 64;
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
static thread_local Bl2PhysxCookieWatch* s_physx_cookie = nullptr;
static thread_local bool s_physx_native_svc = false;
struct PhysxSvcScope {
    bool previous = s_physx_native_svc;
    PhysxSvcScope() { s_physx_native_svc = true; }
    ~PhysxSvcScope() { s_physx_native_svc = previous; }
};
struct PhysxCookieScope {
    Bl2PhysxCookieWatch* previous = s_physx_cookie;
    explicit PhysxCookieScope(Bl2PhysxCookieWatch* current) { s_physx_cookie = current; }
    ~PhysxCookieScope() { s_physx_cookie = previous; }
};
template<class T>
static bool ObservePhysxGuestWrite(DynarmicHost& host, uint32_t address, T value) {
    auto* watch = s_physx_cookie;
    const bool trip = watch && s_active_host == &host && s_active_jit == watch->owner &&
        uint64_t{address} + sizeof(T) <= host.GetGuestMemorySize() && watch->Overlaps(address, sizeof(T)) &&
        watch->Observe(address, sizeof(T), value, host.MemoryRead32(watch->slot));
    if (trip && s_physx_native_svc) {
        // MemoryWrite* may also be called by a native thunk. Its SVC boundary
        // is an interval witness, not an IR memory instruction's precise PC.
        watch->event.source = Bl2PhysxCookieWatch::Source::NativeCallback;
        watch->event.pc = s_active_jit->Regs()[15] - 4;
        watch->event.lr = s_active_jit->Regs()[14];
    }
    return trip;
}
#endif

DynarmicHost::DynarmicHost() = default;

DynarmicHost::~DynarmicHost() {
    // JIT objects keep callbacks and fastmem metadata that refer to this host.
    // Destroy every per-thread core before releasing the guest address space.
    {
        std::lock_guard<std::mutex> lock(m_contexts_mutex);
        m_context_by_thread.clear();
        m_contexts.clear();
    }
    if (m_memory) {
        ResetGuestMappedDirtyTracker();
#if defined(_WIN32)
        VirtualFree(m_memory, 0, MEM_RELEASE);
#else
        munmap(m_memory, m_memory_size);
#endif
        m_memory = nullptr;
    }
}

// Reserve guest memory. MAP_NORESERVE keeps the 4 GiB reservation virtual: untouched pages
// cost nothing and only written pages are ever committed.
static uint8_t* MapGuestMemory(size_t size) {
#if defined(_WIN32)
    return reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : reinterpret_cast<uint8_t*>(p);
#endif
}

bool DynarmicHost::Initialize() {
    // Mapping the whole 32-bit space lets the JIT emit a single `ldr Wt, [Xfastmem, Waddr, uxtw]`
    // per guest access instead of a call into MemoryRead*/MemoryWrite*.
    m_memory_size = GUEST_ADDRESS_SPACE;
    m_memory = MapGuestMemory(m_memory_size);

    bool fastmem = m_memory != nullptr;
    if (m_memory == nullptr) {
        LOGE("Could not reserve %zu bytes for the guest address space; "
             "falling back to %zu bytes without fastmem.", GUEST_ADDRESS_SPACE, GUEST_FALLBACK_SIZE);
        m_memory_size = GUEST_FALLBACK_SIZE;
        m_memory = MapGuestMemory(m_memory_size);
        if (!m_memory) {
            LOGE("Failed to allocate guest memory (%zu bytes)", m_memory_size);
            m_memory_size = 0;
            return false;
        }
    }

#if defined(__ANDROID__) && defined(__aarch64__)
    char software_watch_value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.horn.software_write_watch", software_watch_value) > 0 &&
        software_watch_value[0] == '1' &&
        !InitializeGuestSoftwareWriteWatch(m_memory_size)) {
        LOGE("Could not initialize software write watch; refusing requested mode");
        return false;
    }
#endif

    if (!HeapSelfCheck()) {
        LOGE("HeapSelfCheck failed; refusing to initialize guest host");
        return false;
    }

    // ARM exit trap at 0x00000004: SVC 0 (0xEF000000)
    MemoryWrite32(EXIT_TRAP_ARM, 0xEF000000);

    // Thumb exit trap at 0x00000008: SVC 0 (0xDF00, 0xDF00)
    MemoryWrite32(EXIT_TRAP_THUMB, 0xDF00DF00);

    LOGI("ARM kuser helpers enabled: memory_barrier=0xFFFF0FA0, cmpxchg=0xFFFF0FC0");

    m_thunk_manager = std::make_unique<ThunkManager>(*this);
    if (!m_thunk_manager->RegisterAllThunks()) {
        LOGE("Thunk registration failed; refusing to initialize guest host");
        m_thunk_manager.reset();
        return false;
    }
    if (fastmem) ProtectLowGuard();

    // Horn starts UE3GameThread from NativeCallback_Initialize and may add render,
    // streaming and audio pthreads. Keep the two existing processor IDs per
    // context; reserve one additional ID for lazy synchronous UI reentry.
    m_contexts.reserve(kMaxThreadContexts);
    m_free_context_slots.reserve(kMaxThreadContexts);
    m_context_by_thread.reserve(kMaxThreadContexts);
    m_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxThreadContexts * 3);

    if (!GetOrCreateThreadContext()) {
        LOGE("Failed to create initial per-thread Dynarmic JIT context");
        return false;
    }
    LOGI("Dynarmic Host initialized: %zu bytes guest space, fastmem %s",
         m_memory_size, fastmem ? "ON" : "OFF");
    return true;
}

std::unique_ptr<Dynarmic::A32::Jit> DynarmicHost::CreateJit(size_t processor_id
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
        , Bl2PhysxPageTable* physx_table
#endif
        ) {
    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.write_watch = GuestSoftwareWriteWatchFlags();
    config.write_watch_page_bits = GuestSoftwareWriteWatchPageShift();
    config.processor_id = processor_id;
    config.global_monitor = m_monitor.get();
    config.arch_version = Dynarmic::A32::ArchVersion::v7;  // guest libs are ARMv7-A + NEON
    // Coarse cycle budgets are enabled only for the on-device performance
    // probe. Each exhausted budget yields a guest-PC sample; production keeps
    // Dynarmic's lower-overhead no-cycle-counting path.
    config.enable_cycle_counting = HornPerfEnabled();
    config.always_little_endian = true;                    // guest never uses SETEND; drops CPSR.E handling
    config.define_unpredictable_behaviour = true;          // define rather than halt on odd encodings
    // Ported from the static recompiler: ARM B<cond> closes its block as an If
    // terminal; VST1/VFP doubleword stores write whole registers and VLD1 packs
    // its words in registers. debug.bl2.jit_fusion=0 before launch disables both.
    static const bool translate_fusion = [] {
        char value[PROP_VALUE_MAX] = {};
        return !(__system_property_get("debug.bl2.jit_fusion", value) > 0 && value[0] == '0');
    }();
    config.fuse_conditional_branches = translate_fusion;
    config.wide_vector_transfers = translate_fusion;
    if (m_memory_size == GUEST_ADDRESS_SPACE) {
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(m_memory);
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    if (physx_table) {
        // All-NULL private table: every access uses callbacks and their software
        // dirty publication, while the existing table fallback supplies exact PC.
        config.fastmem_pointer = std::nullopt;
        // The x24 software instrumentation requires page_table==nullptr.
        // This owner instead publishes through the original callbacks.
        config.write_watch = nullptr;
        config.page_table = physx_table;
        config.check_halt_on_memory_access = true;
        config.detect_misaligned_access_via_page_table = 8 | 16 | 32 | 64;
        config.only_detect_misalignment_via_page_table_on_page_boundary = true;
    }
#endif
    return std::make_unique<Dynarmic::A32::Jit>(config);
}

void DynarmicHost::PreCodeTranslationHook(
        bool, std::uint32_t pc, Dynarmic::A32::IREmitter&) {
    HornPerfRecordTranslation(pc);
}

bool DynarmicHost::NormalizePrimaryStackSize(uint32_t requested,
                                             uint32_t& normalized) {
    const uint64_t raw = requested ? requested : kDefaultPrimaryStackSize;
    if (raw < kMinimumArm32StackSize) return false;
    const uint64_t aligned = (raw + 15u) & ~uint64_t{15};
    if (aligned > std::numeric_limits<uint32_t>::max()) return false;
    normalized = static_cast<uint32_t>(aligned);
    return true;
}

bool DynarmicHost::EnsureThreadStackCapacity(ThreadContext& context,
                                             uint32_t requested) {
    uint32_t wanted = 0;
    if (!NormalizePrimaryStackSize(requested, wanted)) return false;
    if (context.stack_base && context.primary_stack_capacity >= wanted) return true;

    const uint64_t total = static_cast<uint64_t>(wanted) + kCallbackStackSize;
    if (total > std::numeric_limits<uint32_t>::max()) {
        LOGE("Guest thread stack request is too large: primary=%u callback=%u",
             wanted, kCallbackStackSize);
        return false;
    }

    // Allocate before releasing the old block. The allocator does not coalesce
    // adjacent blocks, so freeing a smaller stack cannot make a larger request
    // succeed, and this keeps a failed grow atomic for the free context slot.
    const uint32_t new_base = GuestAlloc(static_cast<size_t>(total), 16);
    if (!new_base) {
        LOGE("Guest thread stack allocation failed: primary=%u callback=%u",
             wanted, kCallbackStackSize);
        return false;
    }
    const uint64_t primary_top = static_cast<uint64_t>(new_base) + wanted;
    const uint64_t callback_top = primary_top + kCallbackStackSize;
    if (callback_top > m_memory_size ||
        callback_top > std::numeric_limits<uint32_t>::max()) {
        LOGE("Guest thread stack range is outside the ARM32 arena: base=0x%08X total=%llu",
             new_base, static_cast<unsigned long long>(total));
        GuestFree(new_base);
        return false;
    }

    const uint32_t old_base = context.stack_base;
    context.stack_base = new_base;
    context.primary_stack_capacity = wanted;
    context.stack_top = static_cast<uint32_t>(primary_top);
    context.callback_stack_top = static_cast<uint32_t>(callback_top);
    if (old_base) GuestFree(old_base);
    LOGI("Dynarmic context %zu stack capacity is now %u bytes (base=0x%08X)",
         context.slot, context.primary_stack_capacity, context.stack_base);
    return true;
}

DynarmicHost::ThreadContext* DynarmicHost::GetOrCreateThreadContext(
        uint32_t requested_primary_stack_size) {
    const std::thread::id thread_id = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(m_contexts_mutex);

    auto existing = m_context_by_thread.find(thread_id);
    if (existing != m_context_by_thread.end()) {
        ThreadContext* context = existing->second;
        lock.unlock();
        // Ordinary RunFunction/GetMutex lookups pass zero and must preserve the
        // stack chosen by pthread_create. An explicit Attach may grow an idle
        // stale/reused thread-id binding, but never an executing core.
        if (!requested_primary_stack_size) return context;
        uint32_t wanted = 0;
        if (!NormalizePrimaryStackSize(requested_primary_stack_size, wanted)) return nullptr;
        std::lock_guard<std::recursive_mutex> context_lock(context->mutex);
        if (context->is_running_jit || context->is_running_callback_jit ||
            (s_active_host == this && s_active_jit != nullptr)) {
            LOGE("Cannot attach active Dynarmic context %zu (stack %u, requested %u)",
                 context->slot, context->primary_stack_capacity, wanted);
            return nullptr;
        }
        if (!EnsureThreadStackCapacity(*context, wanted)) return nullptr;
        // An OS thread id may be recycled after a Java-owned thread exits
        // without calling ReleaseCurrentThreadContext. Attach is a fresh lease,
        // so clear stale core state even when the retained stack was large enough.
        std::lock_guard<std::mutex> contexts_lock(m_contexts_mutex);
        auto still_bound = m_context_by_thread.find(thread_id);
        if (still_bound == m_context_by_thread.end() || still_bound->second != context) {
            return nullptr;
        }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
        RetirePhysxCookieCore(*context);
#endif
        m_monitor->ClearProcessor(context->primary_processor_id);
        m_monitor->ClearProcessor(context->callback_processor_id);
        context->jit->Reset();
        context->callback_jit->Reset();
        if (context->sync_jit) {
            m_monitor->ClearProcessor(kMaxThreadContexts * 2 + context->slot);
            context->sync_jit->Reset();
        }
        context->warmed_roles = 0;
        context->deferred.clear();
        m_context_failure_logged.store(false, std::memory_order_release);
        LOGI("Reattached Dynarmic thread context %zu (stack=%u)",
             context->slot, context->primary_stack_capacity);
        return context;
    }

    uint32_t wanted = 0;
    if (!NormalizePrimaryStackSize(requested_primary_stack_size, wanted)) return nullptr;

    if (!m_free_context_slots.empty()) {
        // Prefer the smallest already-large-enough stack. If every free slot is
        // smaller, grow the largest one to minimize unnecessary churn.
        size_t free_index = 0;
        bool found_adequate = false;
        uint32_t selected_capacity = 0;
        for (size_t i = 0; i < m_free_context_slots.size(); ++i) {
            const ThreadContext& candidate = *m_contexts[m_free_context_slots[i]];
            const uint32_t capacity = candidate.primary_stack_capacity;
            if (capacity >= wanted) {
                if (!found_adequate || capacity < selected_capacity) {
                    free_index = i;
                    selected_capacity = capacity;
                    found_adequate = true;
                }
            } else if (!found_adequate && capacity >= selected_capacity) {
                free_index = i;
                selected_capacity = capacity;
            }
        }
        const size_t slot = m_free_context_slots[free_index];
        ThreadContext* context = m_contexts[slot].get();
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
        RetirePhysxCookieCore(*context);
#endif
        if (!EnsureThreadStackCapacity(*context, wanted)) {
            return nullptr; // slot remains free and its previous stack remains valid
        }
        m_monitor->ClearProcessor(context->primary_processor_id);
        m_monitor->ClearProcessor(context->callback_processor_id);
        context->jit->Reset();
        context->callback_jit->Reset();
        if (context->sync_jit) {
            m_monitor->ClearProcessor(kMaxThreadContexts * 2 + context->slot);
            context->sync_jit->Reset();
        }
        context->is_running_jit = false;
        context->is_running_callback_jit = false;
        context->warmed_roles = 0;
        context->deferred.clear();
        auto inserted = m_context_by_thread.emplace(thread_id, context);
        if (!inserted.second) return nullptr;
        m_free_context_slots[free_index] = m_free_context_slots.back();
        m_free_context_slots.pop_back();
        m_context_failure_logged.store(false, std::memory_order_release);
        LOGI("Reused Dynarmic thread context %zu (cpu=%zu, callback_cpu=%zu, stack=%u)",
             context->slot, context->primary_processor_id,
             context->callback_processor_id, context->primary_stack_capacity);
        return context;
    }

    if (!m_monitor || m_contexts.size() >= kMaxThreadContexts) {
        if (!m_context_failure_logged.exchange(true)) {
            LOGE("FATAL: cannot allocate Dynarmic thread context (monitor=%s, used=%zu, max=%zu)",
                 m_monitor ? "ready" : "null", m_contexts.size(), kMaxThreadContexts);
        }
        return nullptr;
    }

    auto context = std::make_unique<ThreadContext>();
    context->slot = m_contexts.size();
    context->primary_processor_id = context->slot * 2;
    context->callback_processor_id = context->primary_processor_id + 1;

    if (!EnsureThreadStackCapacity(*context, wanted)) {
        LOGE("FATAL: guest stack allocation failed for Dynarmic context %zu", context->slot);
        return nullptr;
    }
    try {
        context->jit = CreateJit(context->primary_processor_id);
        if (!context->jit) {
            LOGE("FATAL: primary Dynarmic JIT creation failed for context %zu", context->slot);
            GuestFree(context->stack_base);
            return nullptr;
        }
        // Both JIT pointers are immutable in ordinary mode after publication. This
        // lets InvalidateGuestCode safely wake a foreign long-running core without
        // racing lazy callback-core construction.
        context->callback_jit = CreateJit(context->callback_processor_id);
        if (!context->callback_jit) {
            LOGE("FATAL: callback Dynarmic JIT creation failed for context %zu", context->slot);
            GuestFree(context->stack_base);
            return nullptr;
        }
    } catch (...) {
        GuestFree(context->stack_base);
        throw;
    }

    ThreadContext* result = context.get();
    bool map_inserted = false;
    try {
        auto inserted = m_context_by_thread.emplace(thread_id, result);
        if (!inserted.second) {
            GuestFree(context->stack_base);
            return nullptr;
        }
        map_inserted = true;
        m_contexts.push_back(std::move(context));
    } catch (...) {
        if (map_inserted) m_context_by_thread.erase(thread_id);
        if (context && context->stack_base) GuestFree(context->stack_base);
        throw;
    }
    LOGI("Created Dynarmic thread context %zu (cpu=%zu, callback_cpu=%zu, stack=0x%08X/%u, callback_stack=0x%08X)",
         result->slot, result->primary_processor_id, result->callback_processor_id,
         result->stack_top, result->primary_stack_capacity,
         result->callback_stack_top);
    return result;
}

std::recursive_mutex& DynarmicHost::GetMutex() {
    ThreadContext* context = GetOrCreateThreadContext();
    return context ? context->mutex : m_failed_context_mutex;
}

bool DynarmicHost::AttachCurrentThreadContext(uint32_t primary_stack_size) {
    // Pass the default explicitly so Attach remains a fresh-lease operation
    // even when std::thread reuses an id that still has an idle host binding.
    const uint32_t requested = primary_stack_size ? primary_stack_size
                                                  : kDefaultPrimaryStackSize;
    return GetOrCreateThreadContext(requested) != nullptr;
}

void DynarmicHost::ReleaseCurrentThreadContext() {
    const std::thread::id thread_id = std::this_thread::get_id();
    ThreadContext* context = nullptr;
    {
        std::lock_guard<std::mutex> contexts_lock(m_contexts_mutex);
        auto it = m_context_by_thread.find(thread_id);
        if (it == m_context_by_thread.end()) return;
        context = it->second;
    }
    std::lock_guard<std::recursive_mutex> context_lock(context->mutex);
    if (context->is_running_jit || context->is_running_callback_jit ||
        (s_active_host == this && s_active_jit != nullptr)) {
        LOGE("Refusing to release active Dynarmic thread context %zu", context->slot);
        return;
    }
    std::lock_guard<std::mutex> contexts_lock(m_contexts_mutex);
    auto it = m_context_by_thread.find(thread_id);
    if (it == m_context_by_thread.end() || it->second != context) return;
    m_monitor->ClearProcessor(context->primary_processor_id);
    m_monitor->ClearProcessor(context->callback_processor_id);
    context->jit->Reset();
    context->callback_jit->Reset();
    if (context->sync_jit) {
        m_monitor->ClearProcessor(kMaxThreadContexts * 2 + context->slot);
        context->sync_jit->Reset();
    }
    context->deferred.clear();
    m_context_by_thread.erase(it);
    m_free_context_slots.push_back(context->slot);
    LOGI("Released Dynarmic thread context %zu", context->slot);
}

void DynarmicHost::HaltCurrentThreadExecution(Dynarmic::HaltReason reason) {
    ThreadContext* context = GetOrCreateThreadContext();
    if (!context) return;
    std::lock_guard<std::recursive_mutex> lock(context->mutex);
    context->jit->HaltExecution(reason);
    context->callback_jit->HaltExecution(reason);
    if (context->sync_jit) context->sync_jit->HaltExecution(reason);
}

bool DynarmicHost::IsJitRunning() const {
    return s_active_host == this && s_active_jit != nullptr;
}

bool DynarmicHost::LoadGame(const std::string& elf_path) {
    LOGI("Loading main game ELF from file: %s", elf_path.c_str());
    m_elf_loader = std::make_unique<ElfLoader>(m_memory, 0x40000000);
    if (!m_elf_loader->Load(elf_path)) {
        LOGE("Failed to load ELF: %s", elf_path.c_str());
        return false;
    }

    auto resolver = [this](const std::string& name) -> uint32_t {
        if (m_fmodevent_loader) {
            uint32_t addr = m_fmodevent_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_fmodex_loader) {
            uint32_t addr = m_fmodex_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_elf_loader) {
            uint32_t addr = m_elf_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        return m_thunk_manager->GetOrCreateThunkAddress(name);
    };

    if (!m_elf_loader->Relocate(resolver)) {
        LOGE("Failed to relocate ELF.");
        return false;
    }

    LOGI("Running %zu static initializers in .init_array...", m_elf_loader->GetInitArray().size());
    for (uint32_t init_func : m_elf_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Game ELF loaded and initialized successfully!");
    return true;
}

bool DynarmicHost::LoadGameFromMemory(const uint8_t* data, size_t size) {
    LOGI("Loading main game ELF from memory (%zu bytes)...", size);
    m_elf_loader = std::make_unique<ElfLoader>(m_memory, 0x40000000);
    if (!m_elf_loader->LoadFromMemory(data, size)) {
        LOGE("Failed to load ELF from memory buffer");
        return false;
    }

    auto resolver = [this](const std::string& name) -> uint32_t {
        if (m_fmodevent_loader) {
            uint32_t addr = m_fmodevent_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_fmodex_loader) {
            uint32_t addr = m_fmodex_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_elf_loader) {
            uint32_t addr = m_elf_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        return m_thunk_manager->GetOrCreateThunkAddress(name);
    };

    if (!m_elf_loader->Relocate(resolver)) {
        LOGE("Failed to relocate ELF.");
        return false;
    }

    LOGI("Running %zu static initializers in .init_array...", m_elf_loader->GetInitArray().size());
    for (uint32_t init_func : m_elf_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Game ELF loaded from memory and initialized successfully!");
    return true;
}

bool DynarmicHost::LoadLibraries(const std::string& fmodex_path, const std::string& fmodevent_path, const std::string& game_path) {
    LOGI("Loading libraries: fmodex='%s', fmodevent='%s', game='%s'", fmodex_path.c_str(), fmodevent_path.c_str(), game_path.c_str());

    m_fmodex_loader = std::make_unique<ElfLoader>(m_memory, 0x40400000);
    if (!m_fmodex_loader->Load(fmodex_path)) {
        LOGE("Failed to load libfmodex.so from %s", fmodex_path.c_str());
        return false;
    }

    m_fmodevent_loader = std::make_unique<ElfLoader>(m_memory, 0x40600000);
    if (!m_fmodevent_loader->Load(fmodevent_path)) {
        LOGE("Failed to load libfmodevent.so from %s", fmodevent_path.c_str());
        return false;
    }

    m_elf_loader = std::make_unique<ElfLoader>(m_memory, 0x40000000);
    if (!m_elf_loader->Load(game_path)) {
        LOGE("Failed to load the guest library from %s", game_path.c_str());
        return false;
    }

    auto resolver = [this](const std::string& name) -> uint32_t {
        if (m_fmodevent_loader) {
            uint32_t addr = m_fmodevent_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_fmodex_loader) {
            uint32_t addr = m_fmodex_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_elf_loader) {
            uint32_t addr = m_elf_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        return m_thunk_manager->GetOrCreateThunkAddress(name);
    };

    if (!m_fmodex_loader->Relocate(resolver)) {
        LOGE("Failed to relocate libfmodex.so");
        return false;
    }

    if (!m_fmodevent_loader->Relocate(resolver)) {
        LOGE("Failed to relocate libfmodevent.so");
        return false;
    }

    if (!m_elf_loader->Relocate(resolver)) {
        LOGE("Failed to relocate the guest library");
        return false;
    }

    LOGI("Running initializers for libfmodex.so (%zu funcs)...", m_fmodex_loader->GetInitArray().size());
    for (uint32_t init_func : m_fmodex_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Running initializers for libfmodevent.so (%zu funcs)...", m_fmodevent_loader->GetInitArray().size());
    for (uint32_t init_func : m_fmodevent_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Running initializers for the guest library (%zu funcs)...", m_elf_loader->GetInitArray().size());
    for (uint32_t init_func : m_elf_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("All libraries loaded, relocated, and initialized successfully with sound!");
    return true;
}

bool DynarmicHost::LoadLibrariesFromMemory(const uint8_t* fmodex_data, size_t fmodex_size,
                                           const uint8_t* fmodevent_data, size_t fmodevent_size,
                                           const uint8_t* game_data, size_t game_size) {
    LOGI("Loading libraries from memory: fmodex=%zu bytes, fmodevent=%zu bytes, game=%zu bytes",
         fmodex_size, fmodevent_size, game_size);

    m_fmodex_loader = std::make_unique<ElfLoader>(m_memory, 0x40400000);
    if (!m_fmodex_loader->LoadFromMemory(fmodex_data, fmodex_size)) {
        LOGE("Failed to load libfmodex.so from memory");
        return false;
    }

    m_fmodevent_loader = std::make_unique<ElfLoader>(m_memory, 0x40600000);
    if (!m_fmodevent_loader->LoadFromMemory(fmodevent_data, fmodevent_size)) {
        LOGE("Failed to load libfmodevent.so from memory");
        return false;
    }

    m_elf_loader = std::make_unique<ElfLoader>(m_memory, 0x40000000);
    if (!m_elf_loader->LoadFromMemory(game_data, game_size)) {
        LOGE("Failed to load the guest library from memory");
        return false;
    }

    auto resolver = [this](const std::string& name) -> uint32_t {
        if (m_fmodevent_loader) {
            uint32_t addr = m_fmodevent_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_fmodex_loader) {
            uint32_t addr = m_fmodex_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        if (m_elf_loader) {
            uint32_t addr = m_elf_loader->GetSymbolAddress(name);
            if (addr != 0) return addr;
        }
        return m_thunk_manager->GetOrCreateThunkAddress(name);
    };

    if (!m_fmodex_loader->Relocate(resolver)) {
        LOGE("Failed to relocate libfmodex.so");
        return false;
    }

    if (!m_fmodevent_loader->Relocate(resolver)) {
        LOGE("Failed to relocate libfmodevent.so");
        return false;
    }

    if (!m_elf_loader->Relocate(resolver)) {
        LOGE("Failed to relocate the guest library");
        return false;
    }

    LOGI("Running initializers for libfmodex.so (%zu funcs)...", m_fmodex_loader->GetInitArray().size());
    for (uint32_t init_func : m_fmodex_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Running initializers for libfmodevent.so (%zu funcs)...", m_fmodevent_loader->GetInitArray().size());
    for (uint32_t init_func : m_fmodevent_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("Running initializers for the guest library (%zu funcs)...", m_elf_loader->GetInitArray().size());
    for (uint32_t init_func : m_elf_loader->GetInitArray()) {
        RunFunction(init_func);
    }

    LOGI("All libraries loaded from memory and initialized successfully with sound!");
    return true;
}

uint32_t DynarmicHost::FindSymbol(const std::string& name) {
    if (m_elf_loader) {
        uint32_t addr = m_elf_loader->GetSymbolAddress(name);
        if (addr) return addr;
    }
    if (m_fmodevent_loader) {
        uint32_t addr = m_fmodevent_loader->GetSymbolAddress(name);
        if (addr) return addr;
    }
    if (m_fmodex_loader) {
        uint32_t addr = m_fmodex_loader->GetSymbolAddress(name);
        if (addr) return addr;
    }
    return 0;
}

uint32_t DynarmicHost::RunFunction(uint32_t target_pc, const std::vector<uint32_t>& args) {
    return static_cast<uint32_t>(RunFunction64(target_pc, args));
}

#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
namespace {
constexpr uint32_t kPhysxEntry = 0x42573280;
constexpr uint32_t kPhysxExit = kPhysxEntry + 0x418;
constexpr uint32_t kPhysxThread = 0x424AA9E8;
constexpr uint32_t kPhysxGuardGot = 0x42F366E8;
}

bool DynarmicHost::InstallPhysxCookieWatch(bool verified_original) {
    if (m_physx_cookie_installed) return true;
#if defined(__ANDROID__)
    char enabled[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.physx_cookie_watch", enabled) != 1 || enabled[0] != '1')
        return false;
#else
    return false;
#endif
    // The caller supplies the existing full-file SHA verification result and
    // calls at quiescent startup, before JNI_OnLoad/guest worker entry.
    if (!verified_original || !m_elf_loader || !m_thunk_manager || IsJitRunning() ||
        m_elf_loader->GetBaseAddress() != 0x40000000 ||
        FindSymbol("_ZN7PxScene15generateIslandsEb") != kPhysxEntry ||
        FindSymbol("_Z13NxThreadStartPv") != kPhysxThread ||
        !m_elf_loader->IsReadOnlyRange(kPhysxEntry, 0x5E8) ||
        MemoryRead32(kPhysxEntry) != 0xE59F35B8 ||
        MemoryRead32(kPhysxExit) != 0xE24BD020 ||
        MemoryRead32(kPhysxExit + 4) != 0xE8BD8FF0 ||
        MemoryRead32(kPhysxEntry + 0x5C0) != 0xFFFFC9E8 ||
        MemoryRead32(kPhysxEntry + 0x5C4) != 0x009C6A5C) return false;
    auto& thunks = *m_thunk_manager;
    const uint32_t entry_stub = thunks.RegisterThunk("__bl2_physx_cookie_entry",
        [this](Dynarmic::A32::Jit& jit, DynarmicHost&) { PhysxCookieEntry(jit); });
    const uint32_t exit_stub = thunks.RegisterThunk("__bl2_physx_cookie_exit",
        [this](Dynarmic::A32::Jit& jit, DynarmicHost&) { PhysxCookieExit(jit); });
    if (!entry_stub || !exit_stub) return false;
    const uint32_t entry_svc = MemoryRead32(entry_stub), exit_svc = MemoryRead32(exit_stub);
    if ((entry_svc & 0xFF000000) != 0xEF000000 || !(entry_svc & 0x00FFFFFF) ||
        (exit_svc & 0xFF000000) != 0xEF000000 || !(exit_svc & 0x00FFFFFF)) return false;
    // Only one original instruction is replaced at each site. Original PUSH,
    // comparison, failure path, POP and saved LR remain guest instructions.
    MemoryWrite32(kPhysxEntry, entry_svc);
    MemoryWrite32(kPhysxExit, exit_svc);
    InvalidateGuestCode(kPhysxEntry, 4);
    InvalidateGuestCode(kPhysxExit, 4);
    m_physx_cookie_installed = true;
    LOGI("PhysX cookie watch installed; NxThreadStart primary only, all-NULL private table; "
         "foreign cores and unguarded native/CAS writes remain uncovered");
    return true;
}

void DynarmicHost::PreparePhysxCookieCore(ThreadContext& context, uint32_t target_pc) {
    // RunFunction64 holds this owner's mutex before marking either core active.
    if (!m_physx_cookie_installed || target_pc != kPhysxThread || context.physx_table ||
        context.is_running_jit || context.is_running_callback_jit) return;
    try {
        auto table = std::make_unique<Bl2PhysxPageTable>(); // all entries NULL, never mutated
        auto replacement = CreateJit(context.primary_processor_id, table.get());
        replacement->Regs() = context.jit->Regs();
        replacement->ExtRegs() = context.jit->ExtRegs();
        replacement->SetCpsr(context.jit->Cpsr());
        replacement->SetFpscr(context.jit->Fpscr());
        // Foreign InvalidateGuestCode also holds this lock when visiting Jits.
        std::lock_guard<std::mutex> contexts_lock(m_contexts_mutex);
        m_monitor->ClearProcessor(context.primary_processor_id);
        context.jit = std::move(replacement);
        context.physx_table = std::move(table);
        auto& watch = context.physx_cookie;
        watch = {};
        watch.owner = context.jit.get();
        watch.context_slot = context.slot;
        watch.stack_low = context.stack_base;
        watch.stack_top = context.stack_top;
        LOGI("PhysX cookie owner context=%zu stack=%08X..%08X; callback core unchanged",
             context.slot, watch.stack_low, watch.stack_top);
    } catch (...) {
        LOGE("PhysX cookie owner unavailable context=%zu; ordinary core retained, no coverage",
             context.slot);
    }
}

void DynarmicHost::RetirePhysxCookieCore(ThreadContext& context) {
    if (!context.physx_table) return;
    // Caller holds owner and collection locks (or owns a free, unpublished
    // context). Destroy the old Jit while its immutable table is still alive.
    auto replacement = CreateJit(context.primary_processor_id);
    context.jit = std::move(replacement);
    context.physx_table.reset();
    context.physx_cookie = {};
}

void DynarmicHost::PhysxCookieEntry(Dynarmic::A32::Jit& jit) {
    auto& regs = jit.Regs();
    regs[3] = MemoryRead32(kPhysxEntry + 0x5C0); // exact original LDR r3 literal
    auto* watch = s_physx_cookie;
    if (!watch || watch->owner != &jit || s_active_host != this) return;
    const uint32_t sp = regs[13];
    const uint32_t got = kPhysxEntry + 0x24 + MemoryRead32(kPhysxEntry + 0x5C4);
    const uint32_t got_slot = got + regs[3];
    if (got_slot != kPhysxGuardGot || uint64_t{got_slot} + 4 > m_memory_size ||
        sp < 44 || sp > watch->stack_top || sp - 44 < watch->stack_low) {
        LOGE("PhysX cookie entry outside pinned guard/owner stack; no arm SP=%08X", sp);
        return;
    }
    const uint32_t guard_pointer = MemoryRead32(got_slot);
    if (uint64_t{guard_pointer} + 4 > m_memory_size) {
        LOGE("PhysX cookie guard pointer outside guest memory; no arm ptr=%08X", guard_pointer);
        return;
    }
    if (!watch->Arm(sp - 44, MemoryRead32(guard_pointer))) {
        if (watch->Trip(Bl2PhysxCookieWatch::Cause::Reentry,
                        Bl2PhysxCookieWatch::Source::Observation, watch->slot, 4, 0,
                        MemoryRead32(watch->slot), MemoryRead32(watch->slot)))
            watch->event.pc = kPhysxEntry;
        jit.HaltExecution(Dynarmic::HaltReason::MemoryAbort);
        return;
    }
    watch->entry_sp = sp;
    watch->guard_pointer = guard_pointer;
    watch->scene = regs[0];
    if (watch->sequence == 1) {
        LOGI("PhysX cookie first arm context=%zu owner=%p entry_sp=%08X slot=%08X guard_ptr=%08X guard=%08X scene=%08X",
             watch->context_slot, static_cast<void*>(&jit), sp, watch->slot,
             guard_pointer, watch->expected, watch->scene);
    }
}

void DynarmicHost::PhysxCookieExit(Dynarmic::A32::Jit& jit) {
    jit.Regs()[13] = jit.Regs()[11] - 32; // exact original SUB sp,fp,#32, no flags
    auto* watch = s_physx_cookie;
    if (!watch || watch->owner != &jit || s_active_host != this) return;
    if (watch->phase == Bl2PhysxCookieWatch::Phase::Idle) return; // unarmed/out-of-range entry
    const uint32_t cookie = MemoryRead32(watch->slot);
    if (!watch->Disarm(cookie)) {
        watch->event.pc = kPhysxExit;
        jit.HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    } else if (watch->successful_exits == 1) {
        LOGI("PhysX cookie first disarm context=%zu owner=%p slot=%08X sequence=%llu initial_stores=%u overlaps=%u cookie=%08X",
             watch->context_slot, static_cast<void*>(&jit), watch->slot,
             static_cast<unsigned long long>(watch->sequence), watch->initial_stores,
             watch->overlaps, cookie);
    }
}

Bl2PhysxCookieWatch::NativeToken DynarmicHost::BeginPhysxNativeWrite(uint32_t address, size_t bytes) {
    auto* watch = s_physx_cookie;
    if (!watch || !s_active_host || s_active_jit != watch->owner ||
        watch->phase != Bl2PhysxCookieWatch::Phase::Initialized || !watch->Overlaps(address, bytes) ||
        uint64_t{address} > s_active_host->m_memory_size ||
        bytes > s_active_host->m_memory_size - address) return {};
    return {watch, watch->sequence, watch->slot, s_active_host->MemoryRead32(watch->slot),
            address, s_active_jit->Regs()[15] - 4, s_active_jit->Regs()[14], bytes};
}

void DynarmicHost::EndPhysxNativeWrite(const Bl2PhysxCookieWatch::NativeToken& token) {
    auto* watch = token.watch;
    if (!watch || watch != s_physx_cookie || !s_active_host || s_active_jit != watch->owner ||
        token.sequence != watch->sequence || token.slot != watch->slot ||
        watch->phase != Bl2PhysxCookieWatch::Phase::Initialized) return;
    const uint32_t after = s_active_host->MemoryRead32(token.slot);
    if (token.before == watch->expected && after == watch->expected) return;
    if (watch->Trip(token.before != watch->expected ? Bl2PhysxCookieWatch::Cause::AlreadyCorrupt :
                    Bl2PhysxCookieWatch::Cause::Writer, Bl2PhysxCookieWatch::Source::NativeInterval,
                    token.address, static_cast<uint32_t>(std::min<size_t>(token.bytes, UINT32_MAX)),
                    0, token.before, after)) {
        watch->event.pc = token.pc;
        watch->event.lr = token.lr;
        s_active_jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
    }
}

void DynarmicHost::ReportPhysxCookie(Dynarmic::A32::Jit& jit) {
    auto* watch = s_physx_cookie;
    if (!watch || watch->owner != &jit || watch->phase != Bl2PhysxCookieWatch::Phase::Tripped ||
        watch->reported) return;
    watch->reported = true;
    auto& event = watch->event;
    const bool precise_access = event.source == Bl2PhysxCookieWatch::Source::GuestStore;
    if (precise_access) event.pc = jit.Regs()[15]; // existing IR MemoryAbort current_location
    // Other aborted guest registers can be stale after GetSetElimination.
    // Only entry metadata, memory and the emitter's explicit abort PC are used.
    LOGE("PHYSX_COOKIE cause=%u source=%u precise_access=%u context=%zu slot=%08X "
         "entry_sp=%08X scene=%08X guard_got=%08X guard_ptr=%08X entry_guard=%08X current_guard=%08X "
         "PC=%08X native_LR=%08X instruction=%08X address=%08X bytes=%u value=%016llX "
         "before=%08X predicted_after=%08X current_cookie=%08X initial_stores=%u overlaps=%u",
         static_cast<unsigned>(event.cause), static_cast<unsigned>(event.source), static_cast<unsigned>(precise_access),
         watch->context_slot, watch->slot, watch->entry_sp, watch->scene, kPhysxGuardGot,
         watch->guard_pointer, watch->expected, MemoryRead32(watch->guard_pointer),
         event.pc, event.lr, MemoryRead32(event.pc), event.address, event.bytes,
         static_cast<unsigned long long>(event.value), event.before, event.after,
         MemoryRead32(watch->slot), watch->initial_stores, watch->overlaps);
}
#endif


bool DynarmicHost::InstallJitWarmupTable(std::span<const uint8_t> bytes) {
#if defined(__ANDROID__)
    char enabled[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.jit_warmup", enabled) > 0 && enabled[0] == '0')
        return false;
#endif
    if (!m_elf_loader || m_elf_loader->GetBaseAddress() != 0x40000000 ||
        !m_jit_warmup.Parse(bytes, [this](uint32_t pc) {
            return m_elf_loader->IsReadOnlyRange(pc, 4);
        })) return false;
    m_warm_native_render = FindSymbol("_Z21NativeCallback_RenderPv");
    m_warm_thread_proc = FindSymbol("_ZN22FRunnableThreadAndroid11_ThreadProcEPv");
    const uint32_t vtable = FindSymbol("_ZTV16FRenderingThread");
    m_warm_render_vtable = vtable && vtable <= UINT32_MAX - 8 ? vtable + 8 : 0;
    LOGI("JIT warmup catalog ready main=%zu callback=%zu renderer=%zu",
         m_jit_warmup.PCs(Bl2JitWarmup::Main).size(),
         m_jit_warmup.PCs(Bl2JitWarmup::Callback).size(),
         m_jit_warmup.PCs(Bl2JitWarmup::Renderer).size());
    return m_warm_native_render != 0;
}

namespace {
// Learned warmup state of the current guest thread: its warmed cores by role,
// and how far the role's loaded list has been translated.
thread_local std::array<Dynarmic::A32::Jit*, bl2::jit::kRoleCount> s_learn_jits{};
thread_local std::array<size_t, bl2::jit::kRoleCount> s_learn_cursor{};
thread_local std::array<bool, bl2::jit::kRoleCount> s_learn_exhausted{};
// Runtime translations must always fit: warm only while this much is free.
constexpr size_t kLearnedMinFreeCode = 32u * 1024u * 1024u;
constexpr uint64_t kLearnedEntryBudgetNs = 150'000'000;
// The saver thread writes every 45 s and on request (onPause): the UI thread
// never writes and fsyncs the multi-megabyte list itself.
std::mutex s_learn_save_mutex;
std::condition_variable s_learn_save_cv;
bool s_learn_save_requested = false;

uint64_t LearnedNowNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(now.tv_nsec);
}

void OnBlockEmitted(std::uint64_t descriptor) {
    Bl2HitchCountJitBlock();
    DynarmicHost* const host = s_active_host;
    Dynarmic::A32::Jit* const jit = s_active_jit;
    if (!host || !jit) return;
    for (size_t role = 0; role < s_learn_jits.size(); ++role) {
        if (s_learn_jits[role] == jit) {
            host->RecordLearnedBlock(role, descriptor);
            return;
        }
    }
}
}  // namespace

void DynarmicHost::LoadLearnedJitProfile(const std::string& directory) {
    // The observer also feeds the frame black box when learning is disabled.
    Dynarmic::A32::Jit::SetBlockEmittedHook(&OnBlockEmitted);
#if defined(__ANDROID__)
    char enabled[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.bl2.jit_learn", enabled) > 0 && enabled[0] == '0')
        return;
#endif
    if (m_learned_jit || directory.empty() || !m_elf_loader ||
        m_elf_loader->GetBaseAddress() != 0x40000000) return;
    auto* profile = new bl2::jit::LearnedProfile;
    ElfLoader* const loader = m_elf_loader.get();
    profile->Load(directory + "/bl2_jit_learned.bin", Bl2JitWarmup::kGuestBytes,
                  Bl2JitWarmup::kGuestSha,
                  [loader](uint32_t pc) { return loader->IsReadOnlyRange(pc, 4); });
    m_learned_jit = profile;
#if defined(__ANDROID__)
    // QA: debug.bl2.jit_learn_export=1 rewrites the profile once (0660) for adb.
    if (__system_property_get("debug.bl2.jit_learn_export", enabled) > 0 && enabled[0] == '1')
        profile->SaveNow();
#endif
    __android_log_print(ANDROID_LOG_INFO, "BL2JitLearn", "profile main=%zu callback=%zu renderer=%zu",
         profile->Loaded(bl2::jit::kMain).size(), profile->Loaded(bl2::jit::kCallback).size(),
         profile->Loaded(bl2::jit::kRenderer).size());
    // Periodic persistence off the game threads; onPause requests one more.
    std::thread([profile] {
        pthread_setname_np(pthread_self(), "BL2JitLearn");
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(s_learn_save_mutex);
                s_learn_save_cv.wait_for(lock, std::chrono::seconds(45),
                                         [] { return s_learn_save_requested; });
                s_learn_save_requested = false;
            }
            profile->Save();
        }
    }).detach();
}

void DynarmicHost::RequestLearnedJitSave() {
    if (!m_learned_jit) return;
    {
        std::lock_guard<std::mutex> lock(s_learn_save_mutex);
        s_learn_save_requested = true;
    }
    s_learn_save_cv.notify_one();
}

void DynarmicHost::RecordLearnedBlock(size_t role, uint64_t descriptor) {
    if (m_learned_jit && role < bl2::jit::kRoleCount)
        m_learned_jit->Record(static_cast<bl2::jit::Role>(role), descriptor);
}

void DynarmicHost::WarmLearnedAtEntry(Dynarmic::A32::Jit& jit, size_t role) {
    s_learn_jits[role] = &jit;
    if (!m_learned_jit) return;
    const auto& list = m_learned_jit->Loaded(static_cast<bl2::jit::Role>(role));
    const uint64_t deadline = LearnedNowNs() + kLearnedEntryBudgetNs;
    size_t& cursor = s_learn_cursor[role];
    const size_t first = cursor;
    try {
        while (cursor < list.size()) {
            if (!jit.Precompile(list[cursor])) break;
            ++cursor;
            if ((cursor & 15u) == 0 && LearnedNowNs() >= deadline) break;
        }
    } catch (...) {
        s_learn_exhausted[role] = true;
    }
    __android_log_print(ANDROID_LOG_INFO, "BL2JitLearn", "entry role=%zu compiled=%zu/%zu",
                        role, cursor - first, list.size());
}

void DynarmicHost::WarmLearnedBlocksInCallback(Dynarmic::A32::Jit& jit, uint64_t budget_ns) {
    if (!m_learned_jit || !budget_ns) return;
    size_t role = 0;
    while (role < s_learn_jits.size() && s_learn_jits[role] != &jit) ++role;
    if (role == s_learn_jits.size() || s_learn_exhausted[role]) return;
    const auto& list = m_learned_jit->Loaded(static_cast<bl2::jit::Role>(role));
    size_t& cursor = s_learn_cursor[role];
    if (cursor >= list.size()) return;
    const uint64_t deadline = LearnedNowNs() + budget_ns;
    try {
        while (cursor < list.size()) {
            if (!jit.PrecompileInCallback(list[cursor], kLearnedMinFreeCode)) {
                s_learn_exhausted[role] = true;
                break;
            }
            ++cursor;
            if ((cursor & 7u) == 0 && LearnedNowNs() >= deadline) break;
        }
    } catch (...) {
        s_learn_exhausted[role] = true;
    }
    if (cursor >= list.size() || s_learn_exhausted[role])
        __android_log_print(ANDROID_LOG_INFO, "BL2JitLearn", "idle role=%zu done=%zu/%zu exhausted=%d",
             role, cursor, list.size(), s_learn_exhausted[role]);
}

void DynarmicHost::WarmupForEntry(ThreadContext& context, uint32_t target_pc,
                                const std::vector<uint32_t>& args) {
    // The caller holds the owner context lock, before either core enters Run.
    if (context.is_running_jit || context.is_running_callback_jit) return;
    auto warm = [&](Dynarmic::A32::Jit& jit, Bl2JitWarmup::Role role) {
        const uint8_t bit = uint8_t{1} << role;
        if (context.warmed_roles & bit) return;
        context.warmed_roles |= bit;
        size_t compiled = 0;
        try {
            for (uint32_t pc : m_jit_warmup.PCs(role)) {
                if (!jit.Precompile(pc)) break;
                ++compiled;
            }
        } catch (...) {
            LOGE("JIT warmup role=%zu interrupted after=%zu; retaining ordinary misses",
                 static_cast<size_t>(role), compiled);
        }
        LOGI("JIT warmup role=%zu compiled=%zu/%zu",
             static_cast<size_t>(role), compiled, m_jit_warmup.PCs(role).size());
        WarmLearnedAtEntry(jit, static_cast<size_t>(role));
    };
    if (m_warm_native_render && target_pc == m_warm_native_render) {
        warm(*context.jit, Bl2JitWarmup::Main);
        warm(*context.callback_jit, Bl2JitWarmup::Callback);
    } else if (m_warm_thread_proc && target_pc == m_warm_thread_proc &&
               m_warm_render_vtable && !args.empty() && args[0] &&
               uint64_t{args[0]} + 0x10 <= m_memory_size) {
        // _ThreadProc serves streaming/audio workers too. Its Runnable is +0xC.
        const uint32_t runnable = MemoryRead32(args[0] + 0xC);
        if (runnable && uint64_t{runnable} + 4 <= m_memory_size &&
            MemoryRead32(runnable) == m_warm_render_vtable)
            warm(*context.jit, Bl2JitWarmup::Renderer);
    }
}

uint64_t DynarmicHost::RunFunction64(uint32_t target_pc, const std::vector<uint32_t>& args) {
    ThreadContext* context = GetOrCreateThreadContext();
    if (!context || !context->jit) return 0;
    std::lock_guard<std::recursive_mutex> lock(context->mutex);

    if (context->is_running_jit) {
        // A Java call made from a guest thunk can synchronously re-enter one of
        // Horn's registered natives on the same host thread. The primary JIT is
        // not re-entrant, but the context owns a second core expressly for this.
        return RunGuestCallback64(target_pc, args);
    }

#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    if (context->physx_cookie.phase == Bl2PhysxCookieWatch::Phase::Tripped) return 0;
    PreparePhysxCookieCore(*context, target_pc);
    PhysxCookieScope cookie_scope(context->physx_table ? &context->physx_cookie : nullptr);
#endif
    WarmupForEntry(*context, target_pc, args);
    context->is_running_jit = true;
    struct JitRunningGuard {
        bool& flag;
        ~JitRunningGuard() { flag = false; }
    } running_guard{context->is_running_jit};

    return ExecuteFunction(*context->jit, target_pc, args, context->stack_top,
                           context->primary_stack_capacity);
}

uint32_t DynarmicHost::RunGuestCallback(uint32_t target_pc,
                                        const std::vector<uint32_t>& args) {
    return static_cast<uint32_t>(RunGuestCallback64(target_pc, args));
}

uint64_t DynarmicHost::RunGuestCallback64(uint32_t target_pc,
                                          const std::vector<uint32_t>& args) {
    ThreadContext* context = GetOrCreateThreadContext();
    if (!context) return 0;
    std::lock_guard<std::recursive_mutex> lock(context->mutex);
    if (!context->is_running_jit || s_active_host != this || !s_active_jit) {
        return RunFunction64(target_pc, args);
    }
    if (context->is_running_callback_jit) {
        // A third nested A32 core would need another independently aligned guest
        // stack and processor ID. Preserve void callbacks for the next safe entry,
        // but make the unsupported synchronous-return case visible.
        LOGE("Nested guest callback recursion at 0x%08X deferred; synchronous return unavailable",
             target_pc);
        context->deferred.emplace_back(target_pc, args);
        return 0;
    }
    context->is_running_callback_jit = true;
    struct CallbackGuard {
        bool& flag;
        ~CallbackGuard() { flag = false; }
    } callback_guard{context->is_running_callback_jit};
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    // The callback core remains an explicitly uncovered core.
    PhysxCookieScope cookie_scope(nullptr);
#endif
    return ExecuteFunction(*context->callback_jit, target_pc, args,
                           context->callback_stack_top, kCallbackStackSize);
}

bool DynarmicHost::CallGuest(uint32_t target_pc, const std::vector<uint32_t>& args, uint32_t& result) {
    ThreadContext* context = GetOrCreateThreadContext();
    if (!context || !context->jit || !context->callback_jit || !target_pc || args.size() > 7) return false;
    std::lock_guard<std::recursive_mutex> lock(context->mutex);
    if (s_active_host != this || !s_active_jit) return false;
    Dynarmic::A32::Jit* selected = nullptr;
    bool* running = nullptr;
    uint32_t stack_top = 0;
    if (context->is_running_jit && s_active_jit == context->jit.get() && !context->is_running_callback_jit) {
        selected = context->callback_jit.get(); running = &context->is_running_callback_jit;
        stack_top = context->callback_stack_top;
    } else if (context->is_running_callback_jit && s_active_jit == context->callback_jit.get() && !context->is_running_sync_jit) {
        // A callback can itself hit a reviewed UI hook. Its required return
        // uses one separate core/stack; neither suspended JIT is re-entered.
        if (!context->sync_jit) {
            const uint32_t stack = GuestAlloc(kCallbackStackSize,16);
            if (!stack || uint64_t{stack} + kCallbackStackSize > m_memory_size ||
                uint64_t{stack} + kCallbackStackSize > UINT32_MAX) {
                if (stack) GuestFree(stack);
                return false;
            }
            std::unique_ptr<Dynarmic::A32::Jit> sync;
            try { sync = CreateJit(kMaxThreadContexts * 2 + context->slot); }
            catch (...) { GuestFree(stack); throw; }
            if (!sync) { GuestFree(stack); return false; }
            // InvalidateGuestCode takes this same lock: publication cannot
            // race a foreign cache invalidation or expose a half-built core.
            std::lock_guard<std::mutex> publish(m_contexts_mutex);
            context->sync_stack_top = stack + kCallbackStackSize;
            context->sync_jit = std::move(sync);
        }
        selected = context->sync_jit.get(); running = &context->is_running_sync_jit;
        stack_top = context->sync_stack_top;
    } else return false; // Unsupported depth fails before execution, never defers.
    auto& jit = *selected;
    const auto regs = jit.Regs();
    const auto ext = jit.ExtRegs();
    const auto cpsr = jit.Cpsr(), fpscr = jit.Fpscr();
    struct CallbackState {
        Dynarmic::A32::Jit& jit;
        const std::array<uint32_t,16>& regs;
        const std::array<uint32_t,64>& ext;
        uint32_t cpsr, fpscr;
        bool& running;
        ~CallbackState() {
            jit.Regs() = regs; jit.ExtRegs() = ext;
            jit.SetCpsr(cpsr); jit.SetFpscr(fpscr); running = false;
        }
    } restore{jit,regs,ext,cpsr,fpscr,*running};
    jit.SetCpsr(s_active_jit->Cpsr());
    jit.SetFpscr(s_active_jit->Fpscr());
    *running = true;
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    PhysxCookieScope cookie_scope(nullptr);
#endif
    bool returned = false;
    const auto value = ExecuteFunction(jit,target_pc,args,stack_top,kCallbackStackSize,&returned);
    if (returned) result = static_cast<uint32_t>(value);
    return returned;
}

uint64_t DynarmicHost::ExecuteFunction(Dynarmic::A32::Jit& jit, uint32_t target_pc,
                                       const std::vector<uint32_t>& args,
                                       uint32_t stack_top, uint32_t stack_capacity, bool* returned) {
    if (returned) *returned = false;
    Dynarmic::A32::Jit* previous_active_jit = s_active_jit;
    DynarmicHost* previous_active_host = s_active_host;
    s_active_jit = &jit;
    s_active_host = this;
    const std::uint64_t previous_ticks_remaining = s_ticks_remaining;
    const bool sample_guest_pc = HornPerfEnabled();
    const uint32_t native_render_entry =
        FindSymbol("_Z21NativeCallback_RenderPv");
    const uint32_t android_thread_proc =
        FindSymbol("_ZN22FRunnableThreadAndroid11_ThreadProcEPv");
    const uint32_t render_fence_wait = sample_guest_pc
        ? FindSymbol("_ZNK19FRenderCommandFence4WaitEj") : 0;
    const uint32_t is_threaded_rendering = sample_guest_pc
        ? FindSymbol("GIsThreadedRendering") : 0;
    const uint32_t use_threaded_rendering = sample_guest_pc
        ? FindSymbol("GUseThreadedRendering") : 0;
    const uint32_t rendering_thread = sample_guest_pc
        ? FindSymbol("GRenderingThread") : 0;
    const uint32_t rendering_thread_runnable = sample_guest_pc
        ? FindSymbol("GRenderingThreadRunnable") : 0;
    const uint32_t rendering_thread_healthy = sample_guest_pc
        ? FindSymbol("GIsRenderingThreadHealthy") : 0;
    const bool trace_guest_exit =
        sample_guest_pc &&
        ((native_render_entry && target_pc == native_render_entry) ||
         (android_thread_proc && target_pc == android_thread_proc));
    const std::uint64_t guest_sample_tick_budget =
        trace_guest_exit ? kGuestExitTraceTickBudget : kGuestSampleTickBudget;
    std::array<uint32_t, kGuestExitTracePcCount> recent_guest_pcs{};
    size_t recent_guest_pc_count = 0;
    size_t recent_guest_pc_next = 0;
    uint32_t last_fence_object = 0;
    uint32_t last_fence_count = 0;
    uint32_t last_fence_caller = 0;
    uint32_t last_threaded_rendering = is_threaded_rendering
        ? MemoryRead32(is_threaded_rendering) : 0xFFFFFFFFu;
    uint32_t last_use_threaded_rendering = use_threaded_rendering
        ? MemoryRead32(use_threaded_rendering) : 0xFFFFFFFFu;
    uint32_t last_rendering_thread = rendering_thread
        ? MemoryRead32(rendering_thread) : 0xFFFFFFFFu;
    uint32_t last_rendering_thread_runnable = rendering_thread_runnable
        ? MemoryRead32(rendering_thread_runnable) : 0xFFFFFFFFu;
    if (sample_guest_pc) s_ticks_remaining = guest_sample_tick_budget;
    struct ActiveJitGuard {
        Dynarmic::A32::Jit*& jit_slot;
        Dynarmic::A32::Jit* previous_jit;
        DynarmicHost*& host_slot;
        DynarmicHost* previous_host;
        std::uint64_t& ticks_slot;
        std::uint64_t previous_ticks;
        ~ActiveJitGuard() {
            jit_slot = previous_jit;
            host_slot = previous_host;
            ticks_slot = previous_ticks;
        }
    } active_guard{s_active_jit, previous_active_jit, s_active_host,
                   previous_active_host, s_ticks_remaining,
                   previous_ticks_remaining};

    // Reset argument registers for the call. Leaving unused registers from the
    // previous guest entry makes malformed legacy prototypes nondeterministic.
    for (size_t i = 0; i < 4; ++i) {
        jit.Regs()[i] = 0;
    }
    for (size_t i = 0; i < args.size() && i < 4; ++i) {
        jit.Regs()[i] = args[i];
    }

    // Stack pointer
    jit.Regs()[13] = stack_top;

    // If more than 4 arguments, push onto guest stack
    if (args.size() > 4) {
        const size_t extra_args = args.size() - 4;
        if (extra_args > (std::numeric_limits<uint32_t>::max() / 4u) - 1u) {
            LOGE("Guest call at 0x%08X has an unrepresentable argument vector", target_pc);
            return 0;
        }
        const size_t stack_words = (extra_args + 1u) & ~size_t{1};
        const uint64_t stack_bytes = static_cast<uint64_t>(stack_words) * 4u;
        if (stack_bytes > stack_capacity) {
            LOGE("Guest call at 0x%08X needs %llu argument bytes, stack capacity is %u",
                 target_pc, static_cast<unsigned long long>(stack_bytes), stack_capacity);
            return 0;
        }
        const uint32_t sp = stack_top - static_cast<uint32_t>(stack_bytes);
        jit.Regs()[13] = sp;
        for (size_t i = 0; i < extra_args; ++i) {
            MemoryWrite32(sp + static_cast<uint32_t>(i * 4), args[4 + i]);
        }
    }

    // Return address -> exit trap (with Thumb bit if target is Thumb)
    bool is_thumb = (target_pc & 1) != 0;
    jit.Regs()[14] = is_thumb ? (EXIT_TRAP_THUMB | 1) : EXIT_TRAP_ARM;

    // Set PC
    jit.Regs()[15] = target_pc & ~1u;
    jit.SetCpsr((jit.Cpsr() & ~0x20u) | (is_thumb ? 0x20u : 0u));

    jit.ClearHalt();
    // pthread_exit uses a dedicated halt bit so an orderly guest-thread exit
    // is distinguishable from assertion/JNI/runaway failures. Both the primary
    // and callback JIT are halted, so clear this bit whenever either core is
    // entered again (for example, to run POSIX TLS destructors).
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined2);
    jit.ClearHalt(Dynarmic::HaltReason::UserDefined3);
    Dynarmic::HaltReason hr;
    for (;;) {
        hr = jit.Run();
        // InvalidateCacheRange is deliberately allowed from another host
        // thread. Dynarmic applies the queued range before returning, so resume
        // at the preserved A32 state when invalidation was the only halt reason.
        // Without this loop a trampoline emitted inside an SVC, or a foreign
        // pthread invalidation, would look like a completed guest function.
        if (static_cast<uint32_t>(hr) ==
            static_cast<uint32_t>(Dynarmic::HaltReason::CacheInvalidation)) {
            continue;
        }
        // With cycle counting enabled, a zero halt reason means the coarse
        // instruction budget expired. Record the resumed guest PC and continue
        // the same function with a fresh budget. Real exits use an explicit
        // halt reason from the exit-trap SVC and therefore never enter here.
        if (!hr && sample_guest_pc && s_ticks_remaining == 0) {
            const uint32_t sampled_pc = jit.Regs()[15];
            HornPerfRecordGuestSample(sampled_pc);
            if (trace_guest_exit && is_threaded_rendering &&
                use_threaded_rendering && rendering_thread &&
                rendering_thread_runnable) {
                const uint32_t threaded = MemoryRead32(is_threaded_rendering);
                const uint32_t use_threaded = MemoryRead32(use_threaded_rendering);
                const uint32_t thread = MemoryRead32(rendering_thread);
                const uint32_t runnable = MemoryRead32(rendering_thread_runnable);
                if (threaded != last_threaded_rendering ||
                    use_threaded != last_use_threaded_rendering ||
                    thread != last_rendering_thread ||
                    runnable != last_rendering_thread_runnable) {
                    std::array<uint32_t, 9> thread_fields{};
                    const bool thread_object_valid = thread &&
                        static_cast<uint64_t>(thread) +
                            thread_fields.size() * sizeof(uint32_t) <=
                            GetGuestMemorySize();
                    if (thread_object_valid) {
                        for (size_t i = 0; i < thread_fields.size(); ++i) {
                            thread_fields[i] = MemoryRead32(
                                thread + static_cast<uint32_t>(i * 4));
                        }
                    }
                    LOGI("Render thread state target=0x%08X PC=0x%08X LR=0x%08X "
                         "SP=0x%08X threaded=%u->%u use=%u->%u "
                         "thread=0x%08X->0x%08X runnable=0x%08X->0x%08X "
                         "object_valid=%d fields=[%08X %08X %08X %08X %08X "
                         "%08X %08X %08X %08X]",
                         target_pc, sampled_pc, jit.Regs()[14], jit.Regs()[13],
                         last_threaded_rendering, threaded,
                         last_use_threaded_rendering, use_threaded,
                         last_rendering_thread, thread,
                         last_rendering_thread_runnable, runnable,
                         thread_object_valid ? 1 : 0,
                         thread_fields[0], thread_fields[1], thread_fields[2],
                         thread_fields[3], thread_fields[4], thread_fields[5],
                         thread_fields[6], thread_fields[7], thread_fields[8]);
                    last_threaded_rendering = threaded;
                    last_use_threaded_rendering = use_threaded;
                    last_rendering_thread = thread;
                    last_rendering_thread_runnable = runnable;
                }
            }
            // A fence that remains inside Wait for an entire sampling quantum
            // is abnormal. Capture the saved LR and live counter here rather
            // than guessing which of UE3's many Wait call sites owns the hang.
            // Wait's ARM prologue pushes R4-R11/LR and reserves 0x24 bytes, so
            // the caller LR is at current SP + 0x44 while the loop is active.
            if (render_fence_wait &&
                sampled_pc >= render_fence_wait &&
                sampled_pc < render_fence_wait + 0x198u) {
                const uint32_t stack_pointer = jit.Regs()[13];
                const uint32_t fence_object = jit.Regs()[4];
                const uint32_t fence_target = jit.Regs()[5];
                const uint32_t fence_count = MemoryRead32(fence_object);
                const uint32_t caller = MemoryRead32(stack_pointer + 0x44u);
                if (fence_object != last_fence_object ||
                    fence_count != last_fence_count ||
                    caller != last_fence_caller) {
                    const uint32_t thread = rendering_thread
                        ? MemoryRead32(rendering_thread) : 0;
                    std::array<uint32_t, 9> thread_fields{};
                    if (thread &&
                        static_cast<uint64_t>(thread) +
                            thread_fields.size() * sizeof(uint32_t) <=
                            GetGuestMemorySize()) {
                        for (size_t i = 0; i < thread_fields.size(); ++i) {
                            thread_fields[i] = MemoryRead32(
                                thread + static_cast<uint32_t>(i * 4));
                        }
                    }
                    LOGI("Render fence wait object=0x%08X count=%u target=%u "
                         "caller=0x%08X PC=0x%08X SP=0x%08X threaded=%u "
                         "use_threaded=%u thread=0x%08X healthy=%u "
                         "thread_fields=[%08X %08X %08X %08X %08X %08X "
                         "%08X %08X %08X] "
                         "stack=[%08X %08X %08X %08X %08X %08X]",
                         fence_object, fence_count, fence_target, caller,
                         sampled_pc, stack_pointer,
                         is_threaded_rendering
                             ? MemoryRead32(is_threaded_rendering) : 0xFFFFFFFFu,
                         use_threaded_rendering
                             ? MemoryRead32(use_threaded_rendering) : 0xFFFFFFFFu,
                         rendering_thread
                             ? MemoryRead32(rendering_thread) : 0xFFFFFFFFu,
                         rendering_thread_healthy
                             ? MemoryRead32(rendering_thread_healthy) : 0xFFFFFFFFu,
                         thread_fields[0], thread_fields[1], thread_fields[2],
                         thread_fields[3], thread_fields[4], thread_fields[5],
                         thread_fields[6], thread_fields[7], thread_fields[8],
                         MemoryRead32(stack_pointer + 0x44u),
                         MemoryRead32(stack_pointer + 0x48u),
                         MemoryRead32(stack_pointer + 0x4Cu),
                         MemoryRead32(stack_pointer + 0x50u),
                         MemoryRead32(stack_pointer + 0x54u),
                         MemoryRead32(stack_pointer + 0x58u));
                    last_fence_object = fence_object;
                    last_fence_count = fence_count;
                    last_fence_caller = caller;
                }
            }
            if (trace_guest_exit) {
                recent_guest_pcs[recent_guest_pc_next] = sampled_pc;
                recent_guest_pc_next =
                    (recent_guest_pc_next + 1) % recent_guest_pcs.size();
                recent_guest_pc_count =
                    std::min(recent_guest_pc_count + 1, recent_guest_pcs.size());
            }
            s_ticks_remaining = guest_sample_tick_budget;
            continue;
        }
        break;
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    ReportPhysxCookie(jit); // MemoryAbort is never resumed; preserve its first event.
#endif
    uint32_t final_pc = jit.Regs()[15];
    const bool expected_thread_exit = Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2);
    if (trace_guest_exit) {
        const uint32_t stack_used =
            jit.Regs()[13] <= stack_top ? stack_top - jit.Regs()[13] : 0;
        LOGI("Guest exit trace target=0x%08X reason=0x%X PC=0x%08X LR=0x%08X "
             "R0=0x%08X R1=0x%08X SP=0x%08X stack_top=0x%08X used=%u CPSR=0x%08X samples=%zu",
             target_pc, static_cast<unsigned>(hr), final_pc, jit.Regs()[14],
             jit.Regs()[0], jit.Regs()[1], jit.Regs()[13], stack_top,
             stack_used, jit.Cpsr(), recent_guest_pc_count);
        LOGI("Guest exit registers R2=%08X R3=%08X R4=%08X R5=%08X "
             "R6=%08X R7=%08X R8=%08X R9=%08X R10=%08X R11=%08X R12=%08X "
             "threaded=%u use=%u render_thread=%08X",
             jit.Regs()[2], jit.Regs()[3], jit.Regs()[4], jit.Regs()[5],
             jit.Regs()[6], jit.Regs()[7], jit.Regs()[8], jit.Regs()[9],
             jit.Regs()[10], jit.Regs()[11], jit.Regs()[12],
             is_threaded_rendering ? MemoryRead32(is_threaded_rendering) : 0xFFFFFFFFu,
             use_threaded_rendering ? MemoryRead32(use_threaded_rendering) : 0xFFFFFFFFu,
             rendering_thread ? MemoryRead32(rendering_thread) : 0);
        const size_t first = recent_guest_pc_count == recent_guest_pcs.size()
            ? recent_guest_pc_next : 0;
        for (size_t offset = 0; offset < recent_guest_pc_count; offset += 8) {
            char line[256] = {};
            size_t used = 0;
            const size_t end = std::min(recent_guest_pc_count, offset + 8);
            for (size_t i = offset; i < end && used < sizeof(line); ++i) {
                const uint32_t pc =
                    recent_guest_pcs[(first + i) % recent_guest_pcs.size()];
                const int written = std::snprintf(
                    line + used, sizeof(line) - used, " 0x%08X", pc);
                if (written < 0) break;
                used = std::min(sizeof(line), used + static_cast<size_t>(written));
            }
            LOGI("Guest exit trace PCs %02zu-%02zu:%s", offset, end - 1, line);
        }
    }
    // A fault at zero may leave architectural PC at4, which is also the ARM
    // return trap address. The halt cause must win over that numeric overlap.
    const bool guest_exception = Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined3);
    if (guest_exception || (!expected_thread_exit &&
        final_pc != EXIT_TRAP_ARM && final_pc != (EXIT_TRAP_ARM + 4) &&
        final_pc != (EXIT_TRAP_THUMB & ~1u) && final_pc != ((EXIT_TRAP_THUMB & ~1u) + 2))) {
#if defined(__ANDROID__)
        static std::atomic<unsigned> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
            __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                "Guest halt target=%08X reason=%X PC=%08X LR=%08X SP=%08X CPSR=%08X R0=%08X R1=%08X",
                target_pc, static_cast<unsigned>(hr), final_pc, jit.Regs()[14],
                jit.Regs()[13], jit.Cpsr(), jit.Regs()[0], jit.Regs()[1]);
#endif
        LOGI("RunFunction(0x%08X) unexpected halt with reason=0x%X, PC=0x%08X, LR=0x%08X, R0=0x%08X, SP=0x%08X",
             target_pc, static_cast<unsigned>(hr), final_pc, jit.Regs()[14], jit.Regs()[0], jit.Regs()[13]);
    }

    if (returned) *returned = !expected_thread_exit && !guest_exception &&
        hr == Dynarmic::HaltReason::UserDefined1 &&
        (final_pc == EXIT_TRAP_ARM || final_pc == EXIT_TRAP_ARM + 4 ||
         final_pc == EXIT_TRAP_THUMB || final_pc == EXIT_TRAP_THUMB + 2);
    return (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
}

void DynarmicHost::InvalidateGuestCode(uint32_t addr, size_t size) {
    if (size == 0) return;
    // Dynarmic's invalidation API is cross-thread aware: it queues the range,
    // atomically raises CacheInvalidation and wakes a core already in Run().
    // The collection lock prevents teardown, publication, or optional
    // diagnostic idle-core replacement while we visit every core.
    std::lock_guard<std::mutex> contexts_lock(m_contexts_mutex);
    for (const auto& context : m_contexts) {
        context->jit->InvalidateCacheRange(addr, size);
        context->callback_jit->InvalidateCacheRange(addr, size);
        if (context->sync_jit) context->sync_jit->InvalidateCacheRange(addr, size);
    }
}

void DynarmicHost::DrainDeferred() {
    ThreadContext* context = GetOrCreateThreadContext();
    if (!context) return;
    std::lock_guard<std::recursive_mutex> lock(context->mutex);
    if (context->is_running_jit || context->deferred.empty()) return;
    auto batch = std::move(context->deferred);
    context->deferred.clear();
    for (auto& call : batch) {
        LOGI("Running deferred call to 0x%08X", call.first);
        RunFunction(call.first, call.second);
    }
}

// -------------------------------------------------------------
// Guest heap
//
// Every block carries a 16-byte header immediately before its payload:
//   [-16] magic  [-12] block base  [-8] capacity  [-4] payload size
// The magic is what makes GuestFree safe: a pointer that does not carry it is
// not ours (module data, an interior pointer, a stack top, something the guest
// invented) and is ignored -- exactly as a no-op free ignored everything.
// Freeing clears the magic, so a double free is ignored too.
//
// A plain bump allocator with an empty GuestFree is the usual first version and
// it is a guaranteed leak: the guest does call free(), nobody listens. Measured
// on a Scaleform title that was ~1 MB/s during gameplay.
// -------------------------------------------------------------
namespace {
constexpr uint32_t kBlockMagic = 0x41524D31; // 'ARM1' - live block
constexpr uint32_t kFreedMagic = 0x41524D46; // 'ARMF' - already on a free list
constexpr uint32_t kHeaderSize = 16;
}

uint32_t DynarmicHost::GuestAlloc(size_t size, size_t alignment) {
    HornPerfScope perf_scope(HornPerfCategory::GuestAlloc, size);
    if (size == 0) size = 1;
    if (alignment < 8) alignment = 8;
    if (alignment > std::numeric_limits<uint32_t>::max() ||
        (alignment & (alignment - 1)) != 0 ||
        size > std::numeric_limits<uint32_t>::max()) {
        LOGE("GuestAlloc invalid size/alignment: size=%zu alignment=%zu", size, alignment);
        return 0;
    }
    // Worst case layout: header, then up to a full alignment step of padding.
    const uint64_t need = static_cast<uint64_t>(size) + alignment + kHeaderSize;
    const uint64_t rounded_need = (need + 15u) & ~uint64_t{15};
    if (rounded_need > std::numeric_limits<uint32_t>::max()) {
        LOGE("GuestAlloc request cannot be represented in ARM32: size=%zu alignment=%zu",
             size, alignment);
        return 0;
    }

    uint32_t base = 0, capacity = 0;
    bool recycled = false;
    // BL2: blocks above the largest class come from the coalescing large range;
    // the shared heap below stays the fallback when that range is exhausted.
    const bool large = need > kMaxClassSize && m_large_enabled.load(std::memory_order_acquire) &&
                       LargeAlloc(rounded_need, base, capacity, recycled);
    if (large) {
    } else if (need <= kMaxClassSize) {
        int cls = 0;
        while ((1u << (cls + kMinClassShift)) < need) ++cls;
        capacity = 1u << (cls + kMinClassShift);
        {
            std::lock_guard<std::mutex> lk(m_free_small_mutex[cls]);
            auto& fl = m_free_small[cls];
            if (!fl.empty()) { base = fl.back(); fl.pop_back(); recycled = true; }
        }
    } else {
        capacity = static_cast<uint32_t>(rounded_need);
        {
            std::lock_guard<std::mutex> lk(m_free_huge_mutex);
            auto it = m_free_huge.find(capacity);
            if (it != m_free_huge.end() && !it->second.empty()) {
                base = it->second.back();
                it->second.pop_back();
                if (it->second.empty()) m_free_huge.erase(it);
                recycled = true;
            }
        }
    }
    if (!large && !recycled) {
        std::lock_guard<std::mutex> lock(m_free_huge_mutex);
        auto fit = m_free_huge.end();
        // ponytail: scan reusable spans on an exact-class miss; order the map only if this scan becomes measurable.
        for (auto it = m_free_huge.begin(); it != m_free_huge.end(); ++it) {
            if (!it->second.empty() && it->first >= capacity &&
                (fit == m_free_huge.end() || it->first < fit->first)) fit = it;
        }
        if (fit != m_free_huge.end()) {
            const uint32_t remainder = fit->first - capacity;
            base = fit->second.back();
            fit->second.pop_back();
            if (fit->second.empty()) m_free_huge.erase(fit);
            if (remainder >= (1u << kMinClassShift))
                m_free_huge[remainder].push_back(base + capacity);
            recycled = true;
        }
    }
    if (!large && !recycled) {
        // Lock-free bump allocation. Free-list reuse remains synchronized per
        // size class, while fresh allocations from unrelated guest threads no
        // longer contend on one process-wide mutex.
        uint32_t current = m_heap_current.load(std::memory_order_relaxed);
        for (;;) {
            if (static_cast<uint64_t>(current) + capacity > kHeapLimit) {
                static std::atomic<bool> heap_failure_logged{false};
                if (!heap_failure_logged.exchange(true, std::memory_order_relaxed)) {
                    uint64_t free_small = 0, free_huge = 0, fit_bytes = 0;
                    size_t fit_count = 0;
                    uint32_t largest_huge = 0;
                    for (int cls = 0; cls < kNumSizeClasses; ++cls) {
                        std::lock_guard<std::mutex> lock(m_free_small_mutex[cls]);
                        free_small += uint64_t(m_free_small[cls].size()) << (cls + kMinClassShift);
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_free_huge_mutex);
                        for (const auto& [bytes, blocks] : m_free_huge) {
                            free_huge += uint64_t(bytes) * blocks.size();
                            if (!blocks.empty()) largest_huge = std::max(largest_huge, bytes);
                            if (bytes >= capacity) {
                                fit_count += blocks.size();
                                fit_bytes += uint64_t(bytes) * blocks.size();
                            }
                        }
                    }
                    const uint64_t reserved = m_heap_current.load(std::memory_order_relaxed) - kHeapBase;
                    const uint64_t free_bytes = free_small + free_huge;
                    LOGE("Guest heap failure snapshot requested=%zu alignment=%zu capacity=%u "
                         "reserved=%llu tail=%u free_small=%llu free_huge=%llu "
                         "live_reserved_estimate=%llu huge_fit_count=%zu huge_fit_bytes=%llu "
                         "largest_huge=%u; concurrent snapshot",
                         size, alignment, capacity, static_cast<unsigned long long>(reserved),
                         kHeapLimit - current, static_cast<unsigned long long>(free_small),
                         static_cast<unsigned long long>(free_huge),
                         static_cast<unsigned long long>(reserved > free_bytes ? reserved - free_bytes : 0),
                         fit_count, static_cast<unsigned long long>(fit_bytes), largest_huge);
#if defined(__ANDROID__)
                    __android_log_print(ANDROID_LOG_ERROR, "BL2GuestHeap",
                        "snapshot reserved=%lluMiB free_small=%lluMiB free_huge=%lluMiB "
                        "largest_huge=%uKiB huge_fit=%zu map_used=%lluMiB",
                        static_cast<unsigned long long>(reserved >> 20),
                        static_cast<unsigned long long>(free_small >> 20),
                        static_cast<unsigned long long>(free_huge >> 20), largest_huge >> 10,
                        fit_count,
                        static_cast<unsigned long long>(
                            (m_map_current.load(std::memory_order_relaxed) - kMapBase) >> 20));
#endif
                    if (s_active_jit) {
                        const auto& registers = s_active_jit->Regs();
                        LOGE("Guest heap failure args r0..r3={%08X,%08X,%08X,%08X}",
                             registers[0], registers[1], registers[2], registers[3]);
                    }
                }
                Dl_info caller_info{};
                const void* caller = __builtin_return_address(0);
                const bool found_caller = dladdr(caller, &caller_info) != 0 &&
                                          caller_info.dli_fbase != nullptr;
                const uintptr_t caller_rva = found_caller
                    ? reinterpret_cast<uintptr_t>(caller) -
                      reinterpret_cast<uintptr_t>(caller_info.dli_fbase)
                    : 0;
                const uint32_t guest_pc = s_active_jit ? s_active_jit->Regs()[15] : 0;
                const uint32_t guest_lr = s_active_jit ? s_active_jit->Regs()[14] : 0;
                LOGE("GuestAlloc out of guest heap: at 0x%08X, want %u, limit 0x%08X caller_rva=0x%llX guest_pc=0x%08X guest_lr=0x%08X",
                     current, capacity, kHeapLimit,
                     static_cast<unsigned long long>(caller_rva), guest_pc, guest_lr);
#if defined(__ANDROID__)
                // Quiet builds too: a NULL here becomes a NULL-based guest copy.
                static std::atomic<uint32_t> oom_reports{0};
                if (oom_reports.fetch_add(1, std::memory_order_relaxed) < 16)
                    __android_log_print(ANDROID_LOG_ERROR, "BL2GuestHeap",
                        "out of guest heap: want %u at 0x%08X (limit 0x%08X) guest_pc=%08X guest_lr=%08X",
                        capacity, current, kHeapLimit, guest_pc, guest_lr);
#endif
                return 0;
            }
            if (m_heap_current.compare_exchange_weak(
                    current, current + capacity,
                    std::memory_order_relaxed, std::memory_order_relaxed)) {
                base = current;
                break;
            }
        }
    }

    uint32_t payload = static_cast<uint32_t>(
        (static_cast<uint64_t>(base) + kHeaderSize + alignment - 1) & ~(static_cast<uint64_t>(alignment) - 1));
    if (recycled && static_cast<uint64_t>(payload) + size <= m_memory_size) {
        // Fresh arena memory has always been zero, and guest code that leaned on
        // that would break in confusing ways the first time it saw a recycled
        // block. Cheap insurance at the observed churn.
        memset(m_memory + payload, 0, size);
        NotifyGuestMemoryWritten(payload, size);
    }
    MemoryWrite32(payload - 12, base);
    MemoryWrite32(payload - 8, capacity);
    MemoryWrite32(payload - 4, static_cast<uint32_t>(size));
    __atomic_store_n(reinterpret_cast<uint32_t*>(m_memory + payload - 16),
                     kBlockMagic, __ATOMIC_RELEASE);
    NotifyGuestMemoryWritten(payload - 16, sizeof(uint32_t));
    return payload;
}

void DynarmicHost::GuestFree(uint32_t addr) {
    if (addr < kHeapBase + kHeaderSize) return;         // cannot be one of ours
    if (static_cast<uint64_t>(addr) > m_memory_size) return;
    uint32_t expected = kBlockMagic;
    if (!__atomic_compare_exchange_n(
            reinterpret_cast<uint32_t*>(m_memory + addr - 16), &expected,
            kFreedMagic, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return; // foreign pointer, already freed, or a concurrent double free
    }
    NotifyGuestMemoryWritten(addr - 16, sizeof(uint32_t));
    uint32_t base = MemoryRead32(addr - 12);
    uint32_t capacity = MemoryRead32(addr - 8);

    if (base >= kLargeBase && base < kLargeLimit) {
        LargeFree(base, capacity);
        return;
    }
    if (capacity <= kMaxClassSize) {
        int cls = 0;
        while ((1u << (cls + kMinClassShift)) < capacity) ++cls;
        if (cls >= kNumSizeClasses) return;
        std::lock_guard<std::mutex> lk(m_free_small_mutex[cls]);
        m_free_small[cls].push_back(base);
    } else {
        std::lock_guard<std::mutex> lk(m_free_huge_mutex);
        m_free_huge[capacity].push_back(base);
    }
}

bool DynarmicHost::LargeAlloc(uint64_t need, uint32_t& base, uint32_t& capacity, bool& dirty) {
    const uint64_t bytes = (need + kLargeGranule - 1) & ~uint64_t{kLargeGranule - 1};
    if (bytes > kLargeLimit - kLargeBase) return false;
    std::lock_guard<std::mutex> lock(m_large_mutex);
    // Best fit over coalesced spans; ties keep the lowest address.
    auto fit = m_large_free.end();
    for (auto it = m_large_free.begin(); it != m_large_free.end(); ++it)
        if (it->second >= bytes && (fit == m_large_free.end() || it->second < fit->second)) fit = it;
    if (fit != m_large_free.end()) {
        base = fit->first;
        const uint32_t rest = fit->second - static_cast<uint32_t>(bytes);
        m_large_free.erase(fit);
        if (rest) m_large_free.emplace(base + static_cast<uint32_t>(bytes), rest);
        capacity = static_cast<uint32_t>(bytes);
        dirty = true;
        return true;
    }
    if (uint64_t{m_large_top} + bytes > kLargeLimit) {
#if defined(__ANDROID__)
        static std::atomic<uint32_t> reports{0};
        if (reports.fetch_add(1, std::memory_order_relaxed) < 8)
            __android_log_print(ANDROID_LOG_WARN, "BL2GuestHeap",
                "large range full: want %llu KiB, top 0x%08X, %zu free spans; shared heap",
                static_cast<unsigned long long>(bytes >> 10), m_large_top, m_large_free.size());
#endif
        return false;
    }
    base = m_large_top;
    capacity = static_cast<uint32_t>(bytes);
    m_large_top += capacity;
    // A tail returned to the bump end keeps its old contents.
    dirty = base < m_large_high;
    m_large_high = std::max(m_large_high, m_large_top);
    return true;
}

void DynarmicHost::LargeFree(uint32_t base, uint32_t capacity) {
    std::lock_guard<std::mutex> lock(m_large_mutex);
    uint32_t begin = base, end = base + capacity;
    auto next = m_large_free.lower_bound(begin);
    if (next != m_large_free.end() && next->first == end) {
        end += next->second;
        next = m_large_free.erase(next);
    }
    if (next != m_large_free.begin()) {
        const auto previous = std::prev(next);
        if (previous->first + previous->second == begin) {
            begin = previous->first;
            m_large_free.erase(previous);
        }
    }
    if (end == m_large_top) m_large_top = begin;  // Give the tail back to the bump end.
    else m_large_free.emplace(begin, end - begin);
}

bool DynarmicHost::EnableLargeBlockRegion() {
    if (m_memory_size != GUEST_ADDRESS_SPACE ||
        m_map_current.load(std::memory_order_acquire) > kLargeBase) return false;
    m_large_enabled.store(true, std::memory_order_release);
    // Fail loudly at the source: coalescing, tail return, reuse and zeroing.
    bool ok = true;
    const uint32_t top = m_large_top;
    const uint32_t a = GuestAlloc(3u << 20, 0x1000), b = GuestAlloc(5u << 20, 0x1000);
    ok = ok && a >= kLargeBase && b >= kLargeBase && a < b && (a & 0xFFF) == 0 &&
         GuestAllocSize(a) == (3u << 20) && GuestAllocSize(b) == (5u << 20);
    if (ok) MemoryWrite32(a, 0xA5A5A5A5);
    const uint32_t c = GuestAlloc(2u << 20, 16);  // Keeps a, b off the tail.
    GuestFree(a);
    GuestFree(b);
    const uint32_t merged = GuestAlloc(8u << 20, 0x1000);  // Needs a and b coalesced.
    ok = ok && c && merged && merged < c && MemoryRead32(merged) == 0;
    GuestFree(merged);
    GuestFree(c);
    ok = ok && m_large_top == top && m_large_free.empty();
    if (!ok) {
        m_large_enabled.store(false, std::memory_order_release);
#if defined(__ANDROID__)
        __android_log_print(ANDROID_LOG_ERROR, "BL2GuestHeap", "large range self-check failed; disabled");
#endif
    }
    return ok;
}

uint32_t DynarmicHost::GuestAllocSize(uint32_t addr) {
    if (addr < kHeapBase + kHeaderSize) return 0;
    if (static_cast<uint64_t>(addr) > m_memory_size) return 0;
    if (__atomic_load_n(reinterpret_cast<uint32_t*>(m_memory + addr - 16),
                        __ATOMIC_ACQUIRE) != kBlockMagic) return 0;
    return MemoryRead32(addr - 4);
}

// Address space for the mmap thunk. Separate from the malloc heap on purpose -- see the
// declaration. No free list: a file mapping is handed to the engine for the life of the
// process, and the mmap thunk reuses an identical (file, offset, length) mapping instead of
// making a second one, which is what keeps a re-opened expansion file from costing the arena
// twice.
uint32_t DynarmicHost::GuestAllocMapping(size_t size, size_t alignment) {
    uint32_t current = m_map_current.load(std::memory_order_relaxed);
    for (;;) {
        const uint32_t aligned =
            (current + static_cast<uint32_t>(alignment - 1)) &
            ~static_cast<uint32_t>(alignment - 1);
        const uint32_t limit = m_large_enabled.load(std::memory_order_acquire) ? kLargeBase : kMapLimit;
        if (aligned < current || static_cast<uint64_t>(aligned) + size > limit) {
            LOGE("GuestAllocMapping out of mapping space: at 0x%08X, want %zu",
                 aligned, size);
            return 0;
        }
        const uint32_t next = aligned + static_cast<uint32_t>(size);
        if (m_map_current.compare_exchange_weak(
                current, next, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return aligned;
        }
    }
}

// Runs once at startup. A silently wrong allocator corrupts the guest far away
// from the cause, so pay a few microseconds to fail loudly at the source.
bool DynarmicHost::HeapSelfCheck() {
#define HEAP_CHECK(cond) do { if (!(cond)) { LOGE("HeapSelfCheck FAILED: %s", #cond); ok = false; } } while (0)
    bool ok = true;

    uint32_t a = GuestAlloc(100, 8);
    uint32_t b = GuestAlloc(100, 8);
    HEAP_CHECK(a != 0 && b != 0 && a != b);
    HEAP_CHECK((a & 7) == 0 && (b & 7) == 0);
    HEAP_CHECK(a + 100 <= b - kHeaderSize || b + 100 <= a - kHeaderSize); // no overlap
    HEAP_CHECK(GuestAllocSize(a) == 100);

    uint32_t aligned = GuestAlloc(64, 0x1000);
    HEAP_CHECK(aligned != 0 && (aligned & 0xFFF) == 0);

    // Freed blocks come back, and the payload is handed over zeroed.
    MemoryWrite32(a, 0xDEADBEEF);
    GuestFree(a);
    HEAP_CHECK(GuestAllocSize(a) == 0);        // magic cleared
    GuestFree(a);                              // double free: must be a no-op
    uint32_t reused = GuestAlloc(100, 8);
    HEAP_CHECK(reused == a);
    HEAP_CHECK(MemoryRead32(reused) == 0);

    // Pointers we did not hand out must be ignored, not recycled.
    GuestFree(0x40000000);
    GuestFree(kHeapBase + 4);
    GuestFree(b + 8);                          // interior pointer
    HEAP_CHECK(GuestAllocSize(b) == 100);      // b survived all of that

    // Large blocks recycle by exact capacity instead of rounding to a power of two.
    uint32_t big = GuestAlloc(8 * 1024 * 1024, 0x10000);
    HEAP_CHECK(big != 0 && (big & 0xFFFF) == 0);
    GuestFree(big);
    uint32_t big2 = GuestAlloc(8 * 1024 * 1024, 0x10000);
    HEAP_CHECK(big2 == big);
    const uint32_t split_begin = MemoryRead32(big2 - 12);
    const uint32_t split_end = split_begin + MemoryRead32(big2 - 8);
    const uint32_t heap_before = m_heap_current.load(std::memory_order_relaxed);
    GuestFree(big2);

    // A large free span must serve both small classes and differently sized
    // large requests without advancing the arena or overlapping live payloads.
    uint32_t split_small = GuestAlloc(512 * 1024, 0x1000);
    uint32_t split_large = GuestAlloc(2 * 1024 * 1024, 0x1000);
    HEAP_CHECK(split_small && split_large && (split_small & 0xFFF) == 0 && (split_large & 0xFFF) == 0);
    HEAP_CHECK(m_heap_current.load(std::memory_order_relaxed) == heap_before);
    HEAP_CHECK(split_small >= split_begin && split_small + 512 * 1024 <= split_large - kHeaderSize);
    HEAP_CHECK(split_large + 2 * 1024 * 1024 <= split_end);
    HEAP_CHECK(GuestAllocSize(split_small) == 512 * 1024 && GuestAllocSize(split_large) == 2 * 1024 * 1024);
    MemoryWrite32(split_small, 0x12345678);
    MemoryWrite32(split_small + 512 * 1024 - 4, 0xABCDEF01);
    MemoryWrite32(split_large, 0x87654321);
    HEAP_CHECK(MemoryRead32(split_small) == 0x12345678 && MemoryRead32(split_small + 512 * 1024 - 4) == 0xABCDEF01);
    GuestFree(split_small);
    uint32_t split_again = GuestAlloc(512 * 1024, 0x1000);
    HEAP_CHECK(split_again == split_small && MemoryRead32(split_again) == 0);
    HEAP_CHECK(MemoryRead32(split_large) == 0x87654321);
    GuestFree(split_again);
    GuestFree(split_large);

    // Keep these live so each request splits a span rather than reusing one
    // small-class block. Exhausted size buckets must not accumulate per split.
    uint32_t churn[128]{};
    const size_t free_sizes_before = m_free_huge.size();
    for (auto& address : churn) {
        address = GuestAlloc(16, 8);
        HEAP_CHECK(address && address >= split_begin && address + 16 <= split_end);
        HEAP_CHECK(GuestAllocSize(address) == 16);
    }
    HEAP_CHECK(m_heap_current.load(std::memory_order_relaxed) == heap_before);
    HEAP_CHECK(m_free_huge.size() <= free_sizes_before + 1);
    for (const auto& [bytes, blocks] : m_free_huge) HEAP_CHECK(!blocks.empty());
    for (const auto address : churn) GuestFree(address);

    GuestFree(b);
    GuestFree(reused);
    GuestFree(aligned);
    LOGI("HeapSelfCheck %s", ok ? "OK" : "FAILED");
#undef HEAP_CHECK
    return ok;
}

void DynarmicHost::AddExecutableRange(uint32_t base, uint32_t size) {
    if (!size) return;
    {
        std::lock_guard<std::mutex> lock(m_code_ranges_mutex);
        m_code_ranges.emplace_back(base, base + size);
    }
    LOGI("Executable range: 0x%08X..0x%08X", base, base + size);
}

std::optional<std::uint32_t> DynarmicHost::MemoryReadCode(std::uint32_t vaddr) {
    // A NULL function pointer must not execute zero padding and fall through
    // into the return trap at 4. That falsely reports a live guest stack as a
    // successful function return and silently strands its worker's consumers.
    if (vaddr == 0) return 0xE7FEDEFFu;
    if (auto kuser_instruction = ReadArmKuserHelperCode(vaddr)) {
        return kuser_instruction;
    }

    // Runaway PC containment. The arena is zero-filled and zeros decode as a valid
    // `andeq r0,r0,r0`, so a jump through a garbage pointer makes the JIT translate
    // a fresh block every 0x8000 bytes forever: RSS runs away and the LMK kills the
    // process, a symptom that reads exactly like a memory leak. Feed the JIT a
    // permanently-undefined instruction instead -- ExceptionRaised halts it and
    // prints where the jump came from.
    if (m_elf_loader) {
        // Executable PT_LOAD segments only: a branch into a module's .data,
        // .bss or GOT is as wild as one into the heap, and its words decode
        // into coprocessor loads/stores that no core can translate.
        auto in_module = [vaddr](const ElfLoader* l) {
            return l && l->IsCodeRange(vaddr, 2);
        };
        // Below kTrampolineEnd: the exit traps and the SVC trampolines.
        constexpr uint32_t kTrampolineEnd = 0x00020000;
        bool in_code = vaddr < kTrampolineEnd ||
                       in_module(m_elf_loader.get()) ||
                       in_module(m_fmodex_loader.get()) ||
                       in_module(m_fmodevent_loader.get());
        if (!in_code) {
            std::lock_guard<std::mutex> lock(m_code_ranges_mutex);
            for (const auto& r : m_code_ranges) {
                if (vaddr >= r.first && vaddr < r.second) in_code = true;
            }
        }
        if (!in_code) {
            LOGE("[RUNAWAY] code fetch 0x%08X", vaddr);
            return 0xE7FEDEFFu; // permanently undefined
        }
    }
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) {
        return *reinterpret_cast<uint32_t*>(m_memory + vaddr);
    }
    return std::nullopt;
}

std::uint8_t DynarmicHost::MemoryRead8(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) < m_memory_size) return m_memory[vaddr];
    return 0;
}

std::uint16_t DynarmicHost::MemoryRead16(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) return *reinterpret_cast<uint16_t*>(m_memory + vaddr);
    return 0;
}

std::uint32_t DynarmicHost::MemoryRead32(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) return *reinterpret_cast<uint32_t*>(m_memory + vaddr);
    return 0;
}

std::uint64_t DynarmicHost::MemoryRead64(std::uint32_t vaddr) {
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) return *reinterpret_cast<uint64_t*>(m_memory + vaddr);
    return 0;
}

void DynarmicHost::ProtectLowGuard() {
#if defined(__ANDROID__)
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0 || kLowGuardEnd % static_cast<uint32_t>(page) != 0 ||
        ::mprotect(m_memory, kLowGuardEnd, PROT_READ) != 0) {
        __android_log_print(ANDROID_LOG_WARN, "BL2LowGuard", "low guest pages stay writable");
        return;
    }
    m_low_guard_active.store(true, std::memory_order_release);
#endif
}

void DynarmicHost::WriteLowGuardWord(uint32_t vaddr, uint32_t value) {
    if (vaddr >= kLowGuardEnd || !m_low_guard_active.load(std::memory_order_acquire)) {
        MemoryWrite32(vaddr, value);
        return;
    }
#if defined(__ANDROID__)
    // Rare (runtime trampolines): open only this page for the store.
    const uintptr_t page = static_cast<uintptr_t>(::sysconf(_SC_PAGESIZE));
    uint8_t* const first = m_memory + (vaddr & ~(page - 1));
    std::lock_guard<std::mutex> lock(m_low_guard_mutex);
    if (::mprotect(first, page, PROT_READ | PROT_WRITE) != 0) return;
    *reinterpret_cast<uint32_t*>(m_memory + vaddr) = value;
    ::mprotect(first, page, PROT_READ);
#endif
}

void DynarmicHost::ReportLowGuardWrite(uint32_t vaddr, size_t bytes) {
    static std::atomic<uint32_t> reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) >= 32) return;
#if defined(__ANDROID__)
    const uint32_t pc = s_active_jit ? s_active_jit->Regs()[15] : 0;
    const uint32_t lr = s_active_jit ? s_active_jit->Regs()[14] : 0;
    __android_log_print(ANDROID_LOG_ERROR, "BL2LowGuard",
        "guest store to 0x%08X (%zu bytes) dropped; PC=%08X LR=%08X", vaddr, bytes, pc, lr);
#else
    (void)vaddr;
    (void)bytes;
#endif
}

void DynarmicHost::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, 1);
        return;
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    const bool cookie_trip = ObservePhysxGuestWrite(*this, vaddr, value);
#endif
    if (static_cast<uint64_t>(vaddr) < m_memory_size) {
        m_memory[vaddr] = value;
        NotifyGuestMemoryWritten(vaddr, 1);
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    if (cookie_trip) s_active_jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
#endif
}

void DynarmicHost::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, 2);
        return;
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    const bool cookie_trip = ObservePhysxGuestWrite(*this, vaddr, value);
#endif
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) {
        *reinterpret_cast<uint16_t*>(m_memory + vaddr) = value;
        NotifyGuestMemoryWritten(vaddr, 2);
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    if (cookie_trip) s_active_jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
#endif
}

void DynarmicHost::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, 4);
        return;
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    const bool cookie_trip = ObservePhysxGuestWrite(*this, vaddr, value);
#endif
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) {
        *reinterpret_cast<uint32_t*>(m_memory + vaddr) = value;
        NotifyGuestMemoryWritten(vaddr, 4);
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    if (cookie_trip) s_active_jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
#endif
}

void DynarmicHost::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, 8);
        return;
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    const bool cookie_trip = ObservePhysxGuestWrite(*this, vaddr, value);
#endif
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) {
        *reinterpret_cast<uint64_t*>(m_memory + vaddr) = value;
        NotifyGuestMemoryWritten(vaddr, 8);
    }
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    if (cookie_trip) s_active_jit->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
#endif
}

#if defined(HORN_DYNARMIC_HAS_INTERPRETER_FALLBACK)
void DynarmicHost::InterpreterFallback(std::uint32_t pc, std::size_t num_instructions) {
    LOGE("Interpreter Fallback at 0x%08X", pc);
    if (s_active_jit) s_active_jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}
#endif

bool DynarmicHost::IsReadOnlyMemory(std::uint32_t vaddr) {
    // Dynarmic can issue constant reads up to 64 bits. Requiring all eight
    // bytes to remain in one immutable segment keeps every opcode safe at a
    // segment boundary.
    constexpr size_t kLargestConstantRead = sizeof(uint64_t);
    return (m_elf_loader &&
            m_elf_loader->IsReadOnlyRange(vaddr, kLargestConstantRead)) ||
           (m_fmodex_loader &&
            m_fmodex_loader->IsReadOnlyRange(vaddr, kLargestConstantRead)) ||
           (m_fmodevent_loader &&
            m_fmodevent_loader->IsReadOnlyRange(vaddr, kLargestConstantRead));
}

void DynarmicHost::CallSVC(std::uint32_t swi) {
#if defined(HORN_ENABLE_PHYSX_COOKIE_WATCH)
    PhysxSvcScope cookie_native_scope;
#endif
    Dynarmic::A32::Jit* jit = s_active_jit;
    if (!jit) {
        LOGE("SVC 0x%X without an active JIT", swi);
        return;
    }
    if (jit->Regs()[15] <= 0x40) {
        jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    if (swi == 0) {
        const uint32_t syscall_number = jit->Regs()[7];
        const char* thunk_name = nullptr;
        switch (syscall_number) {
            case 3:  thunk_name = "read"; break;
            case 4:  thunk_name = "write"; break;
            case 5:  thunk_name = "open"; break;
            case 6:  thunk_name = "close"; break;
            case 19: thunk_name = "lseek"; break;
            default:
                LOGE("Unsupported direct ARM syscall r7=%u at PC=0x%08X",
                     syscall_number, jit->Regs()[15]);
                jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
                return;
        }
        if (!m_thunk_manager->DispatchNamedThunk(thunk_name, *jit)) {
            jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        }
        return;
    }
    m_thunk_manager->DispatchThunk(swi, *jit);
}

void DynarmicHost::ExceptionRaised(std::uint32_t pc, Dynarmic::A32::Exception exception) {
    LOGE("Exception at PC: 0x%08X (type %d)", pc, static_cast<int>(exception));
    if (!s_active_jit) return;
    s_active_jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    // Keep the initiating state before pthread cleanup resets this JIT. This
    // is fault-only and independent of timing-changing performance probes.
    const auto regs = s_active_jit->Regs();
    LOGE("Guest exception state PC=%08X LR=%08X SP=%08X CPSR=%08X",
         regs[15], regs[14], regs[13], s_active_jit->Cpsr());
    static std::atomic<uint32_t> dumps{0};
    if (dumps.fetch_add(1, std::memory_order_relaxed) >= 4) return;
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
        "Guest exception initiating_pc=%08X type=%d PC=%08X LR=%08X SP=%08X CPSR=%08X "
        "R0=%08X R1=%08X R2=%08X R3=%08X R4=%08X R5=%08X R6=%08X R7=%08X "
        "R8=%08X R9=%08X R10=%08X R11=%08X R12=%08X",
        pc, static_cast<int>(exception), regs[15], regs[14], regs[13], s_active_jit->Cpsr(),
        regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7],
        regs[8], regs[9], regs[10], regs[11], regs[12]);
#endif
    for (size_t first = 0; first < 16; first += 4) {
        LOGE("Guest exception registers r%zu..r%zu={%08X,%08X,%08X,%08X}",
             first, first + 3, regs[first], regs[first + 1],
             regs[first + 2], regs[first + 3]);
    }
#if !defined(_WIN32)
    // Invalid guest pointers can name unmapped/protected pages. pread on our
    // own memory reports an error instead of crashing this diagnostic.
    const int memory_fd = ::open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    if (memory_fd < 0) {
        LOGE("Guest exception memory open failed errno=%d", errno);
        return;
    }
#endif
    const auto dump_words = [&](const char* label, uint64_t address, size_t count) {
        std::array<uint32_t, 64> words{};
        const size_t bytes = std::min(count, words.size()) * sizeof(uint32_t);
        const uint64_t base = reinterpret_cast<uintptr_t>(m_memory);
        if (!m_memory || address > m_memory_size || bytes > m_memory_size - address ||
            base > static_cast<uint64_t>(INT64_MAX) ||
            address > static_cast<uint64_t>(INT64_MAX) - base ||
            bytes > static_cast<uint64_t>(INT64_MAX) - base - address) {
            LOGE("Guest exception memory label=%s address=%llX invalid_range", label,
                 static_cast<unsigned long long>(address));
            return;
        }
#if defined(_WIN32)
        SIZE_T got = 0;
        const bool success = ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(base + address), words.data(), bytes, &got);
        if (!success || got != bytes) {
            LOGE("Guest exception memory label=%s address=%llX bytes=%zu unreadable", label,
                 static_cast<unsigned long long>(address), bytes);
            return;
        }
#else
        errno = 0;
        const ssize_t got = ::pread64(memory_fd, words.data(), bytes,
                                     static_cast<off64_t>(base + address));
        const int error = errno;
        if (got != static_cast<ssize_t>(bytes)) {
            LOGE("Guest exception memory label=%s address=%llX bytes=%zu got=%zd errno=%d",
                 label, static_cast<unsigned long long>(address), bytes, got, error);
            return;
        }
#endif
        for (size_t i = 0; i + 4 <= bytes / sizeof(uint32_t); i += 4) {
            LOGE("Guest exception memory label=%s address=%08X words={%08X,%08X,%08X,%08X}",
                 label, static_cast<uint32_t>(address + i * sizeof(uint32_t)),
                 words[i], words[i + 1], words[i + 2], words[i + 3]);
        }
    };
    dump_words("sp", regs[13], 64);
    if (regs[11] >= 64) dump_words("fp", regs[11] - 64, 32);
    const uint32_t return_pc = regs[14] & ~1u;
    if (return_pc >= 32) dump_words("lr_code", return_pc - 32, 16);
    for (const unsigned reg : {0u, 1u, 2u, 3u, 4u, 5u, 12u}) {
        if (regs[reg] < 0x20000) continue;
        char label[8];
        std::snprintf(label, sizeof(label), "r%u", reg);
        dump_words(label, regs[reg], 8);
    }
#if !defined(_WIN32)
    ::close(memory_fd);
#endif
}

void DynarmicHost::AddTicks(std::uint64_t ticks) {
    s_ticks_remaining = s_ticks_remaining > ticks
        ? s_ticks_remaining - ticks : 0;
}

std::uint64_t DynarmicHost::GetTicksRemaining() {
    return s_ticks_remaining;
}

// STREX: the ExclusiveMonitor serializes reservation bookkeeping, but normal
// guest stores use fastmem and do not take that monitor lock. The final
// compare-and-store must therefore be one host atomic operation or a racing
// release store can be overwritten with a stale lock value.
template<typename T>
static bool GuestCompareAndSwap(uint8_t* mem, size_t mem_size, uint32_t vaddr, T value, T expected) {
    if (static_cast<uint64_t>(vaddr) + sizeof(T) > mem_size) return false;
    if ((vaddr & (alignof(T) - 1)) != 0) return false;
    T* p = reinterpret_cast<T*>(mem + vaddr);
    return __atomic_compare_exchange_n(p, &expected, value, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

bool DynarmicHost::MemoryWriteExclusive8(std::uint32_t vaddr, std::uint8_t value, std::uint8_t expected) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, sizeof(value));
        return true;  // Dropped like a plain store; no endless STREX retry.
    }
    const bool written = GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
    if (written) NotifyGuestMemoryWritten(vaddr, sizeof(value));
    return written;
}

bool DynarmicHost::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, sizeof(value));
        return true;  // Dropped like a plain store; no endless STREX retry.
    }
    const bool written = GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
    if (written) NotifyGuestMemoryWritten(vaddr, sizeof(value));
    return written;
}

bool DynarmicHost::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, sizeof(value));
        return true;  // Dropped like a plain store; no endless STREX retry.
    }
    const bool written = GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
    if (written) NotifyGuestMemoryWritten(vaddr, sizeof(value));
    return written;
}

bool DynarmicHost::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    if (vaddr < kLowGuardEnd && m_low_guard_active.load(std::memory_order_relaxed)) {
        ReportLowGuardWrite(vaddr, sizeof(value));
        return true;  // Dropped like a plain store; no endless STREX retry.
    }
    const bool written = GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
    if (written) NotifyGuestMemoryWritten(vaddr, sizeof(value));
    return written;
}
