#ifndef FENG_SYMBOL_SYMBOL_H
#define FENG_SYMBOL_SYMBOL_H

#include <stdbool.h>
#include <stddef.h>

#include "parser/parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FengSymbolProfile {
    FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC = 1,
    FENG_SYMBOL_PROFILE_WORKSPACE_CACHE = 2
} FengSymbolProfile;

typedef enum FengSymbolDeclKind {
    FENG_SYMBOL_DECL_KIND_MODULE = 0,
    FENG_SYMBOL_DECL_KIND_TYPE,
    FENG_SYMBOL_DECL_KIND_SPEC,
    FENG_SYMBOL_DECL_KIND_FIT,
    FENG_SYMBOL_DECL_KIND_FUNCTION,
    FENG_SYMBOL_DECL_KIND_BINDING,
    FENG_SYMBOL_DECL_KIND_FIELD,
    FENG_SYMBOL_DECL_KIND_METHOD,
    FENG_SYMBOL_DECL_KIND_CONSTRUCTOR,
    FENG_SYMBOL_DECL_KIND_FINALIZER
} FengSymbolDeclKind;

typedef enum FengSymbolTypeKind {
    FENG_SYMBOL_TYPE_KIND_INVALID = 0,
    FENG_SYMBOL_TYPE_KIND_BUILTIN,
    FENG_SYMBOL_TYPE_KIND_NAMED,
    FENG_SYMBOL_TYPE_KIND_POINTER,
    FENG_SYMBOL_TYPE_KIND_ARRAY
} FengSymbolTypeKind;

typedef struct FengSymbolGraph FengSymbolGraph;
typedef struct FengSymbolModuleGraph FengSymbolModuleGraph;
typedef struct FengSymbolTypeView FengSymbolTypeView;

typedef struct FengSymbolError {
    const char *path;
    char *message;
    FengToken token;
} FengSymbolError;

size_t feng_symbol_graph_module_count(const FengSymbolGraph *graph);
const FengSymbolModuleGraph *feng_symbol_graph_module_at(const FengSymbolGraph *graph,
                                                         size_t index);

void feng_symbol_graph_free(FengSymbolGraph *graph);
void feng_symbol_error_free(FengSymbolError *error);

#ifdef __cplusplus
}
#endif

#endif /* FENG_SYMBOL_SYMBOL_H */
