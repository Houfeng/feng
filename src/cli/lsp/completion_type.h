#ifndef FENG_CLI_LSP_COMPLETION_TYPE_H
#define FENG_CLI_LSP_COMPLETION_TYPE_H

#include "parser/parser.h"
#include "symbol/provider.h"

/* A request-owned type. Keeping the AST spelling first lets existing signature
 * formatters consume instantiated types without a second presentation model.
 * Every child of this view is another view from the same pinned request. */
typedef struct FengLspCompletionType {
    FengTypeRef ref;
    const FengSymbolDeclView *symbol;
    const void *parameter;
    FengSlice builtin;
} FengLspCompletionType;

/* Parameter keys are declaration identities, never unqualified names. */
typedef struct FengLspCompletionBinding {
    const void *parameter;
    FengLspCompletionType *value;
    struct FengLspCompletionBinding *next;
} FengLspCompletionBinding;

/* Scratch ownership is bounded by one completion request. */
typedef struct FengLspCompletionTypes {
    void *allocations;
    bool failed;
    bool (*nominal_equal)(const FengLspCompletionType *, const FengLspCompletionType *);
} FengLspCompletionTypes;

/* Allocate zeroed, aligned scratch storage, recording allocation failures. */
void *feng_lsp_completion_allocate(FengLspCompletionTypes *types, size_t count, size_t size);
/* Release all types, bindings and adapter scratch from a query. */
void feng_lsp_completion_types_dispose(FengLspCompletionTypes *types);
/* Compare full types, including nested arguments and array mutability. */
bool feng_lsp_completion_type_equal(FengLspCompletionTypes *types,
    const FengLspCompletionType *left, const FengLspCompletionType *right);
/* Match a target while collecting exact generic parameter substitutions. */
bool feng_lsp_completion_type_match(FengLspCompletionTypes *types,
    const FengLspCompletionType *target, FengLspCompletionType *value,
    FengLspCompletionBinding **bindings);
/* Apply a binding recursively without mutating source or provider types. */
FengLspCompletionType *feng_lsp_completion_type_substitute(FengLspCompletionTypes *types,
    FengLspCompletionType *type, const FengLspCompletionBinding *bindings);

#endif
