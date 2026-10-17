#pragma once
class DynarmicHost;
namespace bl2::touch { class Ue3GameplaySink; }

// Install only after VerifyBl2TouchInput succeeds for the raw immutable ELF and
// the stage-1 observer is accepted. Adds two narrow ARM veneers. No startup mode
// or capabilities are enabled here; nullptr is refused.
bool InstallBl2TouchGameplayHooks(DynarmicHost& host, bl2::touch::Ue3GameplaySink* sink);
