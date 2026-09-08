#include "test_g24.h"
#include "codegen/codegen.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: G24 Codegen failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Count enclosing C braces, excluding literals and comments. A descriptor
 * definition or dependency array must be outside every function/initializer. */
static size_t g24_c_scope_depth(const char *source, const char *end) {
    size_t depth = 0U;
    for (const char *p = source; p < end; ++p) {
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (p < end && *p != quote) {
                if (*p == '\\' && p + 1 < end) ++p;
                ++p;
            }
        } else if (*p == '/' && p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n') ++p;
        } else if (*p == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) ++p;
        } else if (*p == '{') {
            ++depth;
        } else if (*p == '}') {
            CHECK(depth > 0U);
            --depth;
        }
    }
    return depth;
}

/* Match the generated definition and use by symbol, not by allocation order. */
bool g24_has_static_descriptor_call(const char *source, const char *callee,
                                    const char *display, const char *arguments) {
    const char *prefix = "static const FengFunctionDescriptor ";
    char initializer[1024];
    CHECK(snprintf(initializer, sizeof(initializer), " = {.name = \"%s\"};", display)
          < (int)sizeof(initializer));
    for (const char *p = source; (p = strstr(p, prefix)) != NULL; ++p) {
        const char *name = p + strlen(prefix);
        const char *end = name;
        while (isalnum((unsigned char)*end) || *end == '_') ++end;
        if (end == name || strncmp(end, initializer, strlen(initializer)) != 0 ||
            g24_c_scope_depth(source, p) != 0U) continue;
        char call[2048];
        CHECK(snprintf(call, sizeof(call), "%s(&%.*s%s", callee,
                       (int)(end - name), name, arguments) < (int)sizeof(call));
        if (strstr(source, call) != NULL) return true;
    }
    return false;
}

/* Guard the migrated assertion itself against false positives: a matching
 * name alone, stack storage, or a different descriptor/argument is not proof. */
static void g24_static_assertion_pairs(void) {
    const char *valid = "static const FengFunctionDescriptor d = {.name = \"read\"};\n"
                        "void run(void) { read(&d, _T, value); }";
    CHECK(g24_has_static_descriptor_call(valid, "read", "read", ", _T, value)"));
    CHECK(!g24_has_static_descriptor_call(valid, "write", "read", ", _T, value)"));
    CHECK(!g24_has_static_descriptor_call(valid, "read", "write", ", _T, value)"));
    CHECK(!g24_has_static_descriptor_call(valid, "read", "read", ", _U, value)"));
    CHECK(!g24_has_static_descriptor_call(
        "void run(void) { const FengFunctionDescriptor d = {.name = \"read\"}; read(&d, _T, value); }",
        "read", "read", ", _T, value)"));
    CHECK(!g24_has_static_descriptor_call(
        "void run(void) { static const FengFunctionDescriptor d = {.name = \"read\"}; read(&d, _T, value); }",
        "read", "read", ", _T, value)"));
    CHECK(!g24_has_static_descriptor_call(
        "static const FengFunctionDescriptor d = {.name = \"read\"}; void run(void) { read(&other, _T, value); }",
        "read", "read", ", _T, value)"));
}

/* Compile a positive fixture and verify every generated closed descriptor and
 * transitive dependency array is file-static. C compilation also rejects
 * static initializers that refer to automatic variables or dynamic values. */
