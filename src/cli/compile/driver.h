#ifndef FENG_CLI_COMPILE_DRIVER_H
#define FENG_CLI_COMPILE_DRIVER_H

#include <stdbool.h>
#include <stddef.h>

#include "semantic/semantic.h"

/*
 * Target C compiler driver (P5).
 *
 * Drives the post-codegen build:
 *   1. Resolve the runtime include directory and static library by probing
 *      paths relative to the running `feng` executable: the include root
 *      at `<exe>/../include/` (single fixed position — headers are
 *      platform-independent), and the static library at the requested
 *      platform path `<exe>/../lib/<platform>/`.
 *   2. Mine `extern fn` calling-convention annotations (`@cdecl`,
 *      `@stdcall`, `@fastcall`) across all current programs and flattened
 *      package `.ft` surfaces to derive additional `-l<lib>` link flags.
 *      The reserved library name "libc" / "c" is skipped because it is
 *      implicit on POSIX hosts. Other names have a leading "lib" prefix
 *      stripped.
 *   3. For `bin`, select the configured/bundled Clang, pass an
 *      explicit target plus SDK/sysroot, and compile the generated C source
 *      with the target runtime archive, platform support libraries, the
 *      derived link flags, and any explicit `--lib` inputs from direct
 *      mode, producing the final executable at `out_path`.
 *   4. For `lib`, compile with the same target inputs and archive the
 *      generated object into a static library at `out_path`.
 *
 * On success returns 0. On failure returns non-zero. Tool
 * failures intentionally preserve the generated C path so users can
 * inspect or pass it to a standalone compiler; earlier-phase cleanup is
 * handled by the direct/project command layer before invoking the driver.
 */

struct FengProgram;

typedef struct FengCliDriverOptions {
    /* argv[0] of the Feng process — used to locate runtime artefacts
     * relative to the executable. */
    const char *program_path;
    FengCompileTarget target;
    const char *platform;
    const char *sysroot;
    const char *c_path;
    const char *out_path;
    const struct FengProgram *const *programs;
    size_t program_count;
    const char *const *bundle_paths;
    size_t bundle_count;
    const char *const *link_libs;
    size_t link_lib_count;
    bool release;
    bool keep_intermediate;
} FengCliDriverOptions;

int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts);

#endif /* FENG_CLI_COMPILE_DRIVER_H */
