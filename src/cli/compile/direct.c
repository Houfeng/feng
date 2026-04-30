#include "cli/cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cli/common.h"
#include "cli/compile/driver.h"
#include "cli/compile/options.h"
#include "cli/frontend.h"
#include "codegen/codegen.h"

/* --- helpers ------------------------------------------------------------- */

/* Recursively `mkdir -p` for an arbitrary directory path. Returns 0 on
 * success or if the directory already exists; non-zero with errno set on
 * failure. The path is mutated in-place during traversal but restored
 * before return. */
static int mkdirs(const char *path) {
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(path);
    char *buf = malloc(len + 1U);
    if (buf == NULL) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(buf, path, len + 1U);
    /* Walk every component, creating it if missing. */
    for (size_t i = 1U; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
                int saved = errno;
                free(buf);
                errno = saved;
                return -1;
            }
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0775) != 0 && errno != EEXIST) {
        int saved = errno;
        free(buf);
        errno = saved;
        return -1;
    }
    free(buf);
    return 0;
}

static char *path_join(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    bool need_sep = (la > 0U && a[la - 1U] != '/');
    char *out = malloc(la + (need_sep ? 1U : 0U) + lb + 1U);
    if (out == NULL) return NULL;
    memcpy(out, a, la);
    size_t cursor = la;
    if (need_sep) out[cursor++] = '/';
    memcpy(out + cursor, b, lb);
    out[cursor + lb] = '\0';
    return out;
}

/* --- diagnostic callbacks ----------------------------------------------- */

static void on_parse_error(void *user,
                           const char *path,
                           const FengParseError *error,
                           const FengCliLoadedSource *source) {
    (void)user;
    feng_cli_print_diagnostic(stderr, path, "parse error", error->message,
                              &error->token,
                              source != NULL ? source->source : NULL,
                              source != NULL ? source->source_length : 0U);
}

static void on_semantic_error(void *user,
                              const FengSemanticError *error,
                              size_t error_index,
                              size_t error_count,
                              const FengCliLoadedSource *source) {
    (void)user;
    (void)error_count;
    if (error_index > 0U) fputc('\n', stderr);
    feng_cli_print_diagnostic(stderr, error->path, "semantic error",
                              error->message, &error->token,
                              source != NULL ? source->source : NULL,
                              source != NULL ? source->source_length : 0U);
}

static void on_semantic_info(void *user,
                             const FengSemanticInfo *info,
                             size_t info_index,
                             size_t info_count,
                             const FengCliLoadedSource *source) {
    (void)user;
    (void)info_index;
    (void)info_count;
    feng_cli_print_diagnostic(stderr, info->path, "semantic info",
                              info->message, &info->token,
                              source != NULL ? source->source : NULL,
                              source != NULL ? source->source_length : 0U);
}

/* --- entry --------------------------------------------------------------- */

