#include "cli/cli.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "archive/fb.h"
#include "cli/common.h"
#include "cli/deps/manager.h"
#include "cli/project/common.h"
#include "platform/platform.h"
#include "symbol/ft.h"

/* Parsed `feng pack` options. Platform pointers borrow argv storage. */
typedef struct PackOptions {
    const char *path;
    const char **platforms;
    size_t platform_count;
    const char *sysroot;
} PackOptions;

/* Owned recursive list of regular-file paths relative to one root. */
typedef struct PackFileList {
    char **paths;
    size_t count;
    size_t capacity;
} PackFileList;

/* Print command-specific pack usage. */
static void print_usage(const char *program, FILE *stream) {
    if (stream == stderr) fprintf(stream, "\n");
    fprintf(stream, "Usage:\n");
    fprintf(stream,
            "  %s pack [<path>] [--platform=<platform>]... "
            "[--sysroot=<path>]\n",
            program);
}

/* Format one owned diagnostic string. */
static char *dup_printf(const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *out;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return NULL;
    }
    out = (char *)malloc((size_t)needed + 1U);
    if (out == NULL) {
        va_end(args_copy);
        return NULL;
    }
    vsnprintf(out, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    return out;
}

/* Join two path components with one slash. */
static char *path_join(const char *left, const char *right) {
    return dup_printf("%s%s%s",
                      left,
                      left[0] != '\0' && left[strlen(left) - 1U] != '/' ? "/" : "",
                      right);
}

/* Return whether one path names an available directory. */
static bool dir_exists(const char *path) {
    struct stat status;

    return path != NULL && stat(path, &status) == 0 && S_ISDIR(status.st_mode);
}

/* Release arrays allocated by the pack option parser. */
static void pack_options_dispose(PackOptions *options) {
    if (options == NULL) {
        return;
    }
    free((void *)options->platforms);
    options->platforms = NULL;
    options->platform_count = 0U;
}

/* Parse one `feng pack` invocation without opening the project. */
static FengCliParseResult parse_args(const char *program,
                                     int argc,
                                     char **argv,
                                     PackOptions *out_options) {
    int index;

    memset(out_options, 0, sizeof(*out_options));
    out_options->platforms = argc > 0
        ? (const char **)calloc((size_t)argc, sizeof(*out_options->platforms))
        : NULL;
    if (argc > 0 && out_options->platforms == NULL) {
        fprintf(stderr, "out of memory parsing pack options\n");
        return FENG_CLI_PARSE_ERROR;
    }

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program, stdout);
            pack_options_dispose(out_options);
            return FENG_CLI_PARSE_HELP;
        }
        if (strncmp(arg, "--platform=", 11U) == 0) {
            const char *platform = arg + 11U;

            if (!feng_platform_is_valid(platform)) {
                fprintf(stderr,
                        "invalid target platform: %s\n",
                        platform[0] != '\0' ? platform : "(empty)");
                print_usage(program, stderr);
                pack_options_dispose(out_options);
                return FENG_CLI_PARSE_ERROR;
            }
            out_options->platforms[out_options->platform_count++] = platform;
            continue;
        }
        if (strncmp(arg, "--sysroot=", 10U) == 0) {
            if (out_options->sysroot != NULL) {
                fprintf(stderr, "--sysroot may only be specified once\n");
                print_usage(program, stderr);
                pack_options_dispose(out_options);
                return FENG_CLI_PARSE_ERROR;
            }
            out_options->sysroot = arg + 10U;
            if (out_options->sysroot[0] == '\0') {
                fprintf(stderr, "--sysroot requires a non-empty directory path\n");
                print_usage(program, stderr);
                pack_options_dispose(out_options);
                return FENG_CLI_PARSE_ERROR;
            }
            continue;
        }
        if (strncmp(arg, "--", 2U) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program, stderr);
            pack_options_dispose(out_options);
            return FENG_CLI_PARSE_ERROR;
        }
        if (out_options->path != NULL) {
            fprintf(stderr, "pack accepts at most one <path> argument\n");
            print_usage(program, stderr);
            pack_options_dispose(out_options);
            return FENG_CLI_PARSE_ERROR;
        }
        out_options->path = arg;
    }
    return FENG_CLI_PARSE_OK;
}

