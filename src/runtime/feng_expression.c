#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/* Compare Feng strings by UTF-8 byte contents, following runtime NULL-as-empty convention. */
static bool expression_string_equal(const FengString *left,
                                    const FengString *right) {
    size_t left_length = feng_string_length(left);
    size_t right_length = feng_string_length(right);

    return left_length == right_length &&
           (left_length == 0U ||
            memcmp(feng_string_data(left), feng_string_data(right), left_length) == 0);
}

typedef struct FengExpressionSpecValue {
    void *subject;
    const void *witness;
} FengExpressionSpecValue;

static void expression_require_value_carriers(const FengGenericParamDescriptor *type,
                                              const void *left,
                                              const void *right) {
    if (type == NULL) {
        feng_panic("feng_expression_equal: type descriptor must not be NULL");
    }
    if (left == NULL || right == NULL) {
        feng_panic("feng_expression_equal: value carrier must not be NULL");
    }
}

static bool expression_scalar_equal(FengRuntimeTypeKind type_kind,
                                    const void *left,
                                    const void *right) {
    switch (type_kind) {
        case FENG_RUNTIME_TYPE_BOOL:
            return *(const bool *)left == *(const bool *)right;
        case FENG_RUNTIME_TYPE_I8:
            return *(const int8_t *)left == *(const int8_t *)right;
        case FENG_RUNTIME_TYPE_I16:
            return *(const int16_t *)left == *(const int16_t *)right;
        case FENG_RUNTIME_TYPE_I32:
            return *(const int32_t *)left == *(const int32_t *)right;
        case FENG_RUNTIME_TYPE_I64:
            return *(const int64_t *)left == *(const int64_t *)right;
        case FENG_RUNTIME_TYPE_U8:
            return *(const uint8_t *)left == *(const uint8_t *)right;
        case FENG_RUNTIME_TYPE_U16:
            return *(const uint16_t *)left == *(const uint16_t *)right;
        case FENG_RUNTIME_TYPE_U32:
            return *(const uint32_t *)left == *(const uint32_t *)right;
        case FENG_RUNTIME_TYPE_U64:
            return *(const uint64_t *)left == *(const uint64_t *)right;
        case FENG_RUNTIME_TYPE_F32:
            return *(const float *)left == *(const float *)right;
        case FENG_RUNTIME_TYPE_F64:
            return *(const double *)left == *(const double *)right;
        case FENG_RUNTIME_TYPE_ENUM:
            return *(const int32_t *)left == *(const int32_t *)right;
        default:
            feng_panic("feng_expression_equal: unsupported scalar type_kind=%d",
                       (int)type_kind);
    }
}

static bool expression_pointer_slot_equal(const void *left,
                                          const void *right) {
    return *(void *const *)left == *(void *const *)right;
}

static bool expression_spec_equal(const void *left,
                                  const void *right) {
    const FengExpressionSpecValue *left_value = (const FengExpressionSpecValue *)left;
    const FengExpressionSpecValue *right_value = (const FengExpressionSpecValue *)right;

    return left_value->subject == right_value->subject;
}

bool feng_expression_equal(const FengGenericParamDescriptor *type,
                           const void *left,
                           const void *right) {
    expression_require_value_carriers(type, left, right);

    switch (type->type_kind) {
        case FENG_RUNTIME_TYPE_BOOL:
        case FENG_RUNTIME_TYPE_I8:
        case FENG_RUNTIME_TYPE_I16:
        case FENG_RUNTIME_TYPE_I32:
        case FENG_RUNTIME_TYPE_I64:
        case FENG_RUNTIME_TYPE_U8:
        case FENG_RUNTIME_TYPE_U16:
        case FENG_RUNTIME_TYPE_U32:
        case FENG_RUNTIME_TYPE_U64:
        case FENG_RUNTIME_TYPE_F32:
        case FENG_RUNTIME_TYPE_F64:
        case FENG_RUNTIME_TYPE_ENUM:
            return expression_scalar_equal(type->type_kind, left, right);
        case FENG_RUNTIME_TYPE_STRING:
            return expression_string_equal(*(FengString *const *)left,
                                           *(FengString *const *)right);
        case FENG_RUNTIME_TYPE_ARRAY:
        case FENG_RUNTIME_TYPE_OBJECT:
        case FENG_RUNTIME_TYPE_POINTER:
        case FENG_RUNTIME_TYPE_CALLABLE:
            return expression_pointer_slot_equal(left, right);
        case FENG_RUNTIME_TYPE_SPEC:
            return expression_spec_equal(left, right);
        default:
            feng_panic("feng_expression_equal: unsupported type_kind=%d",
                       (int)type->type_kind);
    }
}
