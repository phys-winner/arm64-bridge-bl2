#include "dynarmic_host.h"
#include "elf_loader.h"
#include "perf_probe.h"
#include "thunk_manager.h"
#include "bridge_log.h"
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

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

    if (!HeapSelfCheck()) {
        LOGE("HeapSelfCheck failed; refusing to initialize guest host");
        return false;
    }

    // ARM exit trap at 0x00000004: SVC 0 (0xEF000000)
    *reinterpret_cast<uint32_t*>(m_memory + EXIT_TRAP_ARM) = 0xEF000000;

    // Thumb exit trap at 0x00000008: SVC 0 (0xDF00, 0xDF00)
    *reinterpret_cast<uint32_t*>(m_memory + EXIT_TRAP_THUMB) = 0xDF00DF00;

    LOGI("ARM kuser helpers enabled: memory_barrier=0xFFFF0FA0, cmpxchg=0xFFFF0FC0");

    m_thunk_manager = std::make_unique<ThunkManager>(*this);
    if (!m_thunk_manager->RegisterAllThunks()) {
        LOGE("Thunk registration failed; refusing to initialize guest host");
        m_thunk_manager.reset();
        return false;
    }

    // Horn starts UE3GameThread from NativeCallback_Initialize and may add render,
    // streaming and audio pthreads. Each native host thread receives two processor
    // IDs: one normal A32 core and one synchronous nested-callback core.
    m_contexts.reserve(kMaxThreadContexts);
    m_free_context_slots.reserve(kMaxThreadContexts);
    m_context_by_thread.reserve(kMaxThreadContexts);
    m_monitor = std::make_unique<Dynarmic::ExclusiveMonitor>(kMaxThreadContexts * 2);

    if (!GetOrCreateThreadContext()) {
        LOGE("Failed to create initial per-thread Dynarmic JIT context");
        return false;
    }
    LOGI("Dynarmic Host initialized: %zu bytes guest space, fastmem %s",
         m_memory_size, fastmem ? "ON" : "OFF");
    return true;
}

std::unique_ptr<Dynarmic::A32::Jit> DynarmicHost::CreateJit(size_t processor_id) {
    Dynarmic::A32::UserConfig config;
    config.callbacks = this;
    config.processor_id = processor_id;
    config.global_monitor = m_monitor.get();
    config.arch_version = Dynarmic::A32::ArchVersion::v7;  // guest libs are ARMv7-A + NEON
    // Coarse cycle budgets are enabled only for the on-device performance
    // probe. Each exhausted budget yields a guest-PC sample; production keeps
    // Dynarmic's lower-overhead no-cycle-counting path.
    config.enable_cycle_counting = HornPerfEnabled();
    config.always_little_endian = true;                    // guest never uses SETEND; drops CPSR.E handling
    config.define_unpredictable_behaviour = true;          // define rather than halt on odd encodings
    if (m_memory_size == GUEST_ADDRESS_SPACE) {
        config.fastmem_pointer = reinterpret_cast<uintptr_t>(m_memory);
    }
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
        m_monitor->ClearProcessor(context->primary_processor_id);
        m_monitor->ClearProcessor(context->callback_processor_id);
        context->jit->Reset();
        context->callback_jit->Reset();
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
        if (!EnsureThreadStackCapacity(*context, wanted)) {
            return nullptr; // slot remains free and its previous stack remains valid
        }
        m_monitor->ClearProcessor(context->primary_processor_id);
        m_monitor->ClearProcessor(context->callback_processor_id);
        context->jit->Reset();
        context->callback_jit->Reset();
        context->is_running_jit = false;
        context->is_running_callback_jit = false;
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
        // Both JIT pointers become immutable before the context is published. This
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
    return ExecuteFunction(*context->callback_jit, target_pc, args,
                           context->callback_stack_top, kCallbackStackSize);
}

