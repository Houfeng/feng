#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cli/cli.h"
#include "cli/tool/tool.h"

#ifndef FENG_CLI_VERSION
#error "FENG_CLI_VERSION must be defined from the repository VERSION file"
#endif

#define FENG_CLI_USAGE_DESCRIPTION_COLUMN 45

static void feng_cli_print_version(const char *program, FILE *stream) {
    fprintf(stream, "%s %s\n", program, FENG_CLI_VERSION);
}

/* Prints one usage syntax line with its description aligned to the help column. */
static void feng_cli_print_usage_line(FILE *stream,
                                      const char *description,
                                      const char *format,
                                      ...) {
    va_list args;
    int syntax_width;

    va_start(args, format);
    syntax_width = vfprintf(stream, format, args);
    va_end(args);

    if (syntax_width < 0) return;
    if (syntax_width < FENG_CLI_USAGE_DESCRIPTION_COLUMN) {
        fprintf(stream, "%*s",
                FENG_CLI_USAGE_DESCRIPTION_COLUMN - syntax_width,
                "");
    } else {
        fputc(' ', stream);
    }
    fprintf(stream, "%s\n", description);
}

void feng_cli_print_usage(const char *program, FILE *stream) {
    int compile_indent = (int)(2U + strlen(program) + strlen(" <files>... "));
    int project_indent = (int)(2U + strlen(program) + strlen(" init       "));
    int deps_indent = (int)(2U + strlen(program) + strlen(" deps       install "));

    if (stream == stderr) fprintf(stream, "\n");
    fprintf(stream, "Usage:\n");
    feng_cli_print_usage_line(
        stream, "Project command", "  %s <command>  [options]", program);
    feng_cli_print_usage_line(
        stream, "Direct compile", "  %s <files>... [options]", program);
    fprintf(stream, "\n");
    fprintf(stream, "Project:\n");
    feng_cli_print_usage_line(
        stream, "Project name", "  %s init       [<name>]", program);
    feng_cli_print_usage_line(
        stream, "Project type", "%*s[--target=bin|lib]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Project root", "  %s build      [<path>]", program);
    feng_cli_print_usage_line(
        stream, "Release mode", "%*s[--release]", project_indent, "");
    feng_cli_print_usage_line(stream,
                              "Target platform",
                              "%*s[--platform=<platform>]...",
                              project_indent,
                              "");
    feng_cli_print_usage_line(
        stream, "Target sysroot", "%*s[--sysroot=<path>]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Keep IR", "%*s[--keep-ir]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Project root", "  %s check      [<path>]", program);
    feng_cli_print_usage_line(
        stream, "Input format", "%*s[--format=text|json]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Project root", "  %s run        [<path>]", program);
    feng_cli_print_usage_line(
        stream, "Release mode", "%*s[--release]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Keep IR", "%*s[--keep-ir]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Program arguments", "%*s[-- <args>...]", project_indent, "");
    feng_cli_print_usage_line(
        stream, "Project root", "  %s clean      [<path>]", program);
    feng_cli_print_usage_line(
        stream, "Project root", "  %s pack       [<path>]", program);
    feng_cli_print_usage_line(stream,
                              "Target platform",
                              "%*s[--platform=<platform>]...",
                              project_indent,
                              "");
    feng_cli_print_usage_line(
        stream, "Target sysroot", "%*s[--sysroot=<path>]", project_indent, "");
    fprintf(stream, "\n");
    fprintf(stream, "Dependence:\n");
    feng_cli_print_usage_line(
        stream, "Package name", "  %s deps       add     <pkg-name>", program);
    feng_cli_print_usage_line(
        stream, "Project root", "%*s[<path>]", deps_indent, "");
    feng_cli_print_usage_line(
        stream, "Package name", "  %s deps       remove  <pkg-name>", program);
    feng_cli_print_usage_line(
        stream, "Project root", "%*s[<path>]", deps_indent, "");
    feng_cli_print_usage_line(
        stream, "Project root", "  %s deps       install [<path>]", program);
    feng_cli_print_usage_line(
        stream, "Force reinstall", "%*s[--force]", deps_indent, "");
    fprintf(stream, "\n");
    fprintf(stream, "Compile:\n");
    feng_cli_print_usage_line(
        stream, "Project type", "  %s <files>... [--target=bin|lib]", program);
    feng_cli_print_usage_line(stream,
                              "Target platform",
                              "%*s[--platform=<platform>]",
                              compile_indent,
                              "");
    feng_cli_print_usage_line(
        stream, "Target sysroot", "%*s[--sysroot=<path>]", compile_indent, "");
    feng_cli_print_usage_line(
        stream, "Output directory", "%*s[--out=<dir>]", compile_indent, "");
    feng_cli_print_usage_line(
        stream, "Artifact name", "%*s[--name=<artifact>]", compile_indent, "");
    feng_cli_print_usage_line(
        stream, "Release mode", "%*s[--release]", compile_indent, "");
    feng_cli_print_usage_line(
        stream, "Keep IR", "%*s[--keep-ir]", compile_indent, "");
    feng_cli_print_usage_line(
        stream, "Package list", "%*s[--pkg=<fb-path>]...", compile_indent, "");
    feng_cli_print_usage_line(
        stream, "Library list", "%*s[--lib=<lib-path>]...", compile_indent, "");
    fprintf(stream, "\n");
    fprintf(stream, "Protocol:\n");
    feng_cli_print_usage_line(
        stream, "Start LSP", "  %s lsp        [--stdio]", program);
    feng_cli_print_usage_line(
        stream, "Start DAP", "  %s dap        [--stdio]", program);
    fprintf(stream, "\n");
    fprintf(stream, "Global:\n");
    feng_cli_print_usage_line(stream, "Display help message", "  -h, --help");
    feng_cli_print_usage_line(
        stream, "Display version info", "  -v, --version");
    fprintf(stream, "\n");
    // fprintf(stderr, "  %s tool compile [--target=bin|lib] [--emit-c=<path>] <file>\n", program);
    // fprintf(stderr, "  %s tool lex <file>\n", program);
    // fprintf(stderr, "  %s tool parse <file>\n", program);
    // fprintf(stderr, "  %s tool semantic [--target=bin|lib] <file> [more files...]\n", program);
    // fprintf(stderr, "  %s tool check [--target=bin|lib] <file> [more files...]\n", program);
    // fprintf(stderr, "\n");
    // fprintf(stderr, "  Direct mode: compile one or more .ff files into <out>/bin via <out>/ir/c.\n");
    // fprintf(stderr, "  --target defaults to 'bin'; '--target=lib' is reserved for `tool` analysis.\n");
    // fprintf(stderr, "  --release selects the release build mode for project/direct builds.\n");
}

