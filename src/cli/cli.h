#ifndef FENG_CLI_CLI_H
#define FENG_CLI_CLI_H

/*
 * Top-level CLI entry/router declarations shared between
 * src/cli/main.c and the per-command translation units.
 *
 * Phase 2 split (see dev/feng-phase2-pending.md):
 *   - main.c is reduced to a router.
 *   - tool/ owns `feng tool ...` subcommands.
 *   - compile/ owns the top-level direct compile mode (P4) and the
 *     legacy `feng compile` debug subcommand until P1 retires it.
 */

void feng_cli_print_usage(const char *program);

int feng_cli_tool_main(const char *program, int argc, char **argv);

int feng_cli_legacy_compile_main(const char *program, int argc, char **argv);

/* P4 direct compile mode entry: `feng <files...> --target=bin --out=<dir>`. */
int feng_cli_direct_main(const char *program, int argc, char **argv);

#endif /* FENG_CLI_CLI_H */
