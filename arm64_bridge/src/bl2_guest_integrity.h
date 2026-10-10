#pragma once

class DynarmicHost;

// Native Android makes a library's RELRO (GOT, vtables) read-only after
// relocation and maps nothing at the bottom of the address space; the guest
// arena has neither protection. A NULL-based guest copy (an out-of-heap
// shader-cache malloc, then uncompress into NULL+16) silently overwrote the
// SVC trampolines and exit traps, and later calls ran data (freezes, an
// audio-thread abort). DynarmicHost now keeps the bottom pages read-only; this
// backstop, started after every bridge patch, compares RELRO and the
// trampolines with their install state every 250 ms, restores any change and
// logs it (tag BL2Integrity). debug.bl2.integrity=0 disables.
void StartBl2GuestIntegrityWatch(DynarmicHost& host);
