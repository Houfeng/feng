#ifndef FENG_CLI_COMPILE_OPTIONS_H
#define FENG_CLI_COMPILE_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>

#include "cli/cli.h"
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

FengCliParseResult feng_cli_legacy_compile_parse(const char *program,
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
    FengCompileTarget target;     /* Direct mode supports BIN and LIB outputs. */
    const char *out_dir;          /* required: <out>/ir/c plus <out>/bin or <out>/lib */
    bool release;                 /* Selects release-vs-debug build mode. */
    bool keep_intermediate;       /* P5: keep generated C across failures/success. */
    const char *artifact_name;    /* optional override for the produced artifact stem */
    const char *platform;         /* required complete target platform */
    const char *sysroot;          /* optional target SDK/sysroot override */
    int input_count;
    const char **inputs;          /* heap-allocated array of borrowed argv ptrs */
    int package_path_count;
    const char **package_paths;   /* heap-allocated array of borrowed argv ptrs */
    int link_lib_count;
    const char **link_libs;       /* heap-allocated array of borrowed argv ptrs */
} FengCliDirectOptions;

FengCliParseResult feng_cli_direct_options_parse(const char *program,
                                                 int argc,
                                                 char **argv,
                                                 FengCliDirectOptions *out);
void feng_cli_direct_options_dispose(FengCliDirectOptions *opts);

#endif /* FENG_CLI_COMPILE_OPTIONS_H */
