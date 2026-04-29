#ifndef FENG_CLI_COMPILE_DRIVER_H
#define FENG_CLI_COMPILE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Host C compiler driver (P5).
 *
 * Drives the post-codegen build:
 *   1. Resolve the runtime include directory and static library, either
 *      via FENG_RUNTIME_INCLUDE / FENG_RUNTIME_LIB environment variables
 *      or by probing paths relative to the running `feng` executable.
 *   2. Mine `extern fn @cdecl("<lib>")` annotations across all programs
 *      to derive additional `-l<lib>` link flags. The reserved library
 *      name "libc" / "c" is skipped because it is implicit on POSIX
 *      hosts. Other names have a leading "lib" prefix stripped.
 *   3. Spawn ${CC:-cc} with a fixed compiler flag set, the generated C
 *      source, the runtime archive, `-lpthread`, and the derived link
 *      flags, producing the final executable at `out_bin_path`.
 *
 * On success returns 0. On failure returns non-zero; the caller is
 * expected to preserve the C path so users can inspect or pass it to a
 * standalone compiler.
 */

struct FengProgram;

typedef struct FengCliDriverOptions {
    /* argv[0] of the host process — used to locate runtime artefacts
     * relative to the executable when no environment override is set. */
    const char *program_path;
    const char *c_path;
    const char *out_bin_path;
    const struct FengProgram *const *programs;
    size_t program_count;
    bool keep_intermediate;
} FengCliDriverOptions;

int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts);

#endif /* FENG_CLI_COMPILE_DRIVER_H */