/* Compare two owned relative-path pointers for deterministic sorting. */
static int compare_path_ptrs(const void *left, const void *right) {
    const char *const *left_path = (const char *const *)left;
    const char *const *right_path = (const char *const *)right;

    return strcmp(*left_path, *right_path);
}

/* Append one owned relative path to a recursive file list. */
static bool pack_file_list_append(PackFileList *list, char *path) {
    char **resized;
    size_t capacity;

    if (list->count == list->capacity) {
        capacity = list->capacity == 0U ? 8U : list->capacity * 2U;
        resized = (char **)realloc(list->paths, capacity * sizeof(*list->paths));
        if (resized == NULL) {
            return false;
        }
        list->paths = resized;
        list->capacity = capacity;
    }
    list->paths[list->count++] = path;
    return true;
}

/* Release every owned path in a recursive file list. */
static void pack_file_list_dispose(PackFileList *list) {
    size_t index;

    if (list == NULL) {
        return;
    }
    for (index = 0U; index < list->count; ++index) {
        free(list->paths[index]);
    }
    free(list->paths);
    memset(list, 0, sizeof(*list));
}

/* Recursively collect regular files beneath one root. */
static bool collect_tree_files_recursive(const char *root,
                                         const char *relative_dir,
                                         PackFileList *list,
                                         char **out_error_message) {
    char *disk_dir = relative_dir[0] != '\0'
        ? path_join(root, relative_dir)
        : dup_printf("%s", root);
    DIR *directory;
    struct dirent *entry;

    if (disk_dir == NULL) {
        *out_error_message = dup_printf("out of memory");
        return false;
    }
    directory = opendir(disk_dir);
    if (directory == NULL) {
        *out_error_message = dup_printf("failed to scan %s: %s",
                                        disk_dir,
                                        strerror(errno));
        free(disk_dir);
        return false;
    }

    while ((entry = readdir(directory)) != NULL) {
        char *relative_path;
        char *disk_path;
        struct stat status;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        relative_path = relative_dir[0] != '\0'
            ? path_join(relative_dir, entry->d_name)
            : dup_printf("%s", entry->d_name);
        disk_path = relative_path != NULL ? path_join(root, relative_path) : NULL;
        if (relative_path == NULL || disk_path == NULL) {
            free(disk_path);
            free(relative_path);
            closedir(directory);
            free(disk_dir);
            *out_error_message = dup_printf("out of memory");
            return false;
        }
        if (stat(disk_path, &status) != 0) {
            *out_error_message = dup_printf("failed to stat %s: %s",
                                            disk_path,
                                            strerror(errno));
            free(disk_path);
            free(relative_path);
            closedir(directory);
            free(disk_dir);
            return false;
        }
        free(disk_path);
        if (S_ISDIR(status.st_mode)) {
            if (!collect_tree_files_recursive(root,
                                              relative_path,
                                              list,
                                              out_error_message)) {
                free(relative_path);
                closedir(directory);
                free(disk_dir);
                return false;
            }
            free(relative_path);
        } else if (S_ISREG(status.st_mode)) {
            if (!pack_file_list_append(list, relative_path)) {
                free(relative_path);
                closedir(directory);
                free(disk_dir);
                *out_error_message = dup_printf("out of memory");
                return false;
            }
        } else {
            free(relative_path);
        }
    }
    closedir(directory);
    free(disk_dir);
    return true;
}

/* Collect and sort one directory's recursive regular-file surface. */
static bool collect_tree_files(const char *root,
                               PackFileList *out_list,
                               char **out_error_message) {
    memset(out_list, 0, sizeof(*out_list));
    if (!dir_exists(root)) {
        return true;
    }
    if (!collect_tree_files_recursive(root, "", out_list, out_error_message)) {
        pack_file_list_dispose(out_list);
        return false;
    }
    qsort(out_list->paths,
          out_list->count,
          sizeof(*out_list->paths),
          compare_path_ptrs);
    return true;
}

