#include "cli/frontend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"

void feng_cli_frontend_bundle_paths_dispose(char **bundle_paths,
                                            size_t bundle_count) {
    size_t index;

    if (bundle_paths == NULL) {
        return;
    }
    for (index = 0U; index < bundle_count; ++index) {
        free(bundle_paths[index]);
    }
    free(bundle_paths);
}

static int load_and_parse(const FengCliFrontendInput *input,
                          FengCliLoadedSource *sources,
                          const FengProgram **programs,
                          const FengCliFrontendCallbacks *callbacks) {
    int path_index;

    for (path_index = 0; path_index < input->path_count; ++path_index) {
        FengParseError error;

        sources[path_index].path = input->paths[path_index];
        sources[path_index].source = feng_cli_read_entire_file(input->paths[path_index],
                                                               &sources[path_index].source_length);
        if (sources[path_index].source == NULL) {
            fprintf(stderr, "failed to read %s: %s\n",
                    input->paths[path_index], strerror(errno));
            return 1;
        }

        if (!feng_parse_source(sources[path_index].source,
                               sources[path_index].source_length,
                               input->paths[path_index],
                               &sources[path_index].program,
                               &error)) {
            if (callbacks != NULL && callbacks->on_parse_error != NULL) {
                callbacks->on_parse_error(callbacks->user,
                                          input->paths[path_index],
                                          &error,
                                          &sources[path_index]);
            }
            return 1;
        }

        programs[path_index] = sources[path_index].program;
    }

    return 0;
}

static int load_package_sources(const FengCliFrontendInput *input,
                                FengSymbolProvider **out_provider,
                                char ***out_bundle_paths,
                                size_t *out_bundle_count) {
    FengSymbolProvider *provider = NULL;
    char **bundle_paths = NULL;
    FengSymbolError error = {0};
    int index;

    if (out_provider == NULL || out_bundle_paths == NULL || out_bundle_count == NULL) {
        return 1;
    }
    *out_provider = NULL;
    *out_bundle_paths = NULL;
    *out_bundle_count = 0U;
    if (input->package_path_count <= 0) {
        return 0;
    }
    if (input->package_paths == NULL) {
        fprintf(stderr, "frontend: package path list is missing\n");
        return 1;
    }
    if (!feng_symbol_provider_create(&provider, &error)) {
        fprintf(stderr,
                "failed to initialize package symbol provider: %s\n",
                error.message != NULL ? error.message : "unknown error");
        feng_symbol_error_free(&error);
        return 1;
    }
    bundle_paths = calloc((size_t)input->package_path_count, sizeof(*bundle_paths));
    if (bundle_paths == NULL) {
        fprintf(stderr, "out of memory\n");
        feng_symbol_provider_free(provider);
        return 1;
    }
    for (index = 0; index < input->package_path_count; ++index) {
        const char *package_path = input->package_paths[index];
        char *resolved_path;

        if (package_path == NULL || package_path[0] == '\0') {
            fprintf(stderr, "frontend: package path must not be empty\n");
            feng_cli_frontend_bundle_paths_dispose(bundle_paths,
                                                   (size_t)input->package_path_count);
            feng_symbol_provider_free(provider);
            return 1;
        }
        resolved_path = realpath(package_path, NULL);
        if (resolved_path == NULL) {
            fprintf(stderr, "failed to resolve package %s: %s\n",
                    package_path, strerror(errno));
            feng_cli_frontend_bundle_paths_dispose(bundle_paths,
                                                   (size_t)input->package_path_count);
            feng_symbol_provider_free(provider);
            return 1;
        }
        bundle_paths[index] = resolved_path;
        if (!feng_symbol_provider_add_bundle(provider, resolved_path, &error)) {
            fprintf(stderr,
                    "failed to load package %s: %s\n",
                    error.path != NULL ? error.path : resolved_path,
                    error.message != NULL ? error.message : "unknown error");
            feng_symbol_error_free(&error);
            feng_cli_frontend_bundle_paths_dispose(bundle_paths,
                                                   (size_t)input->package_path_count);
            feng_symbol_provider_free(provider);
            return 1;
        }
    }

    *out_provider = provider;
    *out_bundle_paths = bundle_paths;
    *out_bundle_count = (size_t)input->package_path_count;
    return 0;
}

/* ---------- frontend run ------------------------------------------------- */

