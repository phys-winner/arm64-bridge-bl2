#include <errno.h>
#include <stdint.h>
#define UNAVAILABLE(name) uint32_t name(void) { return (uint32_t)-ENODEV; }
UNAVAILABLE(TSecOpenChannel)
UNAVAILABLE(TSecCloseChannel)
UNAVAILABLE(TSecMemWrite)
UNAVAILABLE(TSecInitStream)
UNAVAILABLE(TSecCloseStream)
UNAVAILABLE(TSecStreamWait)
UNAVAILABLE(TSecStartStreaming)
UNAVAILABLE(TSecWriteToStreamReloc)
UNAVAILABLE(TSecWriteToStream)
UNAVAILABLE(TSecStopStreamingAndFlush)
uint32_t TSecMemAlloc(void) { return 0; }
uint32_t TSecMemFree(void) { return 0; }
const char* TSecGetTegraVersion(void) { return "t114"; }
