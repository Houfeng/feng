#include "../throw_constraint_helpers.h"

/* All existing declaration entrances retain a distinct builtin bound kind. */
void test_throw_constraint_parser(void) {
    FengProgram *program = throw_constraint_parse(
        "module bounds;spec Named{}"
        "func raise<A,B:Named,E:throw>(error:E){throw error;}"
        "type Owner<T:throw>{func call<U:throw>(value:U){throw value;}}"
        "spec Reader<E:throw>{func read():E;}"
        "spec Action<E:throw>(value:E):void;");
    const FengCallableSignature *function = &program->declarations[1]->as.function_decl;
    THROW_CHECK(function->type_param_count == 3U);
    THROW_CHECK(function->type_params[0].constraint_kind == FENG_CONSTRAINT_NONE);
    THROW_CHECK(function->type_params[1].constraint_kind == FENG_CONSTRAINT_SPEC);
    THROW_CHECK(function->type_params[1].constraint != NULL);
    THROW_CHECK(function->type_params[2].constraint_kind == FENG_CONSTRAINT_THROW);
    THROW_CHECK(function->type_params[2].constraint == NULL);
    THROW_CHECK(program->declarations[2]->as.type_decl.type_params[0].constraint_kind == FENG_CONSTRAINT_THROW);
    THROW_CHECK(program->declarations[2]->as.type_decl.members[0]->as.callable.type_params[0].constraint_kind == FENG_CONSTRAINT_THROW);
    THROW_CHECK(program->declarations[3]->as.spec_decl.type_params[0].constraint_kind == FENG_CONSTRAINT_THROW);
    THROW_CHECK(program->declarations[4]->as.spec_decl.type_params[0].constraint_kind == FENG_CONSTRAINT_THROW);
    FILE *stream = tmpfile();
    THROW_CHECK(stream != NULL);
    feng_program_dump(stream, program);
    rewind(stream);
    char dump[8192] = {0};
    THROW_CHECK(fread(dump, 1U, sizeof(dump) - 1U, stream) > 0U);
    THROW_CHECK(strstr(dump, "E: throw") != NULL && strstr(dump, "T: throw") != NULL);
    fclose(stream);
    feng_program_free(program);

    const char *invalid[] = {
        "let value:throw;", "type Holder{let value:throw;}",
        "func use(value:throw){}", "func make():throw{}",
        "type Box<T>{}let value:Box<throw>;", "spec Errors:throw|string;",
        "spec Child:throw{}", "func work(){try work() catch error:throw{}}",
        "func bad<throw>(){}", "func bad<T:throw|Named>(){}",
        "func bad<T:throw[]>(){}", "func bad<T:throw*>(){}"
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof *invalid; ++i) {
        char source[512];
        snprintf(source, sizeof source, "module invalid;%s", invalid[i]);
        FengParseError error = {0};
        program = NULL;
        THROW_CHECK(!feng_parse_source(source, strlen(source), "invalid.ff", &program, &error));
        const char *code = i >= 9U ? "SE0610" : "SE0002";
        if (strcmp(error.code, code) != 0) fprintf(stderr, "parser case %zu: %s %s\n", i, error.code, error.message);
        THROW_CHECK(strcmp(error.code, code) == 0);
        THROW_CHECK(error.token.line == 1U && error.token.column > 0U);
        feng_program_free(program);
    }
}
