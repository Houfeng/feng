#include "../generic_sibling_constraint_helpers.h"
#include "codegen/codegen.h"

/* Find the definition, skipping prototypes with the same generated symbol. */
static const char *sibling_body(const char *source, const char *symbol) {
    for (const char *cursor = source; (cursor = strstr(cursor, symbol)) != NULL; ++cursor) {
        const char *brace = strchr(cursor, '{');
        const char *semicolon = strchr(cursor, ';');
        if (brace != NULL && (semicolon == NULL || brace < semicolon)) return brace;
    }
    SIBLING_CHECK(false);
    return NULL;
}

/* Shared bodies dispatch through the bounded slot and keep source-order inputs. */
void test_generic_sibling_constraint_codegen(void (*compile_c)(const char *)) {
    char *source = sibling_source(sibling_declarations(),
        "func run():i32{let x=Ref<i32>{value:41};let y=Value<string>{value:\"kept\"};"
        "let a=first<i32,Ref<i32>>(x);let b=later<Ref<i32>,i32>(x);"
        "let c=relayFirst<string,Value<string>>(y);let d=relayLater<Value<string>,string>(y);"
        "let e=inferFirst(x,(i32)1);let f=inferLater(x,(i32)2);"
        "childFirst<i32,Ref<i32>>(x);childLater<Ref<i32>,i32>(x);"
        "ownerFirst<i32,Ref<i32>>(x);ownerLater<Ref<i32>,i32>(x);"
        "retainNominal<string>();"
        "let array:string[]=[c,d];let packet=Packet<string[]>{value:array,marker:\"wide\"};"
        "let result=nested<Ref<Packet<string[]>>,string>(Ref<Packet<string[]>>{value:packet});"
        "let m=Methods<string>();m.first(x,(i32)1);Methods<string>.later(x,(i32)2);"
        "m.fitLater(x,(i32)3);Methods<string>.fitFirst(x,(i32)4);"
        "if result.value[0]!=\"kept\"{return 0;}return a+b+e+f;}\n");
    SiblingConstraintUnit unit = sibling_analyze(source, NULL, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    SIBLING_CHECK(ok && output.c_source != NULL);
    SIBLING_CHECK(strstr(output.c_source,
        "const FengGenericParamDescriptor *_T, const FengGenericParamDescriptor *_U") != NULL);
    const char *names[] = {"void feng__sibling__bounds__first_G__from__",
                          "void feng__sibling__bounds__later_G__from__"};
    const char *witnesses[] = {"_U->witness", "_T->witness"};
    for (size_t i = 0U; i < 2U; ++i) {
        const char *body = sibling_body(output.c_source, names[i]);
        const char *end = strstr(body, "\n}");
        const char *witness = strstr(body, witnesses[i]);
        const char *dispatch = strstr(body, "->get(");
        SIBLING_CHECK(end != NULL && witness != NULL && witness < end);
        SIBLING_CHECK(dispatch != NULL && dispatch < end);
    }
    const char *forwarders[] = {"void feng__sibling__bounds__childFirst_G__from__",
                               "void feng__sibling__bounds__childLater_G__from__"};
    for (size_t i = 0U; i < 2U; ++i) {
        const char *body = sibling_body(output.c_source, forwarders[i]);
        const char *end = strstr(body, "\n}");
        const char *forwarded = strstr(body, i == 0U ? "_Z, _A" : "_A, _Z");
        const char *descriptor = strstr(body, "FengGenericParamDescriptor _");
        SIBLING_CHECK(end != NULL && forwarded != NULL && forwarded < end);
        SIBLING_CHECK(descriptor == NULL || descriptor > end);
    }
    compile_c(output.c_source);
    feng_codegen_error_free(&error);
    feng_codegen_output_free(&output);
    sibling_dispose(&unit);
    free(source);
    puts("generic sibling constraint shared C and dispatch passed");
}