int feng_cli_frontend_run(const FengCliFrontendInput *input,
                          const FengCliFrontendCallbacks *callbacks,
                          const FengCliFrontendOutputs *outputs) {
    FengCliLoadedSource *sources = NULL;
    const FengProgram **programs = NULL;
    FengSymbolProvider *provider = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    int exit_code = 0;
    FengSymbolImportedModuleCache *imported_module_cache = NULL;
    char **bundle_paths = NULL;
    size_t bundle_count = 0U;
    FengSemanticImportedModuleQuery imported_query = {0};
    FengSemanticAnalyzeOptions semantic_options = {0};

    if (input == NULL || input->path_count <= 0 || input->paths == NULL) {
        fprintf(stderr, "frontend: no input files\n");
        return 1;
    }

    sources = (FengCliLoadedSource *)calloc((size_t)input->path_count, sizeof(*sources));
    programs = (const FengProgram **)calloc((size_t)input->path_count, sizeof(*programs));
    if (sources == NULL || programs == NULL) {
        fprintf(stderr, "out of memory\n");
        free(programs);
        feng_cli_free_loaded_sources(sources, (size_t)input->path_count);
        return 1;
    }

    exit_code = load_and_parse(input, sources, programs, callbacks);
    if (exit_code != 0) {
        goto cleanup;
    }

    exit_code = load_package_sources(input, &provider, &bundle_paths, &bundle_count);
    if (exit_code != 0) {
        goto cleanup;
    }

    semantic_options.target = input->target;
    if (provider != NULL) {
        imported_module_cache = feng_symbol_imported_module_cache_create(provider);
        if (imported_module_cache == NULL) {
            fprintf(stderr, "out of memory\n");
            exit_code = 1;
            goto cleanup;
        }
        imported_query = feng_symbol_imported_module_cache_as_query(imported_module_cache);
        semantic_options.imported_modules = &imported_query;
    }

    if (!feng_semantic_analyze_with_options(programs,
                                            (size_t)input->path_count,
                                            &semantic_options,
                                            &analysis,
                                            &errors,
                                            &error_count)) {
        if (error_count == 0U) {
            fprintf(stderr, "semantic analysis failed\n");
        } else if (callbacks != NULL && callbacks->on_semantic_error != NULL) {
            size_t i;

            for (i = 0U; i < error_count; ++i) {
                const FengCliLoadedSource *src = feng_cli_find_loaded_source(
                    sources, (size_t)input->path_count, errors[i].path);
                callbacks->on_semantic_error(callbacks->user,
                                             &errors[i],
                                             i,
                                             error_count,
                                             src);
            }
        }
        exit_code = 1;
    }

    if (analysis != NULL && analysis->info_count > 0U
        && callbacks != NULL && callbacks->on_semantic_info != NULL) {
        size_t i;

        for (i = 0U; i < analysis->info_count; ++i) {
            const FengCliLoadedSource *src = feng_cli_find_loaded_source(
                sources, (size_t)input->path_count, analysis->infos[i].path);
            callbacks->on_semantic_info(callbacks->user,
                                        &analysis->infos[i],
                                        i,
                                        analysis->info_count,
                                        src);
        }
    }

cleanup:
    feng_symbol_provider_free(provider);
    feng_semantic_errors_free(errors, error_count);
    free(programs);

    if (exit_code == 0 && outputs != NULL) {
        if (outputs->out_analysis != NULL) {
            *outputs->out_analysis = analysis;
            analysis = NULL;
        }
        if (outputs->out_sources != NULL) {
            *outputs->out_sources = sources;
            *outputs->out_source_count = (size_t)input->path_count;
            sources = NULL;
        }
        if (outputs->out_imported_module_cache != NULL) {
            *outputs->out_imported_module_cache = imported_module_cache;
            imported_module_cache = NULL;
        }
        if (outputs->out_bundle_paths != NULL) {
            *outputs->out_bundle_paths = bundle_paths;
            bundle_paths = NULL;
        }
        if (outputs->out_bundle_count != NULL) {
            *outputs->out_bundle_count = bundle_count;
            bundle_count = 0U;
        }
    }

    feng_cli_frontend_bundle_paths_dispose(bundle_paths, bundle_count);
    feng_symbol_imported_module_cache_free(imported_module_cache);
    feng_semantic_analysis_free(analysis);
    feng_cli_free_loaded_sources(sources, (size_t)input->path_count);
    return exit_code;
}
