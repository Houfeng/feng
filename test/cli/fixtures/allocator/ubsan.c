/* Verify UBSan remains effective when builtin owns the native allocations. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

extern bool mi_is_in_heap_region(const void *);

/* The successful path checks ownership; the failing path must report overflow. */
int32_t allocator_ubsan_check(int32_t fail) {
    void *pointer = malloc(37);
    assert(pointer != NULL && mi_is_in_heap_region(pointer));
    free(pointer);
    if (fail) {
        volatile int32_t value = INT32_MAX;
        return value + 1;
    }
    return 1;
}
