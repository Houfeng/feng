#include "runtime/feng_runtime.h"
#include "runtime/feng_runtime_internal.h"

#include <string.h>

/* Validate the erased value carriers before dispatching by value kind. */
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

/* Compare trivial values through their optional descriptor equality hook. */
static bool expression_trivial_equal(const FengGenericParamDescriptor *type,
                                     const void *left,
                                     const void *right) {
    const FengTrivialDescriptor *desc = feng_generic_trivial_descriptor(type);

    if (desc == NULL) {
        feng_panic("feng_expression_equal: trivial descriptor must not be NULL");
    }
    if (desc->equal_fn != NULL) {
        return desc->equal_fn(left, right);
    }
    return memcmp(left, right, desc->size) == 0;
}

/* Compare managed pointers by descriptor semantics when present, else identity. */
static bool expression_managed_pointer_equal(const FengGenericParamDescriptor *type,
                                             const void *left,
                                             const void *right) {
    const FengTypeDescriptor *desc = feng_generic_type_descriptor(type);
    void *left_value = *(void *const *)left;
    void *right_value = *(void *const *)right;

    if (desc != NULL && desc->equal_fn != NULL) {
        return desc->equal_fn(left_value, right_value);
    }
    return left_value == right_value;
}

/* Compare by-value aggregates through the aggregate descriptor equality hook. */
static bool expression_aggregate_equal(const FengGenericParamDescriptor *type,
                                       const void *left,
                                       const void *right) {
    const FengAggregateDescriptor *desc = feng_generic_aggregate_descriptor(type);

    if (desc == NULL) {
        feng_panic("feng_expression_equal: aggregate descriptor must not be NULL");
    }
    if (desc->equal_fn == NULL) {
        feng_panic("feng_expression_equal: aggregate descriptor '%s' has no equality function",
                   desc->name != NULL ? desc->name : "<unknown>");
    }
    return desc->equal_fn(left, right);
}

bool feng_expression_equal(const FengGenericParamDescriptor *type,
                           const void *left,
                           const void *right) {
    expression_require_value_carriers(type, left, right);

    switch (type->kind) {
        case FENG_VALUE_TRIVIAL:
            return expression_trivial_equal(type, left, right);
        case FENG_VALUE_MANAGED_POINTER:
            return expression_managed_pointer_equal(type, left, right);
        case FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS:
            return expression_aggregate_equal(type, left, right);
        default:
            feng_panic("feng_expression_equal: unsupported value kind=%d",
                       (int)type->kind);
    }
}
