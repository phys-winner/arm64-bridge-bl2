#pragma once
#include <cstdint>
#include <memory>
class DynarmicHost;
namespace bl2::touch {
// Optional startup-enabled diagnostics, never a second mailbox consumer.
// Host must outlive this object. Call only at the existing input boundary after
// observe(); all temporary values and Movie pins are gone before probe returns.
class Ue3WidgetProbe final {
public:
    using LifetimeGeneration = std::uint64_t (*)();
    Ue3WidgetProbe(DynarmicHost&, LifetimeGeneration);
    ~Ue3WidgetProbe();
    void capture(std::uint32_t completeViewport, std::uint32_t movieOwner, std::uint64_t scene);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
