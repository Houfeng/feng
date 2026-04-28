/* String runtime: literal allocation, concatenation, accessors. Strings are
 * immutable from the language's point of view — concat always produces a fresh
 * +1 reference. */
#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <stdlib.h>
#include <string.h>

const FengTypeDescriptor feng_string_descriptor = {
    .name = "feng.builtin.string",
    .size = 0U, /* variable length; allocator computes per instance */
    .finalizer = NULL,
};

void feng_string_finalize_internal(struct FengString *s) {
    /* Buffer is part of the same allocation as the header — nothing extra to
     * free here. The hook exists so future evolutions (e.g. interning) can
     * clean up secondary tables without touching feng_object.c. */
    (void)s;
}

static FengString *feng_string_allocate(size_t length, uint32_t initial_refcount) {
    struct FengString *s;
    size_t total;

    /* +1 for the trailing NUL. Guard against overflow before the multiply
     * cannot happen (sizeof header + length + 1). */
    if (length > (size_t)(SIZE_MAX) - sizeof(struct FengString) - 1U) {
        feng_panic("feng_string: length %zu exceeds addressable range", length);
    }

    total = sizeof(struct FengString) + length + 1U;
    s = (struct FengString *)malloc(total);
    if (s == NULL) {
        feng_panic("feng_string: out of memory (length=%zu)", length);
    }

    s->header.desc = &feng_string_descriptor;
    s->header.tag = FENG_TYPE_TAG_STRING;
    s->header.refcount = initial_refcount;
    s->length = length;
    s->data[length] = '\0';
    return (FengString *)s;
}

FengString *feng_string_literal(const char *utf8, size_t length) {
    FengString *s = feng_string_allocate(length, FENG_REFCOUNT_IMMORTAL);

    if (length > 0U) {
        memcpy(((struct FengString *)s)->data, utf8, length);
    }
    return s;
}

FengString *feng_string_concat(const FengString *left, const FengString *right) {
    const struct FengString *l = (const struct FengString *)left;
    const struct FengString *r = (const struct FengString *)right;
    size_t left_length = (l != NULL) ? l->length : 0U;
    size_t right_length = (r != NULL) ? r->length : 0U;
    size_t total_length;
    FengString *result;
    struct FengString *raw;

    if (left_length > (size_t)(SIZE_MAX) - right_length) {
        feng_panic("feng_string_concat: combined length overflow");
    }

    total_length = left_length + right_length;
    result = feng_string_allocate(total_length, 1U);
    raw = (struct FengString *)result;

    if (left_length > 0U) {
        memcpy(raw->data, l->data, left_length);
    }
    if (right_length > 0U) {
        memcpy(raw->data + left_length, r->data, right_length);
    }
    return result;
}

size_t feng_string_length(const FengString *s) {
    return s != NULL ? ((const struct FengString *)s)->length : 0U;
}

const char *feng_string_data(const FengString *s) {
    return s != NULL ? ((const struct FengString *)s)->data : "";
}
