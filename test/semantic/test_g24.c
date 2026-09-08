#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "semantic/semantic.h"
#include "test_union_entry_order.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: G24 failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Check one independent program, including the complete diagnostic and its
 * exact source token. Valid programs must have no semantic diagnostics. */
static void g24_source(const char *source, const char *code,
                        const char *marker, const char *lexeme) {
    FengProgram *program = NULL;
    FengParseError parse_error = {0};
    FengSemanticAnalyzeOptions options = {0};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    bool parsed = feng_parse_source(source, strlen(source), "g24.ff", &program, &parse_error);
    if (!parsed) fprintf(stderr, "G24 parse: %s\n%s", parse_error.message, source);
    CHECK(parsed);
    const FengProgram *programs[] = {program};
    options.target = FENG_COMPILE_TARGET_LIB;
    options.pointer_size = feng_get_host_pointer_size();
    bool result = feng_semantic_analyze_with_options(programs, 1U, &options,
                                                     &analysis, &errors, &error_count);
    if (result != (code == NULL) || error_count != (code == NULL ? 0U : 1U) ||
        (code != NULL && error_count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "G24 expected %s\n%s", code != NULL ? code : "success", source);
        for (size_t i = 0U; i < error_count; ++i)
            fprintf(stderr, "%s:%u:%u %s %s\n", errors[i].path, errors[i].token.line,
                    errors[i].token.column, errors[i].code, errors[i].message);
    }
    CHECK(result == (code == NULL));
    CHECK(error_count == (code == NULL ? 0U : 1U));
    if (code != NULL) {
        const char *position = strstr(source, marker);
        unsigned line = 1U, column = 1U;
        CHECK(position != NULL);
        for (const char *p = source; p < position; ++p) {
            if (*p == '\n') { ++line; column = 1U; } else { ++column; }
        }
        if (errors[0].token.line != line || errors[0].token.column != column)
            fprintf(stderr, "G24 expected token %u:%u %s; got %u:%u\n%s", line, column,
                    lexeme, errors[0].token.line, errors[0].token.column, source);
        CHECK(strcmp(errors[0].code, code) == 0);
        CHECK(strcmp(errors[0].path, "g24.ff") == 0);
        CHECK(errors[0].token.line == line && errors[0].token.column == column);
        CHECK(errors[0].token.length == strlen(lexeme));
        CHECK(memcmp(errors[0].token.lexeme, lexeme, strlen(lexeme)) == 0);
    }
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* COMPOSITE20: field type and binding kind after full owner substitution. */
static void g24_intersection_fields(void) {
    char source[2048];
    for (size_t is_static = 0U; is_static < 2U; ++is_static) {
        for (size_t left_var = 0U; left_var < 2U; ++left_var) {
            for (size_t right_var = 0U; right_var < 2U; ++right_var) {
                for (size_t same_type = 0U; same_type < 2U; ++same_type) {
                    for (size_t inherited = 0U; inherited < 2U; ++inherited) {
                        snprintf(source, sizeof(source),
                            "module g24;\nspec Parent<T> { %s%s field: T; }\n"
                            "spec A<T>: Parent<T> {}\n"
                            "spec B<T> { %s%s field: T; }\n"
                            "spec Empty {}\nspec Nested<T>: B<T> & Empty;\n"
                            "spec Both: %s<i32> & Nested<%s>;\n",
                            is_static ? "static " : "", left_var ? "var" : "let",
                            is_static ? "static " : "", right_var ? "var" : "let",
                            inherited ? "A" : "Parent", same_type ? "i32" : "string");
                        g24_source(source, same_type && left_var == right_var ? NULL : "AE0623",
                                   "Both:", "Both");
                    }
                }
            }
        }
    }
    g24_source("module g24;\nspec A<T> { let field: T; }\nspec Both: A<i32> & A<string>;\n",
               "AE0623", "Both:", "Both");
    g24_source("module g24;\nspec A { let field: i32; }\n"
               "spec B { static var field: string; }\nspec Both: A & B;\n",
               NULL, NULL, NULL);
}

/* COMPOSITE10: every index survives the old capacity boundary and teardown. */
static void g24_complete_union_paths(void) {
    const size_t depths[] = {1U, 8U, 9U, 17U};

    for (size_t c = 0U; c < sizeof(depths) / sizeof(depths[0]); ++c) {
        char source[4096];
        size_t used = (size_t)snprintf(source, sizeof(source),
            "module g24.paths;\nspec U1: i32 | string;\n");
        FengParseError parse_error = {0};
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticAnalyzeOptions options = {0};
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;

        for (size_t depth = 2U; depth <= depths[c]; ++depth) {
            int written = snprintf(source + used, sizeof(source) - used,
                "spec U%zu: U%zu | bool;\n", depth, depth - 1U);
            CHECK(written > 0 && (size_t)written < sizeof(source) - used);
            used += (size_t)written;
        }
        CHECK(snprintf(source + used, sizeof(source) - used,
            "func enter(value: i32): U%zu { return value; }\n", depths[c]) > 0);
        CHECK(feng_parse_source(source, strlen(source), "g24_paths.ff", &program, &parse_error));
        const FengProgram *programs[] = {program};
        options.target = FENG_COMPILE_TARGET_LIB;
        options.pointer_size = feng_get_host_pointer_size();
        CHECK(feng_semantic_analyze_with_options(programs, 1U, &options,
                                    &analysis, &errors, &error_count));
        CHECK(error_count == 0U);
        CHECK(analysis->union_coercion_site_count == 1U);
        const FengUnionCoercionSite *site = &analysis->union_coercion_sites[0];
        CHECK(site->path_length == depths[c]);
        for (size_t index = 0U; index < depths[c]; ++index) {
            CHECK(site->path_indices[index] == 0U);
        }
        /* Recording the same site replaces, rather than aliases, its owned
         * path. This also exercises an input borrowed from the old record. */
        CHECK(feng_semantic_record_union_coercion_site(analysis, site->expr,
            site->target_union_decl, site->target_union_type_ref, site->member_index,
            site->member_type_ref, site->path_indices, site->path_length));
        CHECK(analysis->union_coercion_sites[0].path_length == depths[c]);
        feng_semantic_errors_free(errors, error_count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* COMPOSITE11/23: approved migration to shared two-pass path selection. */
static void g24_union_ordered_paths(void) {
    char source[1536];
    const char *forms[] = {"Inner | B", "B | Inner", "Inner | bool"};
    for (size_t shape = 0U; shape < 3U; ++shape) {
        for (size_t generic = 0U; generic < 2U; ++generic) {
            snprintf(source, sizeof(source),
                "module g24;\nspec A {}\nspec B {}\ntype Item: A, B {}\n"
                "spec Inner: A | %s;\nspec Outer: %s;\n"
                "func take<T: Outer>(x: T): T { return x; }\n"
                "func enter(value: Item): %s { return %s; }\n",
                shape == 2U ? "B" : "bool", forms[shape],
                generic ? "Item" : "Outer", generic ? "take<Item>(value)" : "value");
            if (generic) {
                g24_source(source, NULL, NULL, NULL);
            } else {
                const size_t path[] = {shape == 0U ? 1U : 0U, 0U};
                test_assert_union_entry_path(source, "g24.ff", path, shape == 2U ? 2U : 1U);
            }
        }
    }
    g24_source("module g24;\nspec A {}\nspec B {}\ntype Item: A, B {}\n"
        "spec Inner: A | B;\nspec Outer: Inner | Item;\n"
        "func take<T: Outer>(x: T): T { return x; }\n"
        "func enter(value: Item): Outer { take(value); return value; }\n", NULL, NULL, NULL);
}

/* COMPOSITE23/27/28: explicit, inferred and owner entry, plus open forwarding. */
static void g24_union_constraints(void) {
    char source[2048];
    const char *actuals[] = {"i32", "string", "Inner", "Outer", "bool", "f64"};
    for (size_t generic = 0U; generic < 2U; ++generic) {
        for (size_t entry = 0U; entry < 4U; ++entry) {
            for (size_t i = 0U; i < sizeof(actuals) / sizeof(actuals[0]); ++i) {
                const char *constraint = generic ? "OuterOf<i32>" : "Outer";
                char invocation[256];
                if (entry < 2U) {
                    snprintf(invocation, sizeof(invocation), entry == 0U ? "take<%s>(v);" : "take(v);", actuals[i]);
                } else {
                    snprintf(invocation, sizeof(invocation), "let slot: %s<%s>;", entry == 2U ? "Box" : "Choice", actuals[i]);
                }
                snprintf(source, sizeof(source),
                    "module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
                    "spec OuterOf<A>: A | string | bool;\n"
                    "func take<T: %s>(v: T): T { return v; }\n"
                    "type Box<T: %s> {}\nspec Choice<T: %s>: T | u8;\n"
                    "func run(v: %s) { %s }\n",
                    constraint, constraint, constraint, actuals[i], invocation);
                bool valid = i != 5U && (!generic || (i != 2U && i != 3U));
                const char *code = entry < 2U ? "AE0512" : "AE0710";
                char marker[128];
                snprintf(marker, sizeof(marker), "%s>;", actuals[i]);
                g24_source(source, valid ? NULL : code, entry < 2U ? invocation : marker,
                           entry < 2U ? "take" : actuals[i]);
            }
        }
    }
    const char *bounds[] = {"Inner", "Outer", "Other", "Wide", NULL};
    for (size_t i = 0U; i < 5U; ++i) {
        snprintf(source, sizeof(source),
            "module g24;\nspec Inner: i32 | string;\nspec Outer: Inner | bool;\n"
            "spec Other: i32 | string;\nspec Wide: Inner | f64;\n"
            "func take<U: Outer>(v: U): U { return v; }\n"
            "func relay<T%s%s>(v: T): T { return take<T>(v); }\n",
            bounds[i] ? ": " : "", bounds[i] ? bounds[i] : "");
        g24_source(source, i < 2U ? NULL : "AE0512", "take<T>(v)", "take");
    }
    g24_source("module g24;\nspec Named {}\ntype Item: Named {}\n"
        "spec Choice: Named | bool;\nfunc take<T: Choice>(v: T): T { return v; }\n"
        "func run(v: Item): Item { let u: Choice = v; return take(v); }\n", NULL, NULL, NULL);
    g24_source("module g24;\nspec A {}\nspec B {}\nspec Inner: A | bool;\n"
        "spec Outer: Inner | B;\nfunc take<U: Outer>(v: U): U { return v; }\n"
        "func relay<T: Inner>(v: T): T { return take<T>(v); }\n", NULL, NULL, NULL);
}

/* COMPOSITE25/35: implication keeps each generic object component instance. */
static void g24_intersection_constraints(void) {
    const char *actuals[] = {"Both<i32>", "Child<i32>", "Named<i32>", "Both<string>"};
    char source[1536];
    for (size_t target = 0U; target < 2U; ++target) {
        for (size_t explicit_arg = 0U; explicit_arg < 2U; ++explicit_arg) {
            for (size_t i = 0U; i < 4U; ++i) {
                char invocation[128];
                snprintf(invocation, sizeof(invocation), explicit_arg ? "take<%s>(v)" : "take(v)", actuals[i]);
                snprintf(source, sizeof(source),
                    "module g24;\nspec Named<A> { func get(): A; }\nspec Tagged {}\n"
                    "spec Both<A>: Named<A> & Tagged;\nspec Child<A>: Named<A>, Tagged {}\n"
                    "func take<T: %s<i32>>(v: T): T { return v; }\n"
                    "func run(v: %s): %s { return %s; }\n",
                    target ? "Both" : "Named", actuals[i], actuals[i], invocation);
                g24_source(source, i < 2U || (i == 2U && !target) ? NULL : "AE0512", invocation, "take");
            }
        }
    }
}

/* ISSUE-G24-008: a view of A does not satisfy a self constraint on B.
 * Distinct receiver/argument parameters preserve the intended legal use. */
static void g24_intersection_view_arguments(void) {
    char source[2048];
    for (size_t separate = 0U; separate < 2U; ++separate) {
        snprintf(source, sizeof(source),
            "module g24;\nspec Eq<T> { func eq(other: T): bool; }\n"
            "spec Ord<T> { func cmp(other: T): i32; }\nspec Both<T>: Eq<T> & Ord<T>;\n"
            "type A: Eq<A>, Ord<A> { func eq(other: A): bool { return true; } "
            "func cmp(other: A): i32 { return 0; } }\n"
            "type B: Eq<A>, Ord<A> { func eq(other: A): bool { return true; } "
            "func cmp(other: A): i32 { return 0; } }\n"
            "func take<%s>(v: %s, other: T): bool { return v.eq(other); }\n"
            "func run(b: B, a: A): bool { return take(b, a); }\n",
            separate ? "T, U: Both<T>" : "T: Both<T>", separate ? "U" : "T");
        g24_source(source, separate ? NULL : "AE0512", "take(b, a)", "take");
    }
}

/* COMPOSITE37/38: all match entrances retain complete open projection facts.
 * A repeated use deduplicates by identity, not by AST address or source line. */
static void g24_projection_facts(void) {
    const char *bodies[] = {
        "match v { x: i32 { return x; } else { return -1; } }",
        "return match v { x: i32 { x } else { -1 } };",
        "let result: i32 = match v { x: i32 { return x; } else { throw false; } }; return result;",
        "if v match x: i32 { return x; } return -1;",
        "if v match i32 { return 1; } if v match i32 { return 2; } return -1;"
    };
    for (size_t form = 0U; form < sizeof(bodies) / sizeof(bodies[0]); ++form) {
        char source[2048];
        FengProgram *program = NULL;
        FengParseError parse_error = {0};
        FengSemanticAnalysis *analysis = NULL;
        FengSemanticAnalyzeOptions options = {0};
        FengSemanticError *errors = NULL;
        size_t error_count = 0U;
        snprintf(source, sizeof(source),
            "module g24.projections;\nspec Choice<Element>: Element | bool;\n"
            "func read<T: Choice<i32>>(v: T): i32 { %s }\n", bodies[form]);
        CHECK(feng_parse_source(source, strlen(source), "g24_projection.ff", &program, &parse_error));
        const FengProgram *programs[] = {program};
        options.target = FENG_COMPILE_TARGET_LIB;
        options.pointer_size = feng_get_host_pointer_size();
        bool valid = feng_semantic_analyze_with_options(programs, 1U, &options,
            &analysis, &errors, &error_count);
        if (!valid) {
            fprintf(stderr, "%s\n", source);
            for (size_t i = 0U; i < error_count; ++i)
                fprintf(stderr, "%s %s\n", errors[i].code, errors[i].message);
        }
        CHECK(valid && error_count == 0U);
        const FengReifiableDepSet *deps = feng_semantic_lookup_reifiable_dep_set(
            analysis, program->declarations[1]);
        CHECK(deps != NULL && deps->union_projection_count == 1U);
        CHECK(analysis->union_projection_use_count == (form == 4U ? 2U : 1U));
        const FengUnionProjectionDep *dep = &deps->union_projections[0];
        CHECK(dep->subject_type_ref->as.named.segment_count == 1U);
        CHECK(dep->constraint_type_ref->as.named.segment_count == 3U);
        CHECK(dep->constraint_type_ref->as.named.type_arg_count == 1U);
        CHECK(dep->path_count == 1U);
        CHECK((dep->result_type_ref == NULL) == (form == 4U));
        CHECK(feng_semantic_reifiable_dep_set_append_union_projection(
            (FengReifiableDepSet *)deps, dep));
        CHECK(deps->union_projection_count == 1U);
        feng_semantic_errors_free(errors, error_count);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
}

/* Every arrow prefix is retained, including paths past the old fixed union
 * entry capacity. The closed query shares ordinary binding admission. */
static void g24_projection_paths(void) {
    const size_t depths[] = {1U, 2U, 8U, 9U, 17U};
    for (size_t d = 0U; d < sizeof(depths) / sizeof(depths[0]); ++d) {
        size_t depth = depths[d];
        for (size_t infix = 0U; infix < 2U; ++infix) {
            char source[8192], path[2048];
            size_t used = (size_t)snprintf(source, sizeof(source),
                "module g24.paths;\nspec U1: i32 | string;\n");
            for (size_t i = 2U; i <= depth; ++i)
                used += (size_t)snprintf(source + used, sizeof(source) - used,
                    "spec U%zu: U%zu | bool;\n", i, i - 1U);
            size_t path_used = 0U;
            for (size_t i = depth; i > 1U; --i)
                path_used += (size_t)snprintf(path + path_used, sizeof(path) - path_used,
                    "U%zu -> ", i - 1U);
            snprintf(path + path_used, sizeof(path) - path_used, "i32");
            snprintf(source + used, sizeof(source) - used,
                "func read<T: U%zu>(value: T): i32 { %s x: %s %s }\n"
                "func types(a: i32, b: U%zu, c: f64) {}\n",
                depth, infix ? "if value match" : "match value {",
                path, infix ? "{ return x; } return -1;" : "{ return x; } else { return -1; } }", depth);
            FengProgram *program = NULL;
            FengParseError parse_error = {0};
            CHECK(feng_parse_source(source, strlen(source), "g24_paths.ff", &program, &parse_error));
            const FengProgram *programs[] = {program};
            FengSemanticAnalysis *analysis = NULL;
            FengSemanticError *errors = NULL;
            size_t error_count = 0U;
            FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
                                                  .pointer_size = feng_get_host_pointer_size()};
            CHECK(feng_semantic_analyze_with_options(programs, 1U, &options,
                                                     &analysis, &errors, &error_count));
            CHECK(error_count == 0U && analysis->union_projection_use_count == 1U);
            const FengReifiableDepSet *deps = feng_semantic_lookup_reifiable_dep_set(
                analysis, program->declarations[depth]);
            CHECK(deps != NULL && deps->union_projection_count == 1U);
            const FengUnionProjectionDep *dep = &deps->union_projections[0];
            CHECK(dep->path_count == depth);
            for (size_t i = 0U; i < depth; ++i) {
                const FengTypeRef *segment = dep->path[i];
                FengSlice name = segment->as.named.segments[segment->as.named.segment_count - 1U];
                char expected[32];
                if (i + 1U == depth) snprintf(expected, sizeof(expected), "i32");
                else snprintf(expected, sizeof(expected), "U%zu", depth - i - 1U);
                CHECK(name.length == strlen(expected) && memcmp(name.data, expected, name.length) == 0);
            }
            const FengCallableSignature *types = &program->declarations[depth + 1U]->as.function_decl;
            for (size_t actual = 0U; actual < 3U; ++actual) {
                size_t *indices = NULL, count = 0U;
                bool accepted = feng_semantic_query_union_entry(analysis, program,
                    types->params[actual].type, types->params[1].type, &indices, &count);
                CHECK(accepted == (actual != 2U));
                CHECK(count == (actual == 0U ? depth : 0U));
                for (size_t i = 0U; i < count; ++i) CHECK(indices[i] == 0U);
                if (actual != 0U) CHECK(indices == NULL);
                free(indices);
            }
            feng_semantic_errors_free(errors, error_count);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
    }
}

/* Static storage cannot weaken admission: alternate matching and mismatched
 * complete spec instances at direct, recursive and escaped-callable sites. */
static void g24_descriptor_admission_pairs(void) {
    const char *bodies[] = {
        "return take<T>(value);",
        "if count == 0 { return take<T>(value); } return relay<T>(value, count - 1);",
        "let saved: Reader<T> = () { return take<T>(value); }; return saved();"
    };
    for (size_t body = 0U; body < sizeof(bodies) / sizeof(bodies[0]); ++body) {
        for (size_t matches = 0U; matches < 2U; ++matches) {
            char source[2048];
            snprintf(source, sizeof(source),
                "module g24;\nspec Named<A> { func get(): A; }\nspec Tagged {}\n"
                "spec Both<A>: Named<A> & Tagged;\nspec Reader<A>(): A;\n"
                "func take<U: Both<i32>>(value: U): U { return value; }\n"
                "func relay<T: Both<%s>>(value: T, count: i32): T { %s }\n",
                matches ? "i32" : "string", bodies[body]);
            g24_source(source, matches ? NULL : "AE0512", "take<T>(value)", "take");
        }
    }
    for (size_t writable = 0U; writable < 2U; ++writable) {
        for (size_t constrained = 0U; constrained < 2U; ++constrained) {
            char source[1536];
            snprintf(source, sizeof(source),
                "module g24;\nspec Choice: i32 | string;\n"
                "func take<U%s>(value: U): U { return value; }\n"
                "fit T%s { func forward(): T { return take<T>(self[0]); } }\n",
                constrained ? ": Choice" : "", writable ? "[!]" : "[]");
            g24_source(source, constrained ? "AE0512" : NULL, "take<T>(self[0])", "take");
        }
    }
}

/* ISSUE-G24-012: fit-local, owner and method parameters have independent
 * declaration scopes; declaration order and spelling do not change matching. */
static void g24_fit_parameter_scopes(void) {
    for (size_t writable = 0U; writable < 2U; ++writable) {
        for (size_t same_name = 0U; same_name < 2U; ++same_name) {
            for (size_t is_static = 0U; is_static < 2U; ++is_static) {
                for (size_t fit_first = 0U; fit_first < 2U; ++fit_first) {
                    char source[1536], fit[256], owner[384];
                    const char *array = writable ? "[!]" : "[]";
                    snprintf(fit, sizeof(fit),
                        "fit T%s { func selected(): T { return self[0]; } }\n", array);
                    snprintf(owner, sizeof(owner),
                        "type Router<%s> { %sfunc choose<U>(values: U%s): U { return values.selected(); } }\n",
                        same_name ? "T" : "Owner", is_static ? "static " : "", array);
                    snprintf(source, sizeof(source), "module g24;\n%s%s",
                        fit_first ? fit : owner, fit_first ? owner : fit);
                    g24_source(source, NULL, NULL, NULL);
                }
            }
        }
        char source[1536];
        snprintf(source, sizeof(source),
            "module g24;\ntype Element {}\nfit Element%s { func selected(): i32 { return 1; } }\n"
            "type Router<T> { func choose<U>(values: U%s): i32 { return values.selected(); } }\n",
            writable ? "[!]" : "[]", writable ? "[!]" : "[]");
        g24_source(source, "AE0306", "selected();", "selected");
    }
}

/* Entry for independently added G24 semantic coverage; old tests stay intact. */
void test_g24_composite_diagnostics(void) {
    g24_complete_union_paths();
    g24_intersection_fields();
    g24_union_ordered_paths();
    g24_union_constraints();
    g24_intersection_constraints();
    g24_intersection_view_arguments();
    g24_descriptor_admission_pairs();
    g24_fit_parameter_scopes();
    g24_projection_facts();
    g24_projection_paths();
    puts("G24 composite semantic matrices passed");
}
