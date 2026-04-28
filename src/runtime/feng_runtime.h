/* Feng runtime — public ABI consumed by codegen-emitted C code.
 *
 * All symbols listed in this header constitute the Phase 1A runtime contract.
 * Anything not declared here MUST NOT be referenced by generated code; future
 * phases will extend this surface deliberately.
 */
#ifndef FENG_RUNTIME_H
#define FENG_RUNTIME_H

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Type descriptors -------------------------------------------------- */

typedef enum FengTypeTag {
    FENG_TYPE_TAG_STRING = 1,
    FENG_TYPE_TAG_ARRAY = 2,
    FENG_TYPE_TAG_OBJECT = 3,
    FENG_TYPE_TAG_CLOSURE = 4
} FengTypeTag;

typedef void (*FengFinalizerFn)(void *self);

typedef struct FengTypeDescriptor {
    const char *name;            /* fully-qualified, debug-only */
    size_t size;                 /* total instance bytes incl. header (0 for variable-length) */
    FengFinalizerFn finalizer;   /* optional user-defined finalizer */
} FengTypeDescriptor;

/* Built-in descriptors used by string/array helpers. Generated code may also
 * reference them when emitting array-of-string or array-of-array slots. */
extern const FengTypeDescriptor feng_string_descriptor;
extern const FengTypeDescriptor feng_array_descriptor;

/* --- Managed object header --------------------------------------------- */

typedef struct FengManagedHeader {
    const FengTypeDescriptor *desc;
    FengTypeTag tag;
    /* refcount == UINT32_MAX marks the object as immortal (string literals,
     * runtime-internal singletons). retain/release skip such objects. */
    uint32_t refcount;
} FengManagedHeader;

#define FENG_REFCOUNT_IMMORTAL ((uint32_t)0xFFFFFFFFu)

/* --- retain / release / barriers --------------------------------------- */

void *feng_retain(void *obj);
void  feng_release(void *obj);

/* Slot assignment with RC barrier:
 *   release(*slot); *slot = retain(new_value); */
void  feng_assign(void **slot, void *new_value);

/* Take ownership of a slot, leaving it NULL. Refcount is unchanged: the caller
 * inherits the +1 reference. */
void *feng_take(void **slot);

/* --- String ------------------------------------------------------------ */

typedef struct FengString FengString;

/* Allocates a fresh immortal string holding a copy of the supplied UTF-8 bytes.
 * Codegen caches the returned pointer per literal site so each literal allocates
 * exactly once at first use. The buffer is null-terminated. */
FengString *feng_string_literal(const char *utf8, size_t length);

/* Concatenation always returns a fresh +1 string; either operand may be NULL,
 * which is treated as the empty string. */
FengString *feng_string_concat(const FengString *left, const FengString *right);

size_t      feng_string_length(const FengString *s);
const char *feng_string_data(const FengString *s);

/* --- Array ------------------------------------------------------------- */

typedef struct FengArray FengArray;

/* Allocates a zero-initialised array of `length` elements. When
 * `element_is_managed` is true, slots are treated as pointers to managed
 * objects and released on array destruction. `element_desc` is currently used
 * only for diagnostics. */
FengArray *feng_array_new(const FengTypeDescriptor *element_desc,
                          size_t element_size,
                          bool element_is_managed,
                          size_t length);

size_t feng_array_length(const FengArray *array);
void  *feng_array_data(FengArray *array);
void   feng_array_check_index(const FengArray *array, size_t index);

/* --- Object ------------------------------------------------------------ */

/* Allocates a zero-initialised instance whose first member is FengManagedHeader.
 * desc->size MUST cover the full struct including the header. */
void *feng_object_new(const FengTypeDescriptor *desc);

/* --- Exceptions -------------------------------------------------------- */

typedef struct FengExceptionFrame {
    struct FengExceptionFrame *prev;
    jmp_buf jb;
    void   *value;
    int     is_managed;
} FengExceptionFrame;

void feng_exception_push(FengExceptionFrame *frame);
void feng_exception_pop(void);

#if defined(__GNUC__) || defined(__clang__)
void feng_exception_throw(void *value, int is_managed) __attribute__((noreturn));
#else
void feng_exception_throw(void *value, int is_managed);
#endif

FengExceptionFrame *feng_exception_current(void);

/* --- Panic ------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
void feng_panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#else
void feng_panic(const char *fmt, ...);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FENG_RUNTIME_H */
