#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <elf.h>

class ThunkManager;
class ElfLoader {
public:
    bool Attach(void* handle);
    bool RedirectImports(const ThunkManager& manager);
    std::uint32_t GetSymbolAddress(const std::string& name) const;
    std::uint32_t GetBaseAddress() const { return base_; }
    std::uint32_t GetImageSize() const { return size_; }
    bool IsReadOnlyRange(std::uint32_t address, std::size_t bytes) const;
    bool IsCodeRange(std::uint32_t address, std::size_t bytes) const;
private:
    void* handle_ = nullptr;
    std::uint32_t base_ = 0;
    std::uint32_t size_ = 0;
    std::vector<Elf32_Phdr> segments_;
};
