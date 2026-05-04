#ifndef FENG_CLI_COMPILE_DRIVER_H
#define FENG_CLI_COMPILE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

#include "semantic/semantic.h"

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
 *   3. For `bin`, spawn ${CC:-cc} with a fixed compiler flag set, the
 *      generated C source, the runtime archive, `-lpthread`, and the
 *      derived link flags, producing the final executable at `out_path`.
 *   4. For `lib`, compile the generated C to an object file and archive it
 *      into a static library at `out_path`.
 *
 * On success returns 0. On failure returns non-zero. Host-compiler
 * failures intentionally preserve the generated C path so users can
 * inspect or pass it to a standalone compiler; earlier-phase cleanup is
 * handled by the direct/project command layer before invoking the driver.
 */

struct FengProgram;

typedef struct FengCliDriverOptions {
    /* argv[0] of the host process — used to locate runtime artefacts
     * relative to the executable when no environment override is set. */
    const char *program_path;
    FengCompileTarget target;
    const char *c_path;
    const char *out_path;
    const struct FengProgram *const *programs;
    size_t program_count;
    const char *const *bundle_paths;
    size_t bundle_count;
    bool release;
    bool keep_intermediate;
} FengCliDriverOptions;

int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts);

#endif /* FENG_CLI_COMPILE_DRIVER_H */
