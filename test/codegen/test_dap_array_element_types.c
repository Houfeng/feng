#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"
#include "debug/debug.h"
#include <unistd.h>

/* Concrete type records survive sidecars and dependency merging without becoming locals. */
static void union_slot_debug_types(const FengSemanticAnalysis *analysis) {
    FengCodegenMapingSourceMapping mapping = {
        .source_path = "throw_constraint.ff", .package_name = "slot", .package_root = "."};
    FengCodegenOptions options = {.debug_source_mappings = &mapping, .debug_source_mapping_count = 1U};
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    THROW_CHECK(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, &options, &output, &error));
    const char *names[] = {"slot.layout.Small", "slot.layout.Maybe<i8>", "slot.layout.Ref", "slot.layout.Pair",
        "slot.layout.Status", "slot.layout.Readable", "slot.layout.Getter"};
    const char *types[] = {"struct FengSpecValue__slot__layout__Small",
        "struct FengSpecValue__slot__layout__Maybe__G__i8", "struct Feng__slot__layout__Ref *",
        "struct Feng__slot__layout__Pair", "int32_t", "struct FengSpecValue__slot__layout__Readable",
        "struct FengClosure__slot__layout__Getter *"};
    for (size_t n = 0U; n < sizeof names / sizeof *names; ++n) {
        size_t found = 0U;
        for (size_t i = 0U; i < output.debug_info.variable_count; ++i) {
            const FengCodegenMapingVariableRecord *record = &output.debug_info.variables[i];
            if (record->kind != FENG_CODEGEN_MAPING_VARIABLE_TYPE) continue;
            THROW_CHECK(record->frame_backend_symbol == NULL && record->read_expr == NULL &&
                        record->parent_display_type == NULL);
            THROW_CHECK(strcmp(record->display_name, record->display_type) == 0);
            THROW_CHECK(strstr(record->display_type, "<T>") == NULL);
            if (strcmp(record->display_type, names[n]) == 0) {
                THROW_CHECK(strcmp(record->backend_name, types[n]) == 0);
                ++found;
            }
        }
        if (found != 1U) fprintf(stderr, "expected one concrete debug type: %s\n", names[n]);
        THROW_CHECK(found == 1U);
    }
    FengCodegenMapingInfo type_info = {0};
    for (size_t repeat = 0U; repeat < 2U; ++repeat)
        THROW_CHECK(feng_codegen_maping_info_add_variable_with_display_type(&type_info, NULL,
            types[0], names[0], NULL, names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(type_info.variable_count == 1U);
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_display_type(&type_info, "frame",
        types[0], names[0], NULL, names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_display_type(&type_info, NULL,
        NULL, names[0], NULL, names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_display_type(&type_info, NULL,
        "", names[0], NULL, names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_display_type(&type_info, NULL,
        types[0], names[0], "expression", names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_display_type(&type_info, NULL,
        types[0], "other name", NULL, names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_parent_display_type(&type_info, NULL,
        types[0], names[0], NULL, names[0], "parent", FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    THROW_CHECK(!feng_codegen_maping_info_add_variable_with_display_type(&type_info, NULL,
        "void *", names[0], NULL, names[0], FENG_CODEGEN_MAPING_VARIABLE_TYPE));
    char root[] = "temp/union-slot-debug-types-XXXXXX";
    THROW_CHECK(mkdtemp(root) != NULL);
    char binary[256], dependency[256], merged[256];
    THROW_CHECK(snprintf(binary, sizeof binary, "%s/binary", root) > 0);
    THROW_CHECK(snprintf(dependency, sizeof dependency, "%s/dependency.fd", root) > 0);
    THROW_CHECK(snprintf(merged, sizeof merged, "%s/merged.fd", root) > 0);
    FILE *file = fopen(binary, "wb");
    THROW_CHECK(file != NULL && fputs("fingerprint", file) >= 0 && fclose(file) == 0);
    char *message = NULL;
    THROW_CHECK(feng_debug_write_fd(dependency, binary, NULL, 0U, &type_info, &message));
    const char *dependencies[] = {dependency};
    THROW_CHECK(feng_debug_write_merged_fd(merged, binary, &mapping, 1U,
        &output.debug_info, dependencies, 1U, &message));
    FengDebugArtifact loaded = {0};
    THROW_CHECK(feng_debug_read_fd(merged, &loaded, &message));
    THROW_CHECK(loaded.info.variable_count == output.debug_info.variable_count);
    size_t type_count = 0U;
    for (size_t i = 0U; i < loaded.info.variable_count; ++i) {
        const FengCodegenMapingVariableRecord *record = &loaded.info.variables[i];
        if (record->kind == FENG_CODEGEN_MAPING_VARIABLE_TYPE) {
            THROW_CHECK(record->frame_backend_symbol == NULL && record->read_expr == NULL &&
                        record->parent_display_type == NULL && record->backend_name != NULL);
            ++type_count;
        }
    }
    THROW_CHECK(type_count >= sizeof names / sizeof *names);
    feng_debug_artifact_dispose(&loaded);
    free(type_info.variables[0].backend_name);
    type_info.variables[0].backend_name = strdup("void *");
    THROW_CHECK(type_info.variables[0].backend_name != NULL);
    THROW_CHECK(feng_debug_write_fd(dependency, binary, NULL, 0U, &type_info, &message));
    THROW_CHECK(!feng_debug_write_merged_fd(merged, binary, &mapping, 1U,
        &output.debug_info, dependencies, 1U, &message));
    THROW_CHECK(message != NULL && strstr(message, "conflicting type") != NULL);
    type_info.variables[0].read_expr = strdup("invalid type expression");
    THROW_CHECK(type_info.variables[0].read_expr != NULL);
    THROW_CHECK(feng_debug_write_fd(dependency, binary, NULL, 0U, &type_info, &message));
    THROW_CHECK(!feng_debug_read_fd(dependency, &loaded, &message));
    THROW_CHECK(message != NULL && strstr(message, "invalid debug type record") != NULL);
    feng_debug_artifact_dispose(&loaded);
    free(message);
    feng_codegen_maping_info_dispose(&type_info);
    THROW_CHECK(unlink(binary) == 0 && unlink(dependency) == 0 && unlink(merged) == 0 && rmdir(root) == 0);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
}

/* Release emission must not create debugger-only entities or change C output. */
static void dap_array_debug_disabled(const FengSemanticAnalysis *analysis) {
    FengCodegenOutput plain = {0}, disabled = {0};
    FengCodegenOptions options = {0};
    FengCodegenError error = {0};
    THROW_CHECK(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &plain, &error));
    THROW_CHECK(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, &options, &disabled, &error));
    THROW_CHECK(plain.debug_info.variable_count == 0U && disabled.debug_info.variable_count == 0U);
    THROW_CHECK(strcmp(plain.c_source, disabled.c_source) == 0);
    feng_codegen_output_free(&plain);
    feng_codegen_output_free(&disabled);
    feng_codegen_error_free(&error);
}

/* Concrete nominal mappings preserve inline array element size and identity. */
void test_dap_array_element_types_codegen(void) {
    const char *source =
        "open module slot.layout;"
        "@value open type Empty{}"
        "open type Ref{let value:int;}"
        "@value open type Pair{let first:Ref;let second:Ref;}"
        "open enum Status{First=3,Second=7}"
        "open func status():Status{return Status.Second;}"
        "open spec Readable{func read():i32;}"
        "open spec Getter():i32;"
        "open spec Small:i8|u8;"
        "open spec Maybe<T>:Empty|T;"
        "open func small(value:i8):Maybe<i8>{return value;}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    union_slot_debug_types(unit.analysis);
    dap_array_debug_disabled(unit.analysis);
    throw_constraint_dispose(&unit);
}
