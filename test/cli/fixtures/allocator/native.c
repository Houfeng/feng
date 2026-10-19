/* Verify the allocator actually owning ordinary C and Feng runtime allocations. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define ALLOCATOR_WEAK __attribute__((weak_import))
#else
#include <malloc.h>
#define ALLOCATOR_WEAK __attribute__((weak))
#endif

/* A weak observation must not force extraction of the mimalloc archive. */
extern bool mi_is_in_heap_region(const void *) ALLOCATOR_WEAK;
extern void *allocator_shared_allocate(size_t size);
extern void allocator_shared_free(void *pointer);

/* Observe actual allocation ownership, rather than just exported symbols. */
static void check_owner(const void *pointer, int builtin) {
    assert(pointer != NULL);
    assert((mi_is_in_heap_region != NULL && mi_is_in_heap_region(pointer)) == (builtin != 0));
}

/* Free an allocation made by a different thread and return a new allocation. */
static void *exchange_thread(void *pointer) {
    free(pointer);
    return malloc(97);
}

/* Validate zero values, overflow, resizing, alignment and cross-library ownership. */
int32_t allocator_native_check(const intptr_t *values, int32_t builtin) {
    check_owner(values, builtin);
    assert(values[0] == 3 && values[1] == 7);
    unsigned char *p = calloc(32, 4);
    check_owner(p, builtin);
    for (size_t i = 0; i < 128; ++i) assert(p[i] == 0);
    memset(p, 0x5a, 128);
    p = realloc(p, 4096);
    check_owner(p, builtin);
    for (size_t i = 0; i < 128; ++i) assert(p[i] == 0x5a);
#if defined(__APPLE__)
    assert(malloc_size(p) >= 4096);
#else
    assert(malloc_usable_size(p) >= 4096);
#endif
    /* The volatile value keeps the overflow request in the runtime test. */
    volatile size_t huge = SIZE_MAX;
    void *(*volatile checked_calloc)(size_t, size_t) = calloc;
    void *(*volatile checked_malloc)(size_t) = malloc;
    void *failed = realloc(p, huge);
    assert(failed == NULL && p[0] == 0x5a);
    assert(checked_calloc(huge, 2) == NULL);
    allocator_shared_free(p);
    p = allocator_shared_allocate(79);
    check_owner(p, builtin);
    free(p);
    void *aligned = NULL;
    assert(posix_memalign(&aligned, 64, 193) == 0);
    check_owner(aligned, builtin);
    assert((uintptr_t)aligned % 64 == 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, exchange_thread, aligned) == 0);
    void *returned = NULL;
    assert(pthread_join(thread, &returned) == 0);
    check_owner(returned, builtin);
    free(returned);
    free(NULL);
    void *zero = checked_malloc(0);
    if (zero != NULL) check_owner(zero, builtin);
    free(zero);
    zero = checked_calloc(0, 16);
    if (zero != NULL) check_owner(zero, builtin);
    free(zero);
    return 1;
}
