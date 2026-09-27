#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace Dynarmic::A32 {
    class Jit;
}

class DynarmicHost;

using ThunkHandler = std::function<void(Dynarmic::A32::Jit& jit, DynarmicHost& host)>;

class ThunkManager {
public:
    ThunkManager(DynarmicHost& host);
    ~ThunkManager();

    // Returns false when any duplicate thunk/symbol name was observed. A duplicate
    // is a configuration error: silently replacing a handler makes the intended
    // implementation unreachable and must abort host initialization.
    bool RegisterAllThunks();
    // Returns the guest address of this thunk's trampoline.
    uint32_t RegisterThunk(const std::string& name, ThunkHandler handler);
    // Reused compatibility surfaces may overlap; keep the first audited
    // implementation rather than turning a harmless duplicate into a fatal.
    uint32_t RegisterThunkIfAbsent(const std::string& name, ThunkHandler handler);
    void RegisterSymbol(const std::string& name, uint32_t address);

    uint32_t GetOrCreateThunkAddress(const std::string& name);
    uint32_t FindRegisteredSymbol(const std::string& name) const;
    void DispatchThunk(uint32_t thunk_id, Dynarmic::A32::Jit& jit);
    bool DispatchNamedThunk(const std::string& name, Dynarmic::A32::Jit& jit);

    const std::unordered_map<std::string, uint32_t>& GetThunkSymbolTable() const {
        return m_symbol_to_address;
    }

    static uint32_t s_screenWidth;
    static uint32_t s_screenHeight;
    // Virtual screen size reported to the guest for UI scaling; 0 = report the real one.
    static uint32_t s_virtualWidth;
    static uint32_t s_virtualHeight;
    static uint32_t s_forced_dpi;
    static uint32_t s_ui_inset;

private:
    void RegisterGLThunks();
    void RegisterLibcThunks();
    DynarmicHost& m_host;
    static constexpr uint32_t kThunkBase = 0x00010000;
    static constexpr uint32_t kThunkStride = 8;
    uint32_t m_next_thunk_addr = kThunkBase;
    std::unordered_map<std::string, uint32_t> m_symbol_to_address;
    // IDs are dense and start at one. A vector avoids two unordered-map
    // probes on every imported function call; index zero is the sentinel.
    std::vector<ThunkHandler> m_id_to_handler;
    std::vector<std::string> m_id_to_name;
    bool m_registration_failed = false;
};

struct GuestThreadWorker {
    uint32_t start_func;
    uint32_t arg;
};

const std::vector<GuestThreadWorker>& GetGuestThreadWorkers();
void AddGuestThreadWorker(uint32_t start_func, uint32_t arg);

// Device probes the guest usually wants (thunk_manager.cpp). Kept generic: no
// game class is consulted, only android.os / Resources / system properties.
std::string QueryDeviceLanguage();
std::string QueryDeviceCountryCode();
uint32_t QueryDeviceDensity();
uint32_t QueryDeviceIsPad();

// Asset overriding from <externalFilesDir>/... (thunks_libc.cpp).
// Technique: the engine's own archive->archive->plain-file lookup chain is left
// alone; the first source is simply made to miss for any file the user dropped
// into the override root, so the engine's existing fallback does the work.
void SetAssetOverrideRoot(const std::string& root);
bool HasAssetOverride(const char* entry_name);
// plt_slot: the guest PLT entry of the archive-open function to divert.
// real_symbol: that same function's name, resolved in the guest for the tail call.
void InstallAssetOverrideHook(DynarmicHost& host, uint32_t plt_slot, const char* real_symbol);
