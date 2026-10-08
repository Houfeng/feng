#ifndef FENG_TEST_FRIEND_GENERIC_HELPERS_H
#define FENG_TEST_FRIEND_GENERIC_HELPERS_H

#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRIEND_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "friend check failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
    exit(1); } } while (0)

/* Own a source AST and its successful semantic result for subsequent FT tests. */
typedef struct FriendGenericUnit {
    FengProgram *program;
    FengSemanticAnalysis *analysis;
} FriendGenericUnit;

/* Exercise the public parser and retain useful source context on failure. */
static inline FengProgram *friend_generic_parse(const char *source) {
    FengProgram *program = NULL;
    FengParseError error = {0};
    bool ok = feng_parse_source(source, strlen(source), "friend_generic.ff", &program, &error);
    if (!ok) fprintf(stderr, "%s %u:%u %s\n%s\n", error.code, error.token.line,
        error.token.column, error.message, source);
    FRIEND_CHECK(ok && program != NULL);
    return program;
}

/* Require the exact diagnostic and optionally a specific token and source position. */
static inline FriendGenericUnit friend_generic_analyze_at(const char *source,
    const FengSemanticImportedModuleQuery *query, const char *code, const char *token,
    unsigned int line, unsigned int column, const char *message,
    unsigned int related_line, unsigned int related_column) {
    FriendGenericUnit unit = {.program = friend_generic_parse(source)};
    const FengProgram *programs[] = {unit.program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &unit.analysis, &errors, &count);
    bool matched = code == NULL ? ok && count == 0U :
        !ok && count == 1U && strcmp(errors[0].code, code) == 0;
    if (!matched) {
        fprintf(stderr, "expected %s\n%s\n", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %u:%u %s\n",
            errors[i].code, errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    FRIEND_CHECK(matched);
    if (code != NULL) {
        FRIEND_CHECK(strcmp(errors[0].path, "friend_generic.ff") == 0);
        FRIEND_CHECK(errors[0].token.line > 0U && errors[0].token.column > 0U);
        if (line != 0U) FRIEND_CHECK(errors[0].token.line == line);
        if (column != 0U) FRIEND_CHECK(errors[0].token.column == column);
        if (message != NULL) FRIEND_CHECK(strstr(errors[0].message, message) != NULL);
        if (strstr(errors[0].message, "instantiated @friend") != NULL) {
            FRIEND_CHECK(errors[0].related_location_count == 1U);
            const FengSemanticRelatedLocation *related = &errors[0].related_locations[0];
            FRIEND_CHECK(related->path != NULL && related->path[0] != '\0');
            FRIEND_CHECK(strstr(related->message, "@friend authorization") != NULL);
            if (related_line != 0U) FRIEND_CHECK(related->token.line == related_line);
            if (related_column != 0U) FRIEND_CHECK(related->token.column == related_column);
        }
        if (token != NULL) {
            FRIEND_CHECK(errors[0].token.length == strlen(token));
            FRIEND_CHECK(memcmp(errors[0].token.lexeme, token, strlen(token)) == 0);
        }
    }
    feng_semantic_errors_free(errors, count);
    return unit;
}

/* Most matrix entries share the diagnostic contract without prescribing a location. */
static inline FriendGenericUnit friend_generic_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, const char *code, const char *token) {
    return friend_generic_analyze_at(source, query, code, token, 0U, 0U, NULL, 0U, 0U);
}

/* Release analysis before its borrowed source syntax. */
static inline void friend_generic_dispose(FriendGenericUnit *unit) {
    feng_semantic_analysis_free(unit->analysis);
    feng_program_free(unit->program);
}

#endif
