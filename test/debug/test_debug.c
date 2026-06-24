#include "codegen/codegen.h"
#include "debug/debug.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ASSERT(expr)                                                                  \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static const char *kLineSource =
    "module feng.debug.demo;\n"
    "func main(args: string[]) {\n"
    "    let count = 42;\n"
    "    let plus = count + 1;\n"
    "}\n";

static const char *kCaptureSource =
    "module feng.debug.capture;\n"
    "spec Mapper(x: i32): i32;\n"
    "func use_it(): i32 {\n"
    "    var base: i32 = 1;\n"
    "    let mapper: Mapper = (x: i32) -> x + base;\n"
    "    base = 2;\n"
    "    return mapper(40);\n"
    "}\n";

static const char *kFieldSource =
    "module feng.debug.fields;\n"
    "type Point {\n"
    "    let x: i32;\n"
    "    let label: string;\n"
    "}\n"
    "func main(args: string[]) {\n"
    "    let point: Point = Point{x: 7, label: \"p\"};\n"
    "}\n";

static FengProgram *parse_or_die(const char *source, const char *path) {
    FengProgram *program = NULL;
    FengParseError error = {0};

    if (!feng_parse_source(source, strlen(source), path, &program, &error)) {
        fprintf(stderr,
                "%s:%u:%u: parse error: %s\n",
                path,
                error.token.line,
                error.token.column,
                error.message);
        exit(1);
    }
    return program;
}

static FengSemanticAnalysis *analyze_single_or_die(FengProgram *program,
                                                   FengCompileTarget target) {
    const FengProgram *programs[1] = {program};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;

    ASSERT(feng_semantic_analyze(programs,
                                 1U,
                                 target,
                                 &analysis,
                                 &errors,
                                 &error_count));
    if (error_count != 0U) {
        for (size_t i = 0U; i < error_count; ++i) {
            fprintf(stderr,
                    "%s:%u:%u: semantic error: %s\n",
                    errors[i].path,
                    errors[i].token.line,
                    errors[i].token.column,
                    errors[i].message);
        }
        ASSERT(error_count == 0U);
    }
    free(errors);
    return analysis;
}

static char *make_temp_dir(void) {
    char *template_path = strdup("temp/feng_debug_XXXXXX");
    char *result;

    ASSERT(template_path != NULL);
    result = mkdtemp(template_path);
    ASSERT(result != NULL);
    return result;
}

static int remove_dir_recursive(const char *path) {
    char command[1024];
    int written = snprintf(command, sizeof(command), "rm -rf '%s'", path);

    if (written < 0 || (size_t)written >= sizeof(command)) {
        return -1;
    }
    return system(command);
}

static void write_text_file_or_die(const char *path, const char *text) {
    FILE *file = fopen(path, "wb");
    size_t length = strlen(text);

    ASSERT(file != NULL);
    ASSERT(fwrite(text, 1U, length, file) == length);
    ASSERT(fclose(file) == 0);
}

static void write_binary_file_or_die(const char *path,
                                     const unsigned char *bytes,
                                     size_t length) {
    FILE *file = fopen(path, "wb");

    ASSERT(file != NULL);
    ASSERT(fwrite(bytes, 1U, length, file) == length);
    ASSERT(fclose(file) == 0);
}

static unsigned char *read_binary_file_or_die(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    long length;
    unsigned char *bytes;
    size_t read_size;

    ASSERT(file != NULL);
    ASSERT(fseek(file, 0L, SEEK_END) == 0);
    length = ftell(file);
    ASSERT(length >= 0L);
    ASSERT(fseek(file, 0L, SEEK_SET) == 0);
    bytes = (unsigned char *)malloc((size_t)length);
    ASSERT(bytes != NULL);
    read_size = fread(bytes, 1U, (size_t)length, file);
    ASSERT(read_size == (size_t)length);
    ASSERT(fclose(file) == 0);
    if (out_length != NULL) {
        *out_length = (size_t)length;
    }
    return bytes;
}

