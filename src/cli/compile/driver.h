#ifndef FENG_CLI_COMPILE_DRIVER_H
#define FENG_CLI_COMPILE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Host C compiler driver (P5).
 *
 * Inputs:
 *   c_path             : absolute or workspace-relative path to the
 *                        codegen-produced C source aggregate (one file per
 *                        invocation; multi-file Feng programs are merged
 *                        upstream by codegen).
 *   out_bin_path       : final executable path (caller has already created
 *                        the parent directory).
 *   programs/program_count: pointers into the analysis program list, used
 *                        to mine `extern fn @cdecl(<lib>)` annotations for
 *                        additional `-l<lib>` link flags.
 *
 * Returns 0 on success, non-zero on failure. On failure the C path is
 * preserved by the caller for inspection.
 */

struct FengProgram;

typedef struct FengCliDriverOptions {
    const char *c_path;
    const char *out_bin_path;
    const struct FengProgram *const *programs;
    size_t program_count;
} FengCliDriverOptions;

int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts);

#endif /* FENG_CLI_COMPILE_DRIVER_H */
