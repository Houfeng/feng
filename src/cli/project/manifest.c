#include "cli/project/manifest.h"

#include "archive/fm.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static bool write_quoted(FILE *stream, const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;

    if (fputc('"', stream) == EOF) {
        return false;
    }
    while (*cursor != '\0') {
        switch (*cursor) {
            case '\\':
                if (fputs("\\\\", stream) == EOF) {
                    return false;
                }
                break;
            case '"':
                if (fputs("\\\"", stream) == EOF) {
                    return false;
                }
                break;
            case '\n':
                if (fputs("\\n", stream) == EOF) {
                    return false;
                }
                break;
            case '\r':
                if (fputs("\\r", stream) == EOF) {
                    return false;
                }
                break;
            case '\t':
                if (fputs("\\t", stream) == EOF) {
                    return false;
                }
                break;
            default:
                if (fputc((int)*cursor, stream) == EOF) {
                    return false;
                }
                break;
        }
        cursor += 1U;
    }
    return fputc('"', stream) != EOF;
}

static bool write_manifest_field(FILE *stream, const char *key, const char *value) {
    if (fprintf(stream, "%s: ", key) < 0) {
        return false;
    }
    if (!write_quoted(stream, value != NULL ? value : "")) {
        return false;
    }
    return fputc('\n', stream) != EOF;
}

static bool is_local_dependency_value(const char *value) {
    return value != NULL &&
           (strncmp(value, "./", 2U) == 0 || strncmp(value, "../", 3U) == 0 ||
            value[0] == '/');
}

static void set_error(FengCliProjectError *error,
                      const char *path,
                      unsigned int line,
                      const char *message) {
    if (error == NULL) {
        return;
    }
    feng_cli_project_error_dispose(error);
    error->path = path != NULL ? dup_cstr(path) : NULL;
    error->message = message != NULL ? dup_cstr(message) : NULL;
    error->line = line;
}

static bool assign_owned(char **slot,
                         const char *start,
                         const char *end,
                         const char *path,
                         unsigned int line,
                         bool allow_empty,
                         FengCliProjectError *error) {
    char *value;

    if (*slot != NULL) {
        set_error(error, path, line, "duplicate manifest field");
        return false;
    }
    if (!allow_empty && start == end) {
        set_error(error, path, line, "manifest field value must not be empty");
        return false;
    }

    value = dup_n(start, (size_t)(end - start));
    if (value == NULL) {
        set_error(error, path, line, "out of memory");
        return false;
    }
    *slot = value;
    return true;
}

static bool push_dependency(FengCliProjectManifest *manifest,
                            const char *name,
                            const char *value,
                            const char *path,
                            unsigned int line,
                            bool allow_local_path,
                            FengCliProjectError *error) {
    FengCliProjectManifestDependency *resized;
    FengCliProjectManifestDependency *dependency;
    size_t index;
    bool is_local_path;

    for (index = 0U; index < manifest->dependency_count; ++index) {
        if (strcmp(manifest->dependencies[index].name, name) == 0) {
            set_error(error, path, line, "duplicate dependency entry");
            return false;
        }
    }

    is_local_path = is_local_dependency_value(value);
    if (is_local_path && !allow_local_path) {
        set_error(error,
                  path,
                  line,
                  "bundle manifest dependencies must use exact versions, not local paths");
        return false;
    }

    resized = (FengCliProjectManifestDependency *)realloc(
        manifest->dependencies,
        (manifest->dependency_count + 1U) * sizeof(*manifest->dependencies));
    if (resized == NULL) {
        set_error(error, path, line, "out of memory");
        return false;
    }
    manifest->dependencies = resized;
    dependency = &manifest->dependencies[manifest->dependency_count];
    memset(dependency, 0, sizeof(*dependency));
    dependency->name = dup_cstr(name);
    dependency->value = dup_cstr(value);
    if (dependency->name == NULL || dependency->value == NULL) {
        free(dependency->name);
        free(dependency->value);
        dependency->name = NULL;
        dependency->value = NULL;
        set_error(error, path, line, "out of memory");
        return false;
    }
    dependency->line = line;
    dependency->is_local_path = is_local_path;
    manifest->dependency_count += 1U;
    return true;
}

static void set_error_from_fm(FengCliProjectError *out_error, const FengFmError *fm_error) {
    if (fm_error == NULL) {
        return;
    }
    set_error(out_error, fm_error->path, fm_error->line, fm_error->message);
}

typedef enum ManifestMode {
    MANIFEST_MODE_PROJECT = 0,
    MANIFEST_MODE_BUNDLE
} ManifestMode;

