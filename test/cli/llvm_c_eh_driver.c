#include <stdio.h>
#include <string.h>

#include "cli/compile/driver.h"

/* Exercise the existing driver directly with test-owned C and exception runtime. */
int main(int argc, char **argv) {
    if (argc != 7 || (strcmp(argv[1], "bin") != 0 && strcmp(argv[1], "lib") != 0)) {
        fprintf(stderr, "usage: %s bin|lib PLATFORM RELEASE C OUTPUT SUPPORT_ARCHIVE\n", argv[0]);
        return 2;
    }
    const char *libraries[] = {argv[6]};
    FengCliDriverOptions options = {
        .program_path = argv[0],
        .target = strcmp(argv[1], "bin") == 0 ? FENG_COMPILE_TARGET_BIN : FENG_COMPILE_TARGET_LIB,
        .platform = argv[2],
        .release = strcmp(argv[3], "1") == 0,
        .c_path = argv[4],
        .out_path = argv[5],
        .link_libs = libraries,
        .link_lib_count = strcmp(argv[6], "-") == 0 ? 0U : 1U,
        .keep_intermediate = true
    };
    return feng_cli_compile_driver_invoke(&options);
}
