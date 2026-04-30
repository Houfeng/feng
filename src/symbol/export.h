#ifndef FENG_SYMBOL_EXPORT_H
#define FENG_SYMBOL_EXPORT_H

#include <stdbool.h>

#include "semantic/semantic.h"
#include "symbol/symbol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengSymbolExportOptions {
    const char *public_root;
    const char *workspace_root;
    bool emit_docs;
    bool emit_spans;
} FengSymbolExportOptions;

bool feng_symbol_build_graph(const FengSemanticAnalysis *analysis,
                             FengSymbolGraph **out_graph,
                             FengSymbolError *out_error);

bool feng_symbol_export_graph(const FengSymbolGraph *graph,
                              const FengSymbolExportOptions *options,
                              FengSymbolError *out_error);

bool feng_symbol_export_analysis(const FengSemanticAnalysis *analysis,
                                 const FengSymbolExportOptions *options,
                                 FengSymbolError *out_error);

#ifdef __cplusplus
}
#endif

#endif /* FENG_SYMBOL_EXPORT_H */