uint64_t DynarmicHost::ExecuteFunction(Dynarmic::A32::Jit& jit, uint32_t target_pc,
                                       const std::vector<uint32_t>& args,
                                       uint32_t stack_top, uint32_t stack_capacity) {
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
            *reinterpret_cast<uint32_t*>(m_memory + sp + i * 4) = args[4 + i];
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
        LOGI("RunFunction(0x%08X) unexpected halt with reason=0x%X, PC=0x%08X, LR=0x%08X, R0=0x%08X, SP=0x%08X",
             target_pc, static_cast<unsigned>(hr), final_pc, jit.Regs()[14], jit.Regs()[0], jit.Regs()[13]);
    }

    return (static_cast<uint64_t>(jit.Regs()[1]) << 32) | jit.Regs()[0];
}

void DynarmicHost::InvalidateGuestCode(uint32_t addr, size_t size) {
    if (size == 0) return;
    // Dynarmic's invalidation API is cross-thread aware: it queues the range,
    // atomically raises CacheInvalidation and wakes a core already in Run().
    // Context/JIT pointers never change after publication, and the collection
    // lock prevents teardown or publication while we visit every core.
    std::lock_guard<std::mutex> contexts_lock(m_contexts_mutex);
    for (const auto& context : m_contexts) {
        context->jit->InvalidateCacheRange(addr, size);
        context->callback_jit->InvalidateCacheRange(addr, size);
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
    if (need <= kMaxClassSize) {
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
                recycled = true;
            }
        }
    }
    if (!recycled) {
        // Lock-free bump allocation. Free-list reuse remains synchronized per
        // size class, while fresh allocations from unrelated guest threads no
        // longer contend on one process-wide mutex.
        uint32_t current = m_heap_current.load(std::memory_order_relaxed);
        for (;;) {
            if (static_cast<uint64_t>(current) + capacity > kHeapLimit) {
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
    }
    MemoryWrite32(payload - 12, base);
    MemoryWrite32(payload - 8, capacity);
    MemoryWrite32(payload - 4, static_cast<uint32_t>(size));
    __atomic_store_n(reinterpret_cast<uint32_t*>(m_memory + payload - 16),
                     kBlockMagic, __ATOMIC_RELEASE);
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
    uint32_t base = MemoryRead32(addr - 12);
    uint32_t capacity = MemoryRead32(addr - 8);

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
        if (aligned < current || static_cast<uint64_t>(aligned) + size > kMapLimit) {
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
    GuestFree(big2);

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
        auto in_module = [vaddr](const ElfLoader* l) {
            return l && vaddr >= l->GetBaseAddress() &&
                   vaddr < l->GetBaseAddress() + l->GetImageSize();
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

void DynarmicHost::MemoryWrite8(std::uint32_t vaddr, std::uint8_t value) {
    if (static_cast<uint64_t>(vaddr) < m_memory_size) m_memory[vaddr] = value;
}

void DynarmicHost::MemoryWrite16(std::uint32_t vaddr, std::uint16_t value) {
    if (static_cast<uint64_t>(vaddr) + 2 <= m_memory_size) *reinterpret_cast<uint16_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::MemoryWrite32(std::uint32_t vaddr, std::uint32_t value) {
    if (static_cast<uint64_t>(vaddr) + 4 <= m_memory_size) *reinterpret_cast<uint32_t*>(m_memory + vaddr) = value;
}

void DynarmicHost::MemoryWrite64(std::uint32_t vaddr, std::uint64_t value) {
    if (static_cast<uint64_t>(vaddr) + 8 <= m_memory_size) *reinterpret_cast<uint64_t*>(m_memory + vaddr) = value;
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
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive16(std::uint32_t vaddr, std::uint16_t value, std::uint16_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive32(std::uint32_t vaddr, std::uint32_t value, std::uint32_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}

bool DynarmicHost::MemoryWriteExclusive64(std::uint32_t vaddr, std::uint64_t value, std::uint64_t expected) {
    return GuestCompareAndSwap(m_memory, m_memory_size, vaddr, value, expected);
}
