#include "runtime/feng_runtime.h"

/* Floating descriptors use C equality so NaN and signed zero match Feng's
 * scalar equality semantics instead of raw byte equality. */
static bool feng_f32_equal(const void *left, const void *right) {
    return *(const float *)left == *(const float *)right;
}

/* See feng_f32_equal for why this intentionally does not use memcmp. */
static bool feng_f64_equal(const void *left, const void *right) {
    return *(const double *)left == *(const double *)right;
}

/* Canonical policy used by every descriptor whose Feng default zero is the
 * all-zero object representation. Keeping one shared object lets generated
 * code test the policy without duplicating metadata or calling an initializer
 * for ordinary scalar values. */
const FengDefaultZeroInitDescriptor feng_default_zero_bytes_init = {
    .kind = FENG_DEFAULT_ZERO_BYTES,
    .init_fn = NULL,
};

#define FENG_TRIVIAL_DESCRIPTOR(symbol, runtime_name, c_type, equal) \
    const FengTrivialDescriptor symbol = { \
        .name = runtime_name, \
        .size = sizeof(c_type), \
        .default_zero_init = &feng_default_zero_bytes_init, \
        .equal_fn = equal, \
    }

FENG_TRIVIAL_DESCRIPTOR(feng_bool_descriptor, "bool", bool, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_i8_descriptor, "i8", int8_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_i16_descriptor, "i16", int16_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_i32_descriptor, "i32", int32_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_i64_descriptor, "i64", int64_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_u8_descriptor, "u8", uint8_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_u16_descriptor, "u16", uint16_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_u32_descriptor, "u32", uint32_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_u64_descriptor, "u64", uint64_t, NULL);
FENG_TRIVIAL_DESCRIPTOR(feng_f32_descriptor, "f32", float, feng_f32_equal);
FENG_TRIVIAL_DESCRIPTOR(feng_f64_descriptor, "f64", double, feng_f64_equal);
FENG_TRIVIAL_DESCRIPTOR(feng_pointer_descriptor, "pointer", void *, NULL);

#undef FENG_TRIVIAL_DESCRIPTOR
