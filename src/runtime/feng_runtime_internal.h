/* Internal layouts shared between runtime translation units. NOT part of the
 * codegen ABI: generated C code must use the accessor functions in
 * feng_runtime.h instead of poking these fields directly. */
#ifndef FENG_RUNTIME_INTERNAL_H
#define FENG_RUNTIME_INTERNAL_H

#include "runtime/feng_runtime.h"

struct FengString {
    FengManagedHeader header;
    size_t length;
    /* Null-terminated UTF-8 bytes; allocation extends to length + 1. */
    char data[];
};

struct FengArray {
    FengManagedHeader header;
    size_t length;
    size_t element_size;
    const FengTypeDescriptor *element_desc;
    bool element_is_managed;
    /* Heap-allocated storage of length * element_size bytes. */
    void *items;
};

/* Tag-specific child cleanup invoked by feng_release after refcount drops to 0,
 * before the user-defined finalizer (when present) and the final free. */
void feng_string_finalize_internal(struct FengString *s);
void feng_array_finalize_internal(struct FengArray *a);

#endif /* FENG_RUNTIME_INTERNAL_H */
