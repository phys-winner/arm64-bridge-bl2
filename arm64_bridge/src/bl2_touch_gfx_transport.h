#pragma once
#include "bl2_touch_gfx_values.h"
#include <functional>

class DynarmicHost;
namespace bl2::touch {
// One instance/scratch allocation per observer/sink; only one active session at
// a time. The current callback must not call guest code or drain the mailbox.
class GfxHostTransport final : public GfxTransport {
public:
    GfxHostTransport(DynarmicHost&, std::function<bool()> current);
    ~GfxHostTransport() override;
    GfxHostTransport(const GfxHostTransport&) = delete;
    GfxHostTransport& operator=(const GfxHostTransport&) = delete;
    bool read(std::uint32_t, void*, std::size_t) const override;
    bool write(std::uint32_t, const void*, std::size_t) override;
    bool executable(std::uint32_t) const override;
    bool current() const override;
    bool call(std::uint32_t, const std::vector<std::uint32_t>&, std::uint32_t&) override;
    void abandonScratch() override { abandoned_ = true; }
    std::uint32_t scratch();
    std::uint32_t base() const { return base_; }
private:
    DynarmicHost& host_;
    std::function<bool()> current_;
    std::uint32_t base_, scratch_ = 0;
    bool abandoned_ = false;
};
}
