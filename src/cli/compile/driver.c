#include "cli/compile/driver.h"

#include <stdio.h>

/* P5 will replace this stub with the real host-cc invocation. Until then
 * the stub returns a recognisable non-fatal status so the P4 orchestration
 * can still produce <out>/ir/c artifacts and surface a clear next step. */
int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts) {
    if (opts == NULL) return 2;
    fprintf(stderr,
            "warning: host C compiler driver not yet implemented (P5).\n"
            "  emitted: %s\n"
            "  intended: %s\n",
            opts->c_path ? opts->c_path : "(none)",
            opts->out_bin_path ? opts->out_bin_path : "(none)");
    return 0;
}
