#include "parser/parser.h"
#include "semantic/semantic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* Check each diagnostic independently for both static array owner forms. */
void test_array_static_parity_semantics(void) {
    const char *suffixes[] = {"[]", "[!]"};
    const char *bodies[] = {
        "func run(){Missing%s.echo(1);}",
        "func run(){i32%s.missing(1);}",
        "func run(){i32%s.echo(1);}",
        "func run(){i32%s.instance();}",
        "func run(){i32%s.echo(\"wrong\");}",
        "func run(){i32%s.keep<i32,string>(1);}",
        "spec Map<E>(v:E):E;func run(){let f:Map<string> =i32%s.echo;}",
        "func run(){let items:i32%s=[1];items%s.echo(1);}"
    };
    const char *codes[] = {
        "AE1013", "AE0309", "AE0309", "AE0309",
        "AE0512", "AE1015", "AE0522", "AE1013"
    };
    for (size_t form = 0U; form < 2U; ++form) {
        for (size_t i = 0U; i < sizeof bodies / sizeof *bodies; ++i) {
            char body[256], source[768];
            const char *access = suffixes[i == 2U ? 1U - form : form];
            snprintf(body, sizeof body, bodies[i], access, access);
            snprintf(source, sizeof source,
                "module parity;fit T%s{static func echo(v:T):T{return v;}"
                "static func keep<U>(v:U):U{return v;}func instance():i32{return 1;}}\n%s\n",
                suffixes[form], body);
            FengProgram *program = NULL;
            FengParseError parse = {0};
            CHECK(feng_parse_source(source, strlen(source), "parity.ff", &program, &parse));
            const FengProgram *programs[] = {program};
            FengSemanticAnalyzeOptions options = {
                .target = FENG_COMPILE_TARGET_LIB,
                .pointer_size = feng_get_host_pointer_size()
            };
            FengSemanticAnalysis *analysis = NULL;
            FengSemanticError *errors = NULL;
            size_t count = 0U;
            bool ok = feng_semantic_analyze_with_options(
                programs, 1U, &options, &analysis, &errors, &count);
            if (ok || count != 1U || strcmp(errors[0].code, codes[i]) != 0) {
                fprintf(stderr, "%s case %zu expected %s\n", suffixes[form], i, codes[i]);
                for (size_t j = 0U; j < count; ++j) {
                    fprintf(stderr, "%s %s\n", errors[j].code, errors[j].message);
                }
            }
            CHECK(!ok && count == 1U && strcmp(errors[0].code, codes[i]) == 0);
            CHECK(errors[0].token.line == 2U && errors[0].token.column > 0U);
            feng_semantic_errors_free(errors, count);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
        }
    }
    puts("array static semantic diagnostic matrix passed");
}
