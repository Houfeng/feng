#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"
#include "semantic/semantic.h"

#define G22_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: G22 failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Six public declaration categories reachable through any module spelling. */
static const char *g22_provider =
    "open module g22.api;\n"
    "open spec Service { func get(): i32; }\n"
    "open type Record: Service { var field: i32; func get(): i32 { return 3; } }\n"
    "open enum Mode { First, Second }\n"
    "open spec Callback(): i32;\n"
    "open func run(): i32 { return 7; }\n"
    "open let fixed: i32 = 4;\n"
    "open var state: i32 = 5;\n";

/* A single source expectation, including the exact source token rather than
 * just whether semantic analysis happened to fail. The marker is the final
 * occurrence so an alias declaration can precede its conflicting use. */
static void g22_analyze(const char *const *sources, const char *const *paths,
                        size_t count, bool reverse, const char *code,
                        size_t error_source, const char *marker,
                        const char *lexeme, const char *detail) {
    FengProgram *owned[6] = {0};
    const FengProgram *programs[6] = {0};
    FengSemanticAnalyzeOptions options = {0};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool result;

    G22_CHECK(count <= 6U);
    for (size_t index = 0U; index < count; ++index) {
        FengParseError error = {0};
        bool parsed = feng_parse_source(sources[index], strlen(sources[index]),
                                        paths[index], &owned[index], &error);
        if (!parsed) {
            fprintf(stderr, "G22 parse %s: %s\n%s\n", paths[index], error.message, sources[index]);
        }
        G22_CHECK(parsed);
        programs[reverse ? count - 1U - index : index] = owned[index];
    }
    options.target = FENG_COMPILE_TARGET_LIB;
    options.pointer_size = feng_get_host_pointer_size();
    result = feng_semantic_analyze_with_options(programs, count, &options,
                                               &analysis, &errors, &error_count);
    if ((code == NULL && !result) || (code != NULL && (result || error_count != 1U)) ||
        (code != NULL && error_count == 1U && strcmp(errors[0].code, code) != 0)) {
        fprintf(stderr, "G22 expected %s, reverse=%d\n", code != NULL ? code : "success", reverse);
        for (size_t index = 0U; index < count; ++index) fprintf(stderr, "%s\n", sources[index]);
        for (size_t index = 0U; index < error_count; ++index)
            fprintf(stderr, "%s:%u:%u %s %s\n", errors[index].path,
                    errors[index].token.line, errors[index].token.column,
                    errors[index].code, errors[index].message);
    }
    G22_CHECK(result == (code == NULL));
    G22_CHECK(error_count == (code == NULL ? 0U : 1U));
    if (code != NULL) {
        const char *position = NULL;
        const char *cursor = sources[error_source];
        unsigned line = 1U, column = 1U;
        while ((cursor = strstr(cursor, marker)) != NULL) { position = cursor; ++cursor; }
        G22_CHECK(position != NULL);
        for (cursor = sources[error_source]; cursor < position; ++cursor) {
            if (*cursor == '\n') { ++line; column = 1U; } else { ++column; }
        }
        if (errors[0].token.line != line || errors[0].token.column != column ||
            errors[0].token.length != strlen(lexeme)) {
            fprintf(stderr, "G22 token expected %u:%u %s, got %u:%u %.*s: %s\n%s\n",
                    line, column, lexeme, errors[0].token.line, errors[0].token.column,
                    (int)errors[0].token.length, errors[0].token.lexeme,
                    errors[0].message, sources[error_source]);
        }
        G22_CHECK(strcmp(errors[0].path, paths[error_source]) == 0);
        G22_CHECK(strcmp(errors[0].code, code) == 0);
        G22_CHECK(errors[0].token.line == line);
        G22_CHECK(errors[0].token.column == column);
        G22_CHECK(errors[0].token.length == strlen(lexeme));
        G22_CHECK(memcmp(errors[0].token.lexeme, lexeme, strlen(lexeme)) == 0);
        if (detail != NULL) G22_CHECK(strstr(errors[0].message, detail) != NULL);
    }
    feng_semantic_errors_free(errors, error_count);
    feng_semantic_analysis_free(analysis);
    for (size_t index = 0U; index < count; ++index) feng_program_free(owned[index]);
}