static void g24_static_programs(const char *const *sources, size_t count,
                               bool has_dependencies,
                               void (*check_c)(const char *),
                               void (*compile_c)(const char *)) {
    FengProgram **programs = calloc(count, sizeof(*programs));
    CHECK(programs != NULL);
    for (size_t i = 0U; i < count; ++i) {
        FengParseError parse_error = {0};
        CHECK(feng_parse_source(sources[i], strlen(sources[i]), "g24_descriptors.ff",
                                &programs[i], &parse_error));
    }
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    FengSemanticAnalyzeOptions options = {0};
    options.target = FENG_COMPILE_TARGET_LIB;
    options.pointer_size = feng_get_host_pointer_size();
    bool ok = feng_semantic_analyze_with_options((const FengProgram *const *)programs, count, &options,
                                                 &analysis, &errors, &error_count);
    for (size_t i = 0U; i < error_count; ++i)
        fprintf(stderr, "G24 %s:%u %s %s\n", errors[i].path, errors[i].token.line,
                errors[i].code, errors[i].message);
    CHECK(ok && error_count == 0U);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "G24 Codegen %s %s\n", error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    const char *c = output.c_source;
    CHECK(strstr(c, "&(const FengFunctionDescriptor){") == NULL);
    CHECK(strstr(c, "(const FengAggregateDescriptor*[]){") == NULL);
    CHECK(strstr(c, "(const FengTypeDescriptor*[]){") == NULL);
    CHECK(strstr(c, "static const FengFunctionDescriptor _feng_closed_callable_desc_") != NULL);
    if (has_dependencies) {
        CHECK(strstr(c, "__agg[] = {") != NULL || strstr(c, "__type[] = {") != NULL);
    }
    const char *prefixes[] = {
        "static const FengFunctionDescriptor _feng_closed_callable_desc_",
        "static const FengAggregateDescriptor *const _feng_closed_callable_desc_",
        "static const FengTypeDescriptor *const _feng_closed_callable_desc_",
        "static const FengFunctionDescriptor *const _feng_closed_callable_desc_"
    };
    for (size_t i = 0U; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        for (const char *p = c; (p = strstr(p, prefixes[i])) != NULL; ++p)
            CHECK(g24_c_scope_depth(c, p) == 0U);
    }
    if (check_c != NULL) check_c(c);
    compile_c(c);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    for (size_t i = 0U; i < count; ++i) feng_program_free(programs[i]);
    free(programs);
}

/* Single-module convenience for the same compiler/structural assertions. */
static void g24_static_source(const char *source, bool has_dependencies,
                              void (*check_c)(const char *),
                              void (*compile_c)(const char *)) {
    g24_static_programs(&source, 1U, has_dependencies, check_c, compile_c);
}

/* A routing fixture checks positive slot reads and negative raw forwarding;
 * its dependency-free method must keep the static-address fast path. */
static void g24_check_fit_routing(const char *c) {
    const char *required[] = {
        "__saved__from__void(_p_values, _desc->reified_callable_deps[",
        "__saved__from__void(self, _desc->reified_callable_deps[",
        "__sibling__from__void(self, _desc->reified_callable_deps[",
        "__recursive__from__i32(self, _desc->reified_callable_deps[",
        "__plain__from__void(self, &_feng_closed_callable_desc_",
        "static const FengFunctionDescriptor *const _feng_closed_callable_desc_"
    };
    const char *forbidden[] = {
        "__saved__from__void(_p_values, _desc,",
        "__saved__from__void(self, _desc,",
        "__sibling__from__void(self, _desc,",
        "__recursive__from__i32(self, _desc,",
        "__plain__from__void(self, _desc->"
    };
    for (size_t i = 0U; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (strstr(c, required[i]) == NULL) fprintf(stderr, "G24 missing %s\n", required[i]);
        CHECK(strstr(c, required[i]) != NULL);
    }
    for (size_t i = 0U; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i)
        CHECK(strstr(c, forbidden[i]) == NULL);
}

/* ISSUE-G24-010: readonly/writable arrays, differently shaped dependencies,
 * recursive callee graphs, sibling fits and empty callees share one mechanism. */
static void g24_fit_routing(void (*compile_c)(const char *)) {
    for (size_t writable = 0U; writable < 2U; ++writable) {
        const char *array = writable ? "[!]" : "[]";
        char source[4096];
        snprintf(source, sizeof(source),
            "module g24.routing;\n"
            "type Box<T> { open let value: T; func Box(value: T) { self.value = value; } }\n"
            "@value type Cell<T> { open let value: T; func Cell(value: T) { self.value = value; } }\n"
            "type Other<T> { let pad: string = \"pad\"; open let value: T; "
                "func Other(value: T) { self.value = value; } }\n"
            "fit T%s {\n"
            " func plain(): T { return self[0]; }\n"
            " func saved(): T { let box = Box<T>(self[0]); let cell = Cell<T>(box.value); return cell.value; }\n"
            " func sibling(): T { let own = Other<T>(self[0]); own.value; return self.saved(); }\n"
            " func recursive(count: i32): T { if count == 0 { return self.saved(); } return self.recursive(count - 1); }\n"
            " func empty(): T { return self.plain(); } }\n"
            "fit T%s { func across(): T { return self.sibling(); } }\n"
            "func relay<T>(values: T%s): T { return values.saved(); }\n"
            "func run(): string { let a: i32%s = [11, 22]; let b: string%s = [\"text\"];\n"
            " a.across(); b.recursive(3); a.empty(); return relay<string>(b); }\n",
            array, array, array, array, array);
        g24_static_source(source, true, g24_check_fit_routing, compile_c);
    }
}