int feng_cli_direct_main(const char *program, int argc, char **argv) {
    FengCliDirectOptions opts = {0};
    if (!feng_cli_direct_options_parse(program, argc, argv, &opts)) {
        return 1;
    }

    if (opts.release) {
        fprintf(stderr,
                "warning: --release is parsed but not yet implemented; building debug-equivalent output.\n");
    }

    /* Materialise the output layout up front. */
    char *ir_dir = path_join(opts.out_dir, "ir/c");
    char *artifact_dir = path_join(opts.out_dir,
                                   opts.target == FENG_COMPILE_TARGET_BIN ? "bin" : "lib");
    if (ir_dir == NULL || artifact_dir == NULL) {
        fprintf(stderr, "out of memory preparing output layout\n");
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    if (mkdirs(ir_dir) != 0) {
        fprintf(stderr, "failed to create %s: %s\n", ir_dir, strerror(errno));
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    if (mkdirs(artifact_dir) != 0) {
        fprintf(stderr, "failed to create %s: %s\n", artifact_dir, strerror(errno));
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }

    /* Drive the shared frontend pipeline across every input file. */
    FengSemanticAnalysis *analysis = NULL;
    FengCliLoadedSource *sources = NULL;
    size_t source_count = 0U;
    FengCliFrontendInput input = {
        .path_count = opts.input_count,
        .paths = (char **)opts.inputs,
        .target = opts.target,
    };
    FengCliFrontendCallbacks callbacks = {
        .on_parse_error = on_parse_error,
        .on_semantic_error = on_semantic_error,
        .on_semantic_info = on_semantic_info,
        .user = NULL,
    };
    FengCliFrontendOutputs outputs = {
        .out_analysis = &analysis,
        .out_sources = &sources,
        .out_source_count = &source_count,
    };

    int rc = feng_cli_frontend_run(&input, &callbacks, &outputs);
    if (rc != 0) {
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return rc;
    }

    /* Codegen aggregate (multi-file capable, see P3). */
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    bool cg_ok = feng_codegen_emit_program(analysis, opts.target, NULL, &out, &cgerr);
    if (!cg_ok) {
        const FengCliLoadedSource *blame_src = NULL;
        if (cgerr.path != NULL) {
            blame_src = feng_cli_find_loaded_source(sources, source_count, cgerr.path);
        }
        feng_cli_print_diagnostic(stderr,
                                  cgerr.path != NULL ? cgerr.path
                                                     : (source_count > 0 ? sources[0].path : "<input>"),
                                  "codegen error",
                                  cgerr.message ? cgerr.message : "(unknown)",
                                  &cgerr.token,
                                  blame_src != NULL ? blame_src->source : NULL,
                                  blame_src != NULL ? blame_src->source_length : 0U);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_cli_free_loaded_sources(sources, source_count);
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }

    /* Persist the generated C aggregate at <out>/ir/c/feng.c. The fixed
     * filename is intentional for P4: until P5 lands the link step we keep
     * the path predictable so smoke and tooling can introspect output. */
    char *c_path = path_join(ir_dir, "feng.c");
    if (c_path == NULL) {
        fprintf(stderr, "out of memory composing IR path\n");
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_cli_free_loaded_sources(sources, source_count);
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    FILE *f = fopen(c_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "failed to open %s for write: %s\n", c_path, strerror(errno));
        free(c_path);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_cli_free_loaded_sources(sources, source_count);
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    size_t written = fwrite(out.c_source, 1U, out.c_source_length, f);
    fclose(f);
    if (written != out.c_source_length) {
        fprintf(stderr, "short write to %s\n", c_path);
        free(c_path);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_cli_free_loaded_sources(sources, source_count);
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }

    /* Compose the final artifact path. Prefer an explicit `--name` override;
     * otherwise derive the stem from the first input's basename. */
    char *artifact_path = NULL;
    char *artifact_name = NULL;
    if (opts.artifact_name != NULL) {
        artifact_name = malloc(strlen(opts.artifact_name) + 1U);
        if (artifact_name != NULL) {
            strcpy(artifact_name, opts.artifact_name);
        }
    } else {
        const char *first = opts.inputs[0];
        const char *slash = strrchr(first, '/');
        const char *base = slash != NULL ? slash + 1 : first;
        const char *dot = strrchr(base, '.');
        size_t stem_len = dot != NULL ? (size_t)(dot - base) : strlen(base);
        if (stem_len == 0U) stem_len = strlen(base);
        char *stem = malloc(stem_len + 1U);
        if (stem != NULL) {
            memcpy(stem, base, stem_len);
            stem[stem_len] = '\0';
            artifact_name = stem;
        }
    }
    if (artifact_name != NULL && opts.target == FENG_COMPILE_TARGET_LIB) {
        char *lib_name = NULL;
        if (strncmp(artifact_name, "lib", 3) == 0) {
            lib_name = malloc(strlen(artifact_name) + 3U);
            if (lib_name != NULL) {
                snprintf(lib_name, strlen(artifact_name) + 3U, "%s.a", artifact_name);
            }
        } else {
            lib_name = malloc(strlen(artifact_name) + 6U);
            if (lib_name != NULL) {
                snprintf(lib_name, strlen(artifact_name) + 6U, "lib%s.a", artifact_name);
            }
        }
        free(artifact_name);
        artifact_name = lib_name;
    }
    if (artifact_name != NULL) {
        artifact_path = path_join(artifact_dir, artifact_name);
        free(artifact_name);
    }
    if (artifact_path == NULL) {
        fprintf(stderr, "out of memory composing artifact path\n");
        free(c_path);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_cli_free_loaded_sources(sources, source_count);
        free(ir_dir); free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }

    /* Hand off to the host driver. Programs are passed so it can mine
     * @cdecl annotations for additional link libraries. */
    const FengProgram **prog_array = NULL;
    size_t prog_count = 0U;
    for (size_t m = 0; m < analysis->module_count; ++m) {
        prog_count += analysis->modules[m].program_count;
    }
    if (prog_count > 0U) {
        prog_array = calloc(prog_count, sizeof(*prog_array));
        if (prog_array == NULL) {
            fprintf(stderr, "out of memory collecting programs for driver\n");
            free(artifact_path);
            free(c_path);
            feng_codegen_error_free(&cgerr);
            feng_codegen_output_free(&out);
            feng_semantic_analysis_free(analysis);
            feng_cli_free_loaded_sources(sources, source_count);
            free(ir_dir); free(artifact_dir);
            feng_cli_direct_options_dispose(&opts);
            return 1;
        }
        size_t cursor = 0;
        for (size_t m = 0; m < analysis->module_count; ++m) {
            for (size_t p = 0; p < analysis->modules[m].program_count; ++p) {
                prog_array[cursor++] = analysis->modules[m].programs[p];
            }
        }
    }

    FengCliDriverOptions drv = {
        .program_path = program,
        .target = opts.target,
        .c_path = c_path,
        .out_path = artifact_path,
        .programs = prog_array,
        .program_count = prog_count,
        .keep_intermediate = opts.keep_intermediate,
    };
    int drv_rc = feng_cli_compile_driver_invoke(&drv);

    free(prog_array);
    free(artifact_path);
    free(c_path);
    feng_codegen_error_free(&cgerr);
    feng_codegen_output_free(&out);
    feng_semantic_analysis_free(analysis);
    feng_cli_free_loaded_sources(sources, source_count);
    free(ir_dir); free(artifact_dir);
    feng_cli_direct_options_dispose(&opts);
    return drv_rc;
}
