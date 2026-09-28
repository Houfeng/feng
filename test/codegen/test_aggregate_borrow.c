#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Isolate a definition from forward declarations and unrelated functions. */
static char *aggregate_borrow_body(const char *source, const char *name) {
    for (const char *at = source; (at = strstr(at, name)) != NULL; ++at) {
        const char *line = strchr(at, '\n');
        const char *brace = strchr(at, '{');
        if (line == NULL || brace == NULL || brace > line) continue;
        const char *end = strstr(brace, "\n}");
        THROW_CHECK(end != NULL);
        char *body = strndup(brace, (size_t)(end - brace));
        THROW_CHECK(body != NULL);
        return body;
    }
    fprintf(stderr, "missing aggregate borrow function: %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* A borrowed read has no new aggregate owner; a rejected proof must retain it. */
static void aggregate_borrow_expect(const char *source, const char *name,
                                    bool borrowed) {
    char *body = aggregate_borrow_body(source, name);
    bool retains = strstr(body, "feng_aggregate_retain(") != NULL;
    bool registers = strstr(body, "feng_cleanup_push_aggregate(") != NULL;
    if (retains == borrowed || registers == borrowed)
        fprintf(stderr, "aggregate borrow %s: borrowed=%d retains=%d registers=%d\n",
                name, borrowed, retains, registers);
    THROW_CHECK(retains != borrowed && registers != borrowed);
    free(body);
}

/* Closed, immutable inline storage shares one proof across match and calls;
 * mutable, cyclic, dynamic and opaque boundaries keep their owning copies. */
void test_aggregate_borrow_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module aggregate.borrow;"
        "open type Ref{let n:int;}"
        "open spec Choice:bool|Ref;"
        "open type Holder{let value:Choice;}"
        "open type MutableHolder{var value:Choice;}"
        "@value open type Editable{var ref:Ref;func replace(next:Ref){self.ref=next;}}"
        "open spec EditableChoice:bool|Editable;"
        "open type Link{var links:Link[];}"
        "open enum Nonzero{First=13}"
        "open spec NonzeroChoice:Nonzero|Ref;"
        "open type NonzeroHolder{let value:NonzeroChoice;}"
        "open spec LinkChoice:bool|Link;"
        "open spec Reader(value:Choice):int;"
        "open func read(value:Choice):int{return match value{item:Ref{item.n}else{0}};}"
        "open func forward(value:Choice):int{return read(value);}"
        "open func construct():Holder{return Holder();}"
        "open func constructNonzero():NonzeroHolder{return NonzeroHolder();}"
        "open func mutableUnion(var value:Choice){value=false;}"
        "open func mutableReference(var value:Ref,next:Ref){value=next;}"
        "open func readonlyReference(value:Ref):int{return value.n;}"
        "open func readonlyVar(var value:Ref):int{return value.n;}"
        "open func readonlyValue(value:Editable):int{let copy=value;return copy.ref.n;}"
        "open func writingValue(value:Editable,next:Ref){value.ref=next;}"
        "open func forwardingValue(value:Editable,next:Ref){writingValue(value,next);}"
        "open func writingReceiver(value:Editable,next:Ref){value.replace(next);}"
        "open func writingDeferred(value:Editable,next:Ref){defer{value.ref=next;}}"
        "open func writingBranch(value:Editable,next:Ref,choose:bool){if choose{value.ref=next;}}"
        "open type Editor<T>{func replace(var current:T,next:T):T{current=next;return current;}}"
        "open func useEditor(first:Ref,next:Ref):Ref{return Editor<Ref>().replace(first,next);}"
        "open func mutableScalar(var value:int){value=1;}"
        "open func field(value:Holder):int{return read(value.value);}"
        "open func mutableField(value:MutableHolder):int{return read(value.value);}"
        "open func mutableSource(var value:Choice):int{return read(value);}"
        "open func mutableTarget(value:Choice):int{return mutableSource(value);}"
        "open func indirect(callback:Reader,value:Choice):int{return callback(value);}"
        "open func statement(value:Choice):int{match value{item:Ref{return item.n;}"
        "else{return 0;}}}"
        "open func exiting(value:Choice):int{let dead:int=match value{item:Ref{return item.n;}"
        "else{return 0;}};return dead;}"
        "open func mutableBinding(value:Choice):int{return match value{"
        "var item:Ref{item=Ref{n:9};item.n;}else{0}};}"
        "open func mutablePayload(value:EditableChoice):int{return match value{"
        "item:Editable{item.ref.n}else{0}};}"
        "open func cyclic(value:LinkChoice):int{return match value{item:Link{1}else{0}};}"
        "open spec GenericChoice<T>:bool|T;"
        "open func generic<T>(value:GenericChoice<T>):bool{return match value{"
        "item:T{true}else{false}};}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    const FengCodegenMapingSourceMapping mapping = {
        .source_path = unit.program->path, .package_root = ".", .package_name = "aggregate_borrow"
    };
    const FengCodegenOptions options = {
        .debug_source_mappings = &mapping, .debug_source_mapping_count = 1U
    };
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             &options, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted && output.c_source != NULL);
    compile_c(output.c_source);
    const char *borrowed[] = {"__read__", "__forward__", "__field__",
                             "__statement__", "__exiting__"};
    const char *owned[] = {"__mutableField__", "__mutableSource__", "__mutableTarget__",
        "__indirect__", "__mutableBinding__", "__mutablePayload__", "__cyclic__"};
    for (size_t i = 0U; i < sizeof borrowed / sizeof *borrowed; ++i)
        aggregate_borrow_expect(output.c_source, borrowed[i], true);
    for (size_t i = 0U; i < sizeof owned / sizeof *owned; ++i)
        aggregate_borrow_expect(output.c_source, owned[i], false);
    char *generic = aggregate_borrow_body(output.c_source, "__generic_G__");
    THROW_CHECK(strstr(generic, "feng_aggregate_assign(") != NULL);
    THROW_CHECK(strstr(generic, "feng_cleanup_push_aggregate(") != NULL);
    free(generic);
    char *zero = aggregate_borrow_body(output.c_source, "__construct__");
    char *nonzero = aggregate_borrow_body(output.c_source, "__constructNonzero__");
    THROW_CHECK(strstr(zero, "feng_object_new(") != NULL);
    THROW_CHECK(strstr(zero, "feng_aggregate_default_zero_init(") == NULL);
    THROW_CHECK(strstr(nonzero, "feng_aggregate_default_zero_init(") != NULL);
    free(zero);
    free(nonzero);
    aggregate_borrow_expect(output.c_source, "__mutableUnion__", false);
    char *mutable_ref = aggregate_borrow_body(output.c_source, "__mutableReference__");
    char *readonly_ref = aggregate_borrow_body(output.c_source, "__readonlyReference__");
    char *scalar = aggregate_borrow_body(output.c_source, "__mutableScalar__");
    THROW_CHECK(strstr(mutable_ref, "feng_retain(") != NULL);
    THROW_CHECK(strstr(mutable_ref, "feng_cleanup_push(") != NULL);
    THROW_CHECK(strstr(readonly_ref, "feng_retain(") == NULL);
    THROW_CHECK(strstr(scalar, "feng_retain(") == NULL);
    THROW_CHECK(strstr(scalar, "feng_cleanup_push(") == NULL);
    free(mutable_ref);
    free(readonly_ref);
    free(scalar);
    const char *readonly_parameters[] = {"__readonlyVar__", "__readonlyValue__", "__forwardingValue__"};
    const char *writable_parameters[] = {"__writingValue__", "__writingReceiver__",
        "__writingDeferred__", "__writingBranch__"};
    for (size_t i = 0U; i < sizeof readonly_parameters / sizeof *readonly_parameters; ++i) {
        char *body = aggregate_borrow_body(output.c_source, readonly_parameters[i]);
        THROW_CHECK(strstr(body, "_parameter") == NULL);
        free(body);
    }
    for (size_t i = 0U; i < sizeof writable_parameters / sizeof *writable_parameters; ++i) {
        char *body = aggregate_borrow_body(output.c_source, writable_parameters[i]);
        THROW_CHECK(strstr(body, "_parameter") != NULL);
        THROW_CHECK(strstr(body, "feng_aggregate_retain(") != NULL);
        free(body);
    }
    bool saw_readonly_parameter = false, saw_owned_parameter = false, saw_generic_parameter = false;
    for (size_t i = 0U; i < output.debug_info.variable_count; ++i) {
        const FengCodegenMapingVariableRecord *variable = &output.debug_info.variables[i];
        if (variable->kind != FENG_CODEGEN_MAPING_VARIABLE_PARAM) continue;
        if (strcmp(variable->display_name, "current") == 0 &&
            strstr(variable->frame_backend_symbol, "FengGenericMethod__") != NULL) {
            THROW_CHECK(variable->read_expr != NULL && strstr(variable->read_expr, "_parameter") != NULL);
            saw_generic_parameter = true;
        }
        if (strcmp(variable->display_name, "value") != 0) continue;
        if (strstr(variable->frame_backend_symbol, "__readonlyReference__") != NULL) {
            THROW_CHECK(variable->read_expr == NULL);
            saw_readonly_parameter = true;
        }
        if (strstr(variable->frame_backend_symbol, "__mutableReference__") != NULL) {
            THROW_CHECK(variable->read_expr != NULL && strstr(variable->read_expr, "_parameter") != NULL);
            saw_owned_parameter = true;
        }
    }
    THROW_CHECK(saw_readonly_parameter && saw_owned_parameter && saw_generic_parameter);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    puts("aggregate borrow: immutable reads and conservative boundaries passed");
}
