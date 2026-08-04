#include "cli/deps/manager.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "archive/fm.h"
#include "archive/fb.h"
#include "platform/platform.h"
#include "archive/zip.h"
#include "cli/common.h"
#include "cli/project/common.h"

typedef enum ResolvedNodeKind {
    RESOLVED_NODE_BUNDLE = 0,
    RESOLVED_NODE_LOCAL_PROJECT
} ResolvedNodeKind;

typedef struct ResolvedNode {
    ResolvedNodeKind kind;
    char *identity_path;
    char *name;
    char *version;
    char *bundle_path;
    char *debug_fd_path;
    bool visiting;
    bool resolved;
    FengCliDepsResolved subtree;
} ResolvedNode;

typedef struct ResolveState {
    const char *program;
    const char *platform;
    const char *sysroot;
    bool force_remote;
    bool materialize_local_projects;
    bool release;
    char *cache_root;
    char *global_registry;
    bool global_registry_loaded;
    ResolvedNode *nodes;
    size_t node_count;
} ResolveState;

/* Describe whether one package source produced a candidate bundle. */
typedef enum PackageSourceResult {
    PACKAGE_SOURCE_FOUND = 0,
    PACKAGE_SOURCE_ABSENT,
    PACKAGE_SOURCE_ERROR
} PackageSourceResult;

/* Describe whether curl downloaded a package or observed an HTTP absence. */
typedef enum CurlDownloadResult {
    CURL_DOWNLOAD_FOUND = 0,
    CURL_DOWNLOAD_ABSENT,
    CURL_DOWNLOAD_ERROR
} CurlDownloadResult;

static char *dup_n(const char *text, size_t length) {
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}

static char *dup_cstr(const char *text) {
    return dup_n(text, strlen(text));
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

static void trim_trailing_ascii_whitespace(char *text) {
    size_t length;

    if (text == NULL) {
        return;
    }
    length = strlen(text);
    while (length > 0U) {
        char ch = text[length - 1U];

        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        text[length - 1U] = '\0';
        length -= 1U;
    }
}

static bool set_errorf(FengCliProjectError *error,
                       const char *path,
                       unsigned int line,
                       const char *fmt,
                       ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message;

    if (error == NULL) {
        return false;
    }

    feng_cli_project_error_dispose(error);
    error->path = path != NULL ? dup_cstr(path) : NULL;
    error->line = line;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return false;
    }

    message = (char *)malloc((size_t)needed + 1U);
    if (message == NULL) {
        va_end(args_copy);
        return false;
    }
    vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    error->message = message;
    return false;
}

static bool set_remote_install_errorf(FengCliProjectError *error,
                                      const char *path,
                                      unsigned int line,
                                      const char *name,
                                      const char *version,
                                      const char *fmt,
                                      ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *reason;
    bool ok;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return set_errorf(error,
                          path,
                          line,
                          "failed to install %s@%s: unknown error",
                          name,
                          version);
    }

    reason = (char *)malloc((size_t)needed + 1U);
    if (reason == NULL) {
        va_end(args_copy);
        return set_errorf(error,
                          path,
                          line,
                          "failed to install %s@%s: out of memory",
                          name,
                          version);
    }
    vsnprintf(reason, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    ok = set_errorf(error,
                    path,
                    line,
                    "failed to install %s@%s: %s",
                    name,
                    version,
                    reason);
    free(reason);
    return ok;
}

static bool set_remote_install_internal_errorf(FengCliProjectError *error,
                                               const char *name,
                                               const char *version,
                                               const char *fmt,
                                               ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *reason;
    bool ok;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return set_errorf(error,
                          NULL,
                          0U,
                          "failed to install %s@%s: unknown error",
                          name,
                          version);
    }

    reason = (char *)malloc((size_t)needed + 1U);
    if (reason == NULL) {
        va_end(args_copy);
        return set_errorf(error,
                          NULL,
                          0U,
                          "failed to install %s@%s: out of memory",
                          name,
                          version);
    }
    vsnprintf(reason, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    ok = set_errorf(error,
                    NULL,
                    0U,
                    "failed to install %s@%s: %s",
                    name,
                    version,
                    reason);
    free(reason);
    return ok;
}

static char *dup_project_error_detail(const FengCliProjectError *error) {
    if (error == NULL || error->message == NULL) {
        return dup_cstr("unknown error");
    }
    if (error->path != NULL && error->line > 0U) {
        return dup_printf("%s:%u: %s", error->path, error->line, error->message);
    }
    if (error->path != NULL) {
        return dup_printf("%s: %s", error->path, error->message);
    }
    return dup_cstr(error->message);
}

static bool set_local_dependency_errorf(FengCliProjectError *error,
                                        const char *owner_manifest_path,
                                        unsigned int line,
                                        const char *dependency_name,
                                        const char *dependency_value,
                                        const char *fmt,
                                        ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *reason;
    bool ok;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args_copy);
        return set_errorf(error,
                          owner_manifest_path,
                          line,
                          "failed to validate local dependency %s declared as \"%s\": unknown error",
                          dependency_name,
                          dependency_value);
    }

    reason = (char *)malloc((size_t)needed + 1U);
    if (reason == NULL) {
        va_end(args_copy);
        return set_errorf(error,
                          owner_manifest_path,
                          line,
                          "failed to validate local dependency %s declared as \"%s\": out of memory",
                          dependency_name,
                          dependency_value);
    }
    vsnprintf(reason, (size_t)needed + 1U, fmt, args_copy);
    va_end(args_copy);
    ok = set_errorf(error,
                    owner_manifest_path,
                    line,
                    "failed to validate local dependency %s declared as \"%s\": %s",
                    dependency_name,
                    dependency_value,
                    reason);
    free(reason);
    return ok;
}

static bool wrap_local_dependency_error(FengCliProjectError *error,
                                        const char *owner_manifest_path,
                                        unsigned int line,
                                        const char *dependency_name,
                                        const char *dependency_value,
                                        const FengCliProjectError *cause) {
    char *detail = dup_project_error_detail(cause);
    bool ok;

    if (detail == NULL) {
        return set_local_dependency_errorf(error,
                                           owner_manifest_path,
                                           line,
                                           dependency_name,
                                           dependency_value,
                                           "unknown error");
    }
    ok = set_local_dependency_errorf(error,
                                     owner_manifest_path,
                                     line,
                                     dependency_name,
                                     dependency_value,
                                     "%s",
                                     detail);
    free(detail);
    return ok;
}

static bool path_is_absolute(const char *path) {
    return path != NULL && path[0] == '/';
}

static bool path_has_suffix(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);

    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static char *path_join(const char *lhs, const char *rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    bool need_sep = lhs_len > 0U && lhs[lhs_len - 1U] != '/';
    char *out = (char *)malloc(lhs_len + (need_sep ? 1U : 0U) + rhs_len + 1U);
    size_t cursor = 0U;

    if (out == NULL) {
        return NULL;
    }

    memcpy(out + cursor, lhs, lhs_len);
    cursor += lhs_len;
    if (need_sep) {
        out[cursor++] = '/';
    }
    memcpy(out + cursor, rhs, rhs_len);
    cursor += rhs_len;
    out[cursor] = '\0';
    return out;
}

static bool dir_exists(const char *path) {
    struct stat st;

    return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        return dup_cstr(".");
    }
    if (slash == path) {
        return dup_cstr("/");
    }
    return dup_n(path, (size_t)(slash - path));
}

static void trim_trailing_slashes(char *path) {
    size_t length;

    if (path == NULL) {
        return;
    }
    length = strlen(path);
    while (length > 1U && path[length - 1U] == '/') {
        path[length - 1U] = '\0';
        length -= 1U;
    }
}

static bool mkdir_p(const char *path, FengCliProjectError *error) {
    char *mutable_path;
    size_t index;

    mutable_path = dup_cstr(path);
    if (mutable_path == NULL) {
        return set_errorf(error, path, 0U, "out of memory");
    }
    for (index = 1U; mutable_path[index] != '\0'; ++index) {
        if (mutable_path[index] == '/') {
            mutable_path[index] = '\0';
            if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
                free(mutable_path);
                return set_errorf(error,
                                  NULL,
                                  0U,
                                  "failed to create directory: %s",
                                  strerror(errno));
            }
            mutable_path[index] = '/';
        }
    }
    if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
        free(mutable_path);
        return set_errorf(error,
                          NULL,
                          0U,
                          "failed to create directory: %s",
                          strerror(errno));
    }
    free(mutable_path);
    return true;
}

