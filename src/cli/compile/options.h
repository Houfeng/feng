#ifndef FENG_CLI_COMPILE_OPTIONS_H
#define FENG_CLI_COMPILE_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

#include "semantic/semantic.h"

/*
 * Phase 2 P4 introduces a richer top-level direct compile mode.
 * The single-file debug compile path remains available via `feng tool compile`.
 */

typedef struct FengCliLegacyCompileOptions {
    FengCompileTarget target;
    const char *emit_c_path;
    const char *input_path;
} FengCliLegacyCompileOptions;

bool feng_cli_legacy_compile_parse(const char *program,
                                   int argc,
                                   char **argv,
                                   FengCliLegacyCompileOptions *out);

/* P4 direct compile mode options.
 *
 * Inputs come from the top-level invocation `feng <files...> [flags]`.
 * `inputs` borrows pointers into argv and is freed by the caller via
 * feng_cli_direct_options_dispose.
 */
typedef struct FengCliDirectOptions {
    FengCompileTarget target;     /* P4: only BIN supported. */
    const char *out_dir;          /* required: <out>/ir/c, <out>/bin */
    bool release;                 /* P4: parsed but reported as not-yet-implemented */
    bool keep_intermediate;       /* P5: keep generated C across failures/success. */
    const char *artifact_name;    /* optional override for the produced artifact stem */
    int input_count;
    const char **inputs;          /* heap-allocated array of borrowed argv ptrs */
    int package_path_count;
    const char **package_paths;   /* heap-allocated array of borrowed argv ptrs */
} FengCliDirectOptions;

bool feng_cli_direct_options_parse(const char *program,
                                   int argc,
                                   char **argv,
                                   FengCliDirectOptions *out);
void feng_cli_direct_options_dispose(FengCliDirectOptions *opts);

#endif /* FENG_CLI_COMPILE_OPTIONS_H */