int main(int argc, char **argv) {
    const char *program = "feng";
    if (argc > 0 && argv[0] != NULL && argv[0][0] != '\0') {
        const char *slash = strrchr(argv[0], '/');
        program = slash != NULL ? slash + 1 : argv[0];
    }

    if (argc < 2) {
        feng_cli_print_usage(program, stdout);
        return 0;
    }

    const char *cmd = argv[1];
    int rest_argc = argc - 2;
    char **rest_argv = argv + 2;

    if (strcmp(cmd, "tool") == 0) {
        return feng_cli_tool_main(program, rest_argc, rest_argv);
    }

    if (strcmp(cmd, "init") == 0) {
        return feng_cli_project_init_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "build") == 0) {
        return feng_cli_project_build_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "check") == 0) {
        return feng_cli_project_check_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "run") == 0) {
        return feng_cli_project_run_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "clean") == 0) {
        return feng_cli_project_clean_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "pack") == 0) {
        return feng_cli_project_pack_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "deps") == 0) {
        return feng_cli_deps_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "lsp") == 0) {
        return feng_cli_lsp_main(program, rest_argc, rest_argv);
    }
    if (strcmp(cmd, "dap") == 0) {
        return feng_cli_dap_main(program, rest_argc, rest_argv);
    }

    if (strcmp(cmd, "compile") == 0
        || strcmp(cmd, "lex") == 0
        || strcmp(cmd, "parse") == 0
        || strcmp(cmd, "semantic") == 0) {
        fprintf(stderr,
                "`%s %s ...` is no longer a top-level command; use `%s tool %s ...` instead.\n",
                program, cmd, program, cmd);
        feng_cli_print_usage(program, stderr);
        return 1;
    }

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        feng_cli_print_usage(program, stdout);
        return 0;
    }
    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        feng_cli_print_version(program, stdout);
        return 0;
    }

    /* Default route: top-level direct compile mode (P4). Everything from
     * argv[1] onwards is treated as direct-mode arguments (file paths and
     * flags), matching `feng <files...> --target=bin --out=<dir>`. */
    return feng_cli_direct_main(program, argc - 1, argv + 1);
}