static bool copy_file(const char *source_path,
                      const char *dest_path,
                      FengCliProjectError *error) {
    FILE *source = fopen(source_path, "rb");
    FILE *dest = NULL;
    char buffer[8192];
    size_t read_size;

    if (source == NULL) {
        return set_errorf(error,
                          source_path,
                          0U,
                          "failed to open %s: %s",
                          source_path,
                          strerror(errno));
    }
    dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        fclose(source);
        return set_errorf(error,
                          dest_path,
                          0U,
                          "failed to open %s: %s",
                          dest_path,
                          strerror(errno));
    }

    while ((read_size = fread(buffer, 1U, sizeof(buffer), source)) > 0U) {
        if (fwrite(buffer, 1U, read_size, dest) != read_size) {
            fclose(dest);
            fclose(source);
            return set_errorf(error,
                              dest_path,
                              0U,
                              "failed to write %s: %s",
                              dest_path,
                              strerror(errno));
        }
    }
    if (ferror(source)) {
        fclose(dest);
        fclose(source);
        return set_errorf(error,
                          source_path,
                          0U,
                          "failed to read %s",
                          source_path);
    }
    fclose(dest);
    fclose(source);
    return true;
}

static bool resolved_append_unique_path(char ***paths,
                                        size_t *path_count,
                                        const char *path,
                                        FengCliProjectError *error) {
    char **resized;
    size_t index;

    for (index = 0U; index < *path_count; ++index) {
        if (strcmp((*paths)[index], path) == 0) {
            return true;
        }
    }
    resized = (char **)realloc(*paths,
                               (*path_count + 1U) * sizeof(**paths));
    if (resized == NULL) {
        return set_errorf(error, path, 0U, "out of memory");
    }
    *paths = resized;
    (*paths)[*path_count] = dup_cstr(path);
    if ((*paths)[*path_count] == NULL) {
        return set_errorf(error, path, 0U, "out of memory");
    }
    *path_count += 1U;
    return true;
}

static bool resolved_append_unique(FengCliDepsResolved *resolved,
                                   const char *package_path,
                                   FengCliProjectError *error) {
    return resolved_append_unique_path(&resolved->package_paths,
                                       &resolved->package_count,
                                       package_path,
                                       error);
}

static bool resolved_append_debug_fd_unique(FengCliDepsResolved *resolved,
                                            const char *debug_fd_path,
                                            FengCliProjectError *error) {
    return resolved_append_unique_path(&resolved->debug_fd_paths,
                                       &resolved->debug_fd_count,
                                       debug_fd_path,
                                       error);
}

static bool resolved_merge(FengCliDepsResolved *target,
                           const FengCliDepsResolved *source,
                           FengCliProjectError *error) {
    size_t index;

    for (index = 0U; index < source->package_count; ++index) {
        if (!resolved_append_unique(target, source->package_paths[index], error)) {
            return false;
        }
    }
    for (index = 0U; index < source->debug_fd_count; ++index) {
        if (!resolved_append_debug_fd_unique(target, source->debug_fd_paths[index], error)) {
            return false;
        }
    }
    return true;
}

void feng_cli_deps_resolved_dispose(FengCliDepsResolved *resolved) {
    size_t index;

    if (resolved == NULL) {
        return;
    }
    for (index = 0U; index < resolved->package_count; ++index) {
        free(resolved->package_paths[index]);
    }
    for (index = 0U; index < resolved->debug_fd_count; ++index) {
        free(resolved->debug_fd_paths[index]);
    }
    free(resolved->package_paths);
    free(resolved->debug_fd_paths);
    resolved->package_paths = NULL;
    resolved->package_count = 0U;
    resolved->debug_fd_paths = NULL;
    resolved->debug_fd_count = 0U;
}

void feng_cli_deps_manifest_dependency_list_dispose(
    FengCliProjectManifestDependency *dependencies,
    size_t dependency_count) {
    size_t index;

    if (dependencies == NULL) {
        return;
    }
    for (index = 0U; index < dependency_count; ++index) {
        free(dependencies[index].name);
        free(dependencies[index].value);
    }
    free(dependencies);
}

static void resolved_node_dispose(ResolvedNode *node) {
    if (node == NULL) {
        return;
    }
    free(node->identity_path);
    free(node->name);
    free(node->version);
    free(node->bundle_path);
    free(node->debug_fd_path);
    feng_cli_deps_resolved_dispose(&node->subtree);
    memset(node, 0, sizeof(*node));
}

static void resolve_state_dispose(ResolveState *state) {
    size_t index;

    if (state == NULL) {
        return;
    }
    for (index = 0U; index < state->node_count; ++index) {
        resolved_node_dispose(&state->nodes[index]);
    }
    free(state->nodes);
    free(state->cache_root);
    free(state->global_registry);
    state->nodes = NULL;
    state->node_count = 0U;
    state->cache_root = NULL;
    state->global_registry = NULL;
    state->global_registry_loaded = false;
}

static ssize_t find_node_index(const ResolveState *state,
                               ResolvedNodeKind kind,
                               const char *identity_path) {
    size_t index;

    for (index = 0U; index < state->node_count; ++index) {
        if (state->nodes[index].kind == kind &&
            strcmp(state->nodes[index].identity_path, identity_path) == 0) {
            return (ssize_t)index;
        }
    }
    return -1;
}

static ResolvedNode *append_node(ResolveState *state,
                                 ResolvedNodeKind kind,
                                 const char *identity_path,
                                 FengCliProjectError *error) {
    ResolvedNode *resized;
    ResolvedNode *node;

    resized = (ResolvedNode *)realloc(state->nodes,
                                      (state->node_count + 1U) * sizeof(*state->nodes));
    if (resized == NULL) {
        set_errorf(error, identity_path, 0U, "out of memory");
        return NULL;
    }
    state->nodes = resized;
    node = &state->nodes[state->node_count];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->identity_path = dup_cstr(identity_path);
    if (node->identity_path == NULL) {
        set_errorf(error, identity_path, 0U, "out of memory");
        return NULL;
    }
    state->node_count += 1U;
    return node;
}

static bool check_package_version_compatibility(const ResolveState *state,
                                                const char *name,
                                                const char *version,
                                                const char *origin,
                                                FengCliProjectError *error) {
    size_t index;

    for (index = 0U; index < state->node_count; ++index) {
        const ResolvedNode *node = &state->nodes[index];

        if (node->name == NULL || strcmp(node->name, name) != 0) {
            continue;
        }
        if (strcmp(node->version, version) != 0) {
            return set_errorf(error,
                              origin,
                              0U,
                              "dependency version conflict for package %s: %s vs %s",
                              name,
                              node->version,
                              version);
        }
        if (strcmp(node->identity_path, origin) != 0) {
            return set_errorf(error,
                              origin,
                              0U,
                              "package %s@%s resolved from multiple origins: %s and %s",
                              name,
                              version,
                              node->identity_path,
                              origin);
        }
    }

    return true;
}

static bool get_cache_root(ResolveState *state,
                           FengCliProjectError *error,
                           char **out_cache_root) {
    const char *home;

    if (state->cache_root == NULL) {
        home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            home = getenv("USERPROFILE");
        }
        if (home == NULL || home[0] == '\0') {
            return set_errorf(error, NULL, 0U, "HOME is not set; cannot resolve feng cache directory");
        }
        state->cache_root = dup_printf("%s/.feng/cache", home);
        if (state->cache_root == NULL) {
            return set_errorf(error, NULL, 0U, "out of memory");
        }
    }
    *out_cache_root = state->cache_root;
    return true;
}

static bool resolve_registry_value(const char *source_path,
                                   const char *value,
                                   char **out_registry,
                                   FengCliProjectError *error) {
    char *base_dir;
    char *joined;

    if (strncmp(value, "http://", 7U) == 0 || strncmp(value, "https://", 8U) == 0) {
        *out_registry = dup_cstr(value);
        if (*out_registry == NULL) {
            return set_errorf(error, source_path, 0U, "out of memory");
        }
        return true;
    }
    if (strncmp(value, "file://", 7U) == 0) {
        *out_registry = dup_cstr(value + 7U);
        if (*out_registry == NULL) {
            return set_errorf(error, source_path, 0U, "out of memory");
        }
        trim_trailing_slashes(*out_registry);
        return true;
    }
    if (path_is_absolute(value)) {
        *out_registry = dup_cstr(value);
        if (*out_registry == NULL) {
            return set_errorf(error, source_path, 0U, "out of memory");
        }
        trim_trailing_slashes(*out_registry);
        return true;
    }

    base_dir = path_dirname_dup(source_path);
    if (base_dir == NULL) {
        return set_errorf(error, source_path, 0U, "out of memory");
    }
    joined = path_join(base_dir, value);
    free(base_dir);
    if (joined == NULL) {
        return set_errorf(error, source_path, 0U, "out of memory");
    }
    trim_trailing_slashes(joined);
    *out_registry = joined;
    return true;
}

