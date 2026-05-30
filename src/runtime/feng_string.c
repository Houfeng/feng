/* String runtime: literal allocation, concatenation, accessors. Strings are
 * immutable from the language's point of view — concat always produces a fresh
 * +1 reference. */
#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Descriptor equality compares immutable string payload bytes, not pointer
 * identity, so generic equality preserves the language-level string contract. */
static bool feng_string_descriptor_equal(const void *left, const void *right) {
    const FengString *left_string = (const FengString *)left;
    const FengString *right_string = (const FengString *)right;
    size_t left_length = feng_string_length(left_string);
    size_t right_length = feng_string_length(right_string);

    return left_length == right_length &&
           (left_length == 0U ||
            memcmp(feng_string_data(left_string),
                   feng_string_data(right_string),
                   left_length) == 0);
}

const FengTypeDescriptor feng_string_descriptor = {
    .name = "feng.builtin.string",
    .size = 0U, /* variable length; allocator computes per instance */
    .finalizer = NULL,
    .equal_fn = feng_string_descriptor_equal,
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

/* Process-wide singleton empty string used for the `string` default zero
 * value. Allocated lazily on first use under pthread_once so concurrent
 * first-use callers observe the same pointer. The singleton is stamped with
 * FENG_REFCOUNT_IMMORTAL so retain/release on the default value are no-ops. */
static FengString    *feng_string_default_singleton = NULL;
static pthread_once_t feng_string_default_once      = PTHREAD_ONCE_INIT;

static void feng_string_default_init(void) {
    feng_string_default_singleton = feng_string_literal("", 0U);
}

FengString *feng_string_default(void) {
    pthread_once(&feng_string_default_once, feng_string_default_init);
    return feng_string_default_singleton;
}

FengString *feng_string_from_utf8_bytes(FengArray *value, int64_t length) {
    size_t available = feng_array_length(value);
    size_t copy_length;
    const unsigned char *data;
    FengString *result;

    if (length < 0) {
        feng_panic("feng_string_from_utf8_bytes: length must be non-negative, got %lld",
                   (long long)length);
    }

    if (length == 0) {
        return feng_string_default();
    }

    copy_length = (size_t)length;
    if (copy_length > available) {
        feng_panic("feng_string_from_utf8_bytes: length %zu exceeds array length %zu",
                   copy_length,
                   available);
    }

    data = (const unsigned char *)feng_array_data(value);
    if (data == NULL) {
        feng_panic("feng_string_from_utf8_bytes: non-empty array has no payload");
    }

    result = feng_string_allocate(copy_length, 1U);
    memcpy(((struct FengString *)result)->data, data, copy_length);
    return result;
}

/* Returns a freshly allocated byte array containing a copy of the string's
 * current UTF-8 byte sequence. The result is a plain byte[] so standard
 * library code can perform bytes-first I/O without depending on string's
 * internal layout. */
FengArray *feng_string_to_utf8_bytes(FengString *value) {
    const struct FengString *source = (const struct FengString *)value;
    size_t length = source != NULL ? source->length : 0U;
    FengArray *result;
    unsigned char *data;

    result = feng_array_new_kinded(FENG_VALUE_TRIVIAL,
                                   NULL,
                                   NULL,
                                   sizeof(uint8_t),
                                   length);

    if (length == 0U) {
        return result;
    }

    data = (unsigned char *)feng_array_data(result);
    if (data == NULL) {
        feng_panic("feng_string_to_utf8_bytes: non-empty result has no payload");
    }

    memcpy(data, source->data, length);
    return result;
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
