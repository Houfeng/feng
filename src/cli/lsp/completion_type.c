#include "cli/lsp/completion_type.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The payload following this header is aligned for every C object type. */
typedef union CompletionAllocation {
    union CompletionAllocation *next;
    max_align_t alignment;
} CompletionAllocation;

/* All failures propagate through the request rather than producing partial data. */
void *feng_lsp_completion_allocate(FengLspCompletionTypes *types, size_t count, size_t size) {
    if (size != 0U && count > (SIZE_MAX - sizeof(CompletionAllocation)) / size) {
        types->failed = true;
        return NULL;
    }
    CompletionAllocation *allocation = calloc(1U, sizeof(*allocation) + count * size);
    if (allocation == NULL) { types->failed = true; return NULL; }
    allocation->next = types->allocations;
    types->allocations = allocation;
    return allocation + 1;
}

/* Types may share child nodes; the allocation list frees every object once. */
void feng_lsp_completion_types_dispose(FengLspCompletionTypes *types) {
    CompletionAllocation *allocation = types->allocations;
    while (allocation != NULL) {
        CompletionAllocation *next = allocation->next;
        free(allocation);
        allocation = next;
    }
    types->allocations = NULL;
}

/* Child syntax points to the first field of the corresponding complete view. */
static const FengLspCompletionType *child_type(const FengTypeRef *ref) {
    return (const FengLspCompletionType *)ref;
}

/* Find a substitution without allowing one scope's parameter to capture another. */
static FengLspCompletionType *binding_value(const FengLspCompletionBinding *bindings,
                                           const void *parameter) {
    for (; bindings != NULL; bindings = bindings->next)
        if (bindings->parameter == parameter) return bindings->value;
    return NULL;
}

/* Identity and structure jointly determine equality; unresolved names do not. */
bool feng_lsp_completion_type_equal(FengLspCompletionTypes *types,
    const FengLspCompletionType *left, const FengLspCompletionType *right) {
    if (left == NULL || right == NULL || left->ref.kind != right->ref.kind) return false;
    if (left->parameter != NULL || right->parameter != NULL)
        return left->parameter != NULL && left->parameter == right->parameter;
    if (left->builtin.length != 0U || right->builtin.length != 0U)
        return left->builtin.length == right->builtin.length &&
            memcmp(left->builtin.data, right->builtin.data, left->builtin.length) == 0;
    if (left->ref.kind != FENG_TYPE_REF_NAMED)
        return (left->ref.kind != FENG_TYPE_REF_ARRAY || left->ref.array_element_writable == right->ref.array_element_writable) &&
            feng_lsp_completion_type_equal(types, child_type(left->ref.as.inner), child_type(right->ref.as.inner));
    if (!types->nominal_equal(left, right) || left->ref.as.named.type_arg_count != right->ref.as.named.type_arg_count) return false;
    for (size_t i = 0U; i < left->ref.as.named.type_arg_count; ++i)
        if (!feng_lsp_completion_type_equal(types, child_type(left->ref.as.named.type_args[i]), child_type(right->ref.as.named.type_args[i]))) return false;
    return true;
}

/* A failed candidate owns its binding-list head; callers discard that head. */
bool feng_lsp_completion_type_match(FengLspCompletionTypes *types,
    const FengLspCompletionType *target, FengLspCompletionType *value,
    FengLspCompletionBinding **bindings) {
    if (target == NULL || value == NULL) return false;
    if (target->parameter != NULL) {
        FengLspCompletionType *bound = binding_value(*bindings, target->parameter);
        if (bound != NULL) return feng_lsp_completion_type_equal(types, bound, value);
        FengLspCompletionBinding *binding = feng_lsp_completion_allocate(types, 1U, sizeof(*binding));
        if (binding == NULL) return false;
        *binding = (FengLspCompletionBinding){target->parameter, value, *bindings};
        *bindings = binding;
        return true;
    }
    if (target->ref.kind != value->ref.kind) return false;
    if (target->ref.kind != FENG_TYPE_REF_NAMED)
        return (target->ref.kind != FENG_TYPE_REF_ARRAY || target->ref.array_element_writable == value->ref.array_element_writable) &&
            feng_lsp_completion_type_match(types, child_type(target->ref.as.inner), (FengLspCompletionType *)value->ref.as.inner, bindings);
    if (target->builtin.length != 0U || value->builtin.length != 0U)
        return feng_lsp_completion_type_equal(types, target, value);
    if (!types->nominal_equal(target, value) || target->ref.as.named.type_arg_count != value->ref.as.named.type_arg_count) return false;
    for (size_t i = 0U; i < target->ref.as.named.type_arg_count; ++i)
        if (!feng_lsp_completion_type_match(types, child_type(target->ref.as.named.type_args[i]),
            (FengLspCompletionType *)value->ref.as.named.type_args[i], bindings)) return false;
    return true;
}

/* Share unchanged leaves while substituting nested types in request storage. */
FengLspCompletionType *feng_lsp_completion_type_substitute(FengLspCompletionTypes *types,
    FengLspCompletionType *type, const FengLspCompletionBinding *bindings) {
    if (type == NULL || bindings == NULL) return type;
    if (type->parameter != NULL) {
        FengLspCompletionType *value = binding_value(bindings, type->parameter);
        return value != NULL ? value : type;
    }
    if (type->ref.kind == FENG_TYPE_REF_NAMED && type->ref.as.named.type_arg_count == 0U) return type;
    FengLspCompletionType *result = feng_lsp_completion_allocate(types, 1U, sizeof(*result));
    if (result == NULL) return NULL;
    *result = *type;
    if (type->ref.kind != FENG_TYPE_REF_NAMED) {
        FengLspCompletionType *inner = feng_lsp_completion_type_substitute(types, (FengLspCompletionType *)type->ref.as.inner, bindings);
        result->ref.as.inner = inner != NULL ? &inner->ref : NULL;
    } else {
        size_t count = type->ref.as.named.type_arg_count;
        result->ref.as.named.type_args = feng_lsp_completion_allocate(types, count, sizeof(FengTypeRef *));
        if (result->ref.as.named.type_args == NULL) return NULL;
        for (size_t i = 0U; i < count; ++i) {
            FengLspCompletionType *argument = feng_lsp_completion_type_substitute(types, (FengLspCompletionType *)type->ref.as.named.type_args[i], bindings);
            result->ref.as.named.type_args[i] = argument != NULL ? &argument->ref : NULL;
        }
    }
    return result;
}
