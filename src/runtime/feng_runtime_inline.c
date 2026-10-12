/* Emit the unique out-of-line versions of the shared runtime helper bodies.
 * This switch belongs only to this translation unit, never to build flags. */
#define FENG_RUNTIME_INLINE_IMPLEMENTATION
#include "runtime/feng_runtime.h"
#undef FENG_RUNTIME_INLINE_IMPLEMENTATION
