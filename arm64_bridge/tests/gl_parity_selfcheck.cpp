#include <cstdio>
#include <unistd.h>

extern "C" bool Bl2Arm64GlParitySelfCheck();

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    alarm(60);
    const bool passed = Bl2Arm64GlParitySelfCheck();
    alarm(0);
    return passed ? 0 : 1;
}
