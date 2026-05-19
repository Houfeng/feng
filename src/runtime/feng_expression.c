#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/* Validate and normalize an array element index supplied by generated code. */
static size_t expression_checked_index(const char *name,
                                       const FengArray *array,
                                       int64_t index) {
    if (array == NULL) {
        feng_panic("feng_expression_equal: %s array must not be NULL", name);
    }
    if (index < 0) {
        feng_panic("feng_expression_equal: %s index must be non-negative, got %" PRId64,
                   name,
                   index);
    }
    if ((uint64_t)index > (uint64_t)SIZE_MAX) {
        feng_panic("feng_expression_equal: %s index exceeds runtime size range", name);
    }
    feng_array_check_index(array, (size_t)index);
    return (size_t)index;
}

/* Return the address of an inline array element payload slot. */
static const unsigned char *expression_array_element(const struct FengArray *array,
                                                     size_t index) {
    const unsigned char *payload =
        (const unsigned char *)feng_array_payload_inline_const(array);

    if (payload == NULL) {
        feng_panic("feng_expression_equal: array payload is unavailable");
    }
    return payload + index * array->element_size;
}

/* Compare Feng strings by UTF-8 byte contents, following runtime NULL-as-empty convention. */
static bool expression_string_equal(const FengString *left,
                                    const FengString *right) {
    size_t left_length = feng_string_length(left);
    size_t right_length = feng_string_length(right);

    return left_length == right_length &&
           (left_length == 0U ||
            memcmp(feng_string_data(left), feng_string_data(right), left_length) == 0);
}

/* Reject array operands whose payload slots cannot represent the same value type. */
static void expression_require_compatible_arrays(const struct FengArray *left,
                                                 const struct FengArray *right) {
    if (left->element_kind != right->element_kind) {
        feng_panic("feng_expression_equal: array element kind mismatch");
    }
    if (left->element_size != right->element_size) {
        feng_panic("feng_expression_equal: array element size mismatch");
    }
    if ((left->element_desc == &feng_string_descriptor) !=
        (right->element_desc == &feng_string_descriptor)) {
        feng_panic("feng_expression_equal: string element descriptor mismatch");
    }
    if (left->element_desc != NULL && right->element_desc != NULL &&
        left->element_desc != right->element_desc) {
        feng_panic("feng_expression_equal: array element descriptor mismatch");
    }
    if (left->element_kind == FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS &&
        left->element_aggregate != right->element_aggregate) {
        feng_panic("feng_expression_equal: aggregate element descriptor mismatch");
    }
}

/* Compare two array element slots using the runtime value classification. */
bool feng_expression_equal(const FengGenericParamDescriptor *type,
                           const FengArray *left_value,
                           int64_t left_index,
                           const FengArray *right_value,
                           int64_t right_index) {
    const struct FengArray *left = (const struct FengArray *)left_value;
    const struct FengArray *right = (const struct FengArray *)right_value;
    size_t checked_left_index =
        expression_checked_index("left", left_value, left_index);
    size_t checked_right_index =
        expression_checked_index("right", right_value, right_index);
    const unsigned char *left_element;
    const unsigned char *right_element;

    (void)type;

    expression_require_compatible_arrays(left, right);
    left_element = expression_array_element(left, checked_left_index);
    right_element = expression_array_element(right, checked_right_index);

    switch (left->element_kind) {
        case FENG_VALUE_TRIVIAL:
            return memcmp(left_element, right_element, left->element_size) == 0;
        case FENG_VALUE_MANAGED_POINTER: {
            void *left_pointer = *(void *const *)left_element;
            void *right_pointer = *(void *const *)right_element;

            if (left->element_desc == &feng_string_descriptor ||
                right->element_desc == &feng_string_descriptor) {
                return expression_string_equal((const FengString *)left_pointer,
                                               (const FengString *)right_pointer);
            }
            return left_pointer == right_pointer;
        }
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            return memcmp(left_element, right_element, left->element_size) == 0;
        default:
            feng_panic("feng_expression_equal: corrupted element_kind=%d",
                       (int)left->element_kind);
    }
}
