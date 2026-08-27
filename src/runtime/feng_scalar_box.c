#include "runtime/feng_runtime.h"

#include <string.h>

/* Define one concrete builtin ValueBox<T> descriptor. Byte comparison is
 * intentional: it preserves the former scalar-box observable behavior,
 * including signed-zero and NaN payload handling for floating values. */
#define FENG_DEFINE_SCALAR_VALUE_BOX(suffix, runtime_name) \
    static bool feng_value_box_##suffix##_equal( \
        const void *left, const void *right) { \
        const FengValueBox__##suffix *left_box = \
            (const FengValueBox__##suffix *)left; \
        const FengValueBox__##suffix *right_box = \
            (const FengValueBox__##suffix *)right; \
        return memcmp(&left_box->value, \
                      &right_box->value, \
                      sizeof(left_box->value)) == 0; \
    } \
    const FengTypeDescriptor feng_value_box_##suffix##_descriptor = { \
        .name = "feng.<internal>.value_box." runtime_name, \
        .size = sizeof(FengValueBox__##suffix), \
        .finalizer = NULL, \
        .release_children = NULL, \
        .is_potentially_cyclic = false, \
        .managed_field_count = 0, \
        .managed_fields = NULL, \
        .equal_fn = feng_value_box_##suffix##_equal, \
        .reified_generic_params_count = 0, .reified_generic_params = NULL, \
        .reified_field_offset_count = 0,   .reified_field_offsets = NULL, \
        .reified_agg_deps_count = 0,       .reified_agg_deps = NULL, \
        .reified_type_deps_count = 0,      .reified_type_deps = NULL, \
    }

FENG_DEFINE_SCALAR_VALUE_BOX(bool, "bool");
FENG_DEFINE_SCALAR_VALUE_BOX(i8, "i8");
FENG_DEFINE_SCALAR_VALUE_BOX(i16, "i16");
FENG_DEFINE_SCALAR_VALUE_BOX(i32, "i32");
FENG_DEFINE_SCALAR_VALUE_BOX(i64, "i64");
FENG_DEFINE_SCALAR_VALUE_BOX(u8, "u8");
FENG_DEFINE_SCALAR_VALUE_BOX(u16, "u16");
FENG_DEFINE_SCALAR_VALUE_BOX(u32, "u32");
FENG_DEFINE_SCALAR_VALUE_BOX(u64, "u64");
FENG_DEFINE_SCALAR_VALUE_BOX(f32, "f32");
FENG_DEFINE_SCALAR_VALUE_BOX(f64, "f64");

#undef FENG_DEFINE_SCALAR_VALUE_BOX