static void compile_generated_c_or_die(const char *c_source) {
    char *tmp_dir = make_temp_dir();
    char c_path[1024];
    char o_path[1024];
    char command[3072];

    ASSERT(snprintf(c_path, sizeof(c_path), "%s/generated.c", tmp_dir) > 0);
    ASSERT(snprintf(o_path, sizeof(o_path), "%s/generated.o", tmp_dir) > 0);
    write_text_file_or_die(c_path, c_source);
    ASSERT(snprintf(command,
                    sizeof(command),
                    "cc -Isrc -Ithird_party/miniz -std=gnu11 -fexceptions -Werror -c '%s' -o '%s' >/dev/null 2>&1",
                    c_path,
                    o_path) > 0);
    if (system(command) != 0) {
        fprintf(stderr, "generated C failed to compile: %s\n", command);
        ASSERT(false);
    }
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static const FengCodegenMapingFrameRecord *find_frame(const FengCodegenMapingInfo *info,
                                              const char *display_name,
                                              FengCodegenMapingFramePolicy policy) {
    for (size_t i = 0U; i < info->frame_count; ++i) {
        if (strcmp(info->frames[i].display_name, display_name) == 0 &&
            info->frames[i].policy == policy) {
            return &info->frames[i];
        }
    }
    return NULL;
}

static const FengCodegenMapingVariableRecord *find_variable(const FengCodegenMapingInfo *info,
                                                    const char *display_name,
                                                    FengCodegenMapingVariableKind kind,
                                                    const char *read_expr_substr) {
    for (size_t i = 0U; i < info->variable_count; ++i) {
        if (strcmp(info->variables[i].display_name, display_name) != 0 ||
            info->variables[i].kind != kind) {
            continue;
        }
        if (read_expr_substr == NULL) {
            if (info->variables[i].read_expr == NULL) {
                return &info->variables[i];
            }
            continue;
        }
        if (info->variables[i].read_expr != NULL &&
            strstr(info->variables[i].read_expr, read_expr_substr) != NULL) {
            return &info->variables[i];
        }
    }
    return NULL;
}

static const FengCodegenMapingVariableRecord *find_field_entity(const FengCodegenMapingInfo *info,
                                                                const char *parent_display_type,
                                                                const char *display_name,
                                                                const char *read_expr_substr) {
    for (size_t i = 0U; i < info->variable_count; ++i) {
        if (info->variables[i].kind != FENG_CODEGEN_MAPING_VARIABLE_FIELD ||
            info->variables[i].parent_display_type == NULL ||
            strcmp(info->variables[i].parent_display_type, parent_display_type) != 0 ||
            strcmp(info->variables[i].display_name, display_name) != 0) {
            continue;
        }
        if (read_expr_substr == NULL) {
            return &info->variables[i];
        }
        if (info->variables[i].read_expr != NULL &&
            strstr(info->variables[i].read_expr, read_expr_substr) != NULL) {
            return &info->variables[i];
        }
    }
    return NULL;
}

static const char *find_package_root(const FengDebugArtifact *artifact,
                                     const char *package_name) {
    for (size_t i = 0U; i < artifact->package_count; ++i) {
        if (strcmp(artifact->packages[i].package_name, package_name) == 0) {
            return artifact->packages[i].local_root_path;
        }
    }
    return NULL;
}

static void write_fd_or_die(const char *fd_path,
                            const char *binary_path,
                            const FengCodegenMapingSourceMapping *mappings,
                            size_t mapping_count,
                            const FengCodegenMapingInfo *info) {
    char *fd_error = NULL;

    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               mappings,
                               mapping_count,
                               info,
                               &fd_error));
    ASSERT(fd_error == NULL);
    free(fd_error);
}

static void test_resolve_source_builds_logical_uri(void) {
    FengCodegenMapingSourceMapping mappings[1] = {
        {
            .source_path = "/tmp/feng-debug/src/nested/main.ff",
            .package_name = "demo",
            .package_root = "/tmp/feng-debug/src",
        },
    };
    FengCodegenMapingResolvedSource resolved = {0};

    ASSERT(feng_codegen_maping_resolve_source(mappings,
                                     1U,
                                     "/tmp/feng-debug/src/nested/main.ff",
                                     &resolved));
    ASSERT(strcmp(resolved.package_name, "demo") == 0);
    ASSERT(strcmp(resolved.package_root, "/tmp/feng-debug/src") == 0);
    ASSERT(strcmp(resolved.relative_path, "nested/main.ff") == 0);
    ASSERT(strcmp(resolved.logical_uri, "demo://nested/main.ff") == 0);
    feng_codegen_maping_resolved_source_dispose(&resolved);
}

