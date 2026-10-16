#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"
#include "platform/platform.h"

#include <unistd.h>

/* Compile identical layout assertions against every supported target ABI. */
static void union_slot_targets(const char *source) {
    const char *targets[] = {"macos-arm64", "linux-arm64-gnu", "linux-x64-gnu",
                            "linux-arm64-musl", "linux-x64-musl"};
    char root[] = "temp/union-slot-layout-XXXXXX";
    THROW_CHECK(mkdtemp(root) != NULL);
    char path[256];
    THROW_CHECK(snprintf(path, sizeof path, "%s/layout.c", root) > 0);
    FILE *file = fopen(path, "w");
    THROW_CHECK(file != NULL && fputs(source, file) >= 0 && fclose(file) == 0);
    for (size_t i = 0U; i < sizeof targets / sizeof *targets; ++i) {
        char command[1024];
        THROW_CHECK(snprintf(command, sizeof command,
            "build/toolchain/llvm/bin/clang --target=%s -std=gnu11 -Werror -fsyntax-only "
            "-nostdlibinc -DFENG_COMPILE_SDK_FREE -Isrc/runtime "
            "-Ibuild/toolchain/llvm-c-eh/include '%s'",
            feng_platform_clang_target(targets[i]), path) > 0);
        THROW_CHECK(system(command) == 0);
    }
    THROW_CHECK(unlink(path) == 0 && rmdir(root) == 0);
}

/* Keep indices beyond 255 valid without merging distinct trivial members. */
static void union_slot_wide_tag(void (*compile_c)(const char *)) {
    char source[32768];
    size_t used = (size_t)snprintf(source, sizeof source, "open module slot.wide;");
    for (size_t i = 0U; i < 257U; ++i)
        used += (size_t)snprintf(source + used, sizeof source - used,
            "open enum E%zu{First=%zu}", i, i);
    used += (size_t)snprintf(source + used, sizeof source - used, "open spec Many:");
    for (size_t i = 0U; i < 257U; ++i)
        used += (size_t)snprintf(source + used, sizeof source - used, "%sE%zu", i ? "|" : "", i);
    THROW_CHECK(used < sizeof source);
    THROW_CHECK(snprintf(source + used, sizeof source - used,
        ";open func last():Many{return E256.First;}") > 0);
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    THROW_CHECK(feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                          NULL, &output, &error));
    THROW_CHECK(strstr(output.c_source, "uint32_t tag;") != NULL);
    THROW_CHECK(strstr(output.c_source, ".tag = 256U") != NULL);
    THROW_CHECK(strstr(output.c_source, ".managed_slot_count = 257,") != NULL);
    THROW_CHECK(strstr(output.c_source, "_fwd") == NULL);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
}

/* Isolate one emitted static definition without accepting its declaration. */
static char *union_slot_definition(const char *source, const char *name) {
    for (const char *at = source; (at = strstr(at, name)) != NULL; ++at) {
        const char *line = strchr(at, '\n');
        const char *equal = strchr(at, '=');
        if (line == NULL || equal == NULL || equal > line) continue;
        const char *end = strstr(equal, "\n};");
        THROW_CHECK(end != NULL);
        char *definition = strndup(equal, (size_t)(end - equal));
        THROW_CHECK(definition != NULL);
        return definition;
    }
    fprintf(stderr, "missing union descriptor: %s\n", name);
    THROW_CHECK(false);
    return NULL;
}

/* Require one ordered slot per tag, all relative to the same payload base. */
static void union_slot_table(const char *source, const char *name,
                              const char *const *kinds, size_t count) {
    char identity[256];
    THROW_CHECK(snprintf(identity, sizeof identity, "FengSpecAggSlots__slot__layout__%s[]", name) > 0);
    char *table = union_slot_definition(source, identity);
    const char *at = table;
    for (size_t i = 0U; i < count; ++i) {
        char entry[512];
        THROW_CHECK(snprintf(entry, sizeof entry,
            "{ .offset = offsetof(struct FengSpecValue__slot__layout__%s, payload), .kind = %s, .nested = ",
            name, kinds[i]) > 0);
        at = strstr(at, entry);
        THROW_CHECK(at != NULL);
        at += strlen(entry);
        if (strcmp(kinds[i], "FENG_SLOT_NESTED_AGGREGATE") == 0)
            THROW_CHECK(*at == '&');
        else THROW_CHECK(strncmp(at, "NULL", 4U) == 0);
    }
    THROW_CHECK(strstr(at, ".kind = ") == NULL);
    free(table);
    THROW_CHECK(snprintf(identity, sizeof identity, "FengSpecAgg__slot__layout__%s ", name) > 0);
    char *descriptor = union_slot_definition(source, identity);
    char expected[256];
    THROW_CHECK(snprintf(expected, sizeof expected, ".managed_slot_count = %zu,", count) > 0);
    THROW_CHECK(strstr(descriptor, expected) != NULL);
    THROW_CHECK(snprintf(expected, sizeof expected,
        ".managed_slot_choice_offset = offsetof(struct FengSpecValue__slot__layout__%s, tag) + 1,", name) > 0);
    THROW_CHECK(strstr(descriptor, expected) != NULL);
    free(descriptor);
}

