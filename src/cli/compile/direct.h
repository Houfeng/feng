#ifndef FENG_CLI_COMPILE_DIRECT_H
#define FENG_CLI_COMPILE_DIRECT_H

#include <stddef.h>

#include "cli/compile/options.h"
#include "codegen/mapping.h"

/* Project/build-owned source mappings forwarded into direct compile. */
typedef struct FengCliDirectDebugContext {
    const FengCodegenMapingSourceMapping *sources;
    size_t source_count;
    const char *const *dependency_fd_paths;
    size_t dependency_fd_count;
} FengCliDirectDebugContext;

/* Internal direct-compile entry that consumes option arrays and optional debug context. */
int feng_cli_direct_run(const char *program,
                        FengCliDirectOptions *options,
                        const FengCliDirectDebugContext *debug_context);

#endif /* FENG_CLI_COMPILE_DIRECT_H */
