#include "cli/cli.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "archive/fb.h"
#include "cli/common.h"
#include "cli/compile/direct.h"
#include "cli/compile/driver.h"
#include "cli/frontend.h"
#include "debug/debug.h"
#include "codegen/codegen.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"

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

static char *path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    char *out;
    size_t len;

    if (slash == NULL) {
        out = malloc(2U);
        if (out != NULL) {
            out[0] = '.';
            out[1] = '\0';
        }
        return out;
    }

    len = (size_t)(slash - path);
    if (len == 0U) {
        out = malloc(2U);
        if (out != NULL) {
            out[0] = '/';
            out[1] = '\0';
        }
        return out;
    }

    out = malloc(len + 1U);
    if (out == NULL) return NULL;
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

static char *replace_with_sibling_filename(const char *path, const char *filename) {
    char *dir = path_dirname_dup(path);
    char *out;

    if (dir == NULL) return NULL;
    out = path_join(dir, filename);
    free(dir);
    return out;
}

static char *artifact_fd_path(const char *artifact_path) {
    size_t length;
    char *out;

    if (artifact_path == NULL) {
        return NULL;
    }
    length = strlen(artifact_path);
    out = (char *)malloc(length + 4U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, artifact_path, length);
    memcpy(out + length, ".fd", 4U);
    return out;
}

static void cleanup_empty_ir_dirs(const char *c_path) {
    char *ir_c_dir = path_dirname_dup(c_path);
    char *ir_dir;

    if (ir_c_dir == NULL) return;
    ir_dir = path_dirname_dup(ir_c_dir);
    if (ir_dir == NULL) {
        free(ir_c_dir);
        return;
    }

    if (rmdir(ir_c_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr,
                "warning: could not remove empty IR directory %s: %s\n",
                ir_c_dir, strerror(errno));
    }
    if (rmdir(ir_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr,
                "warning: could not remove empty IR directory %s: %s\n",
                ir_dir, strerror(errno));
    }

    free(ir_dir);
    free(ir_c_dir);
}

