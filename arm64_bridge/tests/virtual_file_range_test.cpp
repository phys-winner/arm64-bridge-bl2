#include "virtual_file_range.h"

#include <cstdint>
#include <limits>

static_assert(ResolveVirtualFileRange(4096, 100, 0, 100, 8192).valid);
static_assert(ResolveVirtualFileRange(4096, 100, 0, 100, 8192).absolute_offset == 4096);
static_assert(ResolveVirtualFileRange(4096, 100, 99, 1, 8192).absolute_offset == 4195);
static_assert(!ResolveVirtualFileRange(4096, 100, 99, 2, 8192).valid);
static_assert(!ResolveVirtualFileRange(4096, 100, 100, 1, 8192).valid);
static_assert(!ResolveVirtualFileRange(4096, 100, 0, 0, 8192).valid);
static_assert(!ResolveVirtualFileRange(8100, 100, 0, 1, 8192).valid);
static_assert(!ResolveVirtualFileRange(UINT64_MAX - 3, 20, 0, 1, UINT64_MAX).valid);
static_assert(!ResolveVirtualFileRange(0, 1, 0, 1,
                                       static_cast<uint64_t>(
                                           std::numeric_limits<int64_t>::max()) + 1).valid);
// The supplied main OBB is just under the signed 2 GiB ZIP offset ceiling.
static_assert(ResolveVirtualFileRange(32768, 2113753088ULL, 0,
                                      2113753088ULL, 2147483647ULL).valid);
static_assert(!ResolveVirtualFileRange(32768, 2113753088ULL, 2113753087ULL,
                                       2, 2147483647ULL).valid);

int main() { return 0; }
