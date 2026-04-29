#include "cli/frontend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"

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

int feng_cli_frontend_run(const FengCliFrontendInput *input,
                          const FengCliFrontendCallbacks *callbacks,
                          const FengCliFrontendOutputs *outputs) {
    FengCliLoadedSource *sources = NULL;
    const FengProgram **programs = NULL;
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    int exit_code = 0;

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

    if (!feng_semantic_analyze(programs,
                               (size_t)input->path_count,
                               input->target,
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
    }

    feng_semantic_analysis_free(analysis);
    feng_cli_free_loaded_sources(sources, (size_t)input->path_count);
    return exit_code;
}