static void cleanup_intermediate_outputs(const char *c_path, bool cleanup_dirs) {
    char *object_path = replace_with_sibling_filename(c_path, "feng.o");
    bool can_cleanup_dirs = true;

    if (object_path != NULL && unlink(object_path) != 0 && errno != ENOENT) {
        fprintf(stderr,
                "warning: could not remove intermediate %s: %s\n",
                object_path, strerror(errno));
        can_cleanup_dirs = false;
    }
    if (unlink(c_path) != 0 && errno != ENOENT) {
        fprintf(stderr,
                "warning: could not remove intermediate %s: %s\n",
                c_path, strerror(errno));
        can_cleanup_dirs = false;
    }
    if (cleanup_dirs && can_cleanup_dirs) {
        cleanup_empty_ir_dirs(c_path);
    }

    free(object_path);
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

int feng_cli_direct_run(const char *program,
                        FengCliDirectOptions *parsed_opts,
                        const FengCliDirectDebugContext *debug_context) {
    FengCliDirectOptions opts = {0};
    char *c_path = NULL;
    char *public_symbol_dir = NULL;
    char *workspace_symbol_dir = NULL;
    if (parsed_opts == NULL) {
        return 1;
    }
    opts = *parsed_opts;
    memset(parsed_opts, 0, sizeof(*parsed_opts));

    /* Materialise the output layout up front. */
    char *ir_dir = path_join(opts.out_dir, "ir/c");
    char *artifact_dir = NULL;
    if (opts.target == FENG_COMPILE_TARGET_LIB) {
        char *host_target = NULL;
        char *host_target_error = NULL;
        if (!feng_fb_detect_host_target(&host_target, &host_target_error)) {
            fprintf(stderr, "error: %s\n",
                    host_target_error != NULL ? host_target_error
                                              : "failed to detect host target");
            free(host_target_error);
            free(ir_dir);
            feng_cli_direct_options_dispose(&opts);
            return 1;
        }
        char *lib_base = path_join(opts.out_dir, "lib");
        artifact_dir = lib_base != NULL ? path_join(lib_base, host_target) : NULL;
        free(lib_base);
        free(host_target);
    } else {
        artifact_dir = path_join(opts.out_dir, "bin");
    }
    public_symbol_dir = path_join(opts.out_dir, "mod");
    workspace_symbol_dir = path_join(opts.out_dir, "obj/symbols");
    if (ir_dir == NULL || artifact_dir == NULL || public_symbol_dir == NULL ||
        workspace_symbol_dir == NULL) {
        fprintf(stderr, "out of memory preparing output layout\n");
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    if (mkdirs(ir_dir) != 0) {
        fprintf(stderr, "failed to create %s: %s\n", ir_dir, strerror(errno));
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    if (mkdirs(artifact_dir) != 0) {
        fprintf(stderr, "failed to create %s: %s\n", artifact_dir, strerror(errno));
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    c_path = path_join(ir_dir, "feng.c");
    if (c_path == NULL) {
        fprintf(stderr, "out of memory composing IR path\n");
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    if (!opts.keep_intermediate) {
        /* Non-keep builds should not inherit stale IR from an earlier
         * successful run or a prior host-compiler failure. */
        cleanup_intermediate_outputs(c_path, false);
    }

    /* Drive the shared frontend pipeline across every input file. */
    FengSemanticAnalysis *analysis = NULL;
    FengCliLoadedSource *sources = NULL;
    size_t source_count = 0U;
    FengSymbolImportedModuleCache *imported_module_cache = NULL;
    char **bundle_paths = NULL;
    size_t bundle_count = 0U;
    FengCliFrontendInput input = {
        .path_count = opts.input_count,
        .paths = (char **)opts.inputs,
        .target = opts.target,
        .package_path_count = opts.package_path_count,
        .package_paths = opts.package_paths,
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
        .out_imported_module_cache = &imported_module_cache,
        .out_bundle_paths = &bundle_paths,
        .out_bundle_count = &bundle_count,
    };

    int rc = feng_cli_frontend_run(&input, &callbacks, &outputs);
    if (rc != 0) {
        if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
        free(c_path);
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return rc;
    }

    {
        FengSymbolExportOptions symbol_options = {
            .public_root = public_symbol_dir,
            .workspace_root = workspace_symbol_dir,
            .emit_docs = true,
            .emit_spans = true,
        };
        FengSymbolError symbol_error = {0};

        if (!feng_symbol_export_analysis(analysis, &symbol_options, &symbol_error)) {
            const FengCliLoadedSource *blame_src = NULL;

            if (symbol_error.path != NULL) {
                blame_src = feng_cli_find_loaded_source(sources, source_count, symbol_error.path);
            }
            feng_cli_print_diagnostic(stderr,
                                      symbol_error.path != NULL
                                          ? symbol_error.path
                                          : (source_count > 0 ? sources[0].path : "<input>"),
                                      "symbol export error",
                                      symbol_error.message != NULL ? symbol_error.message : "(unknown)",
                                      &symbol_error.token,
                                      blame_src != NULL ? blame_src->source : NULL,
                                      blame_src != NULL ? blame_src->source_length : 0U);
            if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
            feng_symbol_error_free(&symbol_error);
            free(c_path);
            free(workspace_symbol_dir);
            free(public_symbol_dir);
            free(ir_dir);
            free(artifact_dir);
            feng_semantic_analysis_free(analysis);
            feng_symbol_imported_module_cache_free(imported_module_cache);
            feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
            feng_cli_free_loaded_sources(sources, source_count);
            feng_cli_direct_options_dispose(&opts);
            return 1;
        }
    }

    /* Codegen aggregate (multi-file capable, see P3). */
    FengCodegenOptions codegen_options = {0};
    const FengCodegenOptions *active_codegen_options = NULL;
    FengCodegenOutput out = {0};
    FengCodegenError cgerr = {0};
    if (debug_context != NULL && !opts.release && debug_context->source_count > 0U) {
        codegen_options.emit_line_directives = true;
        codegen_options.debug_source_mappings = debug_context->sources;
        codegen_options.debug_source_mapping_count = debug_context->source_count;
        active_codegen_options = &codegen_options;
    }
    bool cg_ok = feng_codegen_emit_program(analysis,
                                           opts.target,
                                           active_codegen_options,
                                           &out,
                                           &cgerr);
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
        if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_symbol_imported_module_cache_free(imported_module_cache);
        feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
        feng_cli_free_loaded_sources(sources, source_count);
        free(c_path);
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }

    /* Persist the generated C aggregate at <out>/ir/c/feng.c. The fixed
     * filename is intentional for P4: until P5 lands the link step we keep
     * the path predictable so smoke and tooling can introspect output. */
    FILE *f = fopen(c_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "failed to open %s for write: %s\n", c_path, strerror(errno));
        if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
        free(c_path);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_symbol_imported_module_cache_free(imported_module_cache);
        feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
        feng_cli_free_loaded_sources(sources, source_count);
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }
    size_t written = fwrite(out.c_source, 1U, out.c_source_length, f);
    fclose(f);
    if (written != out.c_source_length) {
        fprintf(stderr, "short write to %s\n", c_path);
        if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
        free(c_path);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_symbol_imported_module_cache_free(imported_module_cache);
        feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
        feng_cli_free_loaded_sources(sources, source_count);
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
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
        char *lib_name = feng_fb_host_static_library_file_name(artifact_name);

        free(artifact_name);
        artifact_name = lib_name;
    }
    if (artifact_name != NULL) {
        artifact_path = path_join(artifact_dir, artifact_name);
        free(artifact_name);
    }
    if (artifact_path == NULL) {
        fprintf(stderr, "out of memory composing artifact path\n");
        if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
        free(c_path);
        feng_codegen_error_free(&cgerr);
        feng_codegen_output_free(&out);
        feng_semantic_analysis_free(analysis);
        feng_symbol_imported_module_cache_free(imported_module_cache);
        feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
        feng_cli_free_loaded_sources(sources, source_count);
        free(workspace_symbol_dir);
        free(public_symbol_dir);
        free(ir_dir);
        free(artifact_dir);
        feng_cli_direct_options_dispose(&opts);
        return 1;
    }

    char *fd_path = NULL;
    if (active_codegen_options != NULL) {
        fd_path = artifact_fd_path(artifact_path);
        if (fd_path == NULL) {
            fprintf(stderr, "out of memory composing debug sidecar path\n");
            if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
            free(artifact_path);
            free(c_path);
            feng_codegen_error_free(&cgerr);
            feng_codegen_output_free(&out);
            feng_semantic_analysis_free(analysis);
            feng_symbol_imported_module_cache_free(imported_module_cache);
            feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
            feng_cli_free_loaded_sources(sources, source_count);
            free(workspace_symbol_dir);
            free(public_symbol_dir);
            free(ir_dir);
            free(artifact_dir);
            feng_cli_direct_options_dispose(&opts);
            return 1;
        }
        if (unlink(fd_path) != 0 && errno != ENOENT) {
            fprintf(stderr,
                    "warning: could not remove stale debug sidecar %s: %s\n",
                    fd_path,
                    strerror(errno));
        }
    }

    /* Hand off to the host driver. Programs are passed so it can mine
     * extern calling-convention annotations for additional link libraries. */
    const FengProgram **prog_array = NULL;
    size_t prog_count = 0U;
    for (size_t m = 0; m < analysis->module_count; ++m) {
        prog_count += analysis->modules[m].program_count;
    }
    if (prog_count > 0U) {
        prog_array = calloc(prog_count, sizeof(*prog_array));
        if (prog_array == NULL) {
            fprintf(stderr, "out of memory collecting programs for driver\n");
            if (!opts.keep_intermediate) cleanup_intermediate_outputs(c_path, true);
            free(fd_path);
            free(artifact_path);
            free(c_path);
            feng_codegen_error_free(&cgerr);
            feng_codegen_output_free(&out);
            feng_semantic_analysis_free(analysis);
            feng_symbol_imported_module_cache_free(imported_module_cache);
            feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
            feng_cli_free_loaded_sources(sources, source_count);
            free(workspace_symbol_dir);
            free(public_symbol_dir);
            free(ir_dir);
            free(artifact_dir);
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
        .bundle_paths = (const char *const *)bundle_paths,
        .bundle_count = bundle_count,
        .link_libs = (const char *const *)opts.link_libs,
        .link_lib_count = (size_t)opts.link_lib_count,
        .release = opts.release,
        .keep_intermediate = opts.keep_intermediate,
    };
    int drv_rc = feng_cli_compile_driver_invoke(&drv);

    if (drv_rc == 0 && active_codegen_options != NULL && fd_path != NULL) {
        char *fd_error = NULL;

        if (!feng_debug_write_fd(fd_path,
                                 artifact_path,
                                 debug_context->sources,
                                 debug_context->source_count,
                                 &out.debug_info,
                                 &fd_error)) {
            fprintf(stderr,
                    "failed to write %s: %s\n",
                    fd_path,
                    fd_error != NULL ? fd_error : "(unknown)");
            free(fd_error);
            drv_rc = 1;
        }
    }

    free(prog_array);
    free(fd_path);
    free(artifact_path);
    free(c_path);
    free(workspace_symbol_dir);
    free(public_symbol_dir);
    feng_codegen_error_free(&cgerr);
    feng_codegen_output_free(&out);
    feng_semantic_analysis_free(analysis);
    feng_symbol_imported_module_cache_free(imported_module_cache);
    feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
    feng_cli_free_loaded_sources(sources, source_count);
    free(ir_dir);
    free(artifact_dir);
    feng_cli_direct_options_dispose(&opts);
    return drv_rc;
}

int feng_cli_direct_main(const char *program, int argc, char **argv) {
    FengCliDirectOptions opts = {0};

    if (!feng_cli_direct_options_parse(program, argc, argv, &opts)) {
        return 1;
    }
    return feng_cli_direct_run(program, &opts, NULL);
}
