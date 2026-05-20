#include "cli/cli.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "archive/fb.h"
#include "cli/deps/manager.h"
#include "cli/project/common.h"

static void print_usage(const char *program) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s pack [<path>]\n", program);
}

static bool parse_args(const char *program,
                       int argc,
                       char **argv,
                       const char **out_path) {
    int index;

    *out_path = NULL;

    for (index = 0; index < argc; ++index) {
        const char *arg = argv[index];

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(program);
            return false;
        }
        if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(program);
            return false;
        }
        if (*out_path != NULL) {
            fprintf(stderr, "pack accepts at most one <path> argument\n");
            print_usage(program);
            return false;
        }
        *out_path = arg;
    }

    return true;
}

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

static bool dir_exists(const char *path) {
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void dispose_asset_entries(FengFbBundleDirectoryEntry *asset_entries,
                                  size_t asset_entry_count) {
    size_t index;

    if (asset_entries == NULL) {
        return;
    }
    for (index = 0U; index < asset_entry_count; ++index) {
        free((char *)asset_entries[index].source_root);
    }
    free(asset_entries);
}

static bool build_asset_entries(const FengCliProjectContext *context,
                                FengFbBundleDirectoryEntry **out_entries,
                                char **out_error_message) {
    FengFbBundleDirectoryEntry *entries;
    size_t index;

    *out_entries = NULL;
    if (context->manifest.asset_count == 0U) {
        return true;
    }

    entries = (FengFbBundleDirectoryEntry *)calloc(context->manifest.asset_count, sizeof(*entries));
    if (entries == NULL) {
        if (out_error_message != NULL) {
            *out_error_message = dup_printf("out of memory");
        }
        return false;
    }

    for (index = 0U; index < context->manifest.asset_count; ++index) {
        entries[index].entry_path = context->manifest.assets[index].target_dir;
        entries[index].source_root = dup_printf("%s/%s",
                                                context->asset_stage_root,
                                                context->manifest.assets[index].target_dir);
        if (entries[index].source_root == NULL) {
            dispose_asset_entries(entries, context->manifest.asset_count);
            if (out_error_message != NULL) {
                *out_error_message = dup_printf("out of memory");
            }
            return false;
        }
    }

    *out_entries = entries;
    return true;
}

int feng_cli_project_pack_main(const char *program, int argc, char **argv) {
    const char *path_arg = NULL;
    const bool release = true;
    FengCliProjectContext context = {0};
    FengCliProjectError project_error = {0};
    FengCliDepsResolved resolved = {0};
    FengCliProjectManifestDependency *direct_dependencies = NULL;
    size_t direct_dependency_count = 0U;
    FengFbLibraryBundleSpec spec = {0};
    FengFbBundleDirectoryEntry *asset_entries = NULL;
    char *library_path = NULL;
    char *public_mod_root = NULL;
    char *extlib_root = NULL;
    char *error_message = NULL;
    int rc = 1;

    if (!parse_args(program, argc, argv, &path_arg)) {
        return 1;
    }
    if (!feng_cli_project_prepare_build(program,
                                        path_arg,
                                        release,
                                        &context,
                                        &resolved,
                                        &project_error)) {
        feng_cli_project_print_error(stderr, &project_error);
        feng_cli_project_error_dispose(&project_error);
        return 1;
    }
    if (context.manifest.target != FENG_COMPILE_TARGET_LIB) {
        fprintf(stderr,
                "error: `%s pack` requires `target:lib` in feng.fm\n",
                program);
        goto done;
    }
    rc = feng_cli_project_compile_prepared(program, &context, &resolved, release);
    if (rc != 0) {
        goto done;
    }
    if (!feng_cli_project_stage_assets(&context, &project_error)) {
        feng_cli_project_print_error(stderr, &project_error);
        rc = 1;
        goto done;
    }

    library_path = dup_printf("%s/lib/lib%s.a",
                              context.out_root,
                              context.manifest.name);
    if (library_path == NULL) {
        fprintf(stderr, "error: out of memory while preparing package paths\n");
        rc = 1;
        goto done;
    }
    public_mod_root = dup_printf("%s/mod", context.out_root);
    if (public_mod_root == NULL) {
        fprintf(stderr, "error: out of memory while preparing package paths\n");
        rc = 1;
        goto done;
    }
    if (context.manifest.asset_count > 0U) {
        if (!build_asset_entries(&context, &asset_entries, &error_message)) {
            fprintf(stderr,
                    "error: %s\n",
                    error_message != NULL ? error_message : "failed to prepare asset bundle entries");
            rc = 1;
            goto done;
        }
    }
    extlib_root = dup_printf("%s/extlib", context.out_root);
    if (extlib_root == NULL) {
        fprintf(stderr, "error: out of memory while preparing package paths\n");
        rc = 1;
        goto done;
    }
    if (!dir_exists(extlib_root)) {
        free(extlib_root);
        extlib_root = NULL;
    }
    if (!feng_cli_deps_normalize_direct_dependencies(context.manifest_path,
                                                     &context.manifest,
                                                     &direct_dependencies,
                                                     &direct_dependency_count,
                                                     &project_error)) {
        feng_cli_project_print_error(stderr, &project_error);
        rc = 1;
        goto done;
    }

    spec.package_path = context.package_path;
    spec.package_name = context.manifest.name;
    spec.package_version = context.manifest.version;
    spec.library_path = library_path;
    spec.dependencies = (const FengFbBundleDependency *)direct_dependencies;
    spec.dependency_count = direct_dependency_count;
    spec.public_mod_root = public_mod_root;
    spec.extlib_root = extlib_root;
    spec.asset_entries = asset_entries;
    spec.asset_entry_count = context.manifest.asset_count;

    if (!feng_fb_write_library_bundle(&spec, &error_message)) {
        fprintf(stderr,
                "error: %s\n",
                error_message != NULL ? error_message : "failed to write .fb package");
        rc = 1;
        goto done;
    }

    rc = 0;

done:
    feng_cli_deps_resolved_dispose(&resolved);
    feng_cli_deps_manifest_dependency_list_dispose(direct_dependencies, direct_dependency_count);
    free(error_message);
    free(extlib_root);
    dispose_asset_entries(asset_entries, context.manifest.asset_count);
    free(public_mod_root);
    free(library_path);
    feng_cli_project_context_dispose(&context);
    feng_cli_project_error_dispose(&project_error);
    return rc;
}
