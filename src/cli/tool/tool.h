#ifndef FENG_CLI_TOOL_TOOL_H
#define FENG_CLI_TOOL_TOOL_H

#include <stdio.h>

/*
 * `feng tool ...` subroute.
 *
 * Each file under src/cli/tool/ owns one subcommand and exposes a single
 * `feng_cli_tool_<name>_main(program, argc, argv)` entry, where argv is
 * positioned at the subcommand's own arguments (i.e. argv[0] is the first
 * token after the subcommand name).
 */

int feng_cli_tool_lex_main(const char *program, int argc, char **argv);
int feng_cli_tool_parse_main(const char *program, int argc, char **argv);
int feng_cli_tool_semantic_main(const char *program, int argc, char **argv);
int feng_cli_tool_check_main(const char *program, int argc, char **argv);

void feng_cli_tool_print_usage(const char *program, FILE *stream);

#endif /* FENG_CLI_TOOL_TOOL_H */
