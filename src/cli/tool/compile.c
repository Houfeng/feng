#include "cli/tool/tool.h"

#include "cli/cli.h"

int feng_cli_tool_compile_main(const char *program, int argc, char **argv) {
    return feng_cli_legacy_compile_main(program, argc, argv);
}
