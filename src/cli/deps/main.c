#include "cli/cli.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "cli/common.h"
#include "cli/deps/manager.h"
#include "cli/project/common.h"

static char *dup_cstr(const char *text) {
    size_t length = strlen(text);
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length + 1U);
    return out;
}

static bool write_text_file(const char *path, const char *content, char **out_error_message) {
    FILE *stream = fopen(path, "wb");
    size_t length = strlen(content);

    if (stream == NULL) {
        if (out_error_message != NULL) {
            *out_error_message = dup_cstr(strerror(errno));
        }
        return false;
    }
    if (fwrite(content, 1U, length, stream) != length) {
        fclose(stream);
        if (out_error_message != NULL) {
            *out_error_message = dup_cstr("failed to write file");
        }
        return false;
    }
    if (fclose(stream) != 0) {
        if (out_error_message != NULL) {
            *out_error_message = dup_cstr(strerror(errno));
        }
        return false;
    }
    return true;
}

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s deps add <pkg-name> <version-or-path> [<path>]\n", program);
    fprintf(stderr, "  %s deps remove <pkg-name> [<path>]\n", program);
    fprintf(stderr, "  %s deps install [<path>] [--force]\n", program);
}

static ssize_t find_dependency_index(const FengCliProjectManifest *manifest, const char *name) {
    size_t index;

    for (index = 0U; index < manifest->dependency_count; ++index) {
        if (strcmp(manifest->dependencies[index].name, name) == 0) {
            return (ssize_t)index;
        }
    }
    return -1;
}

static bool is_local_value(const char *value) {
    return strncmp(value, "./", 2U) == 0 || strncmp(value, "../", 3U) == 0 || value[0] == '/';
}

static bool set_dependency(FengCliProjectManifest *manifest,
                           const char *name,
                           const char *value,
                           char **out_error_message) {
    ssize_t index = find_dependency_index(manifest, name);
    char *name_copy;
    char *value_copy;

    name_copy = dup_cstr(name);
    value_copy = dup_cstr(value);
    if (name_copy == NULL || value_copy == NULL) {
        free(value_copy);
        free(name_copy);
        if (out_error_message != NULL) {
            *out_error_message = dup_cstr("out of memory");
        }
        return false;
    }

    if (index >= 0) {
        FengCliProjectManifestDependency *dependency = &manifest->dependencies[(size_t)index];

        free(dependency->value);
        dependency->value = value_copy;
        dependency->is_local_path = is_local_value(value);
        free(name_copy);
        return true;
    }

    {
        FengCliProjectManifestDependency *resized = (FengCliProjectManifestDependency *)realloc(
            manifest->dependencies,
            (manifest->dependency_count + 1U) * sizeof(*manifest->dependencies));
        if (resized == NULL) {
            free(value_copy);
            free(name_copy);
            if (out_error_message != NULL) {
                *out_error_message = dup_cstr("out of memory");
            }
            return false;
        }
        manifest->dependencies = resized;
        manifest->dependencies[manifest->dependency_count].name = name_copy;
        manifest->dependencies[manifest->dependency_count].value = value_copy;
        manifest->dependencies[manifest->dependency_count].line = 0U;
        manifest->dependencies[manifest->dependency_count].is_local_path = is_local_value(value);
        manifest->dependency_count += 1U;
    }

    return true;
}

static bool remove_dependency(FengCliProjectManifest *manifest,
                              const char *name,
                              char **out_error_message) {
    ssize_t index = find_dependency_index(manifest, name);
    size_t cursor;

    if (index < 0) {
        if (out_error_message != NULL) {
            *out_error_message = dup_cstr("dependency not found");
        }
        return false;
    }

    free(manifest->dependencies[(size_t)index].name);
    free(manifest->dependencies[(size_t)index].value);
    for (cursor = (size_t)index + 1U; cursor < manifest->dependency_count; ++cursor) {
        manifest->dependencies[cursor - 1U] = manifest->dependencies[cursor];
    }
    manifest->dependency_count -= 1U;
    return true;
}

static bool load_project_manifest(const char *path_arg,
                                  char **out_manifest_path,
                                  char **out_original_source,
                                  FengCliProjectManifest *out_manifest,
                                  FengCliProjectError *out_error) {
    size_t source_length = 0U;

    if (!feng_cli_project_resolve_manifest_path(path_arg, out_manifest_path, out_error)) {
        return false;
    }
    *out_original_source = feng_cli_read_entire_file(*out_manifest_path, &source_length);
    (void)source_length;
    if (*out_original_source == NULL) {
        return false;
    }
    if (!feng_cli_project_manifest_parse(*out_manifest_path,
                                         *out_original_source,
                                         out_manifest,
                                         out_error)) {
        return false;
    }
    return true;
}

