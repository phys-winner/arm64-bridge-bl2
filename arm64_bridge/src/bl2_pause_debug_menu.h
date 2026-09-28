#pragma once
#include <cstddef>
#include <cstdint>
class DynarmicHost;

bool VerifyBl2PauseDebugMenuInput(const std::uint8_t* bytes, std::size_t size);
bool InstallBl2PauseDebugMenu(DynarmicHost& host);
