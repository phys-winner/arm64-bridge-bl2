#include "guest_software_write_watch.h"
#include "bl2_touch_gfx_transport.h"
#include "dynarmic_host.h"
#include "elf_loader.h"
#include <cstring>
#include <utility>

namespace bl2::touch {
GfxHostTransport::GfxHostTransport(DynarmicHost& host, std::function<bool()> current)
    : host_(host), current_(std::move(current)), base_(host.GetElfLoader().GetBaseAddress()) {}
GfxHostTransport::~GfxHostTransport() { if (scratch_ && !abandoned_) host_.GuestFree(scratch_); }
bool GfxHostTransport::read(std::uint32_t p, void* out, std::size_t bytes) const {
#if defined(BL2_NATIVE_ARM32)
    if (!out || p < 0x10000 || p > host_.GetGuestMemorySize() ||
        bytes > host_.GetGuestMemorySize() - p) return false;
    std::memcpy(out,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p)),bytes);
#else
    if (!out || !host_.GetGuestMemoryBase() || p < 0x10000 ||
        p > host_.GetGuestMemorySize() || bytes > host_.GetGuestMemorySize() - p) return false;
    std::memcpy(out,host_.GetGuestMemoryBase() + p,bytes);
#endif
    return true;
}
bool GfxHostTransport::write(std::uint32_t p, const void* data, std::size_t bytes) {
    // All bridge writes are restricted to this transport's own scratch block.
    if (!data || !scratch_ || p < scratch_ || p - scratch_ > GfxSession::kScratchBytes ||
        bytes > GfxSession::kScratchBytes - (p - scratch_)) return false;
#if defined(BL2_NATIVE_ARM32)
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)),data,bytes);
#else
    std::memcpy(host_.GetGuestMemoryBase() + p,data,bytes);
    NotifyGuestMemoryWritten(p, bytes);
#endif
    return true;
}
bool GfxHostTransport::executable(std::uint32_t pc) const {
    const auto p = pc & ~1u;
    return host_.GetElfLoader().GetImageSize() == 0x03089000 &&
        std::uint64_t(base_) + 0x03089000 <= host_.GetGuestMemorySize() &&
        p >= base_ + 0x00E823B0 && p < base_ + 0x02BB91EC && host_.MemoryReadCode(p).has_value();
}
bool GfxHostTransport::current() const { return !abandoned_ && current_ && current_(); }
bool GfxHostTransport::call(std::uint32_t pc, const std::vector<std::uint32_t>& args, std::uint32_t& out) {
    if (!executable(pc) || args.size() > 7) return false;
    // The only permitted caller is the existing input-boundary SVC. Callback
    // preserves that interrupted execution; RunFunction would replace it.
#if defined(BL2_NATIVE_ARM32)
    out = host_.RunGuestCallback(pc,args); return true;
#else
    return host_.CallGuest(pc,args,out);
#endif
}
std::uint32_t GfxHostTransport::scratch() {
    if (abandoned_) return 0;
    if (!scratch_) scratch_ = host_.GuestAlloc(GfxSession::kScratchBytes,16);
    return scratch_;
}
}
