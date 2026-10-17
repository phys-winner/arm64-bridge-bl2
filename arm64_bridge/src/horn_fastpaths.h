#pragma once

class DynarmicHost;

// Installs optional, signature-checked native implementations of hot, pure
// Horn guest routines. A failed signature or differential self-test leaves
// the original ARM32 routine untouched.
bool InstallHornFastPaths(DynarmicHost& host);
