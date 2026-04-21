#ifndef FENG_SEMANTIC_SEMANTIC_H
#define FENG_SEMANTIC_SEMANTIC_H

#include <stdbool.h>
#include <stddef.h>

#include "parser/parser.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct FengSemanticError {
    const char *path;
    char *message;
    FengToken token;
} FengSemanticError;

typedef struct FengSemanticModule {
    const FengSlice *segments;
    size_t segment_count;
    FengVisibility visibility;
    const FengProgram **programs;
    size_t program_count;
    size_t program_capacity;
} FengSemanticModule;

typedef struct FengSemanticAnalysis {
    FengSemanticModule *modules;
    size_t module_count;
    size_t module_capacity;
} FengSemanticAnalysis;

bool feng_semantic_analyze(const FengProgram *const *programs,
                           size_t program_count,
                           FengSemanticAnalysis **out_analysis,
                           FengSemanticError **out_errors,
                           size_t *out_error_count);

void feng_semantic_analysis_free(FengSemanticAnalysis *analysis);
void feng_semantic_errors_free(FengSemanticError *errors, size_t error_count);

#ifdef __cplusplus
}
#endif

#endif
