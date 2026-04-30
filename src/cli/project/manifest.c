#include "cli/project/manifest.h"

#include <ctype.h>
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

static void trim_slice(const char **start, const char **end) {
    while (*start < *end && isspace((unsigned char)**start)) {
        (*start)++;
    }
    while (*end > *start && isspace((unsigned char)*((*end) - 1))) {
        (*end)--;
    }
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

bool feng_cli_project_manifest_parse(const char *manifest_path,
                                     const char *source,
                                     FengCliProjectManifest *out_manifest,
                                     FengCliProjectError *out_error) {
    const char *cursor = source;
    unsigned int line = 1U;
    FengCliProjectManifest manifest = {0};
    bool target_seen = false;

    if (out_manifest == NULL || source == NULL) {
        set_error(out_error, manifest_path, 0U, "invalid manifest parse request");
        return false;
    }

    manifest.target = FENG_COMPILE_TARGET_BIN;

    while (*cursor != '\0') {
        const char *line_start = cursor;
        const char *line_end = cursor;
        const char *key_start;
        const char *key_end;
        const char *value_start;
        const char *value_end;
        const char *colon;

        while (*line_end != '\0' && *line_end != '\n') {
            line_end++;
        }
        cursor = *line_end == '\n' ? line_end + 1 : line_end;

        key_start = line_start;
        key_end = line_end;
        trim_slice(&key_start, &key_end);
        if (key_start == key_end) {
            if (*line_end == '\n') {
                line++;
            }
            continue;
        }

        colon = key_start;
        while (colon < key_end && *colon != ':') {
            colon++;
        }
        if (colon == key_end) {
            set_error(out_error, manifest_path, line, "manifest line must be `key:value`");
            goto fail;
        }

        key_end = colon;
        value_start = colon + 1;
        value_end = line_end;
        trim_slice(&key_start, &key_end);
        trim_slice(&value_start, &value_end);

        if ((size_t)(key_end - key_start) == 4U && strncmp(key_start, "name", 4U) == 0) {
            if (!assign_owned(&manifest.name, value_start, value_end, manifest_path, line, out_error)) {
                goto fail;
            }
        } else if ((size_t)(key_end - key_start) == 7U && strncmp(key_start, "version", 7U) == 0) {
            if (!assign_owned(&manifest.version, value_start, value_end, manifest_path, line, out_error)) {
                goto fail;
            }
        } else if ((size_t)(key_end - key_start) == 6U && strncmp(key_start, "target", 6U) == 0) {
            char *target_value = NULL;

            if (!assign_owned(&target_value, value_start, value_end, manifest_path, line, out_error)) {
                goto fail;
            }
            if (target_seen) {
                free(target_value);
                set_error(out_error, manifest_path, line, "duplicate manifest field");
                goto fail;
            }
            if (strcmp(target_value, "bin") == 0) {
                manifest.target = FENG_COMPILE_TARGET_BIN;
            } else if (strcmp(target_value, "lib") == 0) {
                manifest.target = FENG_COMPILE_TARGET_LIB;
            } else {
                free(target_value);
                set_error(out_error, manifest_path, line, "target must be `bin` or `lib`");
                goto fail;
            }
            target_seen = true;
            free(target_value);
        } else if ((size_t)(key_end - key_start) == 3U && strncmp(key_start, "src", 3U) == 0) {
            if (!assign_owned(&manifest.src_path, value_start, value_end, manifest_path, line, out_error)) {
                goto fail;
            }
        } else if ((size_t)(key_end - key_start) == 3U && strncmp(key_start, "out", 3U) == 0) {
            if (!assign_owned(&manifest.out_path, value_start, value_end, manifest_path, line, out_error)) {
                goto fail;
            }
        } else {
            set_error(out_error, manifest_path, line, "unsupported manifest field in current Phase 3 slice");
            goto fail;
        }

        if (*line_end == '\n') {
            line++;
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
    return true;

fail:
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
