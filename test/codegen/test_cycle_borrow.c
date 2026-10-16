/* Borrow proofs must be independent of cycle classification, without relaxing
 * mutability, escape, dynamic representation or opaque invocation boundaries. */
#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Find one emitted function body, skipping prototypes and call sites. */
static char *cycle_borrow_body(const char *source, const char *name) {
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
    fprintf(stderr, "missing cycle borrow function: %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* A safe borrowed read needs neither a temporary token nor its unwind node.
 * Rejected proofs retain both; never merely count all operations in the file. */
static void cycle_borrow_expect(const char *source, const char *name, bool borrow) {
    char *body = cycle_borrow_body(source, name);
    bool retain = strstr(body, "feng_aggregate_retain(") != NULL;
    bool cleanup = strstr(body, "feng_cleanup_push_aggregate(") != NULL;
    if (retain == borrow || cleanup == borrow)
        fprintf(stderr, "%s: borrow=%d retain=%d cleanup=%d\n", name, borrow, retain, cleanup);
    THROW_CHECK(retain != borrow && cleanup != borrow);
    /* Spec/callable invocation may still protect its receiver separately. */
    free(body);
}

/* Exercise match/call borrowing, tag-only conversions and owning escapes in
 * generated C, including reference, inline, array and dynamic-subject slots. */
void test_cycle_borrow_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module cycle_borrow;"
        "open spec Maybe<T>:bool|T;"
        "open type Node{open let n:int;open var next:Maybe<Node>;}"
        "open spec Choice:bool|Node;"
        "open spec Nested:Choice|i64;"
        "open type Holder{open let node:Node;open let choice:Choice;}"
        "open type MutableHolder{open var choice:Choice;open var node:Node;}"
        "@value open type Inline{open let node:Node;}"
        "@value open type Editable{open var node:Node;}"
        "open spec InlineChoice:bool|Inline;"
        "open spec EditableChoice:bool|Editable;"
        "open spec ArrayChoice:bool|Node[];"
        "open spec View{open func number():int;}"
        "open spec ViewChoice:bool|View;"
        "open spec Callback():int;"
        "open spec CallbackChoice:bool|Callback;"
        "open spec Reader(value:Choice):int;"
        "open func read(value:Choice):int{return match value{item:Node{item.n}else{0}};}"
        "open func forward(value:Choice):int{return read(value);}"
        "open func field(value:Holder):int{return read(value.choice);}"
        "open func statement(value:Choice):int{match value{let item:Node{return item.n;}else{return 0;}}}"
        "open func infix(value:Choice):int{if value match item:Node{return item.n;}return 0;}"
        "open func nestedRead(value:Nested):int{return match value{item:Choice{read(item)}else{0}};}"
        "open func inlineRead(value:InlineChoice):int{return match value{item:Inline{item.node.n}else{0}};}"
        "open func arrayRead(value:ArrayChoice):int{return match value{item:Node[]{item[0].n}else{0}};}"
        "open func viewRead(value:ViewChoice):int{return match value{item:View{item.number()}else{0}};}"
        "open func callbackRead(value:CallbackChoice):int{return match value{item:Callback{item()}else{0}};}"
        "open func inject(value:Node):int{return read(value);}"
        "open func injectField(value:Holder):int{return read(value.node);}"
        "open func injectNested(value:Node):int{return nestedRead(value);}"
        "open func injectInline(value:Inline):int{return inlineRead(value);}"
        "open func injectArray(value:Node[]):int{return arrayRead(value);}"
        "open func injectView(value:View):int{return viewRead(value);}"
        "open func injectCallback(value:Callback):int{return callbackRead(value);}"
        "open func mutableMatch(value:Choice):int{return match value{var item:Node{item=Node{n:9};item.n}else{0}};}"
        "open func unusedVar(value:Choice):int{return match value{var item:Node{item.n}else{0}};}"
        "open func varStatement(value:Choice):int{match value{var item:Node{return item.n;}else{return 0;}}}"
        "open func varInfix(value:Choice):int{if value match var item:Node{return item.n;}return 0;}"
        "open func mutableInline(value:EditableChoice):int{return match value{item:Editable{item.node.n}else{0}};}"
        "open func mutablePath(value:MutableHolder):int{return read(value.choice);}"
        "open func mutableRoot(var value:Choice):int{return read(value);}"
        "open func injectMutable(var value:Node):int{return read(value);}"
        "open func injectMutablePath(value:MutableHolder):int{return read(value.node);}"
        "open func indirect(reader:Reader,value:Choice):int{return reader(value);}"
        "open func mutating(var value:Choice):int{value=false;return read(value);}"
        "open func mutableTarget(value:Choice):int{return mutating(value);}"
        "open func generic<T>(value:Maybe<T>):bool{return match value{item:T{true}else{false}};}"
        "open func returnInjected(value:Node):Choice{return value;}"
        "open func returnNested(value:Node):Nested{return value;}"
        "open func stored(value:Node):Holder{return Holder{node:value,choice:value};}"
        "open func captured(value:Choice):Callback{match value{item:Node{return (){return item.n;};}else{return (){return 0;};}}}"
        "open func produced():int{return read(Node{n:7});}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                        NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(ok && output.c_source != NULL);
    compile_c(output.c_source);
    const char *borrowed[] = {"__read__", "__forward__", "__field__", "__statement__",
        "__nestedRead__", "__inlineRead__", "__arrayRead__", "__viewRead__", "__callbackRead__",
        "__inject__", "__injectField__", "__injectNested__", "__injectInline__",
        "__injectArray__", "__injectView__", "__injectCallback__"};
    /* Infix matching keeps its existing materialization path in this stage. */
    const char *owned[] = {"__mutableMatch__", "__unusedVar__", "__varStatement__", "__varInfix__", "__infix__",
        "__mutableInline__", "__mutablePath__", "__mutableRoot__", "__injectMutable__",
        "__injectMutablePath__", "__indirect__", "__mutableTarget__"};
    for (size_t i = 0; i < sizeof borrowed / sizeof *borrowed; ++i)
        cycle_borrow_expect(output.c_source, borrowed[i], true);
    for (size_t i = 0; i < sizeof owned / sizeof *owned; ++i)
        cycle_borrow_expect(output.c_source, owned[i], false);
    const char *injected[] = {"__inject__", "__injectField__", "__injectNested__",
        "__injectInline__", "__injectArray__", "__injectView__", "__injectCallback__"};
    for (size_t i = 0; i < sizeof injected / sizeof *injected; ++i) {
        char *body = cycle_borrow_body(output.c_source, injected[i]);
        THROW_CHECK(strstr(body, "feng_retain(") == NULL);
        THROW_CHECK(strstr(body, "feng_cleanup_push(") == NULL);
        free(body);
    }
    const char *escapes[] = {"__returnInjected__", "__returnNested__"};
    for (size_t i = 0; i < sizeof escapes / sizeof *escapes; ++i) {
        char *body = cycle_borrow_body(output.c_source, escapes[i]);
        THROW_CHECK(strstr(body, "feng_aggregate_retain(") != NULL);
        free(body);
    }
    char *generic = cycle_borrow_body(output.c_source, "__generic_G__");
    THROW_CHECK(strstr(generic, "feng_aggregate_assign(") != NULL);
    THROW_CHECK(strstr(generic, "feng_cleanup_push_aggregate(") != NULL);
    free(generic);
    char *produced = cycle_borrow_body(output.c_source, "__produced__");
    THROW_CHECK(strstr(produced, "feng_object_new(") != NULL);
    THROW_CHECK(strstr(produced, "feng_cleanup_push_aggregate(") != NULL);
    free(produced);
    char *captured = cycle_borrow_body(output.c_source, "__captured__");
    THROW_CHECK(strstr(captured, "feng_retain(") != NULL || strstr(captured, "feng_assign(") != NULL);
    free(captured);
    /* The optimization cannot obtain its result by reverting classification. */
    const char *node = strstr(output.c_source, ".name = \"cycle_borrow.Node\"");
    THROW_CHECK(node != NULL);
    const char *cyclic = strstr(node, ".is_potentially_cyclic = true");
    const char *end = strstr(node, "};");
    THROW_CHECK(cyclic != NULL && end != NULL && cyclic < end);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    puts("cycle borrow: stable paths, union injection, owning escapes and conservative boundaries passed");
}
