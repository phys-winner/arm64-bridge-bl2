#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <dynarmic/interface/A32/a32.h>
#include "native_elf_loader.h"

class ThunkManager;
class DynarmicHost {
public:
    static constexpr std::uint64_t GUEST_ADDRESS_SPACE = 0x100000000ULL;
    DynarmicHost();
    ~DynarmicHost();
    bool Initialize();
    bool InitializeNative(void* handle);
    std::uint8_t* GetGuestMemoryBase() const { return nullptr; }
    std::size_t GetGuestMemorySize() const { return UINT32_MAX; }
    ElfLoader& GetElfLoader() { return loader_; }
    ThunkManager& GetThunkManager() { return *manager_; }
    std::uint32_t FindSymbol(const std::string& name) { return loader_.GetSymbolAddress(name); }
    std::uint32_t GuestAlloc(std::size_t size, std::size_t alignment = 8);
    void GuestFree(std::uint32_t address);
    std::uint32_t GuestAllocSize(std::uint32_t address);
    std::uint32_t GuestAllocMapping(std::size_t size, std::size_t alignment);
    std::uint32_t CreateCodeVeneer(std::span<const std::uint32_t> words);
    void AddExecutableRange(std::uint32_t, std::uint32_t) {}
    std::optional<std::uint32_t> MemoryReadCode(std::uint32_t address) { return MemoryRead32(address); }
    std::uint8_t MemoryRead8(std::uint32_t address);
    std::uint16_t MemoryRead16(std::uint32_t address);
    std::uint32_t MemoryRead32(std::uint32_t address);
    std::uint64_t MemoryRead64(std::uint32_t address);
    void MemoryWrite8(std::uint32_t address, std::uint8_t value);
    void MemoryWrite16(std::uint32_t address, std::uint16_t value);
    void MemoryWrite32(std::uint32_t address, std::uint32_t value);
    void MemoryWrite64(std::uint32_t address, std::uint64_t value);
    void InvalidateGuestCode(std::uint32_t address, std::size_t size);
    std::uint32_t RunGuestCallback(std::uint32_t target, const std::vector<std::uint32_t>& args = {});
    std::uint64_t RunGuestCallback64(std::uint32_t target, const std::vector<std::uint32_t>& args = {});
    std::uint32_t RunFunction(std::uint32_t target, const std::vector<std::uint32_t>& args = {}) { return RunGuestCallback(target,args); }
    std::uint64_t RunFunction64(std::uint32_t target, const std::vector<std::uint32_t>& args = {}) { return RunGuestCallback64(target,args); }
    void HaltCurrentThreadExecution(Dynarmic::HaltReason) { std::abort(); }
private:
    void Write(std::uint32_t address, const void* bytes, std::size_t count);
    ElfLoader loader_;
    std::unique_ptr<ThunkManager> manager_;
    std::mutex codeMutex_;
    std::uint8_t* codePage_ = nullptr;
    std::size_t codeUsed_ = 0;
};
DynarmicHost& GetNativeHost();
