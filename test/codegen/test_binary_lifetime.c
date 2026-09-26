#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Select a generated definition, excluding its preceding prototype. */
static char *binary_definition(const char *source, const char *symbol) {
    for (const char *at = source; (at = strstr(at, symbol)) != NULL; ++at) {
        const char *open = strchr(at, '{');
        const char *semicolon = strchr(at, ';');
        if (open == NULL || (semicolon != NULL && semicolon < open)) continue;
        const char *end = strstr(open, "\n}");
        THROW_CHECK(end != NULL);
        char *body = malloc((size_t)(end - open) + 1U);
        THROW_CHECK(body != NULL);
        memcpy(body, open, (size_t)(end - open));
        body[end - open] = '\0';
        return body;
    }
    THROW_CHECK(false);
    return NULL;
}

/* The left +1 token must be protected before evaluating the right operand;
 * a borrowed-only comparison must not acquire an unnecessary ownership token. */
void test_binary_operand_lifetime_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module binary.cleanup;"
        "func fail():string{throw 1;}"
        "open func chain(a:string,b:string,c:string):string{return a+b+c;}"
        "open func guarded(a:string,b:string):string{return (a+b)+fail();}"
        "open func borrowed(a:string,b:string):bool{return a==b;}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                        NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok);
    compile_c(output.c_source);
    const char *names[] = {"__chain__from__", "__guarded__from__"};
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        char *body = binary_definition(output.c_source, names[i]);
        const char *first = strstr(body, "feng_string_concat(");
        const char *push = first != NULL ? strstr(first, "feng_cleanup_push(") : NULL;
        const char *second = push != NULL ? strstr(push, "feng_string_concat(") : NULL;
        const char *release = second != NULL ? strstr(second, "feng_release(") : NULL;
        THROW_CHECK(first != NULL && push != NULL && second != NULL && release != NULL);
        THROW_CHECK(strstr(body, "feng_string_concat(feng_string_concat(") == NULL);
        THROW_CHECK(strstr(body, "feng_frame_release_to(") != NULL);
        THROW_CHECK(strstr(body, "feng_retain(") == NULL);
        if (i == 1U) {
            const char *right = strstr(body, "__fail__from__void()");
            THROW_CHECK(right != NULL && push < right && right < second);
        }
        free(body);
    }
    char *borrowed = binary_definition(output.c_source, "__borrowed__from__");
    THROW_CHECK(strstr(borrowed, "feng_string_equal(a, b)") != NULL);
    THROW_CHECK(strstr(borrowed, "feng_cleanup_push(") == NULL);
    THROW_CHECK(strstr(borrowed, "feng_retain(") == NULL);
    free(borrowed);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
}