/* Compare two regular files byte-for-byte. */
static bool files_have_equal_bytes(const char *left_path,
                                   const char *right_path,
                                   bool *out_equal,
                                   char **out_error_message) {
    FILE *left = fopen(left_path, "rb");
    FILE *right = NULL;
    unsigned char left_buffer[8192];
    unsigned char right_buffer[8192];
    bool equal = true;

    *out_equal = false;
    if (left == NULL) {
        *out_error_message = dup_printf("failed to open %s: %s",
                                        left_path,
                                        strerror(errno));
        return false;
    }
    right = fopen(right_path, "rb");
    if (right == NULL) {
        *out_error_message = dup_printf("failed to open %s: %s",
                                        right_path,
                                        strerror(errno));
        fclose(left);
        return false;
    }
    for (;;) {
        size_t left_count = fread(left_buffer, 1U, sizeof(left_buffer), left);
        size_t right_count = fread(right_buffer, 1U, sizeof(right_buffer), right);

        if (left_count != right_count ||
            memcmp(left_buffer, right_buffer, left_count) != 0) {
            equal = false;
            break;
        }
        if (left_count < sizeof(left_buffer)) {
            if (ferror(left) || ferror(right)) {
                *out_error_message = dup_printf("failed to compare %s and %s",
                                                left_path,
                                                right_path);
                fclose(right);
                fclose(left);
                return false;
            }
            break;
        }
    }
    fclose(right);
    fclose(left);
    *out_equal = equal;
    return true;
}

/* Canonically reserialize one public .ft and return the normalized bytes. */
static bool read_canonical_ft(const char *path,
                              const char *temp_path,
                              char **out_bytes,
                              size_t *out_length,
                              char **out_error_message) {
    FengSymbolFtReadOptions options = {
        .expected_profile = FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
    };
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    bool ok = false;

    *out_bytes = NULL;
    *out_length = 0U;
    if (!feng_symbol_ft_read_file(path, &options, &graph, &error)) {
        *out_error_message = dup_printf("failed to read public symbol table %s: %s",
                                        path,
                                        error.message != NULL
                                            ? error.message
                                            : "unknown error");
        goto done;
    }
    if (feng_symbol_graph_module_count(graph) != 1U) {
        *out_error_message = dup_printf(
            "public symbol table must contain exactly one module: %s",
            path);
        goto done;
    }
    if (!feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U),
                                     FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC,
                                     temp_path,
                                     &error)) {
        *out_error_message = dup_printf(
            "failed to normalize public symbol table %s: %s",
            path,
            error.message != NULL ? error.message : "unknown error");
        goto done;
    }
    *out_bytes = feng_cli_read_entire_file(temp_path, out_length);
    if (*out_bytes == NULL) {
        *out_error_message = dup_printf("failed to read normalized symbol table: %s",
                                        strerror(errno));
        goto done;
    }
    ok = true;

done:
    feng_symbol_error_free(&error);
    feng_symbol_graph_free(graph);
    return ok;
}

/* Compare two public .ft files by canonical semantic serialization. */
static bool ft_files_are_semantically_equal(const char *left_path,
                                            const char *right_path,
                                            const char *temp_root,
                                            bool *out_equal,
                                            char **out_error_message) {
    char *temp_path = path_join(temp_root, ".feng-ft-compare.XXXXXX");
    char *left_bytes = NULL;
    char *right_bytes = NULL;
    size_t left_length = 0U;
    size_t right_length = 0U;
    int temp_fd;
    bool ok = false;

    *out_equal = false;
    if (temp_path == NULL) {
        *out_error_message = dup_printf("out of memory");
        return false;
    }
    temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        *out_error_message = dup_printf("failed to create symbol comparison file: %s",
                                        strerror(errno));
        free(temp_path);
        return false;
    }
    close(temp_fd);
    if (!read_canonical_ft(left_path,
                           temp_path,
                           &left_bytes,
                           &left_length,
                           out_error_message) ||
        !read_canonical_ft(right_path,
                           temp_path,
                           &right_bytes,
                           &right_length,
                           out_error_message)) {
        goto done;
    }
    *out_equal = left_length == right_length &&
        memcmp(left_bytes, right_bytes, left_length) == 0;
    ok = true;

