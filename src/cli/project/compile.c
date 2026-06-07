#include "cli/cli.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "cli/compile/direct.h"
#include "cli/deps/manager.h"
#include "cli/project/common.h"

int feng_cli_project_invoke_direct_compile_with_packages(const char *program,
                                                         const FengCliProjectContext *context,
                                                         bool release,
                                                         bool keep_intermediate,
                                                         size_t package_count,
                                                         const char *const *package_paths,
                                                         size_t dependency_fd_count,
                                                         const char *const *dependency_fd_paths) {
    FengCliDirectOptions options = {0};
    FengCliDirectDebugContext debug_context = {0};
    FengCodegenMapingSourceMapping *debug_sources = NULL;
    const char **inputs = NULL;
    const char **resolved_packages = NULL;
    size_t index;
    int rc = 1;

    if (context == NULL) {
        fprintf(stderr, "project context is required\n");
        return rc;
    }

    inputs = context->source_count > 0U
        ? (const char **)calloc(context->source_count, sizeof(*inputs))
        : NULL;
    resolved_packages = package_count > 0U
        ? (const char **)calloc(package_count, sizeof(*resolved_packages))
        : NULL;
    debug_sources = context->source_count > 0U
        ? (FengCodegenMapingSourceMapping *)calloc(context->source_count, sizeof(*debug_sources))
        : NULL;
    if ((context->source_count > 0U && (inputs == NULL || debug_sources == NULL)) ||
        (package_count > 0U && resolved_packages == NULL)) {
        fprintf(stderr, "out of memory preparing project build context\n");
        goto cleanup;
    }

    for (index = 0U; index < context->source_count; ++index) {
        inputs[index] = context->source_paths[index];
        debug_sources[index].source_path = context->source_paths[index];
        debug_sources[index].package_name = context->manifest.name;
        debug_sources[index].package_root = context->source_root;
    }
    for (index = 0U; index < package_count; ++index) {
        resolved_packages[index] = package_paths[index];
    }

    options.target = context->manifest.target;
    options.out_dir = context->out_root;
    options.release = release;
    options.keep_intermediate = keep_intermediate;
    options.artifact_name = context->manifest.name;
    options.input_count = (int)context->source_count;
    options.inputs = inputs;
    options.package_path_count = (int)package_count;
    options.package_paths = resolved_packages;
    options.link_lib_count = 0;
    options.link_libs = NULL;

    debug_context.sources = debug_sources;
    debug_context.source_count = context->source_count;
    debug_context.dependency_fd_paths = dependency_fd_paths;
    debug_context.dependency_fd_count = dependency_fd_count;
    rc = feng_cli_direct_run(program,
                             &options,
                             release ? NULL : &debug_context);
    inputs = NULL;
    resolved_packages = NULL;

cleanup:
    free((void *)inputs);
    free((void *)resolved_packages);
    free(debug_sources);
    return rc;
}

int feng_cli_project_invoke_direct_compile(const char *program,
                                           const FengCliProjectContext *context,
                                           bool release,
                                           bool keep_intermediate) {
    return feng_cli_project_invoke_direct_compile_with_packages(program,
                                                                context,
                                                                release,
                                                                keep_intermediate,
                                                                0U,
                                                                NULL,
                                                                0U,
                                                                NULL);
}

bool feng_cli_project_resolve_build_dependencies(const char *program,
                                                 const FengCliProjectContext *context,
                                                 bool release,
                                                 FengCliDepsResolved *out_resolved,
                                                 FengCliProjectError *out_error) {
    if (context == NULL || out_resolved == NULL) {
        if (out_error != NULL) {
            out_error->path = NULL;
            out_error->message = NULL;
            out_error->line = 0U;
        }
        return false;
    }

    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    out_resolved->debug_fd_paths = NULL;
    out_resolved->debug_fd_count = 0U;
    return feng_cli_deps_resolve_for_manifest(program,
                                              context->manifest_path,
                                              false,
                                              release,
                                              out_resolved,
                                              out_error);
}

bool feng_cli_project_prepare_build(const char *program,
                                   const char *path_arg,
                                   bool release,
                                   FengCliProjectContext *out_context,
                                   FengCliDepsResolved *out_resolved,
                                   FengCliProjectError *out_error) {
    FengCliProjectContext context = {0};

    if (out_context == NULL || out_resolved == NULL) {
        if (out_error != NULL) {
            out_error->path = NULL;
            out_error->message = NULL;
            out_error->line = 0U;
        }
        return false;
    }

    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    out_resolved->debug_fd_paths = NULL;
    out_resolved->debug_fd_count = 0U;
    if (!feng_cli_project_open(path_arg, &context, out_error)) {
        return false;
    }
    if (!feng_cli_project_resolve_build_dependencies(program,
                                                     &context,
                                                     release,
                                                     out_resolved,
                                                     out_error)) {
        feng_cli_project_context_dispose(&context);
        return false;
    }

    *out_context = context;
    return true;
}

int feng_cli_project_compile_prepared(const char *program,
                                      const FengCliProjectContext *context,
                                      const FengCliDepsResolved *resolved,
                                      bool release,
                                      bool keep_intermediate) {
    return feng_cli_project_invoke_direct_compile_with_packages(program,
                                                                context,
                                                                release,
                                                                keep_intermediate,
                                                                resolved->package_count,
                                                                (const char *const *)resolved->package_paths,
                                                                resolved->debug_fd_count,
                                                                (const char *const *)resolved->debug_fd_paths);
}
