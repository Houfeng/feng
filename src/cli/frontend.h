#ifndef FENG_CLI_FRONTEND_H
#define FENG_CLI_FRONTEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cli/common.h"
#include "semantic/semantic.h"

typedef struct FengSymbolImportedModuleCache FengSymbolImportedModuleCache;

/*
 * Shared frontend pipeline used by `feng tool semantic`, `feng tool check`,
 * and (Phase 2 P4) the top-level direct compile mode.
 *
 * The pipeline owns:
 *   - loading every input file into a FengCliLoadedSource
 *   - parsing each program
 *   - running feng_semantic_analyze across all programs
 *   - emitting diagnostics through caller-supplied sinks
 *
 * Diagnostics are reported via callbacks so both the human-friendly
 * (`tool semantic`) and JSON (`tool check`) callers can share one
 * traversal. Each callback is optional; pass NULL to skip.
 *
 * On any failure (file IO, parse, semantic), feng_cli_frontend_run sets
 * *out_status to a non-zero exit code; on success status remains 0.
 *
 * On success the caller may take ownership of the produced analysis,
 * loaded sources, and imported-module cache via the matching out_* fields.
 * Pass NULL for any out_* the caller does not want; this releases the
 * pipeline-owned state automatically.
 */

typedef struct FengCliFrontendInput {
    int path_count;
    char **paths;
    FengCompileTarget target;
    int package_path_count;
    const char **package_paths;
} FengCliFrontendInput;

typedef struct FengCliFrontendCallbacks {
    /* Called for each parse error encountered. */
    void (*on_parse_error)(void *user,
                           const char *path,
                           const FengParseError *error,
                           const FengCliLoadedSource *source);

    /* Called once per semantic error. error_index/error_count expose
     * positional context for callers that emit separators. */
    void (*on_semantic_error)(void *user,
                              const FengSemanticError *error,
                              size_t error_index,
                              size_t error_count,
                              const FengCliLoadedSource *source);

    /* Called once per semantic info diagnostic. */
    void (*on_semantic_info)(void *user,
                             const FengSemanticInfo *info,
                             size_t info_index,
                             size_t info_count,
                             const FengCliLoadedSource *source);

    void *user;
} FengCliFrontendCallbacks;

typedef struct FengCliFrontendOutputs {
    FengSemanticAnalysis **out_analysis;
    FengCliLoadedSource **out_sources;
    size_t *out_source_count;
    FengSymbolImportedModuleCache **out_imported_module_cache;
    char ***out_bundle_paths;
    size_t *out_bundle_count;
} FengCliFrontendOutputs;

void feng_cli_frontend_bundle_paths_dispose(char **bundle_paths,
                                            size_t bundle_count);

/*
 * Run the shared frontend pipeline. Returns 0 on success, non-zero on
 * failure. Callbacks are invoked synchronously during the run.
 */
int feng_cli_frontend_run(const FengCliFrontendInput *input,
                          const FengCliFrontendCallbacks *callbacks,
                          const FengCliFrontendOutputs *outputs);

#endif /* FENG_CLI_FRONTEND_H */