static bool parse_manifest(const char *manifest_path,
                           const char *source,
                           ManifestMode mode,
                           FengCliProjectManifest *out_manifest,
                           FengCliProjectError *out_error) {
    FengCliProjectManifest manifest = {0};
    FengFmDocument document = {0};
    FengFmError fm_error = {0};
    size_t section_index;
    size_t entry_index;

    if (out_manifest == NULL || source == NULL) {
        set_error(out_error, manifest_path, 0U, "invalid manifest parse request");
        return false;
    }

    manifest.target = FENG_COMPILE_TARGET_BIN;

    if (!feng_fm_parse(manifest_path, source, &document, &fm_error)) {
        set_error_from_fm(out_error, &fm_error);
        goto fail;
    }

    for (section_index = 0U; section_index < document.section_count; ++section_index) {
        const FengFmSection *section = &document.sections[section_index];

        if (strcmp(section->name, "package") != 0 && strcmp(section->name, "dependencies") != 0 &&
            strcmp(section->name, "registry") != 0) {
            set_error(out_error,
                      manifest_path,
                      section->line,
                      "unsupported manifest section");
            goto fail;
        }
        if (mode == MANIFEST_MODE_BUNDLE && strcmp(section->name, "registry") == 0) {
            set_error(out_error,
                      manifest_path,
                      section->line,
                      "bundle manifest must not contain [registry]");
            goto fail;
        }
    }

    for (entry_index = 0U; entry_index < document.entry_count; ++entry_index) {
        const FengFmEntry *entry = &document.entries[entry_index];

        if (strcmp(entry->section, "dependencies") == 0) {
            if (!push_dependency(&manifest,
                                 entry->key,
                                 entry->value,
                                 manifest_path,
                                 entry->line,
                                 mode == MANIFEST_MODE_PROJECT,
                                 out_error)) {
                goto fail;
            }
            continue;
        }
        if (strcmp(entry->section, "registry") == 0) {
            if (strcmp(entry->key, "url") != 0) {
                set_error(out_error,
                          manifest_path,
                          entry->line,
                          "unsupported manifest field");
                goto fail;
            }
            if (!assign_owned(&manifest.registry_url,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
            continue;
        }

        if (strcmp(entry->section, "package") != 0) {
            set_error(out_error,
                      manifest_path,
                      entry->line,
                      "unsupported manifest section");
            goto fail;
        }

        if (strcmp(entry->key, "name") == 0) {
            if (!assign_owned(&manifest.name,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "version") == 0) {
            if (!assign_owned(&manifest.version,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "target") == 0) {
            char *target_value = NULL;
            const char *value_end = entry->value + strlen(entry->value);

            if (mode == MANIFEST_MODE_BUNDLE) {
                set_error(out_error,
                          manifest_path,
                          entry->line,
                          "bundle manifest must not contain build-only package fields");
                goto fail;
            }

            if (!assign_owned(&target_value,
                              entry->value,
                              value_end,
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
            if (manifest.has_target) {
                free(target_value);
                set_error(out_error, manifest_path, entry->line, "duplicate manifest field");
                goto fail;
            }
            if (strcmp(target_value, "bin") == 0) {
                manifest.target = FENG_COMPILE_TARGET_BIN;
            } else if (strcmp(target_value, "lib") == 0) {
                manifest.target = FENG_COMPILE_TARGET_LIB;
            } else {
                free(target_value);
                set_error(out_error, manifest_path, entry->line, "target must be `bin` or `lib`");
                goto fail;
            }
            manifest.has_target = true;
            free(target_value);
        } else if (strcmp(entry->key, "src") == 0) {
            if (mode == MANIFEST_MODE_BUNDLE) {
                set_error(out_error,
                          manifest_path,
                          entry->line,
                          "bundle manifest must not contain build-only package fields");
                goto fail;
            }
            if (!assign_owned(&manifest.src_path,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "out") == 0) {
            if (mode == MANIFEST_MODE_BUNDLE) {
                set_error(out_error,
                          manifest_path,
                          entry->line,
                          "bundle manifest must not contain build-only package fields");
                goto fail;
            }
            if (!assign_owned(&manifest.out_path,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "arch") == 0) {
            if (!assign_owned(&manifest.arch,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              false,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "abi") == 0) {
            if (!assign_owned(&manifest.abi,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              true,
                              out_error)) {
                goto fail;
            }
        } else {
            set_error(out_error,
                      manifest_path,
                      entry->line,
                      "unsupported manifest field");
            goto fail;
        }
    }

    if (manifest.name == NULL) {
        set_error(out_error, manifest_path, 0U, "manifest requires `name` field");
        goto fail;
    }
    if (manifest.version == NULL) {
        set_error(out_error, manifest_path, 0U, "manifest requires `version` field");
        goto fail;
    }
    if (mode == MANIFEST_MODE_PROJECT && !manifest.has_target) {
        set_error(out_error, manifest_path, 0U, "manifest requires `target` field");
        goto fail;
    }
    if (mode == MANIFEST_MODE_PROJECT && manifest.src_path == NULL) {
        manifest.src_path = dup_cstr("src/");
    }
    if (mode == MANIFEST_MODE_PROJECT && manifest.out_path == NULL) {
        manifest.out_path = dup_cstr("build/");
    }
    if (mode == MANIFEST_MODE_PROJECT &&
        (manifest.src_path == NULL || manifest.out_path == NULL)) {
        set_error(out_error, manifest_path, 0U, "out of memory");
        goto fail;
    }

    *out_manifest = manifest;
    feng_fm_error_dispose(&fm_error);
    feng_fm_document_dispose(&document);
    return true;

fail:
    feng_fm_error_dispose(&fm_error);
    feng_fm_document_dispose(&document);
    feng_cli_project_manifest_dispose(&manifest);
    return false;
}

bool feng_cli_project_manifest_parse(const char *manifest_path,
                                     const char *source,
                                     FengCliProjectManifest *out_manifest,
                                     FengCliProjectError *out_error) {
    return parse_manifest(manifest_path,
                          source,
                          MANIFEST_MODE_PROJECT,
                          out_manifest,
                          out_error);
}

bool feng_cli_project_bundle_manifest_parse(const char *manifest_path,
                                            const char *source,
                                            FengCliProjectManifest *out_manifest,
                                            FengCliProjectError *out_error) {
    return parse_manifest(manifest_path,
                          source,
                          MANIFEST_MODE_BUNDLE,
                          out_manifest,
                          out_error);
}

bool feng_cli_project_manifest_write(const char *manifest_path,
                                     const FengCliProjectManifest *manifest,
                                     char **out_error_message) {
    FILE *stream;
    size_t index;

    if (manifest_path == NULL || manifest == NULL) {
        if (out_error_message != NULL) {
            *out_error_message = dup_cstr("invalid manifest write request");
        }
        return false;
    }

    stream = fopen(manifest_path, "wb");
    if (stream == NULL) {
        if (out_error_message != NULL) {
            *out_error_message = dup_printf("failed to open %s: %s", manifest_path, strerror(errno));
        }
        return false;
    }

    if (fputs("[package]\n", stream) == EOF ||
        !write_manifest_field(stream, "name", manifest->name) ||
        !write_manifest_field(stream, "version", manifest->version) ||
        (manifest->has_target &&
         !write_manifest_field(stream,
                               "target",
                               manifest->target == FENG_COMPILE_TARGET_LIB ? "lib" : "bin")) ||
        (manifest->src_path != NULL && !write_manifest_field(stream, "src", manifest->src_path)) ||
        (manifest->out_path != NULL && !write_manifest_field(stream, "out", manifest->out_path)) ||
        (manifest->arch != NULL && !write_manifest_field(stream, "arch", manifest->arch)) ||
        (manifest->abi != NULL && !write_manifest_field(stream, "abi", manifest->abi))) {
        fclose(stream);
        if (out_error_message != NULL) {
            *out_error_message = dup_printf("failed to write %s", manifest_path);
        }
        return false;
    }

    if (manifest->dependency_count > 0U) {
        if (fputc('\n', stream) == EOF || fputs("[dependencies]\n", stream) == EOF) {
            fclose(stream);
            if (out_error_message != NULL) {
                *out_error_message = dup_printf("failed to write %s", manifest_path);
            }
            return false;
        }
        for (index = 0U; index < manifest->dependency_count; ++index) {
            if (!write_manifest_field(stream,
                                      manifest->dependencies[index].name,
                                      manifest->dependencies[index].value)) {
                fclose(stream);
                if (out_error_message != NULL) {
                    *out_error_message = dup_printf("failed to write %s", manifest_path);
                }
                return false;
            }
        }
    }

    if (manifest->registry_url != NULL) {
        if (fputc('\n', stream) == EOF || fputs("[registry]\n", stream) == EOF ||
            !write_manifest_field(stream, "url", manifest->registry_url)) {
            fclose(stream);
            if (out_error_message != NULL) {
                *out_error_message = dup_printf("failed to write %s", manifest_path);
            }
            return false;
        }
    }

    if (fclose(stream) != 0) {
        if (out_error_message != NULL) {
            *out_error_message = dup_printf("failed to flush %s: %s", manifest_path, strerror(errno));
        }
        return false;
    }
    return true;
}

void feng_cli_project_manifest_dispose(FengCliProjectManifest *manifest) {
    size_t index;

    if (manifest == NULL) {
        return;
    }
    free(manifest->name);
    free(manifest->version);
    free(manifest->src_path);
    free(manifest->out_path);
    free(manifest->arch);
    free(manifest->abi);
    free(manifest->registry_url);
    for (index = 0U; index < manifest->dependency_count; ++index) {
        free(manifest->dependencies[index].name);
        free(manifest->dependencies[index].value);
    }
    free(manifest->dependencies);
    manifest->name = NULL;
    manifest->version = NULL;
    manifest->has_target = false;
    manifest->src_path = NULL;
    manifest->out_path = NULL;
    manifest->arch = NULL;
    manifest->abi = NULL;
    manifest->registry_url = NULL;
    manifest->dependencies = NULL;
    manifest->dependency_count = 0U;
}

void feng_cli_project_error_dispose(FengCliProjectError *error) {
    if (error == NULL) {
        return;
    }
    free(error->path);
    free(error->message);
    error->path = NULL;
    error->message = NULL;
    error->line = 0U;
}