done:
    unlink(temp_path);
    free(right_bytes);
    free(left_bytes);
    free(temp_path);
    return ok;
}

/* Compare two staged trees by path and either semantic or byte equality. */
static bool trees_are_equal(const char *left_root,
                            const char *right_root,
                            const char *temp_root,
                            bool semantic_ft,
                            bool *out_equal,
                            char **out_error_message) {
    PackFileList left_files = {0};
    PackFileList right_files = {0};
    size_t index;
    bool ok = false;

    *out_equal = false;
    if (!collect_tree_files(left_root, &left_files, out_error_message) ||
        !collect_tree_files(right_root, &right_files, out_error_message)) {
        goto done;
    }
    if (left_files.count != right_files.count) {
        ok = true;
        goto done;
    }
    for (index = 0U; index < left_files.count; ++index) {
        char *left_path;
        char *right_path;
        bool files_equal;

        if (strcmp(left_files.paths[index], right_files.paths[index]) != 0) {
            ok = true;
            goto done;
        }
        left_path = path_join(left_root, left_files.paths[index]);
        right_path = path_join(right_root, right_files.paths[index]);
        if (left_path == NULL || right_path == NULL) {
            free(right_path);
            free(left_path);
            *out_error_message = dup_printf("out of memory");
            goto done;
        }
        if (semantic_ft) {
            if (!ft_files_are_semantically_equal(left_path,
                                                 right_path,
                                                 temp_root,
                                                 &files_equal,
                                                 out_error_message)) {
                free(right_path);
                free(left_path);
                goto done;
            }
        } else if (!files_have_equal_bytes(left_path,
                                          right_path,
                                          &files_equal,
                                          out_error_message)) {
            free(right_path);
            free(left_path);
            goto done;
        }
        free(right_path);
        free(left_path);
        if (!files_equal) {
            ok = true;
            goto done;
        }
    }
    *out_equal = true;
    ok = true;

done:
    pack_file_list_dispose(&right_files);
    pack_file_list_dispose(&left_files);
    return ok;
}

/* Release asset entry source paths allocated for a bundle spec. */
static void dispose_asset_entries(FengFbBundleDirectoryEntry *entries,
                                  size_t entry_count) {
    size_t index;

    for (index = 0U; index < entry_count; ++index) {
        free((char *)entries[index].source_root);
    }
    free(entries);
}

/* Build ordinary asset bundle entries from the first validated platform. */
static bool build_asset_entries(const FengCliProjectContext *context,
                                const char *platform_out_root,
                                FengFbBundleDirectoryEntry **out_entries,
                                size_t *out_entry_count,
                                char **out_error_message) {
    FengFbBundleDirectoryEntry *entries;
    size_t index;
    size_t entry_index = 0U;
    size_t entry_count = 0U;

    *out_entries = NULL;
    *out_entry_count = 0U;
    for (index = 0U; index < context->manifest.asset_count; ++index) {
        if (!feng_cli_project_asset_targets_extlib(&context->manifest.assets[index])) {
            entry_count++;
        }
    }
    if (entry_count == 0U) {
        return true;
    }
    entries = (FengFbBundleDirectoryEntry *)calloc(entry_count, sizeof(*entries));
    if (entries == NULL) {
        *out_error_message = dup_printf("out of memory");
        return false;
    }
    for (index = 0U; index < context->manifest.asset_count; ++index) {
        const FengCliProjectManifestAsset *asset = &context->manifest.assets[index];
        char *asset_root;

        if (feng_cli_project_asset_targets_extlib(asset)) {
            continue;
        }
        asset_root = path_join(platform_out_root, "assets");
        entries[entry_index].source_root = asset_root != NULL
            ? path_join(asset_root, asset->target_dir)
            : NULL;
        free(asset_root);
        entries[entry_index].entry_path = asset->target_dir;
        if (entries[entry_index].source_root == NULL) {
            dispose_asset_entries(entries, entry_count);
            *out_error_message = dup_printf("out of memory");
            return false;
        }
        entry_index++;
    }
    *out_entries = entries;
    *out_entry_count = entry_count;
    return true;
}

