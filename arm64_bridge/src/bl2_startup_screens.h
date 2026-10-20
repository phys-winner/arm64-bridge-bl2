#pragma once

class DynarmicHost;

// Skip legal screens while preserving the normal profile and character-loading startup.
bool InstallBl2StartupScreens(DynarmicHost& host);