/* Different open receiver types must select different callee slots even when
 * both calls name the same fit member. Slot numbers need not be hard-coded. */
static void g24_check_distinct_fit_slots(const char *c) {
    const char *first = strstr(c, "__saved__from__void(_p_first, _desc->reified_callable_deps[");
    const char *second = strstr(c, "__saved__from__void(_p_second, _desc->reified_callable_deps[");
    CHECK(first != NULL && second != NULL);
    first = strchr(first, '[') + 1;
    second = strchr(second, '[') + 1;
    CHECK(strtoul(first, NULL, 10) != strtoul(second, NULL, 10));
}

/* Two independent caller parameters close the same generic method twice. */
static void g24_fit_parameter_slots(void (*compile_c)(const char *)) {
    g24_static_source(
        "module g24.slots;\n"
        "@value type Cell<T> { open let value: T; func Cell(value: T) { self.value = value; } }\n"
        "fit T[] { func saved(): T { return Cell<T>(self[0]).value; } }\n"
        "func pair<A, B>(first: A[], second: B[]): B { first.saved(); return second.saved(); }\n"
        "func run(): string { let a: i32[] = [12]; let b: string[] = [\"word\"]; return pair<i32, string>(a, b); }\n",
        true, g24_check_distinct_fit_slots, compile_c);
}

/* Source-order-independent nominal identities across two import aliases.
 * Reuse actual types both directly and via independent generic call slots. */
static void g24_fit_nominal_identities(void (*compile_c)(const char *)) {
    const char *sources[] = {
        "module g24.reference;\n"
        "open type Token { open var number: i32; }\n",
        "module g24.value;\n"
        "@value open type Token { open var text: string; }\n",
        "module g24.nominal;\n"
        "import g24.reference as ref; import g24.value as val;\n"
        "type Box<T> { open let value: T; func Box(value: T) { self.value = value; } }\n"
        "@value type Cell<T> { open let value: T; func Cell(value: T) { self.value = value; } }\n"
        "fit T[] { func saved(): T { let b = Box<T>(self[0]); return Cell<T>(b.value).value; } }\n"
        "func pair<A, B>(first: A[], second: B[]): B { first.saved(); return second.saved(); }\n"
        "func run(): string { let a: ref.Token[] = [ref.Token { number: 52 }];\n"
        " let b: val.Token[] = [val.Token { text: \"token\" }];\n"
        " a.saved(); b.saved(); pair<val.Token, ref.Token>(b, a);\n"
        " return pair<ref.Token, val.Token>(a, b).text; }\n"
    };
    g24_static_programs(sources, 3U, true, g24_check_distinct_fit_slots, compile_c);
    const char *reversed[] = {sources[2], sources[1], sources[0]};
    g24_static_programs(reversed, 3U, true, g24_check_distinct_fit_slots, compile_c);
}

/* Projection-only callees retain their own descriptor slots. All generated
 * tables/probes are file-static, and both dependency domains are consumed. */
static void g24_check_projection_predicates(const char *c) {
    CHECK(strstr(c, "static const FengUnionProjection ") != NULL);
    CHECK(strstr(c, ".possible = false") != NULL);
    CHECK(strstr(c, ".required = false") != NULL);
    CHECK(strstr(c, ".required = true, .source_offset = offsetof(") != NULL);
    CHECK(strstr(c, "_desc->reified_union_projections[") != NULL);
    CHECK(strstr(c, "_td->reified_union_projections[") != NULL);
    CHECK(strstr(c, "_desc->reified_callable_deps[") != NULL);
    CHECK(strstr(c, "__materialize_") == NULL);
    const char *prefixes[] = {"static const FengUnionProjection ",
                              "static const FengUnionTagProbe "};
    for (size_t i = 0U; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        for (const char *p = c; (p = strstr(p, prefixes[i])) != NULL; ++p)
            CHECK(g24_c_scope_depth(c, p) == 0U);
    }
}

/* Four shared match entrances plus owner constructors and method-owned
 * projections. Naked leaves, child unions and whole unions close the same
 * open paths to distinct static and dynamic probe combinations. */