/* Release owned paths inside a platform artifact array. */
static void dispose_platform_artifacts(FengFbBundlePlatformArtifact *artifacts,
                                       size_t artifact_count) {
    size_t index;

    if (artifacts == NULL) {
        return;
    }
    for (index = 0U; index < artifact_count; ++index) {
        free((char *)artifacts[index].library_path);
        free((char *)artifacts[index].extlib_root);
    }
    free(artifacts);
}

int feng_cli_project_pack_main(const char *program, int argc, char **argv) {
    PackOptions options = {0};
    FengCliParseResult parse_result;
    FengCliProjectContext context = {0};
    FengCliProjectPlatformSelection selection = {0};
    FengCliProjectError project_error = {0};
    FengCliProjectManifestDependency *dependencies = NULL;
    size_t dependency_count = 0U;
    FengFbBundlePlatformArtifact *artifacts = NULL;
    FengFbBundleDirectoryEntry *asset_entries = NULL;
    size_t asset_entry_count = 0U;
    FengFbLibraryBundleSpec spec = {0};
    char *library_name = NULL;
    char *public_mod_root = NULL;
    char *first_platform_root = NULL;
    char *first_assets_root = NULL;
    char *error_message = NULL;
    size_t index;
    int rc = 1;

    parse_result = parse_args(program, argc, argv, &options);
    if (parse_result != FENG_CLI_PARSE_OK) {
        return parse_result == FENG_CLI_PARSE_HELP ? 0 : 1;
    }
    if (!feng_cli_project_open(options.path, &context, &project_error)) {
        feng_cli_project_print_error(stderr, &project_error);
        goto done;
    }
    if (context.manifest.target != FENG_COMPILE_TARGET_LIB) {
        fprintf(stderr,
                "error: `%s pack` requires `target:lib` in feng.fm\n",
                program);
        goto done;
    }
    if (!feng_cli_project_select_platforms(
            &context,
            options.platforms,
            options.platform_count,
            options.sysroot,
            false,
            &selection,
            &project_error)) {
        feng_cli_project_print_error(stderr, &project_error);
        goto done;
    }

    for (index = 0U; index < selection.platform_count; ++index) {
        rc = feng_cli_project_build_platform(program,
                                             &context,
                                             selection.platforms[index],
                                             options.sysroot,
                                             true,
                                             false,
                                             &project_error);
        if (rc != 0) {
            feng_cli_project_print_error(stderr, &project_error);
            goto done;
        }
    }

    library_name = feng_platform_static_library_file_name(context.manifest.name);
    artifacts = (FengFbBundlePlatformArtifact *)calloc(selection.platform_count,
                                                        sizeof(*artifacts));
    if (library_name == NULL || artifacts == NULL) {
        fprintf(stderr, "error: out of memory preparing package inputs\n");
        goto done;
    }
    for (index = 0U; index < selection.platform_count; ++index) {
        char *platform_root = feng_cli_project_platform_out_root(
            &context,
            selection.platforms[index]);
        char *library_dir = platform_root != NULL
            ? path_join(platform_root, "lib")
            : NULL;
        char *extlib_root = platform_root != NULL
            ? path_join(platform_root, "extlib")
            : NULL;

        artifacts[index].platform = selection.platforms[index];
        artifacts[index].library_path = library_dir != NULL
            ? path_join(library_dir, library_name)
            : NULL;
        if (extlib_root != NULL && dir_exists(extlib_root)) {
            artifacts[index].extlib_root = extlib_root;
            extlib_root = NULL;
        }
        free(extlib_root);
        free(library_dir);
        if (platform_root == NULL || artifacts[index].library_path == NULL) {
            free(platform_root);
            fprintf(stderr, "error: out of memory preparing package inputs\n");
            goto done;
        }

        if (index == 0U) {
            first_platform_root = platform_root;
            platform_root = NULL;
            public_mod_root = path_join(first_platform_root, "mod");
            first_assets_root = path_join(first_platform_root, "assets");
            if (public_mod_root == NULL || first_assets_root == NULL) {
                free(platform_root);
                fprintf(stderr, "error: out of memory preparing package inputs\n");
                goto done;
            }
        } else {
            char *candidate_mod_root = path_join(platform_root, "mod");
            char *candidate_assets_root = path_join(platform_root, "assets");
            bool equal = false;

            if (candidate_mod_root == NULL || candidate_assets_root == NULL ||
                !trees_are_equal(public_mod_root,
                                 candidate_mod_root,
                                 context.out_root,
                                 true,
                                 &equal,
                                 &error_message)) {
                free(candidate_assets_root);
                free(candidate_mod_root);
                free(platform_root);
                fprintf(stderr,
                        "error: %s\n",
                        error_message != NULL
                            ? error_message
                            : "failed to compare public symbol tables");
                goto done;
            }
            if (!equal) {
                fprintf(stderr,
                        "error: public symbol tables differ between %s and %s\n",
                        selection.platforms[0],
                        selection.platforms[index]);
                free(candidate_assets_root);
                free(candidate_mod_root);
                free(platform_root);
                goto done;
            }
            if (!trees_are_equal(first_assets_root,
                                 candidate_assets_root,
                                 context.out_root,
                                 false,
                                 &equal,
                                 &error_message)) {
                free(candidate_assets_root);
                free(candidate_mod_root);
                free(platform_root);
                fprintf(stderr,
                        "error: %s\n",
                        error_message != NULL
                            ? error_message
                            : "failed to compare package assets");
                goto done;
            }
            if (!equal) {
                fprintf(stderr,
                        "error: package assets differ between %s and %s\n",
                        selection.platforms[0],
                        selection.platforms[index]);
                free(candidate_assets_root);
                free(candidate_mod_root);
                free(platform_root);
                goto done;
            }
            free(candidate_assets_root);
            free(candidate_mod_root);
        }
        free(platform_root);
    }

    if (!build_asset_entries(&context,
                             first_platform_root,
                             &asset_entries,
                             &asset_entry_count,
                             &error_message) ||
        !feng_cli_deps_normalize_direct_dependencies(
            context.manifest_path,
            &context.manifest,
            &dependencies,
            &dependency_count,
            &project_error)) {
        if (error_message != NULL) {
            fprintf(stderr, "error: %s\n", error_message);
        } else {
            feng_cli_project_print_error(stderr, &project_error);
        }
        goto done;
    }

    spec.package_path = context.package_path;
    spec.package_name = context.manifest.name;
    spec.package_version = context.manifest.version;
    spec.platform_artifacts = artifacts;
    spec.platform_artifact_count = selection.platform_count;
    spec.dependencies = (const FengFbBundleDependency *)dependencies;
    spec.dependency_count = dependency_count;
    spec.public_mod_root = public_mod_root;
    spec.asset_entries = asset_entries;
    spec.asset_entry_count = asset_entry_count;
    if (!feng_fb_write_library_bundle(&spec, &error_message)) {
        fprintf(stderr,
                "error: %s\n",
                error_message != NULL
                    ? error_message
                    : "failed to write .fb package");
        goto done;
    }
    rc = 0;

done:
    free(error_message);
    free(first_assets_root);
    free(first_platform_root);
    free(public_mod_root);
    free(library_name);
    dispose_asset_entries(asset_entries, asset_entry_count);
    dispose_platform_artifacts(artifacts, selection.platform_count);
    feng_cli_deps_manifest_dependency_list_dispose(dependencies, dependency_count);
    feng_cli_project_error_dispose(&project_error);
    feng_cli_project_platform_selection_dispose(&selection);
    feng_cli_project_context_dispose(&context);
    pack_options_dispose(&options);
    return rc;
}
