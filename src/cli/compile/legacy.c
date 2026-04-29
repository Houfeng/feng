#include "cli/cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/common.h"
#include "cli/compile/options.h"
#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

static int run_legacy_compile(const FengCliLegacyCompileOptions *opts) {
    FengCliLoadedSource src = {0};
    const FengProgram *prog_ptr = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    int exit_code = 0;

    src.path = opts->input_path;
    src.source = feng_cli_read_entire_file(opts->input_path, &src.source_length);
    if (src.source == NULL) {
        fprintf(stderr, "failed to read %s: %s\n", opts->input_path, strerror(errno));
        return 1;
    }
    {
        FengParseError perr;
        if (!feng_parse_source(src.source, src.source_length, opts->input_path,
                               &src.program, &perr)) {
            feng_cli_print_diagnostic(stderr, opts->input_path, "parse error", perr.message,
                                      &perr.token, src.source, src.source_length);
            exit_code = 1;
            goto cleanup;
        }
    }
    prog_ptr = src.program;
    if (!feng_semantic_analyze(&prog_ptr, 1U, opts->target,
                               &analysis, &errors, &error_count)) {
        for (size_t i = 0; i < error_count; ++i) {
            if (i > 0) fputc('\n', stderr);
            feng_cli_print_diagnostic(stderr, errors[i].path, "semantic error",
                                      errors[i].message, &errors[i].token,
                                      src.source, src.source_length);
        }
        exit_code = 1;
        goto cleanup;
    }
    if (!feng_codegen_emit_program(analysis, opts->target, NULL, &out, &cgerr)) {
        feng_cli_print_diagnostic(stderr, opts->input_path, "codegen error",
                                  cgerr.message ? cgerr.message : "unknown",
                                  &cgerr.token, src.source, src.source_length);
        exit_code = 1;
        goto cleanup;
    }
    if (opts->emit_c_path != NULL && strcmp(opts->emit_c_path, "-") != 0) {
        FILE *f = fopen(opts->emit_c_path, "wb");
        if (!f) {
            fprintf(stderr, "failed to open %s for write: %s\n",
                    opts->emit_c_path, strerror(errno));
            exit_code = 1;
            goto cleanup;
        }
        size_t w = fwrite(out.c_source, 1U, out.c_source_length, f);
        fclose(f);
        if (w != out.c_source_length) {
            fprintf(stderr, "failed to write %s\n", opts->emit_c_path);
            exit_code = 1;
            goto cleanup;
        }
    } else {
        fwrite(out.c_source, 1U, out.c_source_length, stdout);
    }

cleanup:
    feng_codegen_error_free(&cgerr);
    feng_codegen_output_free(&out);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(src.program);
    free(src.source);
    return exit_code;
}

int feng_cli_legacy_compile_main(const char *program, int argc, char **argv) {
    FengCliLegacyCompileOptions opts;

    if (!feng_cli_legacy_compile_parse(program, argc, argv, &opts)) {
        return 1;
    }
    return run_legacy_compile(&opts);
}