/* Verify the generated layout, complete tables and unchanged static cleanup. */
void test_union_slot_layout_codegen(void (*compile_c)(const char *)) {
    const char *source =
        "open module slot.layout;"
        "@value open type Empty{}"
        "open type Ref{let value:int;}"
        "@value open type Pair{let first:Ref;let second:Ref;}"
        "open spec View{let value:int;}"
        "open spec Thunk():int;"
        "open spec Small:i8|u8;"
        "open spec Inner:i16|bool;"
        "open spec Choice:i8|Ref|Pair|View|Thunk|Inner;"
        "open spec Maybe<T>:Empty|T;"
        "open spec Nested<T>:Maybe<T>|string;"
        "open type Node{var left:Maybe<Node>;var right:Maybe<Node>;}"
        "open type Holder{var value:Choice;}"
        "open func wrap<T>(value:T):Maybe<T>{return value;}"
        "open func small(value:i8):Maybe<i8>{return wrap<i8>(value);}"
        "open func flag(value:bool):Maybe<bool>{return wrap<bool>(value);}"
        "open func real(value:f64):Maybe<f64>{return wrap<f64>(value);}"
        "open func large(value:Pair):Maybe<Pair>{return wrap<Pair>(value);}"
        "open func nested(value:Maybe<i8>):Maybe<Maybe<i8>>{return wrap<Maybe<i8>>(value);}"
        "open func replace(holder:Holder,value:Choice){holder.value=value;}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool emitted = feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                             NULL, &output, &error);
    if (!emitted) fprintf(stderr, "%s: %s\n", error.code, error.message);
    THROW_CHECK(emitted && output.c_source != NULL);
    THROW_CHECK(strstr(output.c_source, "_fwd") == NULL);
    THROW_CHECK(strstr(output.c_source, "FENG_SLOT_FORWARD") == NULL);
    THROW_CHECK(strstr(output.c_source, "uint32_t tag;") != NULL);
    THROW_CHECK(strstr(output.c_source, "_Alignas(void *) union {") != NULL);
    const char *small[] = {"FENG_SLOT_NONE", "FENG_SLOT_NONE"};
    const char *choice[] = {"FENG_SLOT_NONE", "FENG_SLOT_POINTER", "FENG_SLOT_NESTED_AGGREGATE",
        "FENG_SLOT_NESTED_AGGREGATE", "FENG_SLOT_POINTER", "FENG_SLOT_NESTED_AGGREGATE"};
    union_slot_table(output.c_source, "Small", small, 2U);
    union_slot_table(output.c_source, "Choice", choice, 6U);
    THROW_CHECK(strstr(output.c_source, "feng_release((_o->value).payload.m1)") != NULL);
    THROW_CHECK(strstr(output.c_source, "feng_release(((_o->value).payload.m2).first)") != NULL);
    const char *checks =
        "\n_Static_assert(sizeof(((struct FengSpecValue__slot__layout__Small *)0)->tag) == sizeof(uint32_t), \"tag width\");\n"
        "_Static_assert(offsetof(struct FengSpecValue__slot__layout__Small, payload) == _Alignof(void *), \"small payload prefix\");\n"
        "_Static_assert(sizeof(struct FengSpecValue__slot__layout__Small) == 2 * sizeof(void *), \"small union\");\n"
        "_Static_assert(sizeof(struct FengSpecValue__slot__layout__Maybe__G__slot__layout__Node) == 2 * sizeof(void *), \"managed union\");\n"
        "_Static_assert(sizeof(struct Feng__slot__layout__Node) == sizeof(FengManagedHeader) + 4 * sizeof(void *), \"node layout\");\n";
    size_t bytes = strlen(output.c_source) + strlen(checks) + 16384U;
    char *complete = malloc(bytes);
    THROW_CHECK(complete != NULL);
    THROW_CHECK(snprintf(complete, bytes, "%s%s", output.c_source, checks) > 0);
    /* Test every emitted open/closed carrier rather than guessing generic
     * parameter manglings or accepting only pointer-sized type arguments. */
    const char *prefix = "struct FengSpecValue__slot__layout__";
    for (const char *at = output.c_source; (at = strstr(at, prefix)) != NULL; ++at) {
        const char *end = strchr(at, '{');
        const char *line = strchr(at, '\n');
        const char *semi = strchr(at, ';');
        if (end == NULL || line == NULL || end > line || (semi != NULL && semi < end)) continue;
        if (strncmp(end, "{\n    uint32_t tag;", strlen("{\n    uint32_t tag;")) != 0) continue;
        const char *name_end = at + strlen(prefix);
        while ((*name_end >= 'A' && *name_end <= 'Z') || (*name_end >= 'a' && *name_end <= 'z') ||
               (*name_end >= '0' && *name_end <= '9') || *name_end == '_') ++name_end;
        size_t used = strlen(complete);
        int written = snprintf(complete + used, bytes - used,
            "_Static_assert(offsetof(%.*s, payload) == _Alignof(void *), \"shared payload prefix\");\n",
            (int)(name_end - at), at);
        THROW_CHECK(written > 0 && (size_t)written < bytes - used);
    }
    compile_c(complete);
    union_slot_targets(complete);
    free(complete);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    union_slot_wide_tag(compile_c);
    puts("union slots: compact layout, ordered static choices and static lifecycle verified");
}
