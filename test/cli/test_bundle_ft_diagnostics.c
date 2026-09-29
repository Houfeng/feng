#include "../throw_constraint_helpers.h"
#include "archive/zip.h"
#include "cli/compile/driver.h"
#include "platform/platform.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/ft_internal.h"

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* A real public FT isolates driver diagnostics from unrelated format changes. */
static unsigned char *bundle_diagnostic_fixture(const char *path, size_t *out_size) {
    ThrowConstraintUnit unit = throw_constraint_analyze(
        "open module diagnostic.lifetime;\n"
        "open func value(): int { return 7; }\n", NULL, NULL);
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    FILE *file;
    long length;
    unsigned char *bytes;

    THROW_CHECK(feng_symbol_build_graph(unit.analysis, &graph, &error));
    THROW_CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U),
        FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, path, &error));
    feng_symbol_error_free(&error);
    feng_symbol_graph_free(graph);
    throw_constraint_dispose(&unit);

    file = fopen(path, "rb");
    THROW_CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    THROW_CHECK(length > FENG_SYMBOL_FT_HEADER_SIZE);
    THROW_CHECK(fseek(file, 0, SEEK_SET) == 0);
    bytes = malloc((size_t)length);
    THROW_CHECK(bytes != NULL);
    THROW_CHECK(fread(bytes, 1U, (size_t)length, file) == (size_t)length);
    THROW_CHECK(fclose(file) == 0);
    *out_size = (size_t)length;
    return bytes;
}

/* Keep the ZIP valid so the driver reaches its FT reader failure branch. */
static void bundle_diagnostic_write(const char *path, const unsigned char *bytes,
                                    size_t length) {
    FengZipWriter writer = {0};
    char *error = NULL;

    THROW_CHECK(feng_zip_writer_open(path, &writer, &error));
    THROW_CHECK(feng_zip_writer_add_bytes(&writer, "mod/diagnostic/lifetime.ft",
        bytes, length, FENG_ZIP_COMPRESSION_STORE, &error));
    THROW_CHECK(feng_zip_writer_finalize(&writer, &error));
    feng_zip_writer_dispose(&writer);
    free(error);
}

/* Capture the public driver diagnostic without invoking an external compiler. */
static char *bundle_diagnostic_invoke(const char *bundle, const char *platform,
                                      const char *diagnostic_path) {
    const char *bundles[] = {bundle};
    FengCliDriverOptions options = {
        .program_path = "build/bin/feng",
        .platform = platform,
        .target = FENG_COMPILE_TARGET_BIN,
        .c_path = "temp/unused-bundle-diagnostic.c",
        .out_path = "temp/unused-bundle-diagnostic",
        .bundle_paths = bundles,
        .bundle_count = 1U
    };
    FILE *file = fopen(diagnostic_path, "w+");
    int saved_stderr;
    int status;
    long length;
    char *message;

    THROW_CHECK(file != NULL && fflush(stderr) == 0);
    saved_stderr = dup(STDERR_FILENO);
    THROW_CHECK(saved_stderr >= 0);
    THROW_CHECK(dup2(fileno(file), STDERR_FILENO) >= 0);
    status = feng_cli_compile_driver_invoke(&options);
    THROW_CHECK(fflush(stderr) == 0);
    THROW_CHECK(dup2(saved_stderr, STDERR_FILENO) >= 0);
    THROW_CHECK(close(saved_stderr) == 0);
    THROW_CHECK(status == 1 && fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    THROW_CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    message = malloc((size_t)length + 1U);
    THROW_CHECK(message != NULL);
    THROW_CHECK(fread(message, 1U, (size_t)length, file) == (size_t)length);
    message[length] = '\0';
    THROW_CHECK(fclose(file) == 0);
    return message;
}

/* Preserve package/entry labels and reader reasons across repeated failures. */
void test_bundle_ft_diagnostic_lifetime(void) {
    const char *const reasons[] = {
        "symbol table file is too small",
        "symbol table magic mismatch",
        "symbol table payload fingerprint mismatch",
        "does not contain a target static library under lib/"
    };
    const char *const names[] = {"dependency", "dependency with spaces"};
    char directory[] = "temp/bundle-ft-diagnostic-XXXXXX";
    char fixture_path[512];
    char bundle_path[512];
    char diagnostic_path[512];
    char *platform = NULL;
    unsigned char *original;
    unsigned char *bytes;
    size_t length;
    int written;

    THROW_CHECK(mkdir("temp", 0755) == 0 || errno == EEXIST);
    THROW_CHECK(mkdtemp(directory) != NULL);
    written = snprintf(fixture_path, sizeof(fixture_path), "%s/input.ft", directory);
    THROW_CHECK(written > 0 && (size_t)written < sizeof(fixture_path));
    written = snprintf(diagnostic_path, sizeof(diagnostic_path), "%s/errors.txt", directory);
    THROW_CHECK(written > 0 && (size_t)written < sizeof(diagnostic_path));
    original = bundle_diagnostic_fixture(fixture_path, &length);
    bytes = malloc(length);
    THROW_CHECK(bytes != NULL);
    THROW_CHECK(feng_platform_detect_host_platform(&platform, NULL));

    for (size_t path_kind = 0U; path_kind < sizeof(names) / sizeof(names[0]); ++path_kind) {
        written = snprintf(bundle_path, sizeof(bundle_path), "%s/%s.fb", directory,
                           names[path_kind]);
        THROW_CHECK(written > 0 && (size_t)written < sizeof(bundle_path));
        for (size_t variant = 0U; variant < sizeof(reasons) / sizeof(reasons[0]); ++variant) {
            size_t fixture_size = length;
            char *message;

            memcpy(bytes, original, length);
            if (variant == 0U) {
                fixture_size = FENG_SYMBOL_FT_HEADER_SIZE - 1U;
            } else if (variant == 1U) {
                bytes[0] ^= 0xFFU;
            } else if (variant == 2U) {
                bytes[length - 1U] ^= 0x01U;
            }
            /* The unmodified FT reaches the separate missing-library error,
             * proving the fixture is otherwise accepted without running Clang. */
            bundle_diagnostic_write(bundle_path, bytes, fixture_size);
            message = bundle_diagnostic_invoke(bundle_path, platform, diagnostic_path);
            THROW_CHECK(strstr(message, "error: failed to prepare package libraries: ") != NULL);
            THROW_CHECK(strstr(message, bundle_path) != NULL);
            THROW_CHECK(strstr(message, reasons[variant]) != NULL);
            if (variant < 3U) {
                THROW_CHECK(strstr(message, "failed to read symbol table ") != NULL);
                THROW_CHECK(strstr(message, ":mod/diagnostic/lifetime.ft: ") != NULL);
            } else {
                THROW_CHECK(strstr(message, "failed to read symbol table ") == NULL);
            }
            free(message);
        }
        THROW_CHECK(unlink(bundle_path) == 0);
    }

    free(platform);
    free(bytes);
    free(original);
    THROW_CHECK(unlink(fixture_path) == 0);
    THROW_CHECK(unlink(diagnostic_path) == 0);
    THROW_CHECK(rmdir(directory) == 0);
    puts("bundle FT diagnostics: 8 cases preserve paths, reasons and owned storage");
}
