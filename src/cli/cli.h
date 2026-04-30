#ifndef FENG_CLI_CLI_H
#define FENG_CLI_CLI_H

/*
 * Top-level CLI entry/router declarations shared between
 * src/cli/main.c and the per-command translation units.
 *
 * Phase 2 split (see dev/feng-phase2-guide-delivered.md):
 *   - main.c is reduced to a router.
 *   - tool/ owns `feng tool ...` subcommands, including single-file debug
 *     compile.
 *   - compile/ owns the top-level direct compile mode (P4) plus the shared
 *     implementation used by `feng tool compile`.
 */

void feng_cli_print_usage(const char *program);

int feng_cli_tool_main(const char *program, int argc, char **argv);

int feng_cli_project_build_main(const char *program, int argc, char **argv);
int feng_cli_project_check_main(const char *program, int argc, char **argv);
int feng_cli_project_run_main(const char *program, int argc, char **argv);
int feng_cli_project_clean_main(const char *program, int argc, char **argv);
int feng_cli_project_pack_main(const char *program, int argc, char **argv);

int feng_cli_legacy_compile_main(const char *program, int argc, char **argv);

/* P4 direct compile mode entry: `feng <files...> --target=bin --out=<dir>`. */
int feng_cli_direct_main(const char *program, int argc, char **argv);

#endif /* FENG_CLI_CLI_H */
