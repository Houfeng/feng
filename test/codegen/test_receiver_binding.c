#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

#include <sys/stat.h>
#include <unistd.h>

/* Preserve generated IR for failures in receiver preparation or EH lowering. */
static char *receiver_read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    THROW_CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    THROW_CHECK(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1U);
    THROW_CHECK(text != NULL && fread(text, 1U, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    THROW_CHECK(fclose(file) == 0);
    return text;
}

/* Every closed record using this representation must preserve its policy,
 * including records regenerated for a different generic constraint. */
static void receiver_expect_descriptor(const char *source, const char *symbol,
    const char *binding) {
    size_t matches = 0U;
    for (const char *p = source; (p = strstr(p, "static const FengGenericParamDescriptor ")) != NULL; ++p) {
        const char *end = strchr(p, '\n');
        const char *init = strstr(p, " = {");
        if (init == NULL || end == NULL || init >= end) continue;
        const char *descriptor = strstr(init, symbol);
        if (descriptor == NULL || descriptor >= end) continue;
        const char *policy = strstr(init, binding);
        THROW_CHECK(policy != NULL && policy < end);
        ++matches;
    }
    if (matches == 0U) fprintf(stderr, "missing receiver descriptor: %s\n", symbol);
    THROW_CHECK(matches != 0U);
}

/* Extract a function definition, skipping its forward declaration and calls. */
static char *receiver_function(const char *source, const char *name) {
    for (const char *p = source; (p = strstr(p, name)) != NULL; ++p) {
        const char *body = strchr(p, '{');
        const char *statement = strchr(p, ';');
        const char *line = strchr(p, '\n');
        if (body == NULL || statement == NULL || line == NULL || body > statement || body > line) continue;
        size_t depth = 1U;
        const char *end = body + 1;
        while (*end != '\0' && depth != 0U) {
            if (*end == '{') ++depth;
            if (*end == '}') --depth;
            ++end;
        }
        THROW_CHECK(depth == 0U);
        return strndup(body, (size_t)(end - body));
    }
    fprintf(stderr, "missing receiver definition: %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* Verify selection happens before side effects and that the node/storage
 * declarations belong to the surrounding call scope, not its activation if. */
static void receiver_expect_order(const char *source) {
    char *body = receiver_function(source, "__selected_G__");
    const char *selection = strstr(body, "->receiver_binding == FENG_RECEIVER_SNAPSHOT_VALUE");
    const char *storage = strstr(body, "_Alignas(max_align_t) char _receiver_snapshot");
    const char *node = strstr(body, "FengCleanupNode _cu__receiver_snapshot");
    const char *activate = strstr(body, "if (_receiver_snapshot_enabled");
    const char *retain = activate != NULL ? strstr(activate, "feng_aggregate_retain(") : NULL;
    const char *push = activate != NULL ? strstr(activate, "feng_cleanup_push_aggregate(") : NULL;
    const char *argument = strstr(body, "__mutate_G__");
    const char *invoke = argument != NULL ? strstr(argument, "->read(") : NULL;
    THROW_CHECK(selection != NULL && storage != NULL && node != NULL && activate != NULL);
    THROW_CHECK(selection < storage && storage < node && node < activate);
    THROW_CHECK(retain != NULL && push != NULL && retain < push);
    THROW_CHECK(argument != NULL && invoke != NULL && push < argument && argument < invoke);
    THROW_CHECK(strstr(body, "feng_object_new(") == NULL);
    free(body);
}

/* Source-driven checks cover descriptor classification, both generic domains,
 * constraint projection, and the native EH compiler pipeline at each level. */
void test_receiver_binding_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "module receiver.binding;"
        "spec Read{func read(extra:int):int;}"
        "spec Extra{func extra():int;}"
        "spec Child:Read,Extra{}"
        "spec Both:Extra & Read;"
        "spec Call():int;"
        "type Item:Child{let n:int;func read(extra:int):int{return self.n+extra;}"
        "func extra():int{return self.n;}}"
        "@value type Value:Read{var n:int;let owner:Item;"
        "func read(extra:int):int{self.n+=extra;return self.n;}}"
        "type Slot<T>{var value:T;}"
        "func mutate<T>(slot:Slot<T>,next:T):int{slot.value=next;return 1;}"
        "func selected<T:Read>(slot:Slot<T>,next:T):int{return slot.value.read(mutate<T>(slot,next));}"
        "func parent<T:Child>(slot:Slot<T>,next:T):int{return selected<T>(slot,next);}"
        "func intersection<T:Both>(slot:Slot<T>,next:T):int{return selected<T>(slot,next);}"
        "type Owner<T:Read>{var value:T;"
        "func read(next:T):int{return self.value.read(if true{self.value=next;1;}else{0;});}"
        "func method<U:Read>(first:U,next:U):int{var value=first;"
        "return value.read(if true{value=next;1;}else{0;});}}"
        "func echo<T>(value:T):T{return value;}"
        "open func run():int{let a=Item{n:4};let b=Item{n:5};"
        "let av:Read=a;let bv:Read=b;let ac:Child=a;let bc:Child=b;"
        "let ax:Both=a;let bx:Both=b;"
        "var n=selected<Read>(Slot<Read>{value:av},bv);"
        "n+=selected<Item>(Slot<Item>{value:a},b);"
        "n+=selected<Value>(Slot<Value>{value:Value{n:4,owner:a}},Value{n:5,owner:b});"
        "n+=parent<Child>(Slot<Child>{value:ac},bc);"
        "n+=intersection<Both>(Slot<Both>{value:ax},bx);"
        "let owner=Owner<Read>{value:av};n+=owner.read(bv);"
        "n+=owner.method<Read>(av,bv);"
        "let scalar=echo<int>(1);let text=echo<string>(\"a\");"
        "let array=echo<int[]>([1]);let callback:Call=(){return 1;};"
        "let call=echo<Call>(callback);return n+scalar+call();}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted);
    compile_c(output.c_source);
    const char *snapshot = ".receiver_binding = FENG_RECEIVER_SNAPSHOT_VALUE";
    const char *storage = ".receiver_binding = FENG_RECEIVER_BORROW_STORAGE";
    receiver_expect_descriptor(output.c_source, "&FengTypeDesc__receiver__binding__Item", snapshot);
    receiver_expect_descriptor(output.c_source, "&FengSpecAgg__receiver__binding__Read", snapshot);
    receiver_expect_descriptor(output.c_source, "&FengSpecAgg__receiver__binding__Child", snapshot);
    receiver_expect_descriptor(output.c_source, "&FengSpecAgg__receiver__binding__Both", snapshot);
    receiver_expect_descriptor(output.c_source, "&Feng__receiver__binding__Value__aggregate_desc", storage);
    receiver_expect_descriptor(output.c_source, "&feng_string_descriptor", snapshot);
    receiver_expect_descriptor(output.c_source, "&_feng_closed_array_desc_", snapshot);
    receiver_expect_descriptor(output.c_source, "&FengClosureDesc__receiver__binding__Call", snapshot);
    receiver_expect_descriptor(output.c_source, sizeof(void *) == 8U
        ? "&feng_i64_descriptor" : "&feng_i32_descriptor", storage);
    THROW_CHECK(strstr(output.c_source, "&(const FengGenericParamDescriptor){") == NULL);
    receiver_expect_order(output.c_source);

    char directory[] = "temp/receiver-binding-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char input[256], result[256], command[2048];
    snprintf(input, sizeof input, "%s/generated.c", directory);
    FILE *file = fopen(input, "wb");
    THROW_CHECK(file != NULL && fputs(output.c_source, file) >= 0 && fclose(file) == 0);
    const unsigned levels[] = {0U, 2U, 3U};
    for (size_t i = 0U; i < sizeof levels / sizeof levels[0]; ++i) {
        snprintf(result, sizeof result, "%s/generated-O%u.ll", directory, levels[i]);
        snprintf(command, sizeof command,
            "cc " FENG_TEST_C_EH_FLAGS "-Isrc/runtime -std=gnu11 -fexceptions "
            "-O%u -g -S -emit-llvm '%s' -o '%s'", levels[i], input, result);
        THROW_CHECK(system(command) == 0);
        char *ir = receiver_read_file(result);
        THROW_CHECK(strstr(ir, "invoke ") != NULL && strstr(ir, "landingpad ") != NULL);
        THROW_CHECK(strstr(ir, "call void @__llvm_c_eh_") == NULL);
        free(ir);
    }
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    fprintf(stdout, "generic receiver binding: descriptors, selection order and O0/O2/O3 EH passed\n");
}
