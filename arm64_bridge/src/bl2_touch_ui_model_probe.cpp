#include "bl2_touch_ui_model_probe.h"
#include "bl2_touch_ui_model.h"
#include "../../touch_native_design/arm64_bridge/bl2_touch_input.h"
#include "bridge_log.h"
#include "dynarmic_host.h"
#include "elf_loader.h"

#include <cstring>
#include <string>

namespace {
bool copyMemory(const void* context, std::uint32_t address, void* out, std::size_t bytes) {
    const auto& host = *static_cast<const DynarmicHost*>(context);
#if defined(BL2_NATIVE_ARM32)
    if (address < 0x10000 || address > host.GetGuestMemorySize() ||
        bytes > host.GetGuestMemorySize() - address) return false;
    std::memcpy(out,reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),bytes);
#else
    if (address < 0x10000 || address > host.GetGuestMemorySize() ||
        bytes > host.GetGuestMemorySize() - address || !host.GetGuestMemoryBase()) return false;
    std::memcpy(out,host.GetGuestMemoryBase() + address,bytes);
#endif
    return true;
}
struct Observer {
    const DynarmicHost* host = nullptr;
    std::uint32_t base = 0;
    std::uint64_t scene = 0, nextRead = 0;
    unsigned logs = 0;
    bool verified = false;
    std::string last;
};
Observer observer;
}

void ProbeBl2UiModel(DynarmicHost& host, std::uint32_t owner, std::uint64_t scene) {
    auto& state = observer;
    if (state.host != &host) {
        state = {};
        state.host = &host;
        const auto& elf = host.GetElfLoader();
        state.base = elf.GetBaseAddress();
        state.verified = elf.GetImageSize() == 0x03089000 &&
            elf.GetSymbolAddress("_ZN7UObject11GObjObjectsE") == state.base + 0x02FBA268 &&
            elf.GetSymbolAddress("_ZN5FName5NamesE") == state.base + 0x02FB00C8 &&
            elf.GetSymbolAddress("_Z9FindFieldI9UPropertyEPT_P7UStruct5FName") == state.base + 0x00FC57BC &&
            elf.GetSymbolAddress("_ZN7UObject20execInstanceVariableER6FFramePv") == state.base + 0x00F97344;
        HORN_LOGI("BL2Bridge", "ui-model observer abi-symbols=%u readonly=1",state.verified ? 1u : 0u);
    }
    if (!state.verified || state.logs >= 80) return;
    const auto now = bl2::touch::monotonicMilliseconds();
    if (state.scene == scene && now < state.nextRead) return;
    state.scene = scene; state.nextRead = now + 100;
    if (!owner) { state.last.clear(); return; }
    const auto model = bl2::touch::CaptureBl2UiModel({&host,copyMemory},state.base,owner);
    std::string description = model.movieClass + "/" + model.reason + "/" +
        std::to_string(model.owner) + "/" + std::to_string(model.structure) + "/" +
        std::to_string(model.selectedIndex);
    if (description == state.last) return;
    state.last = description; ++state.logs;
    std::string ids;
    for (const auto id : model.eventIds) { if (!ids.empty()) ids += ','; ids += std::to_string(id); }
    std::string tags;
    for (const auto& button : model.buttons) {
        if (!tags.empty()) tags += ';';
        tags += button.tag + ':';
        for (const auto& key : button.keys) tags += key + ',';
    }
    HORN_LOGI("BL2Bridge", "ui-model valid=%u reason=%s movie=%s owner=%08X list=%08X provider=%08X "
        "providerClass=%s depth=%u selected=%d noCancel=%u structure=%llu bytes=%zu ids=[%s] buttons=[%s]",
        model.valid ? 1u : 0u,model.reason.c_str(),model.movieClass.c_str(),owner,
        static_cast<std::uint32_t>(model.list >> 32),static_cast<std::uint32_t>(model.provider >> 32),
        model.providerClass.c_str(),model.providerDepth,model.selectedIndex,model.noCancel ? 1u : 0u,
        static_cast<unsigned long long>(model.structure),model.bytesRead,ids.c_str(),tags.c_str());
    std::string fields;
    for (const auto& p : model.properties) {
        if (!fields.empty()) fields += ';';
        fields += p.name + "@" + std::to_string(p.offset) + "/" + std::to_string(p.elementSize) +
            "/extra=" + std::to_string(p.extra);
    }
    if (!fields.empty()) HORN_LOGI("BL2Bridge", "ui-model metadata=[%s]",fields.c_str());
}
