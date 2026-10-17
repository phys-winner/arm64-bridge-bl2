#pragma once

class DynarmicHost;

// The pinned RHI's persistent buffers must not convert a pooled, still-live
// mutable store to immutable storage: Mali drops the requested map permissions.
// Install before any guest constructor executes. Reject an incompatible image.
bool InstallBl2FreshPersistentBufferNames(DynarmicHost& host);
