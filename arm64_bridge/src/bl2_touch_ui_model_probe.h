#pragma once
#include <cstdint>

class DynarmicHost;

// Optional stage4 observer. Call only after the canonical guest SHA was verified,
// on the input thread, before the single mailbox drain. It changes no input caps.
// The observer is throttled to 10 Hz and logs only changes (at most 80 records).
void ProbeBl2UiModel(DynarmicHost&, std::uint32_t movieOwner,
                    std::uint64_t sceneIdentity);
