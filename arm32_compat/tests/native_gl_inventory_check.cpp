#include "../src/native_gl_inventory.h"
static_assert(bl2::IsGraphicsOperation("glDrawElements"));
static_assert(bl2::IsGraphicsOperation("eglSwapBuffers"));
static_assert(!bl2::IsGraphicsOperation("pthread_mutex_lock"));
static_assert(!bl2::IsGraphicsOperation(""));
static_assert(!bl2::IsGraphicsOperation("glob"));
int main() { NativeGlInventory(0, "", nullptr); }
