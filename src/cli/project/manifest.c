#include "cli/project/manifest.h"

#include "archive/fm.h"

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
                         FengCliProjectError *error) {
    char *value;

    if (*slot != NULL) {
        set_error(error, path, line, "duplicate manifest field");
        return false;
    }
    if (start == end) {
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

static void set_error_from_fm(FengCliProjectError *out_error, const FengFmError *fm_error) {
    if (fm_error == NULL) {
        return;
    }
    set_error(out_error, fm_error->path, fm_error->line, fm_error->message);
}

bool feng_cli_project_manifest_parse(const char *manifest_path,
                                     const char *source,
                                     FengCliProjectManifest *out_manifest,
                                     FengCliProjectError *out_error) {
    FengCliProjectManifest manifest = {0};
    FengFmDocument document = {0};
    FengFmError fm_error = {0};
    size_t section_index;
    size_t entry_index;
    bool target_seen = false;

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

        if (strcmp(section->name, "package") != 0 && strcmp(section->name, "dependencies") != 0) {
            set_error(out_error,
                      manifest_path,
                      section->line,
                      "unsupported manifest section in current Phase 3 slice");
            goto fail;
        }
    }

    for (entry_index = 0U; entry_index < document.entry_count; ++entry_index) {
        const FengFmEntry *entry = &document.entries[entry_index];

        if (strcmp(entry->section, "dependencies") == 0) {
            continue;
        }
        if (strcmp(entry->section, "package") != 0) {
            set_error(out_error,
                      manifest_path,
                      entry->line,
                      "unsupported manifest section in current Phase 3 slice");
            goto fail;
        }

        if (strcmp(entry->key, "name") == 0) {
            if (!assign_owned(&manifest.name,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "version") == 0) {
            if (!assign_owned(&manifest.version,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "target") == 0) {
            char *target_value = NULL;
            const char *value_end = entry->value + strlen(entry->value);

            if (!assign_owned(&target_value,
                              entry->value,
                              value_end,
                              manifest_path,
                              entry->line,
                              out_error)) {
                goto fail;
            }
            if (target_seen) {
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
            target_seen = true;
            free(target_value);
        } else if (strcmp(entry->key, "src") == 0) {
            if (!assign_owned(&manifest.src_path,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              out_error)) {
                goto fail;
            }
        } else if (strcmp(entry->key, "out") == 0) {
            if (!assign_owned(&manifest.out_path,
                              entry->value,
                              entry->value + strlen(entry->value),
                              manifest_path,
                              entry->line,
                              out_error)) {
                goto fail;
            }
        } else {
            set_error(out_error,
                      manifest_path,
                      entry->line,
                      "unsupported manifest field in current Phase 3 slice");
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
    if (!target_seen) {
        set_error(out_error, manifest_path, 0U, "manifest requires `target` field");
        goto fail;
    }
    if (manifest.src_path == NULL) {
        manifest.src_path = dup_cstr("src/");
    }
    if (manifest.out_path == NULL) {
        manifest.out_path = dup_cstr("build/");
    }
    if (manifest.src_path == NULL || manifest.out_path == NULL) {
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

void feng_cli_project_manifest_dispose(FengCliProjectManifest *manifest) {
    if (manifest == NULL) {
        return;
    }
    free(manifest->name);
    free(manifest->version);
    free(manifest->src_path);
    free(manifest->out_path);
    manifest->name = NULL;
    manifest->version = NULL;
    manifest->src_path = NULL;
    manifest->out_path = NULL;
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