static bool load_global_registry(ResolveState *state,
                                 FengCliProjectError *error) {
    char *cache_root;
    char *feng_dir;
    char *config_path;
    char *config_source = NULL;
    size_t config_length = 0U;
    FengFmDocument document = {0};
    FengFmError fm_error = {0};
    size_t section_index;
    size_t entry_index;

    if (state->global_registry_loaded) {
        return true;
    }

    if (!get_cache_root(state, error, &cache_root)) {
        return false;
    }
    feng_dir = path_dirname_dup(cache_root);
    if (feng_dir == NULL) {
        return set_errorf(error, cache_root, 0U, "out of memory");
    }
    config_path = path_join(feng_dir, "config.fm");
    free(feng_dir);
    if (config_path == NULL) {
        return set_errorf(error, cache_root, 0U, "out of memory");
    }

    config_source = feng_cli_read_entire_file(config_path, &config_length);
    if (config_source == NULL) {
        if (errno == ENOENT) {
            state->global_registry_loaded = true;
            free(config_path);
            return true;
        }
        free(config_path);
        return set_errorf(error,
                          config_path,
                          0U,
                          "failed to read global config: %s",
                          strerror(errno));
    }
    (void)config_length;

    if (!feng_fm_parse(config_path, config_source, &document, &fm_error)) {
        bool ok = set_errorf(error,
                             fm_error.path,
                             fm_error.line,
                             "%s",
                             fm_error.message != NULL ? fm_error.message : "invalid global config");
        feng_fm_error_dispose(&fm_error);
        feng_fm_document_dispose(&document);
        free(config_source);
        free(config_path);
        return ok;
    }

    for (section_index = 0U; section_index < document.section_count; ++section_index) {
        if (strcmp(document.sections[section_index].name, "registry") != 0) {
            bool ok = set_errorf(error,
                                 config_path,
                                 document.sections[section_index].line,
                                 "unsupported section in global feng config");
            feng_fm_error_dispose(&fm_error);
            feng_fm_document_dispose(&document);
            free(config_source);
            free(config_path);
            return ok;
        }
    }
    for (entry_index = 0U; entry_index < document.entry_count; ++entry_index) {
        const FengFmEntry *entry = &document.entries[entry_index];

        if (strcmp(entry->key, "url") != 0) {
            bool ok = set_errorf(error,
                                 config_path,
                                 entry->line,
                                 "unsupported field in global feng config");
            feng_fm_error_dispose(&fm_error);
            feng_fm_document_dispose(&document);
            free(config_source);
            free(config_path);
            return ok;
        }
        if (state->global_registry != NULL) {
            bool ok = set_errorf(error,
                                 config_path,
                                 entry->line,
                                 "duplicate registry url in global feng config");
            feng_fm_error_dispose(&fm_error);
            feng_fm_document_dispose(&document);
            free(config_source);
            free(config_path);
            return ok;
        }
        if (!resolve_registry_value(config_path, entry->value, &state->global_registry, error)) {
            feng_fm_error_dispose(&fm_error);
            feng_fm_document_dispose(&document);
            free(config_source);
            free(config_path);
            return false;
        }
    }

    state->global_registry_loaded = true;
    feng_fm_error_dispose(&fm_error);
    feng_fm_document_dispose(&document);
    free(config_source);
    free(config_path);
    return true;
}

static bool select_registry_for_project(ResolveState *state,
                                        const char *manifest_path,
                                        const FengCliProjectManifest *manifest,
                                        char **out_registry,
                                        FengCliProjectError *error) {
    *out_registry = NULL;
    if (manifest->registry_url != NULL) {
        return resolve_registry_value(manifest_path, manifest->registry_url, out_registry, error);
    }
    if (!load_global_registry(state, error)) {
        return false;
    }
    if (state->global_registry != NULL) {
        *out_registry = dup_cstr(state->global_registry);
        if (*out_registry == NULL) {
            return set_errorf(error, manifest_path, 0U, "out of memory");
        }
    }
    return true;
}

static bool read_project_manifest_from_disk(const char *manifest_path,
                                            FengCliProjectManifest *out_manifest,
                                            FengCliProjectError *out_error) {
    char *source;
    size_t length = 0U;

    source = feng_cli_read_entire_file(manifest_path, &length);
    if (source == NULL) {
        return set_errorf(out_error,
                          manifest_path,
                          0U,
                          "failed to read manifest: %s",
                          strerror(errno));
    }
    (void)length;
    if (!feng_cli_project_manifest_parse(manifest_path, source, out_manifest, out_error)) {
        free(source);
        return false;
    }
    free(source);
    return true;
}

static bool read_bundle_manifest(const char *bundle_path,
                                 FengCliProjectManifest *out_manifest,
                                 FengCliProjectError *out_error) {
    FengZipReader reader = {0};
    char *zip_error = NULL;
    void *manifest_bytes = NULL;
    size_t manifest_size = 0U;
    char *manifest_text = NULL;
    bool ok = false;

    if (!feng_zip_reader_open(bundle_path, &reader, &zip_error)) {
        return set_errorf(out_error,
                          bundle_path,
                          0U,
                          "failed to open bundle: %s",
                          zip_error != NULL ? zip_error : "unknown error");
    }
    if (!feng_zip_reader_read(&reader, "feng.fm", &manifest_bytes, &manifest_size, &zip_error)) {
        feng_zip_reader_dispose(&reader);
        return set_errorf(out_error,
                          bundle_path,
                          0U,
                          "failed to read bundle manifest: %s",
                          zip_error != NULL ? zip_error : "unknown error");
    }

    manifest_text = (char *)malloc(manifest_size + 1U);
    if (manifest_text == NULL) {
        feng_zip_free(manifest_bytes);
        feng_zip_reader_dispose(&reader);
        return set_errorf(out_error, bundle_path, 0U, "out of memory");
    }
    memcpy(manifest_text, manifest_bytes, manifest_size);
    manifest_text[manifest_size] = '\0';

    ok = feng_cli_project_bundle_manifest_parse(bundle_path,
                                                manifest_text,
                                                out_manifest,
                                                out_error);
    free(manifest_text);
    feng_zip_free(manifest_bytes);
    feng_zip_reader_dispose(&reader);
    return ok;
}

/*
 * Download one registry candidate and distinguish an HTTP 404 from transport
 * or server failures so only a definite absence can use the fallback source.
 */