static void test_codegen_emits_line_directives_and_debug_info(void) {
    const char *path = "/tmp/feng-debug-demo/src/main.ff";
    FengCodegenMapingSourceMapping mappings[1] = {
        {
            .source_path = path,
            .package_name = "demo",
            .package_root = "/tmp/feng-debug-demo/src",
        },
    };
    FengCodegenOptions options = {
        .emit_line_directives = true,
        .debug_source_mappings = mappings,
        .debug_source_mapping_count = 1U,
    };
    FengProgram *program = parse_or_die(kLineSource, path);
    FengSemanticAnalysis *analysis = analyze_single_or_die(program, FENG_COMPILE_TARGET_BIN);
    FengCodegenOutput out = {0};
    FengCodegenError error = {0};
    const FengCodegenMapingFrameRecord *main_frame;
    const FengCodegenMapingFrameRecord *wrapper_frame;

    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_BIN,
                                     &options,
                                     &out,
                                     &error));
    ASSERT(out.c_source != NULL);
    ASSERT(strstr(out.c_source, "#line 3 \"demo://main.ff\"") != NULL);
    ASSERT(strstr(out.c_source, "#line 4 \"demo://main.ff\"") != NULL);
    compile_generated_c_or_die(out.c_source);

    main_frame = find_frame(&out.debug_info, "main", FENG_CODEGEN_MAPING_FRAME_VISIBLE);
    wrapper_frame = find_frame(&out.debug_info, "main", FENG_CODEGEN_MAPING_FRAME_HIDDEN);
    ASSERT(main_frame != NULL);
    ASSERT(wrapper_frame != NULL);
    ASSERT(strcmp(wrapper_frame->backend_symbol, "main") == 0);
    ASSERT(find_variable(&out.debug_info, "args", FENG_CODEGEN_MAPING_VARIABLE_PARAM, NULL) != NULL);
    ASSERT(find_variable(&out.debug_info, "count", FENG_CODEGEN_MAPING_VARIABLE_BINDING, NULL) != NULL);
    ASSERT(find_variable(&out.debug_info, "plus", FENG_CODEGEN_MAPING_VARIABLE_BINDING, NULL) != NULL);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_codegen_records_user_type_field_entities(void) {
    const char *path = "/tmp/feng-debug-fields/src/main.ff";
    FengCodegenMapingSourceMapping mappings[1] = {
        {
            .source_path = path,
            .package_name = "demo",
            .package_root = "/tmp/feng-debug-fields/src",
        },
    };
    FengCodegenOptions options = {
        .emit_line_directives = true,
        .debug_source_mappings = mappings,
        .debug_source_mapping_count = 1U,
    };
    FengProgram *program = parse_or_die(kFieldSource, path);
    FengSemanticAnalysis *analysis = analyze_single_or_die(program, FENG_COMPILE_TARGET_BIN);
    FengCodegenOutput out = {0};
    FengCodegenError error = {0};
    const FengCodegenMapingVariableRecord *point;
    const FengCodegenMapingVariableRecord *x_field;
    const FengCodegenMapingVariableRecord *label_field;

    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_BIN,
                                     &options,
                                     &out,
                                     &error));
    point = find_variable(&out.debug_info,
                          "point",
                          FENG_CODEGEN_MAPING_VARIABLE_BINDING,
                          NULL);
    x_field = find_field_entity(&out.debug_info, "feng.debug.fields.Point", "x", "->x");
    label_field = find_field_entity(&out.debug_info, "feng.debug.fields.Point", "label", "->label");
    ASSERT(point != NULL);
    ASSERT(point->display_type != NULL && strcmp(point->display_type, "feng.debug.fields.Point") == 0);
    ASSERT(x_field != NULL);
    ASSERT(x_field->frame_backend_symbol == NULL);
    ASSERT(x_field->backend_name == NULL);
    ASSERT(x_field->display_type != NULL && strcmp(x_field->display_type, "i32") == 0);
    ASSERT(label_field != NULL);
    ASSERT(label_field->display_type != NULL && strcmp(label_field->display_type, "string") == 0);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

