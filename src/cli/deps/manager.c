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
    bool visiting;
    bool resolved;
    FengCliDepsResolved subtree;
} ResolvedNode;

typedef struct ResolveState {
    const char *program;
    bool force_remote;
    bool materialize_local_projects;
    bool release;
    char *cache_root;
    char *global_registry;
    bool global_registry_loaded;
    ResolvedNode *nodes;
    size_t node_count;
} ResolveState;

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
                                  path,
                                  0U,
                                  "failed to create directory %s: %s",
                                  path,
                                  strerror(errno));
            }
            mutable_path[index] = '/';
        }
    }
    if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
        free(mutable_path);
        return set_errorf(error,
                          path,
                          0U,
                          "failed to create directory %s: %s",
                          path,
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

static bool resolved_append_unique(FengCliDepsResolved *resolved,
                                   const char *package_path,
                                   FengCliProjectError *error) {
    char **resized;
    size_t index;

    for (index = 0U; index < resolved->package_count; ++index) {
        if (strcmp(resolved->package_paths[index], package_path) == 0) {
            return true;
        }
    }
    resized = (char **)realloc(resolved->package_paths,
                               (resolved->package_count + 1U) * sizeof(*resolved->package_paths));
    if (resized == NULL) {
        return set_errorf(error, package_path, 0U, "out of memory");
    }
    resolved->package_paths = resized;
    resolved->package_paths[resolved->package_count] = dup_cstr(package_path);
    if (resolved->package_paths[resolved->package_count] == NULL) {
        return set_errorf(error, package_path, 0U, "out of memory");
    }
    resolved->package_count += 1U;
    return true;
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
    free(resolved->package_paths);
    resolved->package_paths = NULL;
    resolved->package_count = 0U;
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

static bool download_with_curl(const char *url,
                               const char *dest_path,
                               FengCliProjectError *error) {
    pid_t child;
    int status = 0;

    child = fork();
    if (child < 0) {
        return set_errorf(error, url, 0U, "failed to fork curl: %s", strerror(errno));
    }
    if (child == 0) {
        execlp("curl", "curl", "-fsSL", "-o", dest_path, url, (char *)NULL);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0) {
        return set_errorf(error, url, 0U, "failed to wait for curl: %s", strerror(errno));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return set_errorf(error,
                          url,
                          0U,
                          "failed to download remote package from %s",
                          url);
    }
    return true;
}

static bool ensure_remote_bundle_cached(ResolveState *state,
                                        const char *registry,
                                        const char *name,
                                        const char *version,
                                        char **out_bundle_path,
                                        FengCliProjectError *error) {
    char *cache_root;
    char *bundle_name;
    char *bundle_path;
    FengCliProjectManifest bundle_manifest = {0};

    if (!get_cache_root(state, error, &cache_root)) {
        return false;
    }
    if (!mkdir_p(cache_root, error)) {
        return false;
    }
    bundle_name = dup_printf("%s-%s.fb", name, version);
    if (bundle_name == NULL) {
        return set_errorf(error, cache_root, 0U, "out of memory");
    }
    bundle_path = path_join(cache_root, bundle_name);
    free(bundle_name);
    if (bundle_path == NULL) {
        return set_errorf(error, cache_root, 0U, "out of memory");
    }

    if (!state->force_remote && access(bundle_path, F_OK) == 0) {
        if (!read_bundle_manifest(bundle_path, &bundle_manifest, error)) {
            free(bundle_path);
            return false;
        }
        if (strcmp(bundle_manifest.name, name) != 0 || strcmp(bundle_manifest.version, version) != 0) {
            feng_cli_project_manifest_dispose(&bundle_manifest);
            free(bundle_path);
            return set_errorf(error,
                              cache_root,
                              0U,
                              "cached bundle metadata does not match %s@%s",
                              name,
                              version);
        }
        feng_cli_project_manifest_dispose(&bundle_manifest);
        *out_bundle_path = bundle_path;
        return true;
    }

    if (registry == NULL) {
        free(bundle_path);
        return set_errorf(error,
                          cache_root,
                          0U,
                          "remote dependency %s@%s requires a configured registry",
                          name,
                          version);
    }

    {
        char *temp_path = dup_printf("%s.tmp.XXXXXX", bundle_path);
        int temp_fd;

        if (temp_path == NULL) {
            free(bundle_path);
            return set_errorf(error, cache_root, 0U, "out of memory");
        }
        temp_fd = mkstemp(temp_path);
        if (temp_fd < 0) {
            free(temp_path);
            free(bundle_path);
            return set_errorf(error,
                              bundle_path,
                              0U,
                              "failed to create temporary cache file: %s",
                              strerror(errno));
        }
        close(temp_fd);

        if (strncmp(registry, "http://", 7U) == 0 || strncmp(registry, "https://", 8U) == 0) {
            char *url = dup_printf("%s%s/packages/%s-%s.fb",
                                   registry,
                                   registry[strlen(registry) - 1U] == '/' ? "" : "/",
                                   name,
                                   version);
            if (url == NULL) {
                unlink(temp_path);
                free(temp_path);
                free(bundle_path);
                return set_errorf(error, registry, 0U, "out of memory");
            }
            if (!download_with_curl(url, temp_path, error)) {
                free(url);
                unlink(temp_path);
                free(temp_path);
                free(bundle_path);
                return false;
            }
            free(url);
        } else {
            char *packages_dir = path_join(registry, "packages");
            char *source_name = dup_printf("%s-%s.fb", name, version);
            char *source_path;

            if (packages_dir == NULL || source_name == NULL) {
                free(source_name);
                free(packages_dir);
                unlink(temp_path);
                free(temp_path);
                free(bundle_path);
                return set_errorf(error, registry, 0U, "out of memory");
            }
            source_path = path_join(packages_dir, source_name);
            free(source_name);
            free(packages_dir);
            if (source_path == NULL) {
                unlink(temp_path);
                free(temp_path);
                free(bundle_path);
                return set_errorf(error, registry, 0U, "out of memory");
            }
            if (!copy_file(source_path, temp_path, error)) {
                free(source_path);
                unlink(temp_path);
                free(temp_path);
                free(bundle_path);
                return false;
            }
            free(source_path);
        }

        if (rename(temp_path, bundle_path) != 0) {
            unlink(temp_path);
            free(temp_path);
            free(bundle_path);
            return set_errorf(error,
                              bundle_path,
                              0U,
                              "failed to publish cached bundle: %s",
                              strerror(errno));
        }
        free(temp_path);
    }

    if (!read_bundle_manifest(bundle_path, &bundle_manifest, error)) {
        unlink(bundle_path);
        free(bundle_path);
        return false;
    }
    if (strcmp(bundle_manifest.name, name) != 0 || strcmp(bundle_manifest.version, version) != 0) {
        feng_cli_project_manifest_dispose(&bundle_manifest);
        unlink(bundle_path);
        free(bundle_path);
        return set_errorf(error,
                          bundle_path,
                          0U,
                          "downloaded bundle metadata does not match %s@%s",
                          name,
                          version);
    }
    feng_cli_project_manifest_dispose(&bundle_manifest);
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
        return false;
    }

    if (child_bundle_path != NULL) {
        FengCliProjectManifest bundle_manifest = {0};

        if (!read_bundle_manifest(child_bundle_path, &bundle_manifest, out_error)) {
            free(child_bundle_path);
            return false;
        }
        free(child_bundle_path);
        if (strcmp(bundle_manifest.name, dependency_name) != 0) {
            bool ok = set_errorf(out_error,
                                 owner_manifest_path,
                                 0U,
                                 "local dependency name mismatch: expected %s but found %s",
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
            return false;
        }
        free(child_manifest_path);
        if (strcmp(child_manifest.name, dependency_name) != 0) {
            bool ok = set_errorf(out_error,
                                 owner_manifest_path,
                                 0U,
                                 "local dependency name mismatch: expected %s but found %s",
                                 dependency_name,
                                 child_manifest.name);
            feng_cli_project_manifest_dispose(&child_manifest);
            return ok;
        }
        if (child_manifest.target != FENG_COMPILE_TARGET_LIB) {
            bool ok = set_errorf(out_error,
                                 owner_manifest_path,
                                 0U,
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
                return false;
            }
            if (child_bundle_path != NULL) {
                FengCliProjectManifest bundle_manifest = {0};

                if (!read_bundle_manifest(child_bundle_path, &bundle_manifest, out_error)) {
                    free(child_bundle_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return false;
                }
                if (strcmp(bundle_manifest.name, source->name) != 0) {
                    feng_cli_project_manifest_dispose(&bundle_manifest);
                    free(child_bundle_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return set_errorf(out_error,
                                      manifest_path,
                                      source->line,
                                      "local dependency name mismatch: expected %s but found %s",
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
                    return false;
                }
                if (strcmp(child_manifest.name, source->name) != 0) {
                    feng_cli_project_manifest_dispose(&child_manifest);
                    free(child_manifest_path);
                    feng_cli_deps_manifest_dependency_list_dispose(dependencies, manifest->dependency_count);
                    return set_errorf(out_error,
                                      manifest_path,
                                      source->line,
                                      "local dependency name mismatch: expected %s but found %s",
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
                                       bool release,
                                       const FengCliDepsResolved *dependencies,
                                       char **out_bundle_path,
                                       FengCliProjectError *error) {
    FengCliProjectContext context = {0};
    FengFbLibraryBundleSpec spec = {0};
    FengCliProjectManifestDependency *direct_dependencies = NULL;
    size_t direct_dependency_count = 0U;
    char *library_path = NULL;
    char *public_mod_root = NULL;
    char *fb_error = NULL;
    int rc;

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

    rc = feng_cli_project_invoke_direct_compile_with_packages(program,
                                                              &context,
                                                              release,
                                                              dependencies->package_count,
                                                              (const char *const *)dependencies->package_paths);
    if (rc != 0) {
        feng_cli_project_context_dispose(&context);
        return set_errorf(error,
                          manifest_path,
                          0U,
                          "failed to build local dependency project");
    }

    library_path = dup_printf("%s/lib/lib%s.a", context.out_root, context.manifest.name);
    public_mod_root = dup_printf("%s/mod", context.out_root);
    if (library_path == NULL || public_mod_root == NULL) {
        free(public_mod_root);
        free(library_path);
        feng_cli_project_context_dispose(&context);
        return set_errorf(error, manifest_path, 0U, "out of memory");
    }
    if (!feng_cli_deps_normalize_direct_dependencies(context.manifest_path,
                                                     &context.manifest,
                                                     &direct_dependencies,
                                                     &direct_dependency_count,
                                                     error)) {
        free(public_mod_root);
        free(library_path);
        feng_cli_project_context_dispose(&context);
        return false;
    }

    spec.package_path = context.package_path;
    spec.package_name = context.manifest.name;
    spec.package_version = context.manifest.version;
    spec.library_path = library_path;
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
        free(public_mod_root);
        free(library_path);
        feng_cli_project_context_dispose(&context);
        return ok;
    }

    *out_bundle_path = dup_cstr(context.package_path);
    free(fb_error);
    feng_cli_deps_manifest_dependency_list_dispose(direct_dependencies, direct_dependency_count);
    free(public_mod_root);
    free(library_path);
    feng_cli_project_context_dispose(&context);
    if (*out_bundle_path == NULL) {
        return set_errorf(error, manifest_path, 0U, "out of memory");
    }
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
                return false;
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
                    return false;
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
                        return set_errorf(error,
                                          manifest_path,
                                          dependency->line,
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
                    if (!read_project_manifest_from_disk(child_manifest_path, &child_manifest, error)) {
                        free(child_manifest_path);
                        free(project_registry);
                        return false;
                    }
                    if (strcmp(child_manifest.name, dependency->name) != 0) {
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return set_errorf(error,
                                          manifest_path,
                                          dependency->line,
                                          "local dependency name mismatch: expected %s but found %s",
                                          dependency->name,
                                          child_manifest.name);
                    }
                    if (child_manifest.target != FENG_COMPILE_TARGET_LIB) {
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return set_errorf(error,
                                          child_manifest_path,
                                          0U,
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
                        return false;
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
                                                      &state->nodes[child_slot].subtree,
                                                      error)) {
                        state->nodes[child_slot].visiting = false;
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return false;
                    }
                    if (state->materialize_local_projects &&
                        !build_local_project_bundle(state->program,
                                                    child_manifest_path,
                                                    state->release,
                                                    &state->nodes[child_slot].subtree,
                                                    &state->nodes[child_slot].bundle_path,
                                                    error)) {
                        state->nodes[child_slot].visiting = false;
                        feng_cli_project_manifest_dispose(&child_manifest);
                        free(child_manifest_path);
                        free(project_registry);
                        return false;
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
                 !resolved_append_unique(out_dependencies, child->bundle_path, error))) {
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
                                  FengCliDepsResolved *out_resolved,
                                  FengCliProjectError *out_error) {
    ResolveState state;
    FengCliProjectManifest manifest = {0};
    bool ok;

    memset(&state, 0, sizeof(state));
    state.program = program;
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
    out_resolved->package_paths = NULL;
    out_resolved->package_count = 0U;
    return resolve_root_manifest(program,
                                 manifest_path,
                                 force_remote,
                                 true,
                     release,
                                 out_resolved,
                                 out_error);
}