/* MODULE05/08/09/10/11/20: all declaration kinds, lookup entrances, unused
 * names, import order and compilation-unit order share the same rule. */
static void g22_alias_collision_matrix(void) {
    static const char *declarations[] = {
        "type helper { static func run(): i32 { return 9; } }\n",
        "enum helper { Only }\n",
        "spec helper { func run(): i32; }\n",
        "func helper(): i32 { return 9; }\n",
        "let helper: i32 = 9;\n",
        "var helper: i32 = 9;\n"
    };
    static const char *uses[] = {
        "func check() {}\n",
        "func check() { let value: helper.Record; }\n",
        "func check(value: helper.Record) {}\n",
        "func check(): helper.Record { throw 1; }\n",
        "func check() { let value = helper.Record(); }\n",
        "func check() { let value = helper.Record { field: 1 }; }\n",
        "func check() { let value = helper.Mode.First; }\n",
        "func check(value: helper.Service) {}\n",
        "func check() { helper.run(); }\n",
        "func check() { let value: g22.api.Callback = helper.run; }\n",
        "func check() { let value = helper.fixed; }\n",
        "func check() { helper.state = 8; }\n",
        "func check() { helper.absent(); }\n",
        "type Box<T> { var value: T; }\nfunc check(value: Box<helper.Record>) {}\n",
        "func check() { let value = helper.Record() { field: 1 }; }\n",
        "func check() { let value = helper; }\n",
        "func check() { let value = helper.Only; }\n"
    };
    const char *paths[] = {"g22_provider.ff", "g22_consumer.ff", "g22_collision.ff"};
    for (size_t kind = 0U; kind < sizeof(declarations)/sizeof(*declarations); ++kind) {
        for (size_t external = 0U; external < 2U; ++external) {
            char conflict[512];
            snprintf(conflict, sizeof(conflict), "%s%s%s",
                     external ? "open module g22.other;\n" : "module g22.consumer;\n",
                     external ? "open " : "", declarations[kind]);
            for (size_t use = 0U; use < sizeof(uses)/sizeof(*uses); ++use) {
                for (size_t order = 0U; order < 2U; ++order) {
                    char consumer[2048];
                    snprintf(consumer, sizeof(consumer), "module g22.consumer;\n%s%s%s%s",
                             external && order ? "import g22.other;\n" : "",
                             "import g22.api as helper;\n",
                             external && !order ? "import g22.other;\n" : "", uses[use]);
                    const char *sources[] = {g22_provider, consumer, conflict};
                    g22_analyze(sources, paths, 3U, order != 0U,
                                use == 0U ? NULL : "AE0005", 1U,
                                use == 15U ? "helper;" : "helper.", "helper",
                                external ? "g22.other" : "g22_collision.ff");
                }
            }
        }
        for (size_t used = 0U; used < 2U; ++used) {
            char consumer[2048];
            size_t length = strcspn(declarations[kind], " ");
            snprintf(consumer, sizeof(consumer), "module g22.consumer;\nimport g22.api as helper;\n%s%s",
                     declarations[kind], used ? uses[8] : uses[0]);
            const char *sources[] = {g22_provider, consumer};
            g22_analyze(sources, paths, 2U, used != 0U, "AE0906", 1U,
                        declarations[kind] + length + 1U,
                        "helper", "already defined in this file");
        }
    }
}

/* MODULE12/16/18: aliased modules contribute no short names, aliases are
 * file-local, and distinct aliases remain valid even with unused collisions. */
