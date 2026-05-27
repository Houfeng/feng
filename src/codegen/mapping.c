#include "codegen/mapping.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *mapping_dup_n(const char *text, size_t length) {
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}


static char *mapping_dup_cstr(const char *text) {
    return text != NULL ? mapping_dup_n(text, strlen(text)) : NULL;
}

static char *mapping_dup_printf(const char *fmt, ...) {
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

static bool mapping_append_raw(void **items,
                               size_t *count,
                               size_t *capacity,
                               size_t item_size,
                               const void *value) {
    void *grown;
    size_t new_capacity;

    if (*count == *capacity) {
        new_capacity = *capacity == 0U ? 8U : (*capacity * 2U);
        grown = realloc(*items, new_capacity * item_size);
        if (grown == NULL) {
            return false;
        }
        *items = grown;
        *capacity = new_capacity;
    }
    memcpy((unsigned char *)(*items) + (*count * item_size), value, item_size);
    ++(*count);
    return true;
}

static bool mapping_cstr_equals(const char *lhs, const char *rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    return strcmp(lhs, rhs) == 0;
}

static bool mapping_relative_path_from_root(const char *root,
                                            const char *path,
                                            char **out_relative) {
    size_t root_length;
    const char *relative_start;

    *out_relative = NULL;
    if (root == NULL || path == NULL) {
        return false;
    }

    root_length = strlen(root);
    while (root_length > 1U && root[root_length - 1U] == '/') {
        root_length--;
    }
    if (root_length == 0U) {
        return false;
    }
    if (strncmp(path, root, root_length) != 0) {
        return false;
    }
    if (root_length == 1U && root[0] == '/') {
        relative_start = path[0] == '/' ? path + 1 : path;
    } else {
        if (path[root_length] != '/') {
            return false;
        }
        relative_start = path + root_length + 1U;
    }
    while (*relative_start == '/') {
        ++relative_start;
    }
    if (*relative_start == '\0') {
        return false;
    }

    *out_relative = mapping_dup_cstr(relative_start);
    return *out_relative != NULL;
}

static void mapping_frame_record_dispose(FengCodegenMapingFrameRecord *frame) {
    free(frame->backend_symbol);
    free(frame->display_name);
    frame->backend_symbol = NULL;
    frame->display_name = NULL;
}

static void mapping_variable_record_dispose(FengCodegenMapingVariableRecord *variable) {
    free(variable->frame_backend_symbol);
    free(variable->backend_name);
    free(variable->display_name);
    free(variable->read_expr);
    free(variable->display_type);
    free(variable->parent_display_type);
    variable->frame_backend_symbol = NULL;
    variable->backend_name = NULL;
    variable->display_name = NULL;
    variable->read_expr = NULL;
    variable->display_type = NULL;
    variable->parent_display_type = NULL;
}

bool feng_codegen_maping_resolve_source(const FengCodegenMapingSourceMapping *sources,
                               size_t source_count,
                               const char *source_path,
                               FengCodegenMapingResolvedSource *out_resolved) {
    size_t index;

    if (out_resolved == NULL) {
        return false;
    }
    memset(out_resolved, 0, sizeof(*out_resolved));
    if (sources == NULL || source_path == NULL) {
        return false;
    }

    for (index = 0U; index < source_count; ++index) {
        char *relative_path = NULL;

        if (sources[index].source_path == NULL ||
            strcmp(sources[index].source_path, source_path) != 0) {
            continue;
        }
        if (sources[index].package_name == NULL ||
            sources[index].package_root == NULL ||
            !mapping_relative_path_from_root(sources[index].package_root,
                                             source_path,
                                             &relative_path)) {
            return false;
        }

        out_resolved->package_name = mapping_dup_cstr(sources[index].package_name);
        out_resolved->package_root = mapping_dup_cstr(sources[index].package_root);
        out_resolved->relative_path = relative_path;
        out_resolved->logical_uri = mapping_dup_printf("%s://%s",
                                                       sources[index].package_name,
                                                       relative_path);
        if (out_resolved->package_name == NULL ||
            out_resolved->package_root == NULL ||
            out_resolved->logical_uri == NULL) {
            feng_codegen_maping_resolved_source_dispose(out_resolved);
            return false;
        }
        return true;
    }
    return false;
}

void feng_codegen_maping_resolved_source_dispose(FengCodegenMapingResolvedSource *resolved) {
    if (resolved == NULL) {
        return;
    }
    free(resolved->package_name);
    free(resolved->package_root);
    free(resolved->relative_path);
    free(resolved->logical_uri);
    resolved->package_name = NULL;
    resolved->package_root = NULL;
    resolved->relative_path = NULL;
    resolved->logical_uri = NULL;
}

void feng_codegen_maping_info_init(FengCodegenMapingInfo *info) {
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
}

void feng_codegen_maping_info_dispose(FengCodegenMapingInfo *info) {
    size_t index;

    if (info == NULL) {
        return;
    }
    for (index = 0U; index < info->frame_count; ++index) {
        mapping_frame_record_dispose(&info->frames[index]);
    }
    for (index = 0U; index < info->variable_count; ++index) {
        mapping_variable_record_dispose(&info->variables[index]);
    }
    free(info->frames);
    free(info->variables);
    memset(info, 0, sizeof(*info));
}

bool feng_codegen_maping_info_add_frame(FengCodegenMapingInfo *info,
                               const char *backend_symbol,
                               const char *display_name,
                               FengCodegenMapingFramePolicy policy) {
    FengCodegenMapingFrameRecord entry;
    size_t index;

    if (info == NULL || backend_symbol == NULL || display_name == NULL) {
        return false;
    }
    for (index = 0U; index < info->frame_count; ++index) {
        if (strcmp(info->frames[index].backend_symbol, backend_symbol) != 0) {
            continue;
        }
        return strcmp(info->frames[index].display_name, display_name) == 0 &&
               info->frames[index].policy == policy;
    }

    entry.backend_symbol = mapping_dup_cstr(backend_symbol);
    entry.display_name = mapping_dup_cstr(display_name);
    entry.policy = policy;
    if (entry.backend_symbol == NULL ||
        entry.display_name == NULL ||
        !mapping_append_raw((void **)&info->frames,
                            &info->frame_count,
                            &info->frame_capacity,
                            sizeof(entry),
                            &entry)) {
        mapping_frame_record_dispose(&entry);
        return false;
    }
    return true;
}

bool feng_codegen_maping_info_add_variable_with_display_type(
    FengCodegenMapingInfo *info,
    const char *frame_backend_symbol,
    const char *backend_name,
    const char *display_name,
    const char *read_expr,
    const char *display_type,
    FengCodegenMapingVariableKind kind) {
    return feng_codegen_maping_info_add_variable_with_parent_display_type(info,
                                                                          frame_backend_symbol,
                                                                          backend_name,
                                                                          display_name,
                                                                          read_expr,
                                                                          display_type,
                                                                          NULL,
                                                                          kind);
}

bool feng_codegen_maping_info_add_variable_with_parent_display_type(
    FengCodegenMapingInfo *info,
    const char *frame_backend_symbol,
    const char *backend_name,
    const char *display_name,
    const char *read_expr,
    const char *display_type,
    const char *parent_display_type,
    FengCodegenMapingVariableKind kind) {
    FengCodegenMapingVariableRecord entry;
    size_t index;

    memset(&entry, 0, sizeof(entry));

    if (info == NULL || display_name == NULL || display_type == NULL ||
        display_type[0] == '\0') {
        return false;
    }
    if (kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD) {
        if (frame_backend_symbol != NULL || backend_name != NULL || read_expr == NULL ||
            read_expr[0] == '\0' || parent_display_type == NULL ||
            parent_display_type[0] == '\0') {
            return false;
        }
    } else if (frame_backend_symbol == NULL || parent_display_type != NULL) {
        return false;
    }
    for (index = 0U; index < info->variable_count; ++index) {
        if (kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD) {
            if (info->variables[index].kind != FENG_CODEGEN_MAPING_VARIABLE_FIELD ||
                !mapping_cstr_equals(info->variables[index].parent_display_type,
                                     parent_display_type) ||
                strcmp(info->variables[index].display_name, display_name) != 0) {
                continue;
            }
            return info->variables[index].frame_backend_symbol == NULL &&
                   info->variables[index].backend_name == NULL &&
                   mapping_cstr_equals(info->variables[index].read_expr, read_expr) &&
                   strcmp(info->variables[index].display_type, display_type) == 0;
        }
        if (info->variables[index].kind == FENG_CODEGEN_MAPING_VARIABLE_FIELD) {
            continue;
        }
        if (strcmp(info->variables[index].frame_backend_symbol,
                   frame_backend_symbol) != 0 ||
            !mapping_cstr_equals(info->variables[index].backend_name, backend_name)) {
            continue;
        }
        if (backend_name == NULL &&
            strcmp(info->variables[index].display_name, display_name) != 0) {
            continue;
        }
        if (strcmp(info->variables[index].display_name, display_name) != 0 ||
            !mapping_cstr_equals(info->variables[index].read_expr, read_expr) ||
            !mapping_cstr_equals(info->variables[index].parent_display_type, parent_display_type) ||
            info->variables[index].kind != kind) {
            return false;
        }
        if (strcmp(info->variables[index].display_type, display_type) != 0) {
            return false;
        }
        return true;
    }

    entry.frame_backend_symbol = mapping_dup_cstr(frame_backend_symbol);
    entry.backend_name = mapping_dup_cstr(backend_name);
    entry.display_name = mapping_dup_cstr(display_name);
    entry.read_expr = mapping_dup_cstr(read_expr);
    entry.display_type = mapping_dup_cstr(display_type);
    entry.parent_display_type = mapping_dup_cstr(parent_display_type);
    entry.kind = kind;
    if ((kind != FENG_CODEGEN_MAPING_VARIABLE_FIELD && entry.frame_backend_symbol == NULL) ||
        entry.display_name == NULL ||
        entry.display_type == NULL ||
        (parent_display_type != NULL && entry.parent_display_type == NULL) ||
        !mapping_append_raw((void **)&info->variables,
                            &info->variable_count,
                            &info->variable_capacity,
                            sizeof(entry),
                            &entry)) {
        mapping_variable_record_dispose(&entry);
        return false;
    }
    return true;
}
