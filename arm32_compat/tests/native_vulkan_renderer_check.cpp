#include "native_vulkan_renderer.h"
#include <cstdio>

int main() {
    static_assert(sizeof(void*) == 4);
    const bool passed = bl2vk::SelfCheck();
    std::puts(passed ? "PASS Vulkan buffer bounds, private constant banks, formats and packing"
                     : "FAIL Vulkan renderer self-check");
    return passed ? 0 : 1;
}
