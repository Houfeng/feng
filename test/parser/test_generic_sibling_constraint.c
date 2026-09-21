#include "../generic_sibling_constraint_helpers.h"

/* Check source names rather than relying on printed AST text. */
static void sibling_named(const FengTypeRef *type, const char *name, size_t arity) {
    SIBLING_CHECK(type != NULL && type->kind == FENG_TYPE_REF_NAMED);
    SIBLING_CHECK(type->as.named.segment_count == 1U);
    SIBLING_CHECK(type->as.named.segments[0].length == strlen(name));
    SIBLING_CHECK(memcmp(type->as.named.segments[0].data, name, strlen(name)) == 0);
    SIBLING_CHECK(type->as.named.type_arg_count == arity);
}

/* The bound belongs to its declared parameter in both source orders. */
static void sibling_parameters(const FengTypeParam *params, size_t count, bool later) {
    SIBLING_CHECK(count == 2U);
    SIBLING_CHECK(params[0].name.length == 1U && params[0].name.data[0] == 'T');
    SIBLING_CHECK(params[1].name.length == 1U && params[1].name.data[0] == 'U');
    size_t bounded = later ? 0U : 1U;
    SIBLING_CHECK(params[bounded].constraint_kind == FENG_CONSTRAINT_SPEC);
    SIBLING_CHECK(params[1U - bounded].constraint_kind == FENG_CONSTRAINT_NONE);
    SIBLING_CHECK(params[1U - bounded].constraint == NULL);
    sibling_named(params[bounded].constraint, "Box", 1U);
    sibling_named(params[bounded].constraint->as.named.type_args[0], later ? "U" : "T", 0U);
}

/* Preserve sibling references on functions, methods and generic owners. */
void test_generic_sibling_constraint_parser(void) {
    const char *heads[] = {"T,U:Box<T>", "T:Box<U>,U"};
    for (size_t order = 0U; order < 2U; ++order) {
        char source[1024];
        int length = snprintf(source, sizeof source,
            "module sibling.parser;func read<%s>(){}type Owner<%s>{}"
            "spec Bound<%s>{}type Methods{func read<%s>(){}}"
            "fit Methods{static func read<%s>(){}}", heads[order], heads[order],
            heads[order], heads[order], heads[order]);
        SIBLING_CHECK(length > 0 && (size_t)length < sizeof source);
        FengProgram *program = sibling_parse(source);
        SIBLING_CHECK(program->declaration_count == 5U);
        const FengCallableSignature *function = &program->declarations[0]->as.function_decl;
        sibling_parameters(function->type_params, function->type_param_count, order != 0U);
        const FengDecl *owner = program->declarations[1];
        sibling_parameters(owner->as.type_decl.type_params,
            owner->as.type_decl.type_param_count, order != 0U);
        const FengDecl *spec = program->declarations[2];
        sibling_parameters(spec->as.spec_decl.type_params,
            spec->as.spec_decl.type_param_count, order != 0U);
        const FengCallableSignature *method = &program->declarations[3]->as.type_decl.members[0]->as.callable;
        sibling_parameters(method->type_params, method->type_param_count, order != 0U);
        method = &program->declarations[4]->as.fit_decl.members[0]->as.callable;
        sibling_parameters(method->type_params, method->type_param_count, order != 0U);
        feng_program_free(program);
    }
    FengProgram *program = sibling_parse(
        "module sibling.parser;func nested<T:Box<Packet<U[]>>,U>(){}");
    const FengTypeRef *bound = program->declarations[0]->as.function_decl.type_params[0].constraint;
    sibling_named(bound, "Box", 1U);
    const FengTypeRef *packet = bound->as.named.type_args[0];
    sibling_named(packet, "Packet", 1U);
    SIBLING_CHECK(packet->as.named.type_args[0]->kind == FENG_TYPE_REF_ARRAY);
    sibling_named(packet->as.named.type_args[0]->as.inner, "U", 0U);
    feng_program_free(program);
    puts("generic sibling constraint parser cases passed");
}
