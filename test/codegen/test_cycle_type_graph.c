/* Descriptor emission must consume concrete graph facts, including hidden
 * object-spec subjects; existing ownership selection is deliberately unchanged. */
#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

/* Locate one descriptor definition by its debug name and check its own flag. */
static void cycle_descriptor_expect(const char *source, const char *name, bool cyclic) {
    char needle[256];
    THROW_CHECK(snprintf(needle, sizeof needle, ".name = \"cycle_codegen.%s\",", name) > 0);
    const char *begin = strstr(source, needle);
    if (begin == NULL) fprintf(stderr, "missing cycle descriptor: %s\n", name);
    THROW_CHECK(begin != NULL);
    const char *end = strstr(begin, "};");
    const char *flag = strstr(begin, cyclic ? ".is_potentially_cyclic = true,"
                                           : ".is_potentially_cyclic = false,");
    if (flag == NULL || end == NULL || flag >= end)
        fprintf(stderr, "incorrect cycle descriptor: %s expected %d\n", name, cyclic);
    THROW_CHECK(flag != NULL && end != NULL && flag < end);
}

/* Check actual declarations, not a global count that an unrelated descriptor
 * could satisfy; compiling generated C also validates each trace table. */
void test_cycle_type_graph_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module cycle_codegen;"
        "open type Leaf{open let n:int;}"
        "open spec Maybe<T>:bool|T;"
        "open type Box<T>{open var item:T;}"
        "open type Phantom<T>{open let n:int;}"
        "@value open type Inline<T>{open var item:Maybe<T>;}"
        "open type Node{open var next:Maybe<Node>;}"
        "open type Owner{open var next:Maybe<Box<Owner>>;}"
        "open type ValueNode{open var next:Inline<ValueNode>;}"
        "open type LeafBox{open let item:Box<Leaf>;}"
        "open type IntBox{open let item:Box<i32>;}"
        "open type PhantomOwner{open let item:Phantom<PhantomOwner>;}"
        "open spec MutableView{open var next:Maybe<Node>;}"
        "open spec ScalarView{open var number:int;}"
        "open type Dynamic{open let subject:MutableView;}"
        "open func wrap<T>(item:T):Box<T>{return Box<T>{item:item};}"
        "open func close(item:Owner):Box<Owner>{return wrap<Owner>(item);}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted && output.c_source != NULL);
    const char *cyclic[] = {"Node", "Owner", "ValueNode", "Box<Owner>",
                            "Dynamic", "MutableView.<spec_default>"};
    const char *acyclic[] = {"Leaf", "LeafBox", "IntBox", "Box<Leaf>", "Box<i32>",
                             "PhantomOwner", "Phantom<PhantomOwner>", "ScalarView.<spec_default>"};
    for (size_t i = 0; i < sizeof cyclic / sizeof *cyclic; ++i)
        cycle_descriptor_expect(output.c_source, cyclic[i], true);
    for (size_t i = 0; i < sizeof acyclic / sizeof *acyclic; ++i)
        cycle_descriptor_expect(output.c_source, acyclic[i], false);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    puts("cycle descriptors: closed generic, inline, unknown and default-subject flags passed");
}
