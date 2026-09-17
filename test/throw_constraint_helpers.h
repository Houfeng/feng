#ifndef FENG_TEST_THROW_CONSTRAINT_HELPERS_H
#define FENG_TEST_THROW_CONSTRAINT_HELPERS_H

#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THROW_CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); \
} } while (0)

/* Owned source and analysis for one independent compiler test. */
typedef struct ThrowConstraintUnit {
    FengProgram *program;
    FengSemanticAnalysis *analysis;
} ThrowConstraintUnit;

/* Parse real source; failures retain their precise source diagnostic. */
static inline FengProgram *throw_constraint_parse(const char *source) {
    FengProgram *program = NULL;
    FengParseError error = {0};
    bool ok = feng_parse_source(source, strlen(source), "throw_constraint.ff", &program, &error);
    if (!ok) fprintf(stderr, "%s:%u:%u: %s\n%s\n", error.code,
        error.token.line, error.token.column, error.message, source);
    THROW_CHECK(ok && program != NULL);
    return program;
}

/* Check success or the expected diagnostic through the public semantic API. */
static inline ThrowConstraintUnit throw_constraint_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, const char *expected_code) {
    ThrowConstraintUnit unit = {.program = throw_constraint_parse(source)};
    const FengProgram *programs[] = {unit.program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
        &unit.analysis, &errors, &count);
    bool matched = expected_code == NULL ? ok && count == 0U : !ok && count > 0U;
    if (expected_code != NULL) {
        bool found = false;
        for (size_t i = 0U; i < count; ++i) {
            if (strcmp(errors[i].code, expected_code) == 0) found = true;
            THROW_CHECK(errors[i].token.line > 0U && errors[i].token.column > 0U);
        }
        matched = matched && found;
    }
    if (!matched) {
        fprintf(stderr, "expected %s\n%s\n", expected_code != NULL ? expected_code : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s: %s\n", errors[i].code, errors[i].message);
    }
    THROW_CHECK(matched);
    feng_semantic_errors_free(errors, count);
    return unit;
}

/* Analysis borrows its source tree, so release it before freeing that tree. */
static inline void throw_constraint_dispose(ThrowConstraintUnit *unit) {
    feng_semantic_analysis_free(unit->analysis);
    feng_program_free(unit->program);
    memset(unit, 0, sizeof(*unit));
}

#endif
