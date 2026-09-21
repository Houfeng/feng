#ifndef FENG_TEST_GENERIC_SIBLING_CONSTRAINT_HELPERS_H
#define FENG_TEST_GENERIC_SIBLING_CONSTRAINT_HELPERS_H

#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIBLING_CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

/* A source tree and its analysis share the caller-owned source lifetime. */
typedef struct SiblingConstraintUnit {
    FengProgram *program;
    FengSemanticAnalysis *analysis;
} SiblingConstraintUnit;

/* Keep the same declaration identities in source and persisted-provider tests. */
static inline const char *sibling_declarations(void) {
    return "open module sibling.bounds;\n"
        "open type T{}\n"
        "open type U{}\n"
        "open type Nominal{open func marker():i32{return 13;}}\n"
        "open func nominal():Nominal{return Nominal();}\n"
        "open func retainNominal<Nominal>():i32{return nominal().marker();}\n"
        "open spec Box<V>{func get():V;}\n"
        "open spec Child<V>:Box<V>{}\n"
        "open spec Tag{func tag():i32;}\n"
        "open spec Both<V>:Box<V>&Tag;\n"
        "open spec Mapper<V>(value:V):V;\n"
        "open spec Choice<V>:V|string;\n"
        "open type Ref<V>:Child<V>,Tag{open let value:V;"
        "func get():V{return self.value;}func tag():i32{return 7;}}\n"
        "@value open type Value<V>:Box<V>{open let value:V;func get():V{return self.value;}}\n"
        "@value open type Packet<V>{open let value:V;open let marker:string;}\n"
        "open func first<T,U:Box<T>>(value:U):T{return value.get();}\n"
        "open func later<T:Box<U>,U>(value:T):U{return value.get();}\n"
        "open func inferFirst<T,U:Box<T>>(value:U,hint:T):T{return first<T,U>(value);}\n"
        "open func inferLater<T:Box<U>,U>(value:T,hint:U):U{return later<T,U>(value);}\n"
        "open func relayFirst<X,Y:Box<X>>(value:Y):X{return later<Y,X>(value);}\n"
        "open func relayLater<Y:Box<X>,X>(value:Y):X{return first<X,Y>(value);}\n"
        "open func childFirst<A,Z:Child<A>>(value:Z):A{return later<Z,A>(value);}\n"
        "open func childLater<Z:Child<A>,A>(value:Z):A{return first<A,Z>(value);}\n"
        "open func nested<T:Box<Packet<U[]>>,U>(value:T):Packet<U[]>{return value.get();}\n"
        "open func mapFirst<V,F:Mapper<V>>(fn:F,value:V):V{return fn(value);}\n"
        "open func mapLater<F:Mapper<V>,V>(fn:F,value:V):V{return fn(value);}\n"
        "open func choiceFirst<V,T:Choice<V>>(value:T):T{return value;}\n"
        "open func choiceLater<T:Choice<V>,V>(value:T):T{return value;}\n"
        "open func bothFirst<V,T:Both<V>>(value:T):V{return value.get();}\n"
        "open func bothLater<T:Both<V>,V>(value:T):V{return value.get();}\n"
        "open type FirstOwner<T,U:Box<T>>{open let item:U;func get():T{return self.item.get();}}\n"
        "open type LaterOwner<T:Box<U>,U>{open let item:T;func get():U{return self.item.get();}}\n"
        "open func ownerFirst<A,Z:Child<A>>(value:Z):A{let owner=LaterOwner<Z,A>{item:value};return owner.get();}\n"
        "open func ownerLater<Z:Child<A>,A>(value:Z):A{let owner=FirstOwner<A,Z>{item:value};return owner.get();}\n"
        "open spec FirstBound<T,U:Box<T>>{}\n"
        "open spec LaterBound<T:Box<U>,U>{}\n"
        "open type Methods<O>{"
        "open func first<T,U:Box<T>>(value:U,hint:T):T{return value.get();}"
        "open static func later<T:Box<U>,U>(value:T,hint:U):U{return value.get();}}\n"
        "open fit Methods<O>{"
        "open func fitLater<T:Box<U>,U>(value:T,hint:U):U{return value.get();}"
        "open static func fitFirst<T,U:Box<T>>(value:U,hint:T):T{return value.get();}}\n";
}

/* Parse through the public frontend and preserve useful failure context. */
static inline FengProgram *sibling_parse(const char *source) {
    FengProgram *program = NULL;
    FengParseError error = {0};
    bool ok = feng_parse_source(source, strlen(source), "sibling_constraint.ff", &program, &error);
    if (!ok) fprintf(stderr, "%s %u:%u %s\n%s\n", error.code,
        error.token.line, error.token.column, error.message, source);
    SIBLING_CHECK(ok && program != NULL);
    return program;
}

/* Require exactly the intended diagnostic, including its source token. */
static inline SiblingConstraintUnit sibling_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, const char *code, const char *token) {
    SiblingConstraintUnit unit = {.program = sibling_parse(source)};
    const FengProgram *programs[] = {unit.program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options,
        &unit.analysis, &errors, &count);
    bool matched = code == NULL ? ok && count == 0U :
        !ok && count == 1U && strcmp(errors[0].code, code) == 0;
    if (!matched) {
        fprintf(stderr, "expected %s\n%s\n", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i)
            fprintf(stderr, "%s %u:%u %s\n", errors[i].code,
                errors[i].token.line, errors[i].token.column, errors[i].message);
    }
    SIBLING_CHECK(matched);
    if (code != NULL) {
        SIBLING_CHECK(strcmp(errors[0].path, "sibling_constraint.ff") == 0);
        SIBLING_CHECK(errors[0].token.line > 0U && errors[0].token.column > 0U);
        SIBLING_CHECK(errors[0].token.length == strlen(token));
        SIBLING_CHECK(memcmp(errors[0].token.lexeme, token, strlen(token)) == 0);
    }
    feng_semantic_errors_free(errors, count);
    return unit;
}

/* Dispose analysis before its borrowed declaration tree. */
static inline void sibling_dispose(SiblingConstraintUnit *unit) {
    feng_semantic_analysis_free(unit->analysis);
    feng_program_free(unit->program);
    memset(unit, 0, sizeof(*unit));
}

/* Assemble a complete case without truncation or borrowed temporary storage. */
static inline char *sibling_source(const char *prefix, const char *body) {
    size_t length = strlen(prefix) + strlen(body) + 1U;
    char *source = malloc(length);
    SIBLING_CHECK(source != NULL);
    SIBLING_CHECK(snprintf(source, length, "%s%s", prefix, body) == (int)length - 1);
    return source;
}

#endif