static CurlDownloadResult download_with_curl(const char *url,
                                             const char *dest_path,
                                             char **out_reason) {
    int pipe_fds[2] = {-1, -1};
    pid_t child;
    int status = 0;
    char buffer[512];
    char *output_text = NULL;
    size_t output_length = 0U;
    ssize_t read_size;
    int read_errno = 0;
    const char *status_marker = "FENG_HTTP_STATUS:";
    char *marker_position = NULL;
    long http_status = 0L;

    *out_reason = NULL;

    if (pipe(pipe_fds) != 0) {
        *out_reason = dup_printf("failed to create curl output pipe: %s", strerror(errno));
        return CURL_DOWNLOAD_ERROR;
    }

    child = fork();
    if (child < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        *out_reason = dup_printf("failed to fork curl: %s", strerror(errno));
        return CURL_DOWNLOAD_ERROR;
    }
    if (child == 0) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
            dup2(pipe_fds[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(pipe_fds[1]);
        execlp("curl",
               "curl",
               "-sS",
               "-L",
               "--fail",
               "--write-out",
               "\nFENG_HTTP_STATUS:%{http_code}\n",
               "-o",
               dest_path,
               url,
               (char *)NULL);
        _exit(127);
    }
    close(pipe_fds[1]);

    while ((read_size = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        char *resized;

        if (read_errno == ENOMEM) {
            continue;
        }
        resized = (char *)realloc(output_text, output_length + (size_t)read_size + 1U);
        if (resized == NULL) {
            free(output_text);
            output_text = NULL;
            output_length = 0U;
            read_errno = ENOMEM;
            continue;
        }
        output_text = resized;
        memcpy(output_text + output_length, buffer, (size_t)read_size);
        output_length += (size_t)read_size;
        output_text[output_length] = '\0';
    }
    if (read_size < 0 && read_errno == 0) {
        read_errno = errno;
    }
    close(pipe_fds[0]);

    if (waitpid(child, &status, 0) < 0) {
        free(output_text);
        *out_reason = dup_printf("failed to wait for curl: %s", strerror(errno));
        return CURL_DOWNLOAD_ERROR;
    }
    if (read_errno != 0) {
        free(output_text);
        *out_reason = dup_printf("failed to read curl stderr: %s",
                                 read_errno == ENOMEM ? "out of memory" : strerror(read_errno));
        return CURL_DOWNLOAD_ERROR;
    }

    if (output_text != NULL) {
        char *cursor = output_text;

        while ((cursor = strstr(cursor, status_marker)) != NULL) {
            marker_position = cursor;
            cursor += strlen(status_marker);
        }
        if (marker_position != NULL) {
            char *status_text = marker_position + strlen(status_marker);
            char *status_end = NULL;

            errno = 0;
            http_status = strtol(status_text, &status_end, 10);
            if (errno != 0 || status_end == status_text) {
                http_status = 0L;
            }
            *marker_position = '\0';
            trim_trailing_ascii_whitespace(output_text);
        }
    }

    if (http_status == 404L) {
        free(output_text);
        *out_reason = dup_cstr("HTTP 404");
        return CURL_DOWNLOAD_ABSENT;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (output_text != NULL && output_text[0] != '\0') {
            *out_reason = output_text;
            return CURL_DOWNLOAD_ERROR;
        }
        free(output_text);
        if (!WIFEXITED(status)) {
            *out_reason = dup_cstr("curl terminated unexpectedly");
            return CURL_DOWNLOAD_ERROR;
        }
        if (WEXITSTATUS(status) == 127) {
            *out_reason = dup_cstr("curl is not available");
            return CURL_DOWNLOAD_ERROR;
        }
        *out_reason = dup_printf("curl exited with status %d", WEXITSTATUS(status));
        return CURL_DOWNLOAD_ERROR;
    }
    free(output_text);
    return CURL_DOWNLOAD_FOUND;
}

static bool validate_remote_bundle(const char *bundle_path,
                                   const char *origin_path,
                                   unsigned int origin_line,
                                   const char *name,
                                   const char *version,
                                   const char *source_label,
                                   FengCliProjectError *error) {
    FengCliProjectManifest bundle_manifest = {0};
    FengCliProjectError bundle_error = {0};

    if (!read_bundle_manifest(bundle_path, &bundle_manifest, &bundle_error)) {
        bool ok = set_remote_install_errorf(error,
                                            origin_path,
                                            origin_line,
                                            name,
                                            version,
                                            "invalid package bundle from %s: %s",
                                            source_label,
                                            bundle_error.message != NULL ? bundle_error.message
                                                                         : "unknown error");
        feng_cli_project_error_dispose(&bundle_error);
        return ok;
    }
    if (strcmp(bundle_manifest.name, name) != 0) {
        bool ok = set_remote_install_errorf(error,
                                            origin_path,
                                            origin_line,
                                            name,
                                            version,
                                            "invalid package bundle from %s: dependency name mismatch, found %s",
                                            source_label,
                                            bundle_manifest.name);
        feng_cli_project_manifest_dispose(&bundle_manifest);
        return ok;
    }
    if (strcmp(bundle_manifest.version, version) != 0) {
        bool ok = set_remote_install_errorf(error,
                                            origin_path,
                                            origin_line,
                                            name,
                                            version,
                                            "invalid package bundle from %s: dependency version mismatch, found %s",
                                            source_label,
                                            bundle_manifest.version);
        feng_cli_project_manifest_dispose(&bundle_manifest);
        return ok;
    }
    feng_cli_project_manifest_dispose(&bundle_manifest);
    return true;
}

/*
 * Copy and validate one filesystem package candidate into the cache staging
 * file. Missing paths are reported separately from all other source errors.
 */
static PackageSourceResult install_filesystem_candidate(const char *source_path,
                                                        const char *temp_path,
                                                        const char *origin_path,
                                                        unsigned int origin_line,
                                                        const char *name,
                                                        const char *version,
                                                        FengCliProjectError *error) {
    struct stat status;
    FengCliProjectError copy_error = {0};

    if (stat(source_path, &status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            return PACKAGE_SOURCE_ABSENT;
        }
        set_remote_install_errorf(error,
                                  origin_path,
                                  origin_line,
                                  name,
                                  version,
                                  "from %s: failed to inspect package: %s",
                                  source_path,
                                  strerror(errno));
        return PACKAGE_SOURCE_ERROR;
    }
    if (!S_ISREG(status.st_mode)) {
        set_remote_install_errorf(error,
                                  origin_path,
                                  origin_line,
                                  name,
                                  version,
                                  "from %s: package candidate is not a regular file",
                                  source_path);
        return PACKAGE_SOURCE_ERROR;
    }
    if (!copy_file(source_path, temp_path, &copy_error)) {
        set_remote_install_errorf(error,
                                  origin_path,
                                  origin_line,
                                  name,
                                  version,
                                  "from %s: %s",
                                  source_path,
                                  copy_error.message != NULL ? copy_error.message
                                                             : "failed to copy package");
        feng_cli_project_error_dispose(&copy_error);
        return PACKAGE_SOURCE_ERROR;
    }
    feng_cli_project_error_dispose(&copy_error);
    if (!validate_remote_bundle(temp_path,
                                origin_path,
                                origin_line,
                                name,
                                version,
                                source_path,
                                error)) {
        return PACKAGE_SOURCE_ERROR;
    }
    return PACKAGE_SOURCE_FOUND;
}

/*
 * Try the configured registry and return the exact checked location for a
 * later combined not-found diagnostic.
 */
static PackageSourceResult install_registry_candidate(const char *registry,
                                                      const char *bundle_name,
                                                      const char *temp_path,
                                                      const char *origin_path,
                                                      unsigned int origin_line,
                                                      const char *name,
                                                      const char *version,
                                                      char **out_location,
                                                      char **out_absent_reason,
                                                      FengCliProjectError *error) {
    *out_location = NULL;
    *out_absent_reason = NULL;

    if (strncmp(registry, "http://", 7U) == 0 ||
        strncmp(registry, "https://", 8U) == 0) {
        CurlDownloadResult download_result;
        char *download_reason = NULL;

        *out_location = dup_printf("%s%s/packages/%s",
                                   registry,
                                   registry[strlen(registry) - 1U] == '/' ? "" : "/",
                                   bundle_name);
        if (*out_location == NULL) {
            set_remote_install_internal_errorf(error, name, version, "out of memory");
            return PACKAGE_SOURCE_ERROR;
        }
        download_result = download_with_curl(*out_location, temp_path, &download_reason);
        if (download_result == CURL_DOWNLOAD_ABSENT) {
            *out_absent_reason = download_reason;
            return PACKAGE_SOURCE_ABSENT;
        }
        if (download_result == CURL_DOWNLOAD_ERROR) {
            set_remote_install_errorf(error,
                                      origin_path,
                                      origin_line,
                                      name,
                                      version,
                                      "from %s: %s",
                                      *out_location,
                                      download_reason != NULL ? download_reason
                                                              : "download failed");
            free(download_reason);
            return PACKAGE_SOURCE_ERROR;
        }
        free(download_reason);
        if (!validate_remote_bundle(temp_path,
                                    origin_path,
                                    origin_line,
                                    name,
                                    version,
                                    *out_location,
                                    error)) {
            return PACKAGE_SOURCE_ERROR;
        }
        return PACKAGE_SOURCE_FOUND;
    }

    {
        char *packages_dir = path_join(registry, "packages");
        PackageSourceResult result;

        if (packages_dir == NULL) {
            set_remote_install_internal_errorf(error, name, version, "out of memory");
            return PACKAGE_SOURCE_ERROR;
        }
        *out_location = path_join(packages_dir, bundle_name);
        free(packages_dir);
        if (*out_location == NULL) {
            set_remote_install_internal_errorf(error, name, version, "out of memory");
            return PACKAGE_SOURCE_ERROR;
        }
        result = install_filesystem_candidate(*out_location,
                                              temp_path,
                                              origin_path,
                                              origin_line,
                                              name,
                                              version,
                                              error);
        if (result == PACKAGE_SOURCE_ABSENT) {
            *out_absent_reason = dup_cstr(strerror(ENOENT));
            if (*out_absent_reason == NULL) {
                set_remote_install_internal_errorf(error, name, version, "out of memory");
                return PACKAGE_SOURCE_ERROR;
            }
        }
        return result;
    }
}

/*
 * Resolve and try the package shipped next to the Feng installation. The
 * resolver is shared with all other install-relative toolchain lookups.
 */
static PackageSourceResult install_bundled_candidate(ResolveState *state,
                                                     const char *bundle_name,
                                                     const char *temp_path,
                                                     const char *origin_path,
                                                     unsigned int origin_line,
                                                     const char *name,
                                                     const char *version,
                                                     char **out_location,
                                                     FengCliProjectError *error) {
    char *relative_path;
    char *resolve_error = NULL;
    PackageSourceResult result;

    *out_location = NULL;
    relative_path = dup_printf("pkg/%s", bundle_name);
    if (relative_path == NULL) {
        set_remote_install_internal_errorf(error, name, version, "out of memory");
        return PACKAGE_SOURCE_ERROR;
    }
    *out_location = feng_cli_resolve_install_path(state->program,
                                                  relative_path,
                                                  &resolve_error);
    free(relative_path);
    if (*out_location == NULL) {
        set_remote_install_errorf(error,
                                  origin_path,
                                  origin_line,
                                  name,
                                  version,
                                  "failed to resolve bundled package path: %s",
                                  resolve_error != NULL ? resolve_error : "unknown error");
        free(resolve_error);
        return PACKAGE_SOURCE_ERROR;
    }
    free(resolve_error);
    result = install_filesystem_candidate(*out_location,
                                          temp_path,
                                          origin_path,
                                          origin_line,
                                          name,
                                          version,
                                          error);
    return result;
}

static bool ensure_remote_bundle_cached(ResolveState *state,
                                        const char *registry,
                                        const char *name,
                                        const char *version,
                                        const char *origin_path,
                                        unsigned int origin_line,
                                        char **out_bundle_path,
                                        FengCliProjectError *error) {
    char *cache_root;
    char *bundle_name;
    char *bundle_path;
    char *temp_path = NULL;
    char *registry_location = NULL;
    char *registry_absent_reason = NULL;
    char *bundled_location = NULL;
    PackageSourceResult source_result = PACKAGE_SOURCE_ABSENT;
    int temp_fd;

    if (!get_cache_root(state, error, &cache_root)) {
        return false;
    }
    if (!mkdir_p(cache_root, error)) {
        bool ok = set_remote_install_internal_errorf(error,
                                                     name,
                                                     version,
                                                     "%s",
                                                     error->message != NULL
                                                         ? error->message
                                                         : "failed to prepare cache directory");
        return ok;
    }
    bundle_name = dup_printf("%s-%s.fb", name, version);
    if (bundle_name == NULL) {
        return set_remote_install_internal_errorf(error, name, version, "out of memory");
    }
    bundle_path = path_join(cache_root, bundle_name);
    if (bundle_path == NULL) {
        free(bundle_name);
        return set_remote_install_internal_errorf(error, name, version, "out of memory");
    }

    if (!state->force_remote && access(bundle_path, F_OK) == 0) {
        if (validate_remote_bundle(bundle_path,
                                   origin_path,
                                   origin_line,
                                   name,
                                   version,
                                   "cached bundle",
                                   error)) {
            free(bundle_name);
            *out_bundle_path = bundle_path;
            return true;
        }
        feng_cli_project_error_dispose(error);
    }

    temp_path = dup_printf("%s.tmp.XXXXXX", bundle_path);
    if (temp_path == NULL) {
        free(bundle_name);
        free(bundle_path);
        return set_remote_install_internal_errorf(error, name, version, "out of memory");
    }
    temp_fd = mkstemp(temp_path);
    if (temp_fd < 0) {
        free(temp_path);
        free(bundle_name);
        free(bundle_path);
        return set_remote_install_internal_errorf(error,
                                                  name,
                                                  version,
                                                  "failed to create temporary cache file: %s",
                                                  strerror(errno));
    }
    close(temp_fd);

    if (registry != NULL) {
        source_result = install_registry_candidate(registry,
                                                   bundle_name,
                                                   temp_path,
                                                   origin_path,
                                                   origin_line,
                                                   name,
                                                   version,
                                                   &registry_location,
                                                   &registry_absent_reason,
                                                   error);
    }
    if (source_result == PACKAGE_SOURCE_ABSENT) {
        source_result = install_bundled_candidate(state,
                                                  bundle_name,
                                                  temp_path,
                                                  origin_path,
                                                  origin_line,
                                                  name,
                                                  version,
                                                  &bundled_location,
                                                  error);
    }
    if (source_result == PACKAGE_SOURCE_ERROR) {
        free(bundled_location);
        free(registry_absent_reason);
        free(registry_location);
        unlink(temp_path);
        free(temp_path);
        free(bundle_name);
        free(bundle_path);
        return false;
    }
    if (source_result == PACKAGE_SOURCE_ABSENT) {
        bool ok;

        if (registry != NULL) {
            ok = set_remote_install_errorf(error,
                                           origin_path,
                                           origin_line,
                                           name,
                                           version,
                                           "package not found; registry candidate %s: %s; bundled package candidate %s: %s",
                                           registry_location,
                                           registry_absent_reason != NULL
                                               ? registry_absent_reason
                                               : "not found",
                                           bundled_location,
                                           strerror(ENOENT));
        } else {
            ok = set_remote_install_errorf(error,
                                           origin_path,
                                           origin_line,
                                           name,
                                           version,
                                           "package not found; no configured registry available; bundled package candidate %s: %s",
                                           bundled_location,
                                           strerror(ENOENT));
        }
        free(bundled_location);
        free(registry_absent_reason);
        free(registry_location);
        unlink(temp_path);
        free(temp_path);
        free(bundle_name);
        free(bundle_path);
        return ok;
    }

    if (rename(temp_path, bundle_path) != 0) {
        free(bundled_location);
        free(registry_absent_reason);
        free(registry_location);
        unlink(temp_path);
        free(temp_path);
        free(bundle_name);
        free(bundle_path);
        return set_remote_install_internal_errorf(error,
                                                  name,
                                                  version,
                                                  "failed to publish cached bundle: %s",
                                                  strerror(errno));
    }
    free(bundled_location);
    free(registry_absent_reason);
    free(registry_location);
    free(temp_path);
    free(bundle_name);
    *out_bundle_path = bundle_path;
    return true;
}

static bool resolve_dependency_target(const char *owner_manifest_path,
                                      const FengCliProjectManifestDependency *dependency,
                                      char **out_manifest_path,
                                      char **out_bundle_path,
                                      FengCliProjectError *error) {
    char *base_dir;
    char *joined;
    char *resolved;
    struct stat st;

    *out_manifest_path = NULL;
    *out_bundle_path = NULL;
    if (!dependency->is_local_path) {
        return true;
    }

    base_dir = path_dirname_dup(owner_manifest_path);
    if (base_dir == NULL) {
        return set_errorf(error, owner_manifest_path, dependency->line, "out of memory");
    }
    joined = path_is_absolute(dependency->value) ? dup_cstr(dependency->value)
                                                 : path_join(base_dir, dependency->value);
    free(base_dir);
    if (joined == NULL) {
        return set_errorf(error, owner_manifest_path, dependency->line, "out of memory");
    }

    resolved = realpath(joined, NULL);
    free(joined);
    if (resolved == NULL) {
        return set_errorf(error,
                          owner_manifest_path,
                          dependency->line,
                          "local dependency path not found: %s",
                          dependency->value);
    }
    if (stat(resolved, &st) != 0) {
        free(resolved);
        return set_errorf(error,
                          owner_manifest_path,
                          dependency->line,
                          "failed to stat local dependency path: %s",
                          strerror(errno));
    }
    if (S_ISREG(st.st_mode)) {
        if (path_has_suffix(resolved, ".fb")) {
            *out_bundle_path = resolved;
            return true;
        }
        if (strcmp(strrchr(resolved, '/') != NULL ? strrchr(resolved, '/') + 1U : resolved,
                   "feng.fm") == 0) {
            *out_manifest_path = resolved;
            return true;
        }
        free(resolved);
        return set_errorf(error,
                          owner_manifest_path,
                          dependency->line,
                          "local dependency path must point to .fb, a project directory, or feng.fm");
    }
    if (S_ISDIR(st.st_mode)) {
        char *manifest_path = path_join(resolved, "feng.fm");
        free(resolved);
        if (manifest_path == NULL) {
            return set_errorf(error, owner_manifest_path, dependency->line, "out of memory");
        }
        if (stat(manifest_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(manifest_path);
            return set_errorf(error,
                              owner_manifest_path,
                              dependency->line,
                              "local dependency directory does not contain feng.fm");
        }
        *out_manifest_path = manifest_path;
        return true;
    }

    free(resolved);
    return set_errorf(error,
                      owner_manifest_path,
                      dependency->line,
                      "local dependency path must point to a regular file or directory");
}

bool feng_cli_deps_validate_local_dependency(const char *owner_manifest_path,
                                             const char *dependency_name,
                                             const char *dependency_value,
                                             FengCliProjectError *out_error) {
    FengCliProjectManifestDependency dependency = {
        .name = (char *)dependency_name,
        .value = (char *)dependency_value,
        .line = 0U,
        .is_local_path = true,
    };
    char *child_manifest_path = NULL;
    char *child_bundle_path = NULL;

    if (!resolve_dependency_target(owner_manifest_path,
                                   &dependency,
                                   &child_manifest_path,
                                   &child_bundle_path,
                                   out_error)) {
        return wrap_local_dependency_error(out_error,
                                           owner_manifest_path,
                                           dependency.line,
                                           dependency_name,
                                           dependency_value,
                                           out_error);
    }

    if (child_bundle_path != NULL) {
        FengCliProjectManifest bundle_manifest = {0};

        if (!read_bundle_manifest(child_bundle_path, &bundle_manifest, out_error)) {
            free(child_bundle_path);
            return wrap_local_dependency_error(out_error,
                                               owner_manifest_path,
                                               dependency.line,
                                               dependency_name,
                                               dependency_value,
                                               out_error);
        }
        free(child_bundle_path);
        if (strcmp(bundle_manifest.name, dependency_name) != 0) {
            bool ok = set_local_dependency_errorf(out_error,
                                                  owner_manifest_path,
                                                  dependency.line,
                                                  dependency_name,
                                                  dependency_value,
                                                  "dependency name mismatch: expected %s but found %s",
                                                  dependency_name,
                                                  bundle_manifest.name);
            feng_cli_project_manifest_dispose(&bundle_manifest);
            return ok;
        }
        feng_cli_project_manifest_dispose(&bundle_manifest);
        return true;
    }

    if (child_manifest_path != NULL) {
        FengCliProjectManifest child_manifest = {0};

        if (!read_project_manifest_from_disk(child_manifest_path, &child_manifest, out_error)) {
            free(child_manifest_path);
            return wrap_local_dependency_error(out_error,
                                               owner_manifest_path,
                                               dependency.line,
                                               dependency_name,
                                               dependency_value,
                                               out_error);
        }
        free(child_manifest_path);
        if (strcmp(child_manifest.name, dependency_name) != 0) {
            bool ok = set_local_dependency_errorf(out_error,
                                                  owner_manifest_path,
                                                  dependency.line,
                                                  dependency_name,
                                                  dependency_value,
                                                  "dependency name mismatch: expected %s but found %s",
                                                  dependency_name,
                                                  child_manifest.name);
            feng_cli_project_manifest_dispose(&child_manifest);
            return ok;
        }
        if (child_manifest.target != FENG_COMPILE_TARGET_LIB) {
            bool ok = set_local_dependency_errorf(out_error,
                                                  owner_manifest_path,
                                                  dependency.line,
                                                  dependency_name,
                                                  dependency_value,
                                                  "local dependency project must use target: \"lib\"");
            feng_cli_project_manifest_dispose(&child_manifest);
            return ok;
        }
        feng_cli_project_manifest_dispose(&child_manifest);
        return true;
    }

    return true;
}

bool feng_cli_deps_normalize_direct_dependencies(const char *manifest_path,
                                                 const FengCliProjectManifest *manifest,
                                                 FengCliProjectManifestDependency **out_dependencies,
                                                 size_t *out_dependency_count,
                                                 FengCliProjectError *out_error) {
    FengCliProjectManifestDependency *dependencies = NULL;
    size_t dependency_count = 0U;
    size_t index;

    *out_dependencies = NULL;
    *out_dependency_count = 0U;

    if (manifest->dependency_count == 0U) {
        return true;
    }

    dependencies = (FengCliProjectManifestDependency *)calloc(manifest->dependency_count,
                                                              sizeof(*dependencies));
    if (dependencies == NULL) {
        return set_errorf(out_error, manifest_path, 0U, "out of memory");
    }

    for (index = 0U; index < manifest->dependency_count; ++index) {
        const FengCliProjectManifestDependency *source = &manifest->dependencies[index];
        FengCliProjectManifestDependency *dest = &dependencies[index];

        dest->name = dup_cstr(source->name);
        dest->line = source->line;
        dest->is_local_path = false;
        if (dest->name == NULL) {
            feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
            return set_errorf(out_error, manifest_path, source->line, "out of memory");
        }

        if (!source->is_local_path) {
            dest->value = dup_cstr(source->value);
            if (dest->value == NULL) {
                feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                return set_errorf(out_error, manifest_path, source->line, "out of memory");
            }
            dependency_count += 1U;
            continue;
        }

        {
            char *child_manifest_path = NULL;
            char *child_bundle_path = NULL;

            if (!resolve_dependency_target(manifest_path,
                                           source,
                                           &child_manifest_path,
                                           &child_bundle_path,
                                           out_error)) {
                feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                return wrap_local_dependency_error(out_error,
                                                   manifest_path,
                                                   source->line,
                                                   source->name,
                                                   source->value,
                                                   out_error);
            }
            if (child_bundle_path != NULL) {
                FengCliProjectManifest bundle_manifest = {0};

                if (!read_bundle_manifest(child_bundle_path, &bundle_manifest, out_error)) {
                    free(child_bundle_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return wrap_local_dependency_error(out_error,
                                                       manifest_path,
                                                       source->line,
                                                       source->name,
                                                       source->value,
                                                       out_error);
                }
                if (strcmp(bundle_manifest.name, source->name) != 0) {
                    feng_cli_project_manifest_dispose(&bundle_manifest);
                    free(child_bundle_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return set_local_dependency_errorf(out_error,
                                                       manifest_path,
                                                       source->line,
                                                       source->name,
                                                       source->value,
                                                       "dependency name mismatch: expected %s but found %s",
                                                       source->name,
                                                       bundle_manifest.name);
                }
                dest->value = dup_cstr(bundle_manifest.version);
                feng_cli_project_manifest_dispose(&bundle_manifest);
                free(child_bundle_path);
            } else {
                FengCliProjectManifest child_manifest = {0};

                if (!read_project_manifest_from_disk(child_manifest_path, &child_manifest, out_error)) {
                    free(child_manifest_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return wrap_local_dependency_error(out_error,
                                                       manifest_path,
                                                       source->line,
                                                       source->name,
                                                       source->value,
                                                       out_error);
                }
                if (strcmp(child_manifest.name, source->name) != 0) {
                    feng_cli_project_manifest_dispose(&child_manifest);
                    free(child_manifest_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return set_local_dependency_errorf(out_error,
                                                       manifest_path,
                                                       source->line,
                                                       source->name,
                                                       source->value,
                                                       "dependency name mismatch: expected %s but found %s",
                                                       source->name,
                                                       child_manifest.name);
                }
                dest->value = dup_cstr(child_manifest.version);
                feng_cli_project_manifest_dispose(&child_manifest);
                free(child_manifest_path);
            }
        }
        if (dest->value == NULL) {
            feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
            return set_errorf(out_error, manifest_path, source->line, "out of memory");
        }
        dependency_count += 1U;
    }

    *out_dependencies = dependencies;
    *out_dependency_count = dependency_count;
    return true;
}

static bool build_local_project_bundle(const char *program,
                                       const char *manifest_path,
                                       const char *platform,
                                       const char *sysroot,
                                       bool release,
                                       const FengCliDepsResolved *dependencies,
                                       char **out_bundle_path,
                                       char **out_debug_fd_path,
                                       FengCliProjectError *error) {
    FengCliProjectContext context = {0};
    FengCliProjectPlatformSelection selection = {0};
    FengFbLibraryBundleSpec spec = {0};
    FengFbBundlePlatformArtifact platform_artifact = {0};
    FengCliProjectManifestDependency *direct_dependencies = NULL;
    size_t direct_dependency_count = 0U;
    char *host_library_name = NULL;
    char *platform_out_root = NULL;
    char *library_path = NULL;
    char *debug_fd_path = NULL;
    char *public_mod_root = NULL;
    char *extlib_root = NULL;
    char *fb_error = NULL;
    int rc;

    if (out_debug_fd_path != NULL) {
        *out_debug_fd_path = NULL;
    }

    if (!feng_cli_project_open(manifest_path, &context, error)) {
        return false;
    }
    if (context.manifest.target != FENG_COMPILE_TARGET_LIB) {
        feng_cli_project_context_dispose(&context);
        return set_errorf(error,
                          manifest_path,
                          0U,
                          "local dependency project %s must use target: \"lib\"",
                          manifest_path);
    }
    {
        const char *requested_platforms[] = {platform};

        if (!feng_cli_project_select_platforms(&context,
                                               requested_platforms,
                                               1U,
                                               sysroot,
                                               false,
                                               &selection,
                                               error)) {
            feng_cli_project_context_dispose(&context);
            return false;
        }
    }

    rc = feng_cli_project_invoke_direct_compile_with_packages(program,
                                                              &context,
                                                              platform,
                                                              sysroot,
                                                              release,
                                                              false,
                                                              dependencies->package_count,
                                                              (const char *const *)dependencies->package_paths,
                                                              0U,
                                                              NULL);
    if (rc != 0) {
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return set_errorf(error,
                          manifest_path,
                          0U,
                          "failed to build local dependency project");
    }
    if (!feng_cli_project_stage_assets(&context, platform, error)) {
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return false;
    }

    host_library_name = feng_platform_static_library_file_name(context.manifest.name);
    if (host_library_name == NULL) {
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return set_errorf(error, manifest_path, 0U, "out of memory");
    }
    platform_out_root = feng_cli_project_platform_out_root(&context, platform);
    library_path = platform_out_root != NULL
        ? dup_printf("%s/lib/%s", platform_out_root, host_library_name)
        : NULL;
    if (!release && library_path != NULL) {
        debug_fd_path = dup_printf("%s.fd", library_path);
    }
    public_mod_root = platform_out_root != NULL
        ? dup_printf("%s/mod", platform_out_root)
        : NULL;
    extlib_root = platform_out_root != NULL
        ? dup_printf("%s/extlib", platform_out_root)
        : NULL;
    if (platform_out_root == NULL || library_path == NULL ||
        public_mod_root == NULL || extlib_root == NULL ||
        (!release && debug_fd_path == NULL)) {
        free(host_library_name);
        free(platform_out_root);
        free(debug_fd_path);
        free(extlib_root);
        free(public_mod_root);
        free(library_path);
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return set_errorf(error, manifest_path, 0U, "out of memory");
    }
    if (!dir_exists(extlib_root)) {
        free(extlib_root);
        extlib_root = NULL;
    }
    if (!release && access(debug_fd_path, F_OK) != 0) {
        free(host_library_name);
        free(debug_fd_path);
        free(extlib_root);
        free(public_mod_root);
        free(library_path);
        free(platform_out_root);
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return set_errorf(error,
                          manifest_path,
                          0U,
                          "failed to locate local dependency debug sidecar");
    }
    if (!feng_cli_deps_normalize_direct_dependencies(context.manifest_path,
                                                     &context.manifest,
                                                     &direct_dependencies,
                                                     &direct_dependency_count,
                                                     error)) {
        free(host_library_name);
        free(debug_fd_path);
        free(extlib_root);
        free(public_mod_root);
        free(library_path);
        free(platform_out_root);
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return false;
    }

    spec.package_path = context.package_path;
    spec.package_name = context.manifest.name;
    spec.package_version = context.manifest.version;
    platform_artifact.platform = platform;
    platform_artifact.library_path = library_path;
    platform_artifact.extlib_root = extlib_root;
    spec.platform_artifacts = &platform_artifact;
    spec.platform_artifact_count = 1U;
    spec.dependencies = (const FengFbBundleDependency *)direct_dependencies;
    spec.dependency_count = direct_dependency_count;
    spec.public_mod_root = public_mod_root;

    if (!feng_fb_write_library_bundle(&spec, &fb_error)) {
        bool ok = set_errorf(error,
                             context.package_path,
                             0U,
                             "%s",
                             fb_error != NULL ? fb_error : "failed to write local dependency bundle");
        free(fb_error);
        feng_cli_deps_manifest_dependency_list_dispose(direct_dependencies, direct_dependency_count);
        free(host_library_name);
        free(debug_fd_path);
        free(extlib_root);
        free(public_mod_root);
        free(library_path);
        free(platform_out_root);
        feng_cli_project_platform_selection_dispose(&selection);
        feng_cli_project_context_dispose(&context);
        return ok;
    }

    *out_bundle_path = dup_cstr(context.package_path);
    free(fb_error);
    feng_cli_deps_manifest_dependency_list_dispose(direct_dependencies, direct_dependency_count);
    free(host_library_name);
    free(extlib_root);
    free(public_mod_root);
    free(library_path);
    free(platform_out_root);
    feng_cli_project_platform_selection_dispose(&selection);
    feng_cli_project_context_dispose(&context);
    if (*out_bundle_path == NULL) {
        free(debug_fd_path);
        return set_errorf(error, manifest_path, 0U, "out of memory");
    }
    if (out_debug_fd_path != NULL) {
        *out_debug_fd_path = debug_fd_path;
        debug_fd_path = NULL;
    }
    free(debug_fd_path);
    return true;
}

static bool resolve_bundle_node(ResolveState *state,
                                const char *bundle_path,
                                const char *expected_name,
                                const char *expected_version,
                                const char *registry,
                                ResolvedNode **out_node,
                                FengCliProjectError *error);

static bool resolve_project_dependencies(ResolveState *state,
                                         const char *manifest_path,
                                         const FengCliProjectManifest *manifest,
                                         FengCliDepsResolved *out_dependencies,
                                         FengCliProjectError *error) {
    char *project_registry = NULL;
    size_t index;

    if (!select_registry_for_project(state, manifest_path, manifest, &project_registry, error)) {
        return false;
    }

    for (index = 0U; index < manifest->dependency_count; ++index) {
        const FengCliProjectManifestDependency *dependency = &manifest->dependencies[index];
        ResolvedNode *child = NULL;

        if (dependency->is_local_path) {
            char *child_manifest_path = NULL;
            char *child_bundle_path = NULL;
            size_t child_slot = 0U;

            if (!resolve_dependency_target(manifest_path,
                                           dependency,
                                           &child_manifest_path,
                                           &child_bundle_path,
                                           error)) {
                free(project_registry);
                return wrap_local_dependency_error(error,
                                                   manifest_path,
                                                   dependency->line,
                                                   dependency->name,
                                                   dependency->value,
                                                   error);
            }
            if (child_bundle_path != NULL) {
                if (!resolve_bundle_node(state,
                                         child_bundle_path,
                                         dependency->name,
                                         NULL,
                                         project_registry,
                                         &child,
                                         error)) {
                    free(child_bundle_path);
                    free(project_registry);
                    return wrap_local_dependency_error(error,
                                                       manifest_path,
                                                       dependency->line,
                                                       dependency->name,
                                                       dependency->value,
                                                       error);
                }
                free(child_bundle_path);
            } else {
                ssize_t node_index;
                FengCliProjectManifest child_manifest = {0};

                node_index = find_node_index(state,
                                             RESOLVED_NODE_LOCAL_PROJECT,
                                             child_manifest_path);
                if (node_index >= 0) {
                    child_slot = (size_t)node_index;
                    child = &state->nodes[child_slot];
                    if (child->visiting) {
                        free(child_manifest_path);
                        free(project_registry);
                        return set_local_dependency_errorf(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           "local dependency cycle detected at %s",
                                                           child->identity_path);
                    }
                } else {
                    child = append_node(state,
                                        RESOLVED_NODE_LOCAL_PROJECT,
                                        child_manifest_path,
                                        error);
                    if (child == NULL) {
                        free(child_manifest_path);
                        free(project_registry);
                        return false;
                    }
                    child_slot = state->node_count - 1U;
                }
                if (!child->resolved) {
                    FengCliDepsResolved child_subtree = {0};

                    if (!read_project_manifest_from_disk(child_manifest_path, &child_manifest, error)) {
                        free(child_manifest_path);
                        free(project_registry);
                        return wrap_local_dependency_error(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           error);
                    }
                    if (strcmp(child_manifest.name, dependency->name) != 0) {
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return set_local_dependency_errorf(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           "dependency name mismatch: expected %s but found %s",
                                                           dependency->name,
                                                           child_manifest.name);
                    }
                    if (child_manifest.target != FENG_COMPILE_TARGET_LIB) {
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return set_local_dependency_errorf(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           "local dependency project must use target: \"lib\"");
                    }
                    if (!check_package_version_compatibility(state,
                                                            child_manifest.name,
                                                            child_manifest.version,
                                                            child_manifest_path,
                                                            error)) {
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return wrap_local_dependency_error(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           error);
                    }
                    state->nodes[child_slot].visiting = true;
                    state->nodes[child_slot].name = dup_cstr(child_manifest.name);
                    state->nodes[child_slot].version = dup_cstr(child_manifest.version);
                    if (state->nodes[child_slot].name == NULL ||
                        state->nodes[child_slot].version == NULL) {
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return set_errorf(error, child_manifest_path, 0U, "out of memory");
                    }
                    if (!resolve_project_dependencies(state,
                                                      child_manifest_path,
                                                      &child_manifest,
                                                      &child_subtree,
                                                      error)) {
                        state->nodes[child_slot].visiting = false;
                        feng_cli_deps_resolved_dispose(&child_subtree);
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return wrap_local_dependency_error(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           error);
                    }
                    /* Recursion may grow state->nodes, so publish by index after it returns. */
                    state->nodes[child_slot].subtree = child_subtree;
                    if (state->materialize_local_projects &&
                        !build_local_project_bundle(state->program,
                                                    child_manifest_path,
                                                    state->platform,
                                                    state->sysroot,
                                                    state->release,
                                                    &state->nodes[child_slot].subtree,
                                                    &state->nodes[child_slot].bundle_path,
                                                    &state->nodes[child_slot].debug_fd_path,
                                                    error)) {
                        state->nodes[child_slot].visiting = false;
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return wrap_local_dependency_error(error,
                                                           manifest_path,
                                                           dependency->line,
                                                           dependency->name,
                                                           dependency->value,
                                                           error);
                    }
                    state->nodes[child_slot].resolved = true;
                    state->nodes[child_slot].visiting = false;
                    feng_cli_project_manifest_dispose(&child_manifest);
                }
                child = &state->nodes[child_slot];
                free(child_manifest_path);
            }
        } else {
            char *child_bundle_path = NULL;

            if (!ensure_remote_bundle_cached(state,
                                             project_registry,
                                             dependency->name,
                                             dependency->value,
                                             manifest_path,
                                             dependency->line,
                                             &child_bundle_path,
                                             error)) {
                free(project_registry);
                return false;
            }
            if (!resolve_bundle_node(state,
                                     child_bundle_path,
                                     dependency->name,
                                     dependency->value,
                                     project_registry,
                                     &child,
                                     error)) {
                free(child_bundle_path);
                free(project_registry);
                return false;
            }
            free(child_bundle_path);
        }

        if (child != NULL) {
            if (!resolved_merge(out_dependencies, &child->subtree, error) ||
                (child->bundle_path != NULL &&
                 !resolved_append_unique(out_dependencies, child->bundle_path, error)) ||
                (child->debug_fd_path != NULL &&
                 !resolved_append_debug_fd_unique(out_dependencies, child->debug_fd_path, error))) {
                free(project_registry);
                return false;
            }
        }
    }

    free(project_registry);
    return true;
}

static bool resolve_bundle_node(ResolveState *state,
                                const char *bundle_path,
                                const char *expected_name,
                                const char *expected_version,
                                const char *registry,
                                ResolvedNode **out_node,
                                FengCliProjectError *error) {
    ssize_t node_index;
    size_t node_slot;
    ResolvedNode *node;
    FengCliProjectManifest manifest = {0};
    size_t index;

    node_index = find_node_index(state, RESOLVED_NODE_BUNDLE, bundle_path);
    if (node_index >= 0) {
        node = &state->nodes[(size_t)node_index];
        if (node->visiting) {
            return set_errorf(error,
                              bundle_path,
                              0U,
                              "package dependency cycle detected at %s",
                              bundle_path);
        }
        if (expected_name != NULL && node->name != NULL && strcmp(node->name, expected_name) != 0) {
            return set_errorf(error,
                              bundle_path,
                              0U,
                              "dependency name mismatch: expected %s but found %s",
                              expected_name,
                              node->name);
        }
        if (expected_version != NULL && node->version != NULL && strcmp(node->version, expected_version) != 0) {
            return set_errorf(error,
                              bundle_path,
                              0U,
                              "dependency version mismatch: expected %s but found %s",
                              expected_version,
                              node->version);
        }
        *out_node = node;
        return true;
    }

    node = append_node(state, RESOLVED_NODE_BUNDLE, bundle_path, error);
    if (node == NULL) {
        return false;
    }
    node_slot = state->node_count - 1U;
    node->visiting = true;

    if (!read_bundle_manifest(bundle_path, &manifest, error)) {
        state->nodes[node_slot].visiting = false;
        return false;
    }
    if (expected_name != NULL && strcmp(manifest.name, expected_name) != 0) {
        feng_cli_project_manifest_dispose(&manifest);
        state->nodes[node_slot].visiting = false;
        return set_errorf(error,
                          bundle_path,
                          0U,
                          "dependency name mismatch: expected %s but found %s",
                          expected_name,
                          manifest.name);
    }
    if (expected_version != NULL && strcmp(manifest.version, expected_version) != 0) {
        feng_cli_project_manifest_dispose(&manifest);
        state->nodes[node_slot].visiting = false;
        return set_errorf(error,
                          bundle_path,
                          0U,
                          "dependency version mismatch: expected %s but found %s",
                          expected_version,
                          manifest.version);
    }
    if (!check_package_version_compatibility(state,
                                            manifest.name,
                                            manifest.version,
                                            bundle_path,
                                            error)) {
        feng_cli_project_manifest_dispose(&manifest);
        state->nodes[node_slot].visiting = false;
        return false;
    }
    state->nodes[node_slot].name = dup_cstr(manifest.name);
    state->nodes[node_slot].version = dup_cstr(manifest.version);
    state->nodes[node_slot].bundle_path = dup_cstr(bundle_path);
    if (state->nodes[node_slot].name == NULL ||
        state->nodes[node_slot].version == NULL ||
        state->nodes[node_slot].bundle_path == NULL) {
        feng_cli_project_manifest_dispose(&manifest);
        state->nodes[node_slot].visiting = false;
        return set_errorf(error, bundle_path, 0U, "out of memory");
    }

    for (index = 0U; index < manifest.dependency_count; ++index) {
        const FengCliProjectManifestDependency *dependency = &manifest.dependencies[index];
        char *child_bundle_path = NULL;
        ResolvedNode *child;

        if (!ensure_remote_bundle_cached(state,
                                         registry,
                                         dependency->name,
                                         dependency->value,
                                         bundle_path,
                                         dependency->line,
                                         &child_bundle_path,
                                         error)) {
            feng_cli_project_manifest_dispose(&manifest);
            state->nodes[node_slot].visiting = false;
            return false;
        }
        if (!resolve_bundle_node(state,
                                 child_bundle_path,
                                 dependency->name,
                                 dependency->value,
                                 registry,
                                 &child,
                                 error)) {
            free(child_bundle_path);
            feng_cli_project_manifest_dispose(&manifest);
            state->nodes[node_slot].visiting = false;
            return false;
        }
        free(child_bundle_path);
        node = &state->nodes[node_slot];
        if (!resolved_merge(&node->subtree, &child->subtree, error) ||
            !resolved_append_unique(&node->subtree, child->bundle_path, error)) {
            feng_cli_project_manifest_dispose(&manifest);
            state->nodes[node_slot].visiting = false;
            return false;
        }
    }

    state->nodes[node_slot].resolved = true;
    state->nodes[node_slot].visiting = false;
    *out_node = &state->nodes[node_slot];
    feng_cli_project_manifest_dispose(&manifest);
    return true;
}

static bool resolve_root_manifest(const char *program,
                                  const char *manifest_path,
                                  bool force_remote,
                                  bool materialize_local_projects,
                                  bool release,
                                  const char *platform,
                                  const char *sysroot,
                                  FengCliDepsResolved *out_resolved,
                                  FengCliProjectError *out_error) {
    ResolveState state;
    FengCliProjectManifest manifest = {0};
    bool ok;

    memset(&state, 0, sizeof(state));
    state.program = program;
    state.platform = platform;
    state.sysroot = sysroot;
    state.force_remote = force_remote;
    state.materialize_local_projects = materialize_local_projects;
    state.release = release;

    if (!read_project_manifest_from_disk(manifest_path, &manifest, out_error)) {
        return false;
    }
    ok = resolve_project_dependencies(&state,
                                      manifest_path,
                                      &manifest,
                                      out_resolved,
                                      out_error);
    feng_cli_project_manifest_dispose(&manifest);
    resolve_state_dispose(&state);
    return ok;
}

bool feng_cli_deps_install_for_manifest(const char *program,
                                        const char *manifest_path,
                                        bool force_remote,
                                        FengCliProjectError *out_error) {
    FengCliDepsResolved resolved = {0};
    bool ok = resolve_root_manifest(program,
                                    manifest_path,
                                    force_remote,
                                    false,
                                    false,
                                    NULL,
                                    NULL,
                                    &resolved,
                                    out_error);

    feng_cli_deps_resolved_dispose(&resolved);
    return ok;
}

bool feng_cli_deps_resolve_for_manifest(const char *program,
                                        const char *manifest_path,
                                        bool force_remote,
                                        bool release,
                                        FengCliDepsResolved *out_resolved,
                                        FengCliProjectError *out_error) {
    char *host_platform = NULL;
    char *host_error = NULL;
    bool ok;

    if (!feng_platform_detect_host_platform(&host_platform, &host_error)) {
        bool result = set_errorf(out_error,
                                 manifest_path,
                                 0U,
                                 "%s",
                                 host_error != NULL
                                     ? host_error
                                     : "failed to detect host platform");
        free(host_error);
        return result;
    }
    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    out_resolved->debug_fd_paths = NULL;
    out_resolved->debug_fd_count = 0U;
    ok = resolve_root_manifest(program,
                               manifest_path,
                               force_remote,
                               true,
                               release,
                               host_platform,
                               NULL,
                               out_resolved,
                               out_error);
    free(host_platform);
    return ok;
}

bool feng_cli_deps_resolve_for_manifest_platform(
    const char *program,
    const char *manifest_path,
    bool force_remote,
    bool release,
    const char *platform,
    const char *sysroot,
    FengCliDepsResolved *out_resolved,
    FengCliProjectError *out_error) {
    if (platform == NULL || !feng_platform_is_valid(platform)) {
        return set_errorf(out_error,
                          manifest_path,
                          0U,
                          "invalid target platform for dependency resolution");
    }
    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    out_resolved->debug_fd_paths = NULL;
    out_resolved->debug_fd_count = 0U;
    return resolve_root_manifest(program,
                                 manifest_path,
                                 force_remote,
                                 true,
                                 release,
                                 platform,
                                 sysroot,
                                 out_resolved,
                                 out_error);
}
