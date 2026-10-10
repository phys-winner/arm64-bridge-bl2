#pragma once

class DynarmicHost;

// Call only after the pinned original ELF has passed the JNI loader's complete
// SHA-256 check. The guest remains ARM32 on the ARM64 host.
bool InstallBl2RenderWithoutNvsi(DynarmicHost& host);
