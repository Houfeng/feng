#include "codegen/codegen.h"
#include "debug/debug.h"
#include "parser/parser.h"
#include "semantic/semantic.h"

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
    "mod feng.debug.demo;\n"
    "fn main(args: string[]) {\n"
    "    let count = 42;\n"
    "    let plus = count + 1;\n"
    "}\n";

static const char *kCaptureSource =
    "mod feng.debug.capture;\n"
    "spec Mapper(x: int): int;\n"
    "fn use_it(): int {\n"
    "    var base: int = 1;\n"
    "    let mapper: Mapper = (x: int) -> x + base;\n"
    "    base = 2;\n"
    "    return mapper(40);\n"
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
    char *template_path = strdup("/tmp/feng_debug_XXXXXX");
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
    ASSERT(feng_codegen_maping_info_add_variable(&info,
                                        "feng__demo__main",
                                        "args",
                                        "args",
                                        NULL,
                                        FENG_CODEGEN_MAPING_VARIABLE_PARAM));
    ASSERT(feng_codegen_maping_info_add_variable(&info,
                                        "feng__demo__main",
                                        NULL,
                                        "captured",
                                        "(_lambda->capture->value)",
                                        FENG_CODEGEN_MAPING_VARIABLE_CAPTURE));

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
    ASSERT(artifact.info.variable_count == 2U);
    ASSERT(find_frame(&artifact.info, "main", FENG_CODEGEN_MAPING_FRAME_VISIBLE) != NULL);
    ASSERT(find_variable(&artifact.info,
                         "captured",
                         FENG_CODEGEN_MAPING_VARIABLE_CAPTURE,
                         "capture->value") != NULL);

    free(fingerprint_error);
    free(fd_error);
    feng_debug_artifact_dispose(&artifact);
    feng_codegen_maping_info_dispose(&info);
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
    test_resolve_source_builds_logical_uri();
    test_debug_fd_round_trip();
    test_codegen_emits_line_directives_and_debug_info();
    test_codegen_records_capture_mappings();
    return 0;
}
