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

/* Opaque compile-time set of source declarations selected by the exact
 * package-public Symbol writer closure. */
typedef struct FengSymbolPackageSelection FengSymbolPackageSelection;

bool feng_symbol_build_graph(const FengSemanticAnalysis *analysis,
                             FengSymbolGraph **out_graph,
                             FengSymbolError *out_error);

bool feng_symbol_export_graph(const FengSymbolGraph *graph,
                              const FengSymbolExportOptions *options,
                              FengSymbolError *out_error);

bool feng_symbol_export_analysis(const FengSemanticAnalysis *analysis,
                                 const FengSymbolExportOptions *options,
                                 FengSymbolError *out_error);

/* Build the package-public declaration selection for an existing graph
 * without serializing FT. Outer compilation drivers may adapt this opaque
 * result to a core query interface without exposing Symbol types to Codegen. */
bool feng_symbol_build_package_selection(
    const FengSymbolGraph *graph,
    FengSymbolPackageSelection **out_selection,
    FengSymbolError *out_error);

/* Return whether one AST declaration/member belongs to the selected
 * package-public declaration closure. */
bool feng_symbol_package_selection_contains(
    const FengSymbolPackageSelection *selection,
    const void *source_node);

/* Release a package-public source selection. */
void feng_symbol_package_selection_free(
    FengSymbolPackageSelection *selection);

#ifdef __cplusplus
}
#endif

#endif /* FENG_SYMBOL_EXPORT_H */