static void g24_projection_predicates(void (*compile_c)(const char *)) {
    g24_static_source(
        "module g24.projections;\n"
        "spec Inner: i32 | string; spec Outer: Inner | bool;\n"
        "func plain_match(v: i32): bool { let value: bool = match v { 0 { return true; } else { return false; } }; return value; }\n"
        "func plain_if(v: bool): string { let value: string = if v { return \"yes\"; } else { throw \"no\"; }; return value; }\n"
        "func normal_if(v: bool): bool { let value: bool = if v { return true; } else { false }; return value; }\n"
        "func statement<T: Outer>(v: T): bool { match v { Inner -> i32 { return true; } else { return false; } } }\n"
        "func expression<T: Outer>(v: T): bool { return match v { Inner -> string { true } else { false } }; }\n"
        "func exits<T: Outer>(v: T): bool { let value: bool = match v { Inner -> i32 { return true; } else { return false; } }; return value; }\n"
        "func predicate<T: Outer>(v: T): bool { return v match Inner -> i32; }\n"
        "func relay<T: Outer>(v: T): bool { return predicate<T>(v); }\n"
        "func outer<T: Outer>(v: T): bool { return relay<T>(v); }\n"
        "type Host<T: Outer> { open let hit: bool; func Host(v: T) { self.hit = v match Inner -> i32; }\n"
        " func method<U: Outer>(v: U): bool { return v match Inner -> string; } }\n"
        "@value type Cell<T: Outer> { open let hit: bool; func Cell(v: T) { self.hit = v match Inner -> i32; } }\n"
        "func run(): bool { let child: Inner = 73; let whole: Outer = child;\n"
        " statement<i32>(73); statement<Inner>(child); statement<Outer>(whole);\n"
        " expression<string>(\"word\"); expression<Inner>(child); expression<Outer>(whole);\n"
        " exits<i32>(73); exits<Inner>(child); exits<Outer>(whole);\n"
        " predicate<bool>(true); predicate<i32>(73); predicate<Inner>(child); predicate<Outer>(whole);\n"
        " let host = Host<i32>(73); host.method<string>(\"word\");\n"
        " let cell = Cell<Inner>(child); return outer<Outer>(whole) && host.hit && cell.hit; }\n",
        false, g24_check_projection_predicates, compile_c);
}

/* ISSUE-G24-020: both nominal spec labels must resolve their own methods;
 * statement, result, infix and all-exit paths share the same type identity. */
static void g24_union_entry_identity(void (*compile_c)(const char *)) {
    g24_static_source(
        "module g24.entryidentity;\n"
        "spec A { func left(): i32; } spec B { func right(): i32; } spec U: A | B;\n"
        "func keep<T>(v: T): T { return v; }\n"
        "func statement(v: U): i32 { match v { x: A { return x.left(); } x: B { return x.right(); } } return -1; }\n"
        "func expression(v: U): i32 { return match v { x: A { x.left() } x: B { x.right() } else { -1 } }; }\n"
        "func infix(v: U): bool { return v match B; }\n"
        "func all_exit(v: U): i32 { let n: i32 = match v { x: A { return x.left(); } x: B { return x.right(); } else { return -1; } }; return n; }\n"
        "func use(): i32 { return keep<i32>(7); }\n",
        false, NULL, compile_c);
}

/* ISSUE-G24-009: empty calls, full dependencies, recursive forwarding and
 * escaped closures all share immutable descriptors, never invocation data. */
void test_g24_static_descriptors(void (*compile_c)(const char *)) {
    g24_static_assertion_pairs();
    g24_projection_predicates(compile_c);
    g24_union_entry_identity(compile_c);
    g24_fit_routing(compile_c);
    g24_fit_parameter_slots(compile_c);
    g24_fit_nominal_identities(compile_c);
    g24_static_source(
        "module g24.storage;\n"
        "spec Reader<T>(): T;\n"
        "func identity<T>(value: T): T { return value; }\n"
        "func again<T>(value: T, depth: i32): T {\n"
        " if depth == 0 { return identity<T>(value); }\n"
        " return again<T>(value, depth - 1); }\n"
        "func capture<T>(value: T): Reader<T> { return () { return identity<T>(value); }; }\n"
        "func run(): string { let first = capture<i32>(23); let second = capture<string>(\"word\");\n"
        " first(); again<i32>(42, 3); return again<string>(second(), 2); }\n",
        false, NULL, compile_c);
    g24_static_source(
        "module g24.storage;\n"
        "type Box<T> { open let value: T; func Box(value: T) { self.value = value; } }\n"
        "@value type Cell<T> { open let value: T; func Cell(value: T) { self.value = value; } }\n"
        "fit T[] { func saved(): T { let box = Box<T>(self[0]); let cell = Cell<T>(box.value); return cell.value; } }\n"
        "func run(): string { let a: i32[] = [11, 22]; let b: string[] = [\"text\"];\n"
        " a.saved(); b.saved(); a.saved(); return b.saved(); }\n",
        true, NULL, compile_c);
}