static bool rollback_manifest(const char *manifest_path, const char *original_source) {
    return write_text_file(manifest_path, original_source, NULL);
}

static int deps_add_main(const char *program, int argc, char **argv) {
    const char *name;
    const char *value;
    const char *path_arg = NULL;
    char *manifest_path = NULL;
    char *original_source = NULL;
    char *write_error = NULL;
    char *mutation_error = NULL;
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};
    int rc = 1;

    if (argc < 2 || argc > 3) {
        print_usage(program);
        return 1;
    }
    name = argv[0];
    value = argv[1];
    if (argc == 3) {
        path_arg = argv[2];
    }

    if (!load_project_manifest(path_arg, &manifest_path, &original_source, &manifest, &error)) {
        feng_cli_project_print_error(stderr, &error);
        goto done;
    }
    if (!set_dependency(&manifest, name, value, &mutation_error)) {
        fprintf(stderr, "%s\n", mutation_error != NULL ? mutation_error : "failed to update dependency");
        goto done;
    }
    if (!feng_cli_project_manifest_write(manifest_path, &manifest, &write_error)) {
        fprintf(stderr, "%s\n", write_error != NULL ? write_error : "failed to write manifest");
        goto done;
    }
    if (!feng_cli_deps_install_for_manifest(program, manifest_path, false, &error)) {
        (void)rollback_manifest(manifest_path, original_source);
        feng_cli_project_print_error(stderr, &error);
        goto done;
    }

    rc = 0;

done:
    free(mutation_error);
    free(write_error);
    free(original_source);
    free(manifest_path);
    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
    return rc;
}

static int deps_remove_main(const char *program, int argc, char **argv) {
    const char *name;
    const char *path_arg = NULL;
    char *manifest_path = NULL;
    char *original_source = NULL;
    char *write_error = NULL;
    char *mutation_error = NULL;
    FengCliProjectManifest manifest = {0};
    FengCliProjectError error = {0};
    int rc = 1;

    if (argc < 1 || argc > 2) {
        print_usage(program);
        return 1;
    }
    name = argv[0];
    if (argc == 2) {
        path_arg = argv[1];
    }

    if (!load_project_manifest(path_arg, &manifest_path, &original_source, &manifest, &error)) {
        feng_cli_project_print_error(stderr, &error);
        goto done;
    }
    if (!remove_dependency(&manifest, name, &mutation_error)) {
        fprintf(stderr, "%s\n", mutation_error != NULL ? mutation_error : "failed to remove dependency");
        goto done;
    }
    if (!feng_cli_project_manifest_write(manifest_path, &manifest, &write_error)) {
        fprintf(stderr, "%s\n", write_error != NULL ? write_error : "failed to write manifest");
        goto done;
    }

    rc = 0;

done:
    free(mutation_error);
    free(write_error);
    free(original_source);
    free(manifest_path);
    feng_cli_project_manifest_dispose(&manifest);
    feng_cli_project_error_dispose(&error);
    return rc;
}

static int deps_install_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    bool force = false;
    char *manifest_path = NULL;
    FengCliProjectError error = {0};
    int index;
    int rc = 1;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--force") == 0) {
            force = true;
            continue;
        }
        if (strncmp(arg, "--", 2) == 0) {
            print_usage(program);
            goto done;
        }
        if (path_arg != NULL) {
            print_usage(program);
            goto done;
        }
        path_arg = arg;
    }

    if (!feng_cli_project_resolve_manifest_path(path_arg, &manifest_path, &error)) {
        feng_cli_project_print_error(stderr, &error);
        goto done;
    }
    if (!feng_cli_deps_install_for_manifest(program, manifest_path, force, &error)) {
        feng_cli_project_print_error(stderr, &error);
        goto done;
    }

    rc = 0;

done:
    free(manifest_path);
    feng_cli_project_error_dispose(&error);
    return rc;
}

int feng_cli_deps_main(const char *program, int argc, char **argv) {
    if (argc < 1) {
        print_usage(program);
        return 1;
    }
    if (strcmp(argv[0], "add") == 0) {
        return deps_add_main(program, argc - 1, argv + 1);
    }
    if (strcmp(argv[0], "remove") == 0) {
        return deps_remove_main(program, argc - 1, argv + 1);
    }
    if (strcmp(argv[0], "install") == 0) {
        return deps_install_main(program, argc - 1, argv + 1);
    }
    if (strcmp(argv[0], "--help") == 0 || strcmp(argv[0], "-h") == 0) {
        print_usage(program);
        return 0;
    }

    print_usage(program);
    return 1;
}