static void g22_alias_isolation_matrix(void) {
    static const char *consumers[] = {
        "module g22.consumer;\nimport g22.api;\nimport g22.other as other;\nfunc check(value: Record) {}\n",
        "module g22.consumer;\nimport g22.api as first;\nimport g22.other as second;\nfunc check(a: first.Record, b: second.Record) {}\n",
        "module g22.consumer;\nimport g22.other;\nimport g22.api as helper;\nfunc check() { helper.run(); }\n",
        "module g22.consumer;\nimport g22.api as helper;\nfunc check() { helper.run(); }\n",
        "module g22.consumer;\nimport g22.api as first;\nimport g22.api as second;\nfunc check() { first.run(); second.state = 2; }\n"
    };
    const char *paths[] = {"g22_provider.ff", "g22_consumer.ff", "g22_other.ff"};
    const char *other = "open module g22.other;\nopen type Record {}\nlet helper = 1;\n";
    for (size_t index = 0U; index < sizeof(consumers)/sizeof(*consumers); ++index) {
        const char *sources[] = {g22_provider, consumers[index], other};
        for (size_t order = 0U; order < 2U; ++order)
            g22_analyze(sources, paths, 3U, order != 0U, NULL, 0U, NULL, NULL, NULL);
    }
    {
        const char *sources[] = {g22_provider,
            "module g22.consumer;\nimport g22.api as helper;\nfunc check() { helper.run(); }\n",
            "module g22.consumer;\nimport g22.api as helper;\nfunc other() { helper.run(); }\n"};
        g22_analyze(sources, paths, 3U, false, NULL, 0U, NULL, NULL, NULL);
    }
    {
        const char *sources[] = {g22_provider,
            "module g22.consumer;\nimport g22.api as helper;\nfunc check() { helper.run(); }\n",
            "module g22.consumer;\nfunc other() { helper.run(); }\n"};
        g22_analyze(sources, paths, 3U, false, "AE0001", 2U, "helper.run", "helper", NULL);
    }
}

/* MODULE01/02/06/19: minimal invalid names and alias-only syntax, with each
 * diagnostic located independently of any imported implementation body. */