static void test_debug_fd_round_trip(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x01U, 0x02U};
    char *tmp_dir = make_temp_dir();
    char binary_path[1024];
    char fd_path[1024];
    char *fingerprint_error = NULL;
    char *fd_error = NULL;
    uint64_t fingerprint;
    FengCodegenMapingSourceMapping mappings[2] = {
        {
            .source_path = "/tmp/demo/src/main.ff",
            .package_name = "demo",
            .package_root = "/tmp/demo/src",
        },
        {
            .source_path = "/tmp/dep/src/lib.ff",
            .package_name = "dep",
            .package_root = "/tmp/dep/src",
        },
    };
    FengCodegenMapingInfo info = {0};
    FengDebugArtifact artifact = {0};

    ASSERT(snprintf(binary_path, sizeof(binary_path), "%s/demo.bin", tmp_dir) > 0);
    ASSERT(snprintf(fd_path, sizeof(fd_path), "%s/demo.bin.fd", tmp_dir) > 0);
    write_binary_file_or_die(binary_path, kBinaryBytes, sizeof(kBinaryBytes));

    feng_codegen_maping_info_init(&info);
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                     "feng__demo__main",
                                     "main",
                                     FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                     "main",
                                     "main",
                                     FENG_CODEGEN_MAPING_FRAME_HIDDEN));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                           "feng__demo__main",
                                                           "args",
                                                           "args",
                                                           NULL,
                                                           "string[]",
                                                           FENG_CODEGEN_MAPING_VARIABLE_PARAM));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                           "feng__demo__main",
                                                           NULL,
                                                           "captured",
                                                           "(_lambda->capture->value)",
                                                           "i32",
                                                           FENG_CODEGEN_MAPING_VARIABLE_CAPTURE));
    ASSERT(feng_codegen_maping_info_add_variable_with_parent_display_type(
        &info,
        NULL,
        NULL,
        "age",
        ".age",
        "i64",
        "Person",
        FENG_CODEGEN_MAPING_VARIABLE_FIELD));

    ASSERT(feng_debug_write_fd(fd_path,
                               binary_path,
                               mappings,
                               2U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);
    ASSERT(feng_debug_read_fd(fd_path, &artifact, &fd_error));
    ASSERT(fd_error == NULL);

    fingerprint = feng_debug_fnv1a64_file(binary_path, &fingerprint_error);
    ASSERT(fingerprint_error == NULL);
    ASSERT(strcmp(artifact.binary_path, binary_path) == 0);
    ASSERT(artifact.binary_fingerprint == fingerprint);
    ASSERT(artifact.package_count == 2U);
    ASSERT(artifact.info.frame_count == 2U);
    ASSERT(artifact.info.variable_count == 3U);
    ASSERT(find_frame(&artifact.info, "main", FENG_CODEGEN_MAPING_FRAME_VISIBLE) != NULL);
    ASSERT(find_variable(&artifact.info,
                         "captured",
                         FENG_CODEGEN_MAPING_VARIABLE_CAPTURE,
                         "capture->value") != NULL);
    ASSERT(find_field_entity(&artifact.info, "Person", "age", ".age") != NULL);

    free(fingerprint_error);
    free(fd_error);
    feng_debug_artifact_dispose(&artifact);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static void test_debug_fd_matches_golden_bytes(void) {
    static const unsigned char kBinaryBytes[] = {0x7fU, 'F', 'E', 'N', 'G', 0x01U, 0x02U};
    static const unsigned char kExpectedFdBytes[] = {
        0x46, 0x44, 0x30, 0x31, 0x02, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x4d, 0x45, 0x54, 0x41, 0x00, 0x00, 0x00, 0x00,
        0xb0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x53, 0x54, 0x52, 0x53, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x4b, 0x47, 0x53,
        0x00, 0x00, 0x00, 0x00, 0x76, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x46, 0x52, 0x4d, 0x53, 0x00, 0x00, 0x00, 0x00,
        0x86, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x45, 0x4e, 0x54, 0x53, 0x00, 0x00, 0x00, 0x00, 0x9e, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xa7, 0x2a, 0xbe, 0xba, 0x24, 0x66, 0x1f, 0x31,
        0x22, 0x00, 0x00, 0x00, 0x74, 0x65, 0x6d, 0x70, 0x2f, 0x66, 0x65, 0x6e,
        0x67, 0x5f, 0x64, 0x65, 0x62, 0x75, 0x67, 0x5f, 0x66, 0x64, 0x5f, 0x67,
        0x6f, 0x6c, 0x64, 0x65, 0x6e, 0x2f, 0x64, 0x65, 0x6d, 0x6f, 0x2e, 0x62,
        0x69, 0x6e, 0x04, 0x00, 0x00, 0x00, 0x64, 0x65, 0x6d, 0x6f, 0x0d, 0x00,
        0x00, 0x00, 0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x64, 0x65, 0x6d, 0x6f, 0x2f,
        0x73, 0x72, 0x63, 0x03, 0x00, 0x00, 0x00, 0x64, 0x65, 0x70, 0x0c, 0x00,
        0x00, 0x00, 0x2f, 0x74, 0x6d, 0x70, 0x2f, 0x64, 0x65, 0x70, 0x2f, 0x73,
        0x72, 0x63, 0x10, 0x00, 0x00, 0x00, 0x66, 0x65, 0x6e, 0x67, 0x5f, 0x5f,
        0x64, 0x65, 0x6d, 0x6f, 0x5f, 0x5f, 0x6d, 0x61, 0x69, 0x6e, 0x04, 0x00,
        0x00, 0x00, 0x6d, 0x61, 0x69, 0x6e, 0x04, 0x00, 0x00, 0x00, 0x61, 0x72,
        0x67, 0x73, 0x08, 0x00, 0x00, 0x00, 0x73, 0x74, 0x72, 0x69, 0x6e, 0x67,
        0x5b, 0x5d, 0x08, 0x00, 0x00, 0x00, 0x63, 0x61, 0x70, 0x74, 0x75, 0x72,
        0x65, 0x64, 0x19, 0x00, 0x00, 0x00, 0x28, 0x5f, 0x6c, 0x61, 0x6d, 0x62,
        0x64, 0x61, 0x2d, 0x3e, 0x63, 0x61, 0x70, 0x74, 0x75, 0x72, 0x65, 0x2d,
        0x3e, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x29, 0x03, 0x00, 0x00, 0x00, 0x69,
        0x33, 0x32, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x07, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x08, 0x00,
        0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x0b, 0x00,
        0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    static const char *kGoldenDir = "temp/feng_debug_fd_golden";
    static const char *kBinaryPath = "temp/feng_debug_fd_golden/demo.bin";
    static const char *kFdPath = "temp/feng_debug_fd_golden/demo.bin.fd";
    FengCodegenMapingSourceMapping mappings[2] = {
        {
            .source_path = "/tmp/demo/src/main.ff",
            .package_name = "demo",
            .package_root = "/tmp/demo/src",
        },
        {
            .source_path = "/tmp/dep/src/lib.ff",
            .package_name = "dep",
            .package_root = "/tmp/dep/src",
        },
    };
    FengCodegenMapingInfo info = {0};
    unsigned char *actual_bytes;
    size_t actual_length = 0U;
    char *fd_error = NULL;

    ASSERT(remove_dir_recursive(kGoldenDir) == 0);
    ASSERT(mkdir(kGoldenDir, 0775) == 0);
    write_binary_file_or_die(kBinaryPath, kBinaryBytes, sizeof(kBinaryBytes));

    feng_codegen_maping_info_init(&info);
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "feng__demo__main",
                                              "main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_frame(&info,
                                              "main",
                                              "main",
                                              FENG_CODEGEN_MAPING_FRAME_HIDDEN));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                           "feng__demo__main",
                                                           "args",
                                                           "args",
                                                           NULL,
                                                           "string[]",
                                                           FENG_CODEGEN_MAPING_VARIABLE_PARAM));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&info,
                                                           "feng__demo__main",
                                                           NULL,
                                                           "captured",
                                                           "(_lambda->capture->value)",
                                                           "i32",
                                                           FENG_CODEGEN_MAPING_VARIABLE_CAPTURE));
    ASSERT(feng_debug_write_fd(kFdPath,
                               kBinaryPath,
                               mappings,
                               2U,
                               &info,
                               &fd_error));
    ASSERT(fd_error == NULL);

    actual_bytes = read_binary_file_or_die(kFdPath, &actual_length);
    ASSERT(actual_length == sizeof(kExpectedFdBytes));
    ASSERT(memcmp(actual_bytes, kExpectedFdBytes, sizeof(kExpectedFdBytes)) == 0);

    free(actual_bytes);
    free(fd_error);
    feng_codegen_maping_info_dispose(&info);
    ASSERT(remove_dir_recursive(kGoldenDir) == 0);
}

