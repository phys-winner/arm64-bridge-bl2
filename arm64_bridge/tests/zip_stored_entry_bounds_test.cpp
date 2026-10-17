#include "zip_stored_entry_bounds.h"

#include <cstdint>

static_assert(ResolveZipStoredEntryBounds(1000, 900, 100, 8, 2, 760, 760).valid);
static_assert(ResolveZipStoredEntryBounds(1000, 900, 100, 8, 2, 760, 760).data_offset == 140);
static_assert(!ResolveZipStoredEntryBounds(1000, 900, 100, 8, 2, 761, 761).valid);
static_assert(!ResolveZipStoredEntryBounds(1000, 900, 880, 8, 2, 1, 1).valid);
static_assert(!ResolveZipStoredEntryBounds(1000, 900, 901, 0, 0, 1, 1).valid);
static_assert(!ResolveZipStoredEntryBounds(1000, 1001, 100, 0, 0, 1, 1).valid);
static_assert(!ResolveZipStoredEntryBounds(1000, 900, 100, 8, 2, 760, 761).valid);
static_assert(!ResolveZipStoredEntryBounds(UINT64_MAX, UINT64_MAX, UINT64_MAX,
                                           0, 0, 0, 0).valid);
// Original main OBB fits as one STORED entry inside an APK below 2 GiB.
static_assert(ResolveZipStoredEntryBounds(2147483647ULL, 2147480000ULL,
                                          32768ULL, 64, 0,
                                          2113753088ULL, 2113753088ULL).valid);

int main() { return 0; }