static void g22_name_diagnostic_matrix(void) {
    /* Source, expected diagnostic, marker, exact diagnostic token. */
    static const char *cases[][4] = {
        {"module g22.consumer;\nimport absent.target;\n", "AE0902", "absent.target", "absent"},
        {"module g22.consumer;\nimport absent.target as api;\n", "AE0902", "absent.target", "absent"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check() { api.absent(); }\n", "AE0903", "absent()", "absent"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check(x: api) {}\n", "AE0901", "api)", "api"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check() { let x: api; }\n", "AE0901", "api; }", "api"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check() { let x = api; }\n", "AE0904", "api;", "api"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check() { api(); }\n", "AE0904", "api()", "api"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check() { run(); }\n", "AE0001", "run()", "run"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check(x: Record) {}\n", "AE1013", "Record", "Record"},
        {"module g22.consumer;\nfunc check(x: absent.target.Record) {}\n", "AE1013", "absent.target", "absent"},
        {"module g22.consumer;\nfunc check() { absent.target.run(); }\n", "AE0001", "absent.target", "absent"},
        {"module g22.consumer;\nfunc check() { g22.api.absent(); }\n", "AE0903", "absent()", "absent"},
        {"module g22.consumer;\nfunc check() { let value = g22.api.absent; }\n", "AE0903", "absent;", "absent"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check(x: api.Absent) {}\n", "AE1013", "api.Absent", "api"},
        {"module g22.consumer;\nfunc check(x: g22.api.Absent) {}\n", "AE1013", "g22.api.Absent", "g22"},
        {"module g22.consumer;\nimport g22.api as api;\nimport g22.api as api;\n", "AE0902", "g22.api as api", "g22"},
        {"module g22.consumer;\nimport g22.api as api;\nimport g22.api as api;\nfunc check() { api.run(); }\n", "AE0902", "g22.api as api", "g22"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc take(value: i32) {}\nfunc check() { take(api); }\n", "AE0904", "api);", "api"},
        {"module g22.consumer;\nimport g22.api;\nfunc check(value: Absent) {}\n", "AE1013", "Absent", "Absent"},
        {"module g22.consumer;\nimport g22.api;\nfunc check() { absent(); }\n", "AE0001", "absent()", "absent"},
        {"module g22.consumer;\nimport g22.api;\nfunc check() { let value = absent; }\n", "AE0001", "absent;", "absent"},
        {"module g22.consumer;\nimport g22.api as api;\nfunc check() { let value = api.absent; }\n", "AE0903", "absent;", "absent"}
    };
    const char *paths[] = {"g22_provider.ff", "g22_consumer.ff"};
    for (size_t index = 0U; index < sizeof(cases)/sizeof(*cases); ++index) {
        const char *sources[] = {g22_provider, cases[index][0]};
        g22_analyze(sources, paths, 2U, false, cases[index][1], 1U,
                    cases[index][2], cases[index][3], NULL);
    }
}

/* MODULE03/06/21: private declarations are visible to sibling files, while
 * only public names cross module boundaries, under all three spellings. */
static void g22_top_level_visibility_matrix(void) {
    static const char *declarations[] = {
        "type Hidden {}", "enum Hidden { Only }", "spec Hidden {}",
        "func Hidden(): i32 { return 3; }", "let Hidden: i32 = 4;", "var Hidden: i32 = 5;"
    };
    const char *paths[] = {"g22_visibility_provider.ff", "g22_visibility_consumer.ff"};
    for (size_t kind = 0U; kind < 6U; ++kind) {
        for (size_t access = 0U; access < 4U; ++access) {
            for (size_t visibility = 0U; visibility < 3U; ++visibility) {
                char provider[512], consumer[1024], use[512];
                const char *prefix = access == 2U ? "api." : access == 3U ? "g22.hidden." : "";
                const char *modifier = visibility == 0U ? "" : visibility == 1U ? "seal " : "open ";
                snprintf(provider, sizeof(provider), "open module g22.hidden;\n%s%s\n", modifier, declarations[kind]);
                if (kind < 3U) snprintf(use, sizeof(use), "func check(value: %sHidden) {}\n", prefix);
                else snprintf(use, sizeof(use), "func check() { let value = %sHidden%s; }\n", prefix, kind == 3U ? "()" : "");
                snprintf(consumer, sizeof(consumer), "%s%s%s",
                         access == 0U ? "open module g22.hidden;\n" : "module g22.consumer;\n",
                         access == 1U ? "import g22.hidden;\n" : access == 2U ? "import g22.hidden as api;\n" : "", use);
                const char *sources[] = {provider, consumer};
                bool accepted = access == 0U || visibility == 2U;
                const char *code = kind < 3U ? "AE1013" : access == 1U ? "AE0001" : "AE0903";
                const char *lexeme = kind < 3U && access == 2U ? "api" :
                                     kind < 3U && access == 3U ? "g22" : "Hidden";
                char marker[128];
                snprintf(marker, sizeof(marker), "%sHidden", kind < 3U ? prefix : "");
                g22_analyze(sources, paths, 2U, false, accepted ? NULL : code, 1U, marker, lexeme, NULL);
            }
        }
    }
    for (size_t sealed = 0U; sealed < 2U; ++sealed) {
        const char *sources[] = {
            sealed ? "seal module g22.internal; open type Public {}" : "module g22.internal; open type Public {}",
            "module g22.consumer; import g22.internal as api; func check(value: api.Public) {}"
        };
        g22_analyze(sources, paths, 2U, false, NULL, 0U, NULL, NULL, NULL);
    }
}

/* MODULE04/13/14/16: type/value imports collide lazily with other imports or
 * module declarations, and never form a cross-module overload set. */
static void g22_ordinary_import_collision_matrix(void) {
    static const char *declarations[] = {
        "type Name {}", "enum Name { Only }", "spec Name {}",
        "func Name(): i32 { return 1; }", "let Name = 1;", "var Name = 1;"
    };
    const char *paths[] = {"g22_left.ff", "g22_right.ff", "g22_third.ff", "g22_use.ff"};
    for (size_t kind = 0U; kind < 6U; ++kind) {
        char left[256], right[256], third[256], consumer[512];
        snprintf(left, sizeof(left), "open module g22.left; open %s", declarations[kind]);
        snprintf(right, sizeof(right), "open module g22.right; open %s", declarations[kind]);
        snprintf(third, sizeof(third), "open module g22.third; open %s", declarations[kind]);
        for (size_t used = 0U; used < 2U; ++used) {
            snprintf(consumer, sizeof(consumer), "module g22.consumer;\nimport g22.left;\nimport g22.right;\nimport g22.third;\n%s",
                     !used ? "func check() {}" : kind < 3U ? "func check(value: Name) {}" :
                     kind == 3U ? "func check() { Name(); }" : "func check() { let value = Name; }");
            const char *sources[] = {left, right, third, consumer};
            for (size_t reversed = 0U; reversed < 2U; ++reversed) {
                snprintf(consumer, sizeof(consumer), "module g22.consumer;\n%s%s",
                         reversed ? "import g22.third;\nimport g22.right;\nimport g22.left;\n" :
                                    "import g22.left;\nimport g22.right;\nimport g22.third;\n",
                         !used ? "func check() {}" : kind < 3U ? "func check(value: Name) {}" :
                         kind == 3U ? "func check() { Name(); }" : "func check() { let value = Name; }");
                g22_analyze(sources, paths, 4U, reversed != 0U, used ? "AE0005" : NULL,
                            3U, "Name", "Name", "g22.third");
            }
        }
    }
    {
        const char *sources[] = {
            "open module g22.left; open func Name(value: i32): i32 { return value; }",
            "open module g22.right; open func Name(value: string): string { return value; }",
            "module g22.consumer; import g22.left; import g22.right; func check() { Name(1); }"
        };
        g22_analyze(sources, paths, 3U, false, "AE0005", 2U, "Name(1)", "Name", NULL);
    }
}

/* MODULE13/14/20: ordinary imports never gain or lose precedence against
 * same-file or sibling declarations. An unimporting sibling remains clean. */
static void g22_import_local_collision_matrix(void) {
    static const char *declarations[] = {
        "type Name {}", "enum Name { Only }", "spec Name {}",
        "func Name(): i32 { return 1; }", "let Name = 1;", "var Name = 1;"
    };
    const char *paths[] = {"g22_external.ff", "g22_importing.ff", "g22_sibling.ff"};
    for (size_t kind = 0U; kind < 6U; ++kind) {
        const char *use = kind < 3U ? "func check(value: Name) {}" :
                          kind == 3U ? "func check() { Name(); }" : "func check() { let value = Name; }";
        for (size_t sibling_decl = 0U; sibling_decl < 2U; ++sibling_decl) {
            for (size_t used = 0U; used < 2U; ++used) {
                for (size_t order = 0U; order < 2U; ++order) {
                    char provider[256], consumer[1024], sibling[512], sibling_use[128];
                    snprintf(provider, sizeof(provider), "open module g22.external;\nopen %s\n", declarations[kind]);
                    snprintf(sibling_use, sizeof(sibling_use), "%s",
                             kind < 3U ? "func independent(value: Name) {}" :
                             kind == 3U ? "func independent() { Name(); }" : "func independent() { let value = Name; }");
                    snprintf(consumer, sizeof(consumer), "module g22.consumer;\nimport g22.external;\n%s\n%s\n",
                             sibling_decl ? "" : declarations[kind], used ? use : "func check() {}");
                    snprintf(sibling, sizeof(sibling), "module g22.consumer;\n%s\n%s\n",
                             sibling_decl ? declarations[kind] : "", sibling_use);
                    const char *sources[] = {provider, consumer, sibling};
                    g22_analyze(sources, paths, 3U, order != 0U, used ? "AE0005" : NULL,
                                1U, "Name", "Name", "g22.external");
                }
            }
        }
    }
    for (size_t local = 0U; local < 2U; ++local) {
        const char *sources[] = {
            "open module g22.external;\nopen func Name(): i32 { return 1; }\n",
            local ? "module g22.consumer;\nimport g22.external;\nlet Name = 2;\nfunc check() { Name(); }\n" :
                    "open module g22.second;\nopen let Name = 2;\n",
            "module g22.consumer;\nimport g22.external;\nimport g22.second;\nfunc check() { Name(); }\n"
        };
        g22_analyze(sources, paths, local ? 2U : 3U, false, "AE0005", local ? 1U : 2U,
                    "Name()", "Name", NULL);
    }
}

/* MODULE17: member absence on a local root must not fall back to a module. */
static void g22_local_root_missing_member(void) {
    const char *paths[] = {"g22_root_provider.ff", "g22_root_consumer.ff"};
    static const char *bodies[] = {
        "func check(g22: Root) { let value = g22.api.fixed; }",
        "func check() { let g22 = Root(); let value = g22.api.fixed; }",
        "func check() { var g22 = Root(); let value = g22.api.fixed; }"
    };
    for (size_t index = 0U; index < 3U; ++index) {
        char consumer[512];
        snprintf(consumer, sizeof(consumer), "module app; type Leaf {} type Root { var api: Leaf; } %s", bodies[index]);
        const char *sources[] = {g22_provider, consumer};
        g22_analyze(sources, paths, 2U, false, "AE0306", 1U, "fixed;", "fixed", NULL);
    }
}

/* MODULE22/24: member access checks happen when reading, writing, calling,
 * constructing or forming a method value, under both same-module and
 * cross-module access. Owner-internal accesses remain legal in every case. */
static void g22_member_visibility_matrix(void) {
    static const char *uses[][4] = {
        {"let x = value.field;", "AE0308", "field;", "field"},
        {"value.field = 1;", "AE0308", "field =", "field"},
        {"let x = Vault { field: 1 };", "AE1007", "field: 1", "field"},
        {"let x = Vault.shared;", "AE0305", "shared;", "shared"},
        {"Vault.shared = 1;", "AE0305", "shared =", "shared"},
        {"value.read();", "AE0308", "read()", "read"},
        {"let x: Reader = value.read;", "AE0308", "read;", "read"},
        {"Vault.readShared();", "AE0305", "readShared()", "readShared"},
        {"let x: Reader = Vault.readShared;", "AE0305", "readShared;", "readShared"}
    };
    const char *paths[] = {"g22_member_provider.ff", "g22_member_consumer.ff"};
    for (size_t access = 0U; access < 2U; ++access) {
        for (size_t visibility = 0U; visibility < 3U; ++visibility) {
            const char *modifier = visibility == 0U ? "seal " : visibility == 1U ? "open " : "";
            char provider[1024];
            snprintf(provider, sizeof(provider),
                     "open module g22.members;\nopen type Vault {\n"
                     "  %svar field: i32;\n  %sstatic var shared: i32;\n"
                     "  %sfunc read(): i32 { return self.field; }\n"
                     "  %sstatic func readShared(): i32 { return Vault.shared; }\n"
                     "  func owner(): i32 { return self.field + Vault.shared + self.read() + Vault.readShared(); }\n}\n",
                     modifier, modifier, modifier, modifier);
            for (size_t use = 0U; use < sizeof(uses)/sizeof(*uses); ++use) {
              for (size_t other_type = 0U; other_type < 2U; ++other_type) {
                char consumer[512];
                snprintf(consumer, sizeof(consumer), "%s\nspec Reader(): i32;\n%sfunc check(value: Vault) { %s }%s\n",
                         access ? "module g22.consumer;\nimport g22.members;" : "open module g22.members;",
                         other_type ? "type Outsider { " : "", uses[use][0], other_type ? " }" : "");
                const char *sources[] = {provider, consumer};
                g22_analyze(sources, paths, 2U, false, visibility == 0U ? uses[use][1] : NULL,
                            1U, uses[use][2], uses[use][3], NULL);
              }
            }
        }
    }
}

/* MODULE06: none of the six imported short-name categories leaks into a
 * sibling file, even though ordinary module declarations are shared. */
static void g22_short_name_file_isolation(void) {
    static const char *uses[][3] = {
        {"func check(value: Record) {}", "AE1013", "Record"},
        {"func check(value: Mode) {}", "AE1013", "Mode"},
        {"func check(value: Service) {}", "AE1013", "Service"},
        {"func check() { run(); }", "AE0001", "run"},
        {"func check() { let x = fixed; }", "AE0001", "fixed"},
        {"func check() { let x = state; }", "AE0001", "state"}
    };
    const char *paths[] = {"g22_provider.ff", "g22_importing.ff", "g22_sibling.ff"};
    for (size_t index = 0U; index < 6U; ++index) {
        char sibling[256];
        snprintf(sibling, sizeof(sibling), "module g22.consumer;\n%s\n", uses[index][0]);
        const char *sources[] = {g22_provider, "module g22.consumer;\nimport g22.api;\n", sibling};
        for (size_t reverse = 0U; reverse < 2U; ++reverse)
            g22_analyze(sources, paths, 3U, reverse != 0U, uses[index][1], 2U,
                        uses[index][2], uses[index][2], NULL);
        snprintf(sibling, sizeof(sibling), "module g22.consumer;\nimport g22.api;\n%s\n", uses[index][0]);
        g22_analyze(sources, paths, 3U, false, NULL, 0U, NULL, NULL, NULL);
        const char *at = strstr(uses[index][0], uses[index][2]);
        G22_CHECK(at != NULL);
        snprintf(sibling, sizeof(sibling), "module g22.consumer;\n%.*sg22.api.%s\n",
                 (int)(at - uses[index][0]), uses[index][0], at);
        g22_analyze(sources, paths, 3U, true, NULL, 0U, NULL, NULL, NULL);
    }
}

/* A binding's closed type belongs to its declaration file, including nested
 * array elements and generic arguments. Consumer homonyms never retarget it. */
static void g22_binding_type_provenance_matrix(void) {
    const char *type_declarations =
        "open spec Counter(): i32;\n"
        "open spec Reader<T>(value: T): T;\n"
        "open type Item { open let value: i32 = 17; }\n"
        "open spec Producer(): Item;\n";
    char types[1024], owner[2048], consumer[2048];
    const char *sources[] = {types, owner, consumer};
    const char *paths[] = {"g22_types.ff", "g22_owner.ff", "g22_reader.ff"};
    const char *actions[][4] = {
        {"let copied = %scallback; let result = copied();", NULL, NULL, NULL},
        {"let result: i32 = %secho(3);", NULL, NULL, NULL},
        {"let result: i32 = %sentries[0].value;", NULL, NULL, NULL},
        {"let result = %smaker().value == 17;", NULL, NULL, NULL},
        {"let result = %sinferred(7) == 7;", NULL, NULL, NULL},
        {"%scallback(1);", "AE0506", "callback(1)", "callback"},
        {"%secho(\"bad\");", "AE0506", "echo(\"bad\")", "echo"},
        {"%sfixed();", "AE0507", "fixed()", "fixed"},
        {"%sfixed = 2;", "AE0104", "fixed =", "fixed"}
    };

    snprintf(types, sizeof(types), "open module g22.types;\n%s", type_declarations);
    for (size_t origin = 0U; origin < 3U; ++origin) {
        const char *prefix = origin == 2U ? "definitions." : "";
        const char *imports = origin == 1U ? "import g22.types;\n" :
                              origin == 2U ? "import g22.types as definitions;\n" : "";
        snprintf(owner, sizeof(owner),
                 "open module g22.owner;\n%s%s"
                 "func read(): i32 { return 5; }\n"
                 "func identity(value: i32): i32 { return value; }\n"
                 "func create(): %sItem { return %sItem {}; }\n"
                 "open let callback: %sCounter = read;\n"
                 "open let maker: %sProducer = create;\n"
                 "open let echo: %sReader<i32> = identity;\n"
                 "open let entries: %sItem[] = [%sItem {}];\n"
                 "open let inferred = echo;\n"
                 "open let fixed: i32 = 9;\n",
                 imports, origin == 0U ? type_declarations : "",
                 prefix, prefix, prefix, prefix, prefix, prefix, prefix);
        for (size_t form = 0U; form < 3U; ++form) {
            const char *access = form == 0U ? "api." : form == 1U ? "g22.owner." : "";
            const char *consumer_import = form == 0U ? "import g22.owner as api;\n" :
                                          form == 2U ? "import g22.owner;\n" : "";
            for (size_t use = 0U; use < sizeof(actions) / sizeof(actions[0]); ++use) {
                char body[512];
                snprintf(body, sizeof(body), actions[use][0], access);
                snprintf(consumer, sizeof(consumer),
                         "module g22.consumer;\n%s"
                         "type Counter {}\ntype Reader<T> {}\ntype Item {}\n"
                         "func check() { %s }\n", consumer_import, body);
                for (size_t order = 0U; order < 2U; ++order) {
                    g22_analyze(sources, paths, 3U, order != 0U, actions[use][1],
                                2U, actions[use][2], actions[use][3], NULL);
                }
            }
        }
    }
}

/* Public test entry registered alongside the existing semantic suite. */
void test_g22_module_diagnostics(void) {
    g22_alias_collision_matrix();
    g22_alias_isolation_matrix();
    g22_name_diagnostic_matrix();
    g22_top_level_visibility_matrix();
    g22_ordinary_import_collision_matrix();
    g22_import_local_collision_matrix();
    g22_local_root_missing_member();
    g22_member_visibility_matrix();
    g22_short_name_file_isolation();
    g22_binding_type_provenance_matrix();
    puts("G22 semantic alias/name matrices passed");
}