static void test_debug_fd_merge_dependencies(void) {
    static const unsigned char kRootBytes[] = {0x01U, 0x02U, 0x03U, 0x04U};
    static const unsigned char kDepBytes[] = {0x11U, 0x12U, 0x13U, 0x14U};
    char *tmp_dir = make_temp_dir();
    char root_binary_path[1024];
    char root_fd_path[1024];
    char dep_one_binary_path[1024];
    char dep_one_fd_path[1024];
    char dep_two_binary_path[1024];
    char dep_two_fd_path[1024];
    const char *dependency_fd_paths[2];
    char *merge_error = NULL;
    char *fingerprint_error = NULL;
    uint64_t fingerprint;
    FengCodegenMapingSourceMapping root_mappings[1] = {
        {
            .source_path = "/tmp/app/src/main.ff",
            .package_name = "app",
            .package_root = "/tmp/app/src",
        },
    };
    FengCodegenMapingSourceMapping dep_one_mappings[1] = {
        {
            .source_path = "/tmp/std/src/io.ff",
            .package_name = "std",
            .package_root = "/tmp/std/src",
        },
    };
    FengCodegenMapingSourceMapping dep_two_mappings[1] = {
        {
            .source_path = "/tmp/util/src/help.ff",
            .package_name = "util",
            .package_root = "/tmp/util/src",
        },
    };
    FengCodegenMapingInfo root_info = {0};
    FengCodegenMapingInfo dep_one_info = {0};
    FengCodegenMapingInfo dep_two_info = {0};
    FengDebugArtifact artifact = {0};

    ASSERT(snprintf(root_binary_path, sizeof(root_binary_path), "%s/app.bin", tmp_dir) > 0);
    ASSERT(snprintf(root_fd_path, sizeof(root_fd_path), "%s/app.bin.fd", tmp_dir) > 0);
    ASSERT(snprintf(dep_one_binary_path, sizeof(dep_one_binary_path), "%s/std.a", tmp_dir) > 0);
    ASSERT(snprintf(dep_one_fd_path, sizeof(dep_one_fd_path), "%s/std.a.fd", tmp_dir) > 0);
    ASSERT(snprintf(dep_two_binary_path, sizeof(dep_two_binary_path), "%s/util.a", tmp_dir) > 0);
    ASSERT(snprintf(dep_two_fd_path, sizeof(dep_two_fd_path), "%s/util.a.fd", tmp_dir) > 0);

    write_binary_file_or_die(root_binary_path, kRootBytes, sizeof(kRootBytes));
    write_binary_file_or_die(dep_one_binary_path, kDepBytes, sizeof(kDepBytes));
    write_binary_file_or_die(dep_two_binary_path, kDepBytes, sizeof(kDepBytes));

    feng_codegen_maping_info_init(&root_info);
    feng_codegen_maping_info_init(&dep_one_info);
    feng_codegen_maping_info_init(&dep_two_info);

    ASSERT(feng_codegen_maping_info_add_frame(&root_info,
                                              "feng__app__main",
                                              "main",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&root_info,
                                                           "feng__app__main",
                                                           "args",
                                                           "args",
                                                           NULL,
                                                           "string[]",
                                                           FENG_CODEGEN_MAPING_VARIABLE_PARAM));

    ASSERT(feng_codegen_maping_info_add_frame(&dep_one_info,
                                              "feng__std__io__print",
                                              "std.io.print",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&dep_one_info,
                                                           "feng__std__io__print",
                                                           "text",
                                                           "text",
                                                           NULL,
                                                           "string",
                                                           FENG_CODEGEN_MAPING_VARIABLE_PARAM));

    ASSERT(feng_codegen_maping_info_add_frame(&dep_two_info,
                                              "feng__util__help",
                                              "util.help",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&dep_two_info,
                                                           "feng__util__help",
                                                           "value",
                                                           "value",
                                                           NULL,
                                                           "i32",
                                                           FENG_CODEGEN_MAPING_VARIABLE_PARAM));

    write_fd_or_die(dep_one_fd_path,
                    dep_one_binary_path,
                    dep_one_mappings,
                    1U,
                    &dep_one_info);
    write_fd_or_die(dep_two_fd_path,
                    dep_two_binary_path,
                    dep_two_mappings,
                    1U,
                    &dep_two_info);

    dependency_fd_paths[0] = dep_one_fd_path;
    dependency_fd_paths[1] = dep_two_fd_path;
    ASSERT(feng_debug_write_merged_fd(root_fd_path,
                                      root_binary_path,
                                      root_mappings,
                                      1U,
                                      &root_info,
                                      dependency_fd_paths,
                                      2U,
                                      &merge_error));
    ASSERT(merge_error == NULL);
    ASSERT(feng_debug_read_fd(root_fd_path, &artifact, &merge_error));
    ASSERT(merge_error == NULL);

    fingerprint = feng_debug_fnv1a64_file(root_binary_path, &fingerprint_error);
    ASSERT(fingerprint_error == NULL);
    ASSERT(strcmp(artifact.binary_path, root_binary_path) == 0);
    ASSERT(artifact.binary_fingerprint == fingerprint);
    ASSERT(artifact.package_count == 3U);
    ASSERT(strcmp(find_package_root(&artifact, "app"), "/tmp/app/src") == 0);
    ASSERT(strcmp(find_package_root(&artifact, "std"), "/tmp/std/src") == 0);
    ASSERT(strcmp(find_package_root(&artifact, "util"), "/tmp/util/src") == 0);
    ASSERT(find_frame(&artifact.info, "main", FENG_CODEGEN_MAPING_FRAME_VISIBLE) != NULL);
    ASSERT(find_frame(&artifact.info, "std.io.print", FENG_CODEGEN_MAPING_FRAME_VISIBLE) != NULL);
    ASSERT(find_frame(&artifact.info, "util.help", FENG_CODEGEN_MAPING_FRAME_VISIBLE) != NULL);
    ASSERT(find_variable(&artifact.info, "args", FENG_CODEGEN_MAPING_VARIABLE_PARAM, NULL) != NULL);
    ASSERT(find_variable(&artifact.info, "text", FENG_CODEGEN_MAPING_VARIABLE_PARAM, NULL) != NULL);
    ASSERT(find_variable(&artifact.info, "value", FENG_CODEGEN_MAPING_VARIABLE_PARAM, NULL) != NULL);

    free(fingerprint_error);
    free(merge_error);
    feng_debug_artifact_dispose(&artifact);
    feng_codegen_maping_info_dispose(&root_info);
    feng_codegen_maping_info_dispose(&dep_one_info);
    feng_codegen_maping_info_dispose(&dep_two_info);
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static void test_debug_fd_merge_rejects_conflicting_package_roots(void) {
    static const unsigned char kBinaryBytes[] = {0x21U, 0x22U, 0x23U, 0x24U};
    char *tmp_dir = make_temp_dir();
    char root_binary_path[1024];
    char root_fd_path[1024];
    char dep_one_binary_path[1024];
    char dep_one_fd_path[1024];
    char dep_two_binary_path[1024];
    char dep_two_fd_path[1024];
    const char *dependency_fd_paths[2];
    char *merge_error = NULL;
    FengCodegenMapingSourceMapping root_mappings[1] = {
        {
            .source_path = "/tmp/app/src/main.ff",
            .package_name = "app",
            .package_root = "/tmp/app/src",
        },
    };
    FengCodegenMapingSourceMapping dep_one_mappings[1] = {
        {
            .source_path = "/tmp/std-a/src/io.ff",
            .package_name = "std",
            .package_root = "/tmp/std-a/src",
        },
    };
    FengCodegenMapingSourceMapping dep_two_mappings[1] = {
        {
            .source_path = "/tmp/std-b/src/io.ff",
            .package_name = "std",
            .package_root = "/tmp/std-b/src",
        },
    };
    FengCodegenMapingInfo root_info = {0};
    FengCodegenMapingInfo dep_one_info = {0};
    FengCodegenMapingInfo dep_two_info = {0};

    ASSERT(snprintf(root_binary_path, sizeof(root_binary_path), "%s/app.bin", tmp_dir) > 0);
    ASSERT(snprintf(root_fd_path, sizeof(root_fd_path), "%s/app.bin.fd", tmp_dir) > 0);
    ASSERT(snprintf(dep_one_binary_path, sizeof(dep_one_binary_path), "%s/std-a.a", tmp_dir) > 0);
    ASSERT(snprintf(dep_one_fd_path, sizeof(dep_one_fd_path), "%s/std-a.a.fd", tmp_dir) > 0);
    ASSERT(snprintf(dep_two_binary_path, sizeof(dep_two_binary_path), "%s/std-b.a", tmp_dir) > 0);
    ASSERT(snprintf(dep_two_fd_path, sizeof(dep_two_fd_path), "%s/std-b.a.fd", tmp_dir) > 0);

    write_binary_file_or_die(root_binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    write_binary_file_or_die(dep_one_binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    write_binary_file_or_die(dep_two_binary_path, kBinaryBytes, sizeof(kBinaryBytes));

    feng_codegen_maping_info_init(&root_info);
    feng_codegen_maping_info_init(&dep_one_info);
    feng_codegen_maping_info_init(&dep_two_info);

    ASSERT(feng_codegen_maping_info_add_frame(&dep_one_info,
                                              "feng__std__io",
                                              "std.io",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_frame(&dep_two_info,
                                              "feng__std__io",
                                              "std.io",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));

    write_fd_or_die(dep_one_fd_path,
                    dep_one_binary_path,
                    dep_one_mappings,
                    1U,
                    &dep_one_info);
    write_fd_or_die(dep_two_fd_path,
                    dep_two_binary_path,
                    dep_two_mappings,
                    1U,
                    &dep_two_info);

    dependency_fd_paths[0] = dep_one_fd_path;
    dependency_fd_paths[1] = dep_two_fd_path;
    ASSERT(!feng_debug_write_merged_fd(root_fd_path,
                                       root_binary_path,
                                       root_mappings,
                                       1U,
                                       &root_info,
                                       dependency_fd_paths,
                                       2U,
                                       &merge_error));
    ASSERT(merge_error != NULL);
    ASSERT(strstr(merge_error, "maps package 'std'") != NULL);
    ASSERT(access(root_fd_path, F_OK) != 0);

    free(merge_error);
    feng_codegen_maping_info_dispose(&root_info);
    feng_codegen_maping_info_dispose(&dep_one_info);
    feng_codegen_maping_info_dispose(&dep_two_info);
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static void test_debug_fd_merge_rejects_conflicting_variable_mappings(void) {
    static const unsigned char kBinaryBytes[] = {0x31U, 0x32U, 0x33U, 0x34U};
    char *tmp_dir = make_temp_dir();
    char root_binary_path[1024];
    char root_fd_path[1024];
    char dep_one_binary_path[1024];
    char dep_one_fd_path[1024];
    char dep_two_binary_path[1024];
    char dep_two_fd_path[1024];
    const char *dependency_fd_paths[2];
    char *merge_error = NULL;
    FengCodegenMapingSourceMapping root_mappings[1] = {
        {
            .source_path = "/tmp/app/src/main.ff",
            .package_name = "app",
            .package_root = "/tmp/app/src",
        },
    };
    FengCodegenMapingSourceMapping dep_mappings[1] = {
        {
            .source_path = "/tmp/dep/src/lib.ff",
            .package_name = "dep",
            .package_root = "/tmp/dep/src",
        },
    };
    FengCodegenMapingInfo root_info = {0};
    FengCodegenMapingInfo dep_one_info = {0};
    FengCodegenMapingInfo dep_two_info = {0};

    ASSERT(snprintf(root_binary_path, sizeof(root_binary_path), "%s/app.bin", tmp_dir) > 0);
    ASSERT(snprintf(root_fd_path, sizeof(root_fd_path), "%s/app.bin.fd", tmp_dir) > 0);
    ASSERT(snprintf(dep_one_binary_path, sizeof(dep_one_binary_path), "%s/dep-one.a", tmp_dir) > 0);
    ASSERT(snprintf(dep_one_fd_path, sizeof(dep_one_fd_path), "%s/dep-one.a.fd", tmp_dir) > 0);
    ASSERT(snprintf(dep_two_binary_path, sizeof(dep_two_binary_path), "%s/dep-two.a", tmp_dir) > 0);
    ASSERT(snprintf(dep_two_fd_path, sizeof(dep_two_fd_path), "%s/dep-two.a.fd", tmp_dir) > 0);

    write_binary_file_or_die(root_binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    write_binary_file_or_die(dep_one_binary_path, kBinaryBytes, sizeof(kBinaryBytes));
    write_binary_file_or_die(dep_two_binary_path, kBinaryBytes, sizeof(kBinaryBytes));

    feng_codegen_maping_info_init(&root_info);
    feng_codegen_maping_info_init(&dep_one_info);
    feng_codegen_maping_info_init(&dep_two_info);

    ASSERT(feng_codegen_maping_info_add_frame(&dep_one_info,
                                              "feng__dep__helper",
                                              "dep.helper",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_frame(&dep_two_info,
                                              "feng__dep__helper",
                                              "dep.helper",
                                              FENG_CODEGEN_MAPING_FRAME_VISIBLE));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&dep_one_info,
                                                           "feng__dep__helper",
                                                           "slot",
                                                           "value",
                                                           NULL,
                                                           "i32",
                                                           FENG_CODEGEN_MAPING_VARIABLE_BINDING));
    ASSERT(feng_codegen_maping_info_add_variable_with_display_type(&dep_two_info,
                                                           "feng__dep__helper",
                                                           "slot",
                                                           "other",
                                                           NULL,
                                                           "i32",
                                                           FENG_CODEGEN_MAPING_VARIABLE_BINDING));

    write_fd_or_die(dep_one_fd_path,
                    dep_one_binary_path,
                    dep_mappings,
                    1U,
                    &dep_one_info);
    write_fd_or_die(dep_two_fd_path,
                    dep_two_binary_path,
                    dep_mappings,
                    1U,
                    &dep_two_info);

    dependency_fd_paths[0] = dep_one_fd_path;
    dependency_fd_paths[1] = dep_two_fd_path;
    ASSERT(!feng_debug_write_merged_fd(root_fd_path,
                                       root_binary_path,
                                       root_mappings,
                                       1U,
                                       &root_info,
                                       dependency_fd_paths,
                                       2U,
                                       &merge_error));
    ASSERT(merge_error != NULL);
    ASSERT(strstr(merge_error, "remaps variable 'other'") != NULL ||
           strstr(merge_error, "remaps variable 'value'") != NULL);
    ASSERT(access(root_fd_path, F_OK) != 0);

    free(merge_error);
    feng_codegen_maping_info_dispose(&root_info);
    feng_codegen_maping_info_dispose(&dep_one_info);
    feng_codegen_maping_info_dispose(&dep_two_info);
    ASSERT(remove_dir_recursive(tmp_dir) == 0);
    free(tmp_dir);
}

static void test_codegen_records_capture_mappings(void) {
    const char *path = "/tmp/feng-debug-demo/src/capture.ff";
    FengCodegenMapingSourceMapping mappings[1] = {
        {
            .source_path = path,
            .package_name = "demo",
            .package_root = "/tmp/feng-debug-demo/src",
        },
    };
    FengCodegenOptions options = {
        .emit_line_directives = true,
        .debug_source_mappings = mappings,
        .debug_source_mapping_count = 1U,
    };
    FengProgram *program = parse_or_die(kCaptureSource, path);
    FengSemanticAnalysis *analysis = analyze_single_or_die(program, FENG_COMPILE_TARGET_LIB);
    FengCodegenOutput out = {0};
    FengCodegenError error = {0};
    bool saw_lambda_frame = false;

    ASSERT(feng_codegen_emit_program(analysis,
                                     FENG_COMPILE_TARGET_LIB,
                                     &options,
                                     &out,
                                     &error));
    ASSERT(out.c_source != NULL);
    compile_generated_c_or_die(out.c_source);

    ASSERT(find_variable(&out.debug_info,
                         "base",
                         FENG_CODEGEN_MAPING_VARIABLE_BINDING,
                         "->value") != NULL);
    ASSERT(find_variable(&out.debug_info,
                         "base",
                         FENG_CODEGEN_MAPING_VARIABLE_CAPTURE,
                         "_lambda->") != NULL);
    for (size_t i = 0U; i < out.debug_info.frame_count; ++i) {
        if (strncmp(out.debug_info.frames[i].display_name, "lambda@", 7U) == 0) {
            saw_lambda_frame = true;
            break;
        }
    }
    ASSERT(saw_lambda_frame);

    feng_codegen_output_free(&out);
    feng_codegen_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

int main(void) {
    (void)system("rm -rf temp");
    (void)mkdir("temp", 0755);

    test_resolve_source_builds_logical_uri();
    test_debug_fd_round_trip();
    test_debug_fd_matches_golden_bytes();
    test_debug_fd_merge_dependencies();
    test_debug_fd_merge_rejects_conflicting_package_roots();
    test_debug_fd_merge_rejects_conflicting_variable_mappings();
    test_codegen_emits_line_directives_and_debug_info();
    test_codegen_records_user_type_field_entities();
    test_codegen_records_capture_mappings();
    return 0;
}
