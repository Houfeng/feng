#include "debug/debug.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Buffer used to assemble binary .fd sections before writing them to disk. */
typedef struct FengDebugBuffer {
    unsigned char *data;
    size_t length;
    size_t capacity;
} FengDebugBuffer;

/* One interned string entry used by the .fd string table. */
typedef struct FengDebugStringEntry {
    char *text;
    uint32_t id;
} FengDebugStringEntry;

/* In-memory string interner for .fd serialization. */
typedef struct FengDebugStringTable {
    FengDebugStringEntry *items;
    size_t count;
    size_t capacity;
} FengDebugStringTable;

/* Parsed metadata for one .fd section directory entry. */
typedef struct FengDebugParsedSection {
    const unsigned char *data;
    size_t size;
    uint32_t record_count;
    bool present;
} FengDebugParsedSection;

/* Duplicates a byte range as a NUL-terminated string. */
static char *debug_dup_n(const char *text, size_t length) {
    char *out = (char *)malloc(length + 1U);

    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length);
    out[length] = '\0';
    return out;
}

/* Duplicates a C string. */
static char *debug_dup_cstr(const char *text) {
    return text != NULL ? debug_dup_n(text, strlen(text)) : NULL;
}

/* Formats a heap-owned string. */
static char *debug_dup_printf(const char *fmt, ...) {
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

/* Replaces the optional caller-owned error string. */
static void debug_set_error(char **out_error_message, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message = NULL;

    if (out_error_message == NULL) {
        return;
    }

    free(*out_error_message);
    *out_error_message = NULL;

    va_start(args, fmt);
    va_copy(args_copy, args);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed >= 0) {
        message = (char *)malloc((size_t)needed + 1U);
        if (message != NULL) {
            vsnprintf(message, (size_t)needed + 1U, fmt, args_copy);
        }
    }
    va_end(args_copy);
    *out_error_message = message;
}

/* Appends one item to a raw growable array. */
static bool debug_append_raw(void **items,
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

/* Appends raw bytes to a growable serialization buffer. */
static bool debug_buffer_append(FengDebugBuffer *buffer,
                                const void *data,
                                size_t length) {
    unsigned char *grown;
    size_t need;
    size_t capacity;

    if (length == 0U) {
        return true;
    }
    if (buffer->length > (size_t)-1 - length) {
        return false;
    }
    need = buffer->length + length;
    if (need <= buffer->capacity) {
        memcpy(buffer->data + buffer->length, data, length);
        buffer->length = need;
        return true;
    }

    capacity = buffer->capacity == 0U ? 128U : buffer->capacity;
    while (capacity < need) {
        if (capacity > (size_t)-1 / 2U) {
            return false;
        }
        capacity *= 2U;
    }
    grown = (unsigned char *)realloc(buffer->data, capacity);
    if (grown == NULL) {
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length = need;
    return true;
}

/* Appends one 16-bit little-endian integer to a serialization buffer. */
static bool debug_buffer_append_u16_le(FengDebugBuffer *buffer, uint16_t value) {
    unsigned char bytes[2];

    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    return debug_buffer_append(buffer, bytes, sizeof(bytes));
}

/* Appends one 32-bit little-endian integer to a serialization buffer. */
static bool debug_buffer_append_u32_le(FengDebugBuffer *buffer, uint32_t value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
    return debug_buffer_append(buffer, bytes, sizeof(bytes));
}

/* Appends one 64-bit little-endian integer to a serialization buffer. */
static bool debug_buffer_append_u64_le(FengDebugBuffer *buffer, uint64_t value) {
    unsigned char bytes[8];

    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
    bytes[4] = (unsigned char)((value >> 32U) & 0xffU);
    bytes[5] = (unsigned char)((value >> 40U) & 0xffU);
    bytes[6] = (unsigned char)((value >> 48U) & 0xffU);
    bytes[7] = (unsigned char)((value >> 56U) & 0xffU);
    return debug_buffer_append(buffer, bytes, sizeof(bytes));
}

/* Releases a growable serialization buffer. */
static void debug_buffer_dispose(FengDebugBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

/* Reads a little-endian 16-bit integer from a memory-mapped .fd blob. */
static uint16_t debug_read_u16_le(const unsigned char *data) {
    return (uint16_t)data[0] |
           (uint16_t)((uint16_t)data[1] << 8U);
}

/* Reads a little-endian 32-bit integer from a memory-mapped .fd blob. */
static uint32_t debug_read_u32_le(const unsigned char *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

/* Reads a little-endian 64-bit integer from a memory-mapped .fd blob. */
static uint64_t debug_read_u64_le(const unsigned char *data) {
    return (uint64_t)data[0] |
           ((uint64_t)data[1] << 8U) |
           ((uint64_t)data[2] << 16U) |
           ((uint64_t)data[3] << 24U) |
           ((uint64_t)data[4] << 32U) |
           ((uint64_t)data[5] << 40U) |
           ((uint64_t)data[6] << 48U) |
           ((uint64_t)data[7] << 56U);
}

/* Returns true when two optional strings are equal. */
static bool debug_cstr_equals(const char *lhs, const char *rhs) {
    if (lhs == rhs) {
        return true;
    }
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    return strcmp(lhs, rhs) == 0;
}

/* Derives a package-relative source path from one package root. */
static bool debug_relative_path_from_root(const char *root,
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
    if (*relative_start == '\0') {
        return false;
    }

    *out_relative = debug_dup_cstr(relative_start);
    return *out_relative != NULL;
}

/* Releases one frame record. */
static void debug_frame_record_dispose(FengDebugFrameRecord *frame) {
    free(frame->backend_symbol);
    free(frame->display_name);
    frame->backend_symbol = NULL;
    frame->display_name = NULL;
}

/* Releases one variable record. */
static void debug_variable_record_dispose(FengDebugVariableRecord *variable) {
    free(variable->frame_backend_symbol);
    free(variable->backend_name);
    free(variable->display_name);
    free(variable->read_expr);
    variable->frame_backend_symbol = NULL;
    variable->backend_name = NULL;
    variable->display_name = NULL;
    variable->read_expr = NULL;
}

/* Releases one artifact package mapping. */
static void debug_artifact_package_dispose(FengDebugArtifactPackage *package) {
    free(package->package_name);
    free(package->local_root_path);
    package->package_name = NULL;
    package->local_root_path = NULL;
}

/* Releases all strings in the serialization string table. */
static void debug_string_table_dispose(FengDebugStringTable *table) {
    size_t index;

    for (index = 0U; index < table->count; ++index) {
        free(table->items[index].text);
    }
    free(table->items);
    table->items = NULL;
    table->count = 0U;
    table->capacity = 0U;
}

/* Interns one optional string for .fd serialization. */
static bool debug_string_table_intern(FengDebugStringTable *table,
                                      const char *text,
                                      uint32_t *out_id) {
    FengDebugStringEntry entry;
    size_t index;

    if (out_id == NULL) {
        return false;
    }
    if (text == NULL) {
        *out_id = 0U;
        return true;
    }
    for (index = 0U; index < table->count; ++index) {
        if (strcmp(table->items[index].text, text) == 0) {
            *out_id = table->items[index].id;
            return true;
        }
    }
    if (table->count >= UINT32_MAX) {
        return false;
    }
    entry.text = debug_dup_cstr(text);
    entry.id = (uint32_t)table->count + 1U;
    if (entry.text == NULL ||
        !debug_append_raw((void **)&table->items,
                          &table->count,
                          &table->capacity,
                          sizeof(entry),
                          &entry)) {
        free(entry.text);
        return false;
    }
    *out_id = entry.id;
    return true;
}

/* Builds the .fd string section from the interned string table. */
static bool debug_build_strings_section(const FengDebugStringTable *table,
                                        FengDebugBuffer *buffer) {
    size_t index;

    for (index = 0U; index < table->count; ++index) {
        size_t length = strlen(table->items[index].text);

        if (length > UINT32_MAX) {
            return false;
        }
        if (!debug_buffer_append_u32_le(buffer, (uint32_t)length) ||
            !debug_buffer_append(buffer, table->items[index].text, length)) {
            return false;
        }
    }
    return true;
}

/* Collects unique package-root mappings from source inputs. */
static bool debug_collect_packages(const FengDebugSourceMapping *sources,
                                   size_t source_count,
                                   FengDebugArtifactPackage **out_packages,
                                   size_t *out_package_count,
                                   char **out_error_message) {
    FengDebugArtifactPackage *packages = NULL;
    size_t package_count = 0U;
    size_t package_capacity = 0U;
    size_t source_index;

    *out_packages = NULL;
    *out_package_count = 0U;

    for (source_index = 0U; source_index < source_count; ++source_index) {
        size_t package_index;
        FengDebugArtifactPackage package_entry;

        if (sources[source_index].package_name == NULL ||
            sources[source_index].package_root == NULL) {
            debug_set_error(out_error_message,
                            "debug source mapping for '%s' is missing package metadata",
                            sources[source_index].source_path != NULL
                                ? sources[source_index].source_path
                                : "(unknown)");
            goto fail;
        }
        for (package_index = 0U; package_index < package_count; ++package_index) {
            if (strcmp(packages[package_index].package_name,
                       sources[source_index].package_name) != 0) {
                continue;
            }
            if (strcmp(packages[package_index].local_root_path,
                       sources[source_index].package_root) != 0) {
                debug_set_error(out_error_message,
                                "package '%s' maps to multiple local roots",
                                sources[source_index].package_name);
                goto fail;
            }
            break;
        }
        if (package_index < package_count) {
            continue;
        }

        package_entry.package_name = debug_dup_cstr(sources[source_index].package_name);
        package_entry.local_root_path = debug_dup_cstr(sources[source_index].package_root);
        if (package_entry.package_name == NULL ||
            package_entry.local_root_path == NULL ||
            !debug_append_raw((void **)&packages,
                              &package_count,
                              &package_capacity,
                              sizeof(package_entry),
                              &package_entry)) {
            debug_artifact_package_dispose(&package_entry);
            debug_set_error(out_error_message, "out of memory collecting package mappings");
            goto fail;
        }
    }

    *out_packages = packages;
    *out_package_count = package_count;
    return true;

fail:
    while (package_count > 0U) {
        package_count--;
        debug_artifact_package_dispose(&packages[package_count]);
    }
    free(packages);
    return false;
}

/* Serializes the fixed metadata section. */
static bool debug_build_meta_section(FengDebugBuffer *buffer,
                                     uint32_t binary_path_id,
                                     uint64_t fingerprint) {
    return debug_buffer_append_u32_le(buffer, binary_path_id) &&
           debug_buffer_append_u32_le(buffer, 0U) &&
           debug_buffer_append_u64_le(buffer, fingerprint);
}

/* Serializes the package-root mapping section. */
static bool debug_build_packages_section(FengDebugBuffer *buffer,
                                         const FengDebugArtifactPackage *packages,
                                         size_t package_count,
                                         FengDebugStringTable *strings) {
    size_t index;

    for (index = 0U; index < package_count; ++index) {
        uint32_t package_name_id;
        uint32_t local_root_id;

        if (!debug_string_table_intern(strings, packages[index].package_name, &package_name_id) ||
            !debug_string_table_intern(strings, packages[index].local_root_path, &local_root_id) ||
            !debug_buffer_append_u32_le(buffer, package_name_id) ||
            !debug_buffer_append_u32_le(buffer, local_root_id)) {
            return false;
        }
    }
    return true;
}

/* Serializes the frame mapping section. */
static bool debug_build_frames_section(FengDebugBuffer *buffer,
                                       const FengDebugInfo *info,
                                       FengDebugStringTable *strings) {
    size_t index;

    if (info == NULL) {
        return true;
    }
    for (index = 0U; index < info->frame_count; ++index) {
        uint32_t backend_id;
        uint32_t display_id;

        if (!debug_string_table_intern(strings, info->frames[index].backend_symbol, &backend_id) ||
            !debug_string_table_intern(strings, info->frames[index].display_name, &display_id) ||
            !debug_buffer_append_u32_le(buffer, backend_id) ||
            !debug_buffer_append_u32_le(buffer, display_id) ||
            !debug_buffer_append_u32_le(buffer, (uint32_t)info->frames[index].policy)) {
            return false;
        }
    }
    return true;
}

/* Serializes the variable mapping section. */
static bool debug_build_variables_section(FengDebugBuffer *buffer,
                                          const FengDebugInfo *info,
                                          FengDebugStringTable *strings) {
    size_t index;

    if (info == NULL) {
        return true;
    }
    for (index = 0U; index < info->variable_count; ++index) {
        uint32_t frame_id;
        uint32_t backend_id;
        uint32_t display_id;
        uint32_t read_expr_id;

        if (!debug_string_table_intern(strings,
                                       info->variables[index].frame_backend_symbol,
                                       &frame_id) ||
            !debug_string_table_intern(strings,
                                       info->variables[index].backend_name,
                                       &backend_id) ||
            !debug_string_table_intern(strings,
                                       info->variables[index].display_name,
                                       &display_id) ||
            !debug_string_table_intern(strings,
                                       info->variables[index].read_expr,
                                       &read_expr_id) ||
            !debug_buffer_append_u32_le(buffer, frame_id) ||
            !debug_buffer_append_u32_le(buffer, backend_id) ||
            !debug_buffer_append_u32_le(buffer, display_id) ||
            !debug_buffer_append_u32_le(buffer, read_expr_id) ||
            !debug_buffer_append_u32_le(buffer, (uint32_t)info->variables[index].kind)) {
            return false;
        }
    }
    return true;
}

/* Writes one raw serialized section entry into the .fd directory. */
static bool debug_write_section_entry(FILE *file,
                                      const char tag[4],
                                      uint64_t offset,
                                      uint64_t size,
                                      uint32_t record_count) {
    FengDebugBuffer entry = {0};
    bool ok = debug_buffer_append(&entry, tag, 4U) &&
              debug_buffer_append_u32_le(&entry, 0U) &&
              debug_buffer_append_u64_le(&entry, offset) &&
              debug_buffer_append_u64_le(&entry, size) &&
              debug_buffer_append_u32_le(&entry, record_count) &&
              debug_buffer_append_u32_le(&entry, 0U) &&
              fwrite(entry.data, 1U, entry.length, file) == entry.length;

    debug_buffer_dispose(&entry);
    return ok;
}

/* Writes one fully-built serialization buffer to disk. */
static bool debug_write_buffer(FILE *file, const FengDebugBuffer *buffer) {
    return buffer->length == 0U ||
           fwrite(buffer->data, 1U, buffer->length, file) == buffer->length;
}

/* Interns every string referenced by the final .fd payload. */
static bool debug_collect_strings(FengDebugStringTable *strings,
                                  const char *binary_path,
                                  const FengDebugArtifactPackage *packages,
                                  size_t package_count,
                                  const FengDebugInfo *info,
                                  uint32_t *out_binary_path_id) {
    size_t index;

    if (!debug_string_table_intern(strings, binary_path, out_binary_path_id)) {
        return false;
    }
    for (index = 0U; index < package_count; ++index) {
        uint32_t ignored;

        if (!debug_string_table_intern(strings, packages[index].package_name, &ignored) ||
            !debug_string_table_intern(strings, packages[index].local_root_path, &ignored)) {
            return false;
        }
    }
    if (info == NULL) {
        return true;
    }
    for (index = 0U; index < info->frame_count; ++index) {
        uint32_t ignored;

        if (!debug_string_table_intern(strings, info->frames[index].backend_symbol, &ignored) ||
            !debug_string_table_intern(strings, info->frames[index].display_name, &ignored)) {
            return false;
        }
    }
    for (index = 0U; index < info->variable_count; ++index) {
        uint32_t ignored;

        if (!debug_string_table_intern(strings,
                                       info->variables[index].frame_backend_symbol,
                                       &ignored) ||
            !debug_string_table_intern(strings,
                                       info->variables[index].backend_name,
                                       &ignored) ||
            !debug_string_table_intern(strings,
                                       info->variables[index].display_name,
                                       &ignored) ||
            !debug_string_table_intern(strings,
                                       info->variables[index].read_expr,
                                       &ignored)) {
            return false;
        }
    }
    return true;
}

/* Writes one complete .fd file payload. */
static bool debug_write_file_payload(FILE *file,
                                     const FengDebugBuffer *meta,
                                     const FengDebugBuffer *strings,
                                     const FengDebugBuffer *packages,
                                     const FengDebugBuffer *frames,
                                     const FengDebugBuffer *variables,
                                     uint32_t string_count,
                                     uint32_t package_count,
                                     uint32_t frame_count,
                                     uint32_t variable_count) {
    static const unsigned char magic[4] = {'F', 'D', '0', '1'};
    const uint16_t section_count = 5U;
    const uint64_t header_size = 16U;
    const uint64_t dir_entry_size = 32U;
    const uint64_t dir_size = (uint64_t)section_count * dir_entry_size;
    uint64_t meta_offset = header_size + dir_size;
    uint64_t strings_offset = meta_offset + (uint64_t)meta->length;
    uint64_t packages_offset = strings_offset + (uint64_t)strings->length;
    uint64_t frames_offset = packages_offset + (uint64_t)packages->length;
    uint64_t variables_offset = frames_offset + (uint64_t)frames->length;
    FengDebugBuffer header = {0};
    bool ok = true;

    ok = fwrite(magic, 1U, sizeof(magic), file) == sizeof(magic) &&
         debug_buffer_append_u16_le(&header, 1U) &&
         debug_buffer_append_u16_le(&header, section_count) &&
         debug_buffer_append_u64_le(&header, header_size) &&
         fwrite(header.data, 1U, header.length, file) == header.length;
    debug_buffer_dispose(&header);
    if (!ok) {
        return false;
    }

    return debug_write_section_entry(file, "META", meta_offset, meta->length, 1U) &&
           debug_write_section_entry(file, "STRS", strings_offset, strings->length, string_count) &&
           debug_write_section_entry(file, "PKGS", packages_offset, packages->length, package_count) &&
           debug_write_section_entry(file, "FRMS", frames_offset, frames->length, frame_count) &&
           debug_write_section_entry(file, "VARS", variables_offset, variables->length, variable_count) &&
           debug_write_buffer(file, meta) &&
           debug_write_buffer(file, strings) &&
           debug_write_buffer(file, packages) &&
           debug_write_buffer(file, frames) &&
           debug_write_buffer(file, variables);
}

/* Reads one whole .fd file into memory. */
static bool debug_read_entire_file(const char *path,
                                   unsigned char **out_data,
                                   size_t *out_size,
                                   char **out_error_message) {
    FILE *file = NULL;
    unsigned char *data = NULL;
    long size_long;
    size_t size;

    *out_data = NULL;
    *out_size = 0U;
    file = fopen(path, "rb");
    if (file == NULL) {
        debug_set_error(out_error_message,
                        "failed to open %s: %s",
                        path,
                        strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        debug_set_error(out_error_message,
                        "failed to seek %s: %s",
                        path,
                        strerror(errno));
        fclose(file);
        return false;
    }
    size_long = ftell(file);
    if (size_long < 0L) {
        debug_set_error(out_error_message,
                        "failed to size %s: %s",
                        path,
                        strerror(errno));
        fclose(file);
        return false;
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        debug_set_error(out_error_message,
                        "failed to rewind %s: %s",
                        path,
                        strerror(errno));
        fclose(file);
        return false;
    }

    size = (size_t)size_long;
    data = (unsigned char *)malloc(size > 0U ? size : 1U);
    if (data == NULL) {
        debug_set_error(out_error_message, "out of memory reading %s", path);
        fclose(file);
        return false;
    }
    if (size > 0U && fread(data, 1U, size, file) != size) {
        debug_set_error(out_error_message,
                        "failed to read %s: %s",
                        path,
                        strerror(errno));
        free(data);
        fclose(file);
        return false;
    }

    fclose(file);
    *out_data = data;
    *out_size = size;
    return true;
}

/* Releases a temporary heap array of loaded strings. */
static void debug_free_loaded_strings(char **strings, size_t string_count) {
    size_t index;

    if (strings == NULL) {
        return;
    }
    for (index = 1U; index <= string_count; ++index) {
        free(strings[index]);
    }
    free(strings);
}

/* Parses the section directory and extracts known .fd sections. */
static bool debug_parse_sections(const unsigned char *data,
                                 size_t size,
                                 FengDebugParsedSection *meta,
                                 FengDebugParsedSection *strings,
                                 FengDebugParsedSection *packages,
                                 FengDebugParsedSection *frames,
                                 FengDebugParsedSection *variables,
                                 char **out_error_message) {
    uint16_t version;
    uint16_t section_count;
    uint64_t dir_offset;

    if (size < 16U || memcmp(data, "FD01", 4U) != 0) {
        debug_set_error(out_error_message, "invalid debug sidecar header");
        return false;
    }
    version = debug_read_u16_le(data + 4U);
    section_count = debug_read_u16_le(data + 6U);
    dir_offset = debug_read_u64_le(data + 8U);
    if (version != 1U) {
        debug_set_error(out_error_message, "unsupported debug sidecar version %u", version);
        return false;
    }
    if (dir_offset > size ||
        (uint64_t)size - dir_offset < ((uint64_t)section_count * 32U)) {
        debug_set_error(out_error_message, "invalid debug sidecar section directory");
        return false;
    }

    for (uint16_t index = 0U; index < section_count; ++index) {
        const unsigned char *entry = data + dir_offset + ((uint64_t)index * 32U);
        const unsigned char *tag = entry;
        uint64_t offset = debug_read_u64_le(entry + 8U);
        uint64_t length = debug_read_u64_le(entry + 16U);
        uint32_t record_count = debug_read_u32_le(entry + 24U);
        FengDebugParsedSection *target = NULL;

        if (offset > size || (uint64_t)size - offset < length) {
            debug_set_error(out_error_message, "invalid debug sidecar section bounds");
            return false;
        }
        if (memcmp(tag, "META", 4U) == 0) {
            target = meta;
        } else if (memcmp(tag, "STRS", 4U) == 0) {
            target = strings;
        } else if (memcmp(tag, "PKGS", 4U) == 0) {
            target = packages;
        } else if (memcmp(tag, "FRMS", 4U) == 0) {
            target = frames;
        } else if (memcmp(tag, "VARS", 4U) == 0) {
            target = variables;
        } else {
            continue;
        }
        if (target->present) {
            debug_set_error(out_error_message, "duplicate debug sidecar section");
            return false;
        }
        target->present = true;
        target->data = data + offset;
        target->size = (size_t)length;
        target->record_count = record_count;
    }

    return true;
}

/* Parses the interned string table section. */
static bool debug_parse_strings_section(const FengDebugParsedSection *section,
                                        char ***out_strings,
                                        size_t *out_string_count,
                                        char **out_error_message) {
    char **strings = NULL;
    const unsigned char *cursor;
    const unsigned char *end;

    *out_strings = NULL;
    *out_string_count = 0U;
    if (!section->present) {
        debug_set_error(out_error_message, "debug sidecar is missing STRS section");
        return false;
    }
    strings = (char **)calloc((size_t)section->record_count + 1U, sizeof(*strings));
    if (strings == NULL && section->record_count > 0U) {
        debug_set_error(out_error_message, "out of memory reading debug strings");
        return false;
    }

    cursor = section->data;
    end = section->data + section->size;
    for (size_t index = 1U; index <= (size_t)section->record_count; ++index) {
        uint32_t length;

        if ((size_t)(end - cursor) < 4U) {
            debug_set_error(out_error_message, "truncated debug string section");
            debug_free_loaded_strings(strings, (size_t)section->record_count);
            return false;
        }
        length = debug_read_u32_le(cursor);
        cursor += 4U;
        if ((size_t)(end - cursor) < length) {
            debug_set_error(out_error_message, "truncated debug string payload");
            debug_free_loaded_strings(strings, (size_t)section->record_count);
            return false;
        }
        strings[index] = debug_dup_n((const char *)cursor, length);
        if (strings[index] == NULL) {
            debug_set_error(out_error_message, "out of memory reading debug string");
            debug_free_loaded_strings(strings, (size_t)section->record_count);
            return false;
        }
        cursor += length;
    }
    if (cursor != end) {
        debug_set_error(out_error_message, "invalid debug string section padding");
        debug_free_loaded_strings(strings, (size_t)section->record_count);
        return false;
    }

    *out_strings = strings;
    *out_string_count = (size_t)section->record_count;
    return true;
}

/* Resolves one string-table id from a loaded .fd string table. */
static const char *debug_lookup_loaded_string(char **strings,
                                              size_t string_count,
                                              uint32_t id) {
    if (id == 0U) {
        return NULL;
    }
    if ((size_t)id > string_count) {
        return NULL;
    }
    return strings[id];
}

/* Parses the fixed META section from a loaded .fd. */
static bool debug_parse_meta_section(const FengDebugParsedSection *section,
                                     char **strings,
                                     size_t string_count,
                                     FengDebugArtifact *artifact,
                                     char **out_error_message) {
    uint32_t binary_path_id;
    const char *binary_path;

    if (!section->present || section->record_count != 1U || section->size != 16U) {
        debug_set_error(out_error_message, "invalid debug META section");
        return false;
    }
    binary_path_id = debug_read_u32_le(section->data);
    binary_path = debug_lookup_loaded_string(strings, string_count, binary_path_id);
    if (binary_path == NULL) {
        debug_set_error(out_error_message, "invalid debug binary path string id");
        return false;
    }
    artifact->binary_path = debug_dup_cstr(binary_path);
    artifact->binary_fingerprint = debug_read_u64_le(section->data + 8U);
    if (artifact->binary_path == NULL) {
        debug_set_error(out_error_message, "out of memory reading debug META section");
        return false;
    }
    return true;
}

/* Parses the package-root mapping section from a loaded .fd. */
static bool debug_parse_packages_section(const FengDebugParsedSection *section,
                                         char **strings,
                                         size_t string_count,
                                         FengDebugArtifact *artifact,
                                         char **out_error_message) {
    const unsigned char *cursor;
    const unsigned char *end;

    if (!section->present || section->record_count == 0U) {
        return true;
    }
    if (section->size != (size_t)section->record_count * 8U) {
        debug_set_error(out_error_message, "invalid debug PKGS section");
        return false;
    }
    artifact->packages = (FengDebugArtifactPackage *)calloc(section->record_count,
                                                            sizeof(*artifact->packages));
    if (artifact->packages == NULL) {
        debug_set_error(out_error_message, "out of memory reading debug packages");
        return false;
    }
    artifact->package_count = (size_t)section->record_count;
    cursor = section->data;
    end = section->data + section->size;
    for (size_t index = 0U; cursor < end; ++index) {
        const char *package_name = debug_lookup_loaded_string(strings,
                                                              string_count,
                                                              debug_read_u32_le(cursor));
        const char *local_root = debug_lookup_loaded_string(strings,
                                                            string_count,
                                                            debug_read_u32_le(cursor + 4U));

        if (package_name == NULL || local_root == NULL) {
            debug_set_error(out_error_message, "invalid debug package string id");
            return false;
        }
        artifact->packages[index].package_name = debug_dup_cstr(package_name);
        artifact->packages[index].local_root_path = debug_dup_cstr(local_root);
        if (artifact->packages[index].package_name == NULL ||
            artifact->packages[index].local_root_path == NULL) {
            debug_set_error(out_error_message, "out of memory reading debug package mapping");
            return false;
        }
        cursor += 8U;
    }
    return true;
}

/* Parses the frame mapping section from a loaded .fd. */
static bool debug_parse_frames_section(const FengDebugParsedSection *section,
                                       char **strings,
                                       size_t string_count,
                                       FengDebugInfo *info,
                                       char **out_error_message) {
    const unsigned char *cursor;
    const unsigned char *end;

    if (!section->present || section->record_count == 0U) {
        return true;
    }
    if (section->size != (size_t)section->record_count * 12U) {
        debug_set_error(out_error_message, "invalid debug FRMS section");
        return false;
    }
    info->frames = (FengDebugFrameRecord *)calloc(section->record_count, sizeof(*info->frames));
    if (info->frames == NULL) {
        debug_set_error(out_error_message, "out of memory reading debug frames");
        return false;
    }
    info->frame_count = (size_t)section->record_count;
    info->frame_capacity = info->frame_count;
    cursor = section->data;
    end = section->data + section->size;
    for (size_t index = 0U; cursor < end; ++index) {
        const char *backend_symbol = debug_lookup_loaded_string(strings,
                                                                string_count,
                                                                debug_read_u32_le(cursor));
        const char *display_name = debug_lookup_loaded_string(strings,
                                                              string_count,
                                                              debug_read_u32_le(cursor + 4U));
        uint32_t policy = debug_read_u32_le(cursor + 8U);

        if (backend_symbol == NULL || display_name == NULL ||
            policy > (uint32_t)FENG_DEBUG_FRAME_COLLAPSE) {
            debug_set_error(out_error_message, "invalid debug frame record");
            return false;
        }
        info->frames[index].backend_symbol = debug_dup_cstr(backend_symbol);
        info->frames[index].display_name = debug_dup_cstr(display_name);
        info->frames[index].policy = (FengDebugFramePolicy)policy;
        if (info->frames[index].backend_symbol == NULL ||
            info->frames[index].display_name == NULL) {
            debug_set_error(out_error_message, "out of memory reading debug frame");
            return false;
        }
        cursor += 12U;
    }
    return true;
}

/* Parses the variable mapping section from a loaded .fd. */
static bool debug_parse_variables_section(const FengDebugParsedSection *section,
                                          char **strings,
                                          size_t string_count,
                                          FengDebugInfo *info,
                                          char **out_error_message) {
    const unsigned char *cursor;
    const unsigned char *end;

    if (!section->present || section->record_count == 0U) {
        return true;
    }
    if (section->size != (size_t)section->record_count * 20U) {
        debug_set_error(out_error_message, "invalid debug VARS section");
        return false;
    }
    info->variables = (FengDebugVariableRecord *)calloc(section->record_count,
                                                        sizeof(*info->variables));
    if (info->variables == NULL) {
        debug_set_error(out_error_message, "out of memory reading debug variables");
        return false;
    }
    info->variable_count = (size_t)section->record_count;
    info->variable_capacity = info->variable_count;
    cursor = section->data;
    end = section->data + section->size;
    for (size_t index = 0U; cursor < end; ++index) {
        const char *frame_backend_symbol = debug_lookup_loaded_string(strings,
                                                                      string_count,
                                                                      debug_read_u32_le(cursor));
        const char *backend_name = debug_lookup_loaded_string(strings,
                                                              string_count,
                                                              debug_read_u32_le(cursor + 4U));
        const char *display_name = debug_lookup_loaded_string(strings,
                                                              string_count,
                                                              debug_read_u32_le(cursor + 8U));
        const char *read_expr = debug_lookup_loaded_string(strings,
                                                           string_count,
                                                           debug_read_u32_le(cursor + 12U));
        uint32_t kind = debug_read_u32_le(cursor + 16U);

        if (frame_backend_symbol == NULL || display_name == NULL ||
            kind > (uint32_t)FENG_DEBUG_VARIABLE_SELF) {
            debug_set_error(out_error_message, "invalid debug variable record");
            return false;
        }
        info->variables[index].frame_backend_symbol = debug_dup_cstr(frame_backend_symbol);
        info->variables[index].backend_name = debug_dup_cstr(backend_name);
        info->variables[index].display_name = debug_dup_cstr(display_name);
        info->variables[index].read_expr = debug_dup_cstr(read_expr);
        info->variables[index].kind = (FengDebugVariableKind)kind;
        if (info->variables[index].frame_backend_symbol == NULL ||
            info->variables[index].display_name == NULL) {
            debug_set_error(out_error_message, "out of memory reading debug variable");
            return false;
        }
        cursor += 20U;
    }
    return true;
}

bool feng_debug_resolve_source(const FengDebugSourceMapping *sources,
                               size_t source_count,
                               const char *source_path,
                               FengDebugResolvedSource *out_resolved) {
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
            !debug_relative_path_from_root(sources[index].package_root,
                                           source_path,
                                           &relative_path)) {
            return false;
        }

        out_resolved->package_name = debug_dup_cstr(sources[index].package_name);
        out_resolved->package_root = debug_dup_cstr(sources[index].package_root);
        out_resolved->relative_path = relative_path;
        out_resolved->logical_uri = debug_dup_printf("%s://%s",
                                                     sources[index].package_name,
                                                     relative_path);
        if (out_resolved->package_name == NULL ||
            out_resolved->package_root == NULL ||
            out_resolved->logical_uri == NULL) {
            feng_debug_resolved_source_dispose(out_resolved);
            return false;
        }
        return true;
    }
    return false;
}

void feng_debug_resolved_source_dispose(FengDebugResolvedSource *resolved) {
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

void feng_debug_info_init(FengDebugInfo *info) {
    if (info != NULL) {
        memset(info, 0, sizeof(*info));
    }
}

void feng_debug_info_dispose(FengDebugInfo *info) {
    size_t index;

    if (info == NULL) {
        return;
    }
    for (index = 0U; index < info->frame_count; ++index) {
        debug_frame_record_dispose(&info->frames[index]);
    }
    for (index = 0U; index < info->variable_count; ++index) {
        debug_variable_record_dispose(&info->variables[index]);
    }
    free(info->frames);
    free(info->variables);
    memset(info, 0, sizeof(*info));
}

bool feng_debug_info_add_frame(FengDebugInfo *info,
                               const char *backend_symbol,
                               const char *display_name,
                               FengDebugFramePolicy policy) {
    FengDebugFrameRecord entry;
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

    entry.backend_symbol = debug_dup_cstr(backend_symbol);
    entry.display_name = debug_dup_cstr(display_name);
    entry.policy = policy;
    if (entry.backend_symbol == NULL ||
        entry.display_name == NULL ||
        !debug_append_raw((void **)&info->frames,
                          &info->frame_count,
                          &info->frame_capacity,
                          sizeof(entry),
                          &entry)) {
        debug_frame_record_dispose(&entry);
        return false;
    }
    return true;
}

bool feng_debug_info_add_variable(FengDebugInfo *info,
                                  const char *frame_backend_symbol,
                                  const char *backend_name,
                                  const char *display_name,
                                  const char *read_expr,
                                  FengDebugVariableKind kind) {
    FengDebugVariableRecord entry;
    size_t index;

    if (info == NULL || frame_backend_symbol == NULL || display_name == NULL) {
        return false;
    }
    for (index = 0U; index < info->variable_count; ++index) {
        if (strcmp(info->variables[index].frame_backend_symbol,
                   frame_backend_symbol) != 0 ||
            !debug_cstr_equals(info->variables[index].backend_name, backend_name)) {
            continue;
        }
        if (backend_name == NULL &&
            strcmp(info->variables[index].display_name, display_name) != 0) {
            continue;
        }
        return strcmp(info->variables[index].display_name, display_name) == 0 &&
               debug_cstr_equals(info->variables[index].read_expr, read_expr) &&
               info->variables[index].kind == kind;
    }

    entry.frame_backend_symbol = debug_dup_cstr(frame_backend_symbol);
    entry.backend_name = debug_dup_cstr(backend_name);
    entry.display_name = debug_dup_cstr(display_name);
    entry.read_expr = debug_dup_cstr(read_expr);
    entry.kind = kind;
    if (entry.frame_backend_symbol == NULL ||
        entry.display_name == NULL ||
        !debug_append_raw((void **)&info->variables,
                          &info->variable_count,
                          &info->variable_capacity,
                          sizeof(entry),
                          &entry)) {
        debug_variable_record_dispose(&entry);
        return false;
    }
    return true;
}

uint64_t feng_debug_fnv1a64_file(const char *path, char **out_error_message) {
    FILE *file;
    unsigned char buffer[8192];
    size_t read_size;
    uint64_t hash = 14695981039346656037ULL;

    if (out_error_message != NULL) {
        free(*out_error_message);
        *out_error_message = NULL;
    }
    if (path == NULL) {
        debug_set_error(out_error_message, "binary path is required");
        return 0U;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        debug_set_error(out_error_message,
                        "failed to open %s: %s",
                        path,
                        strerror(errno));
        return 0U;
    }

    while ((read_size = fread(buffer, 1U, sizeof(buffer), file)) > 0U) {
        size_t index;

        for (index = 0U; index < read_size; ++index) {
            hash ^= (uint64_t)buffer[index];
            hash *= 1099511628211ULL;
        }
    }
    if (ferror(file)) {
        debug_set_error(out_error_message,
                        "failed to read %s: %s",
                        path,
                        strerror(errno));
        fclose(file);
        return 0U;
    }
    fclose(file);
    return hash;
}

bool feng_debug_write_fd(const char *fd_path,
                         const char *binary_path,
                         const FengDebugSourceMapping *sources,
                         size_t source_count,
                         const FengDebugInfo *info,
                         char **out_error_message) {
    FengDebugArtifactPackage *packages = NULL;
    size_t package_count = 0U;
    char *fingerprint_error = NULL;
    uint64_t fingerprint;
    FengDebugStringTable string_table = {0};
    FengDebugBuffer meta = {0};
    FengDebugBuffer strings = {0};
    FengDebugBuffer packages_buffer = {0};
    FengDebugBuffer frames = {0};
    FengDebugBuffer variables = {0};
    uint32_t binary_path_id = 0U;
    FILE *file = NULL;
    bool ok = false;

    if (out_error_message != NULL) {
        free(*out_error_message);
        *out_error_message = NULL;
    }
    if (fd_path == NULL || binary_path == NULL) {
        debug_set_error(out_error_message, "debug sidecar path and binary path are required");
        return false;
    }
    if (!debug_collect_packages(sources,
                                source_count,
                                &packages,
                                &package_count,
                                out_error_message)) {
        return false;
    }

    fingerprint = feng_debug_fnv1a64_file(binary_path, &fingerprint_error);
    if (fingerprint_error != NULL) {
        debug_set_error(out_error_message, "%s", fingerprint_error);
        free(fingerprint_error);
        goto cleanup;
    }
    if (!debug_collect_strings(&string_table,
                               binary_path,
                               packages,
                               package_count,
                               info,
                               &binary_path_id) ||
        !debug_build_meta_section(&meta, binary_path_id, fingerprint) ||
        !debug_build_strings_section(&string_table, &strings) ||
        !debug_build_packages_section(&packages_buffer,
                                      packages,
                                      package_count,
                                      &string_table) ||
        !debug_build_frames_section(&frames, info, &string_table) ||
        !debug_build_variables_section(&variables, info, &string_table)) {
        debug_set_error(out_error_message, "out of memory writing debug sidecar");
        goto cleanup;
    }

    if (string_table.count > UINT32_MAX ||
        package_count > UINT32_MAX ||
        (info != NULL && (info->frame_count > UINT32_MAX || info->variable_count > UINT32_MAX))) {
        debug_set_error(out_error_message, "debug sidecar exceeds format limits");
        goto cleanup;
    }

    file = fopen(fd_path, "wb");
    if (file == NULL) {
        debug_set_error(out_error_message,
                        "failed to open %s: %s",
                        fd_path,
                        strerror(errno));
        goto cleanup;
    }
    if (!debug_write_file_payload(file,
                                  &meta,
                                  &strings,
                                  &packages_buffer,
                                  &frames,
                                  &variables,
                                  (uint32_t)string_table.count,
                                  (uint32_t)package_count,
                                  info != NULL ? (uint32_t)info->frame_count : 0U,
                                  info != NULL ? (uint32_t)info->variable_count : 0U) ||
        fclose(file) != 0) {
        file = NULL;
        unlink(fd_path);
        debug_set_error(out_error_message,
                        "failed to write %s: %s",
                        fd_path,
                        strerror(errno));
        goto cleanup;
    }
    file = NULL;
    ok = true;

cleanup:
    if (file != NULL) {
        fclose(file);
        unlink(fd_path);
    }
    for (size_t index = 0U; index < package_count; ++index) {
        debug_artifact_package_dispose(&packages[index]);
    }
    free(packages);
    debug_string_table_dispose(&string_table);
    debug_buffer_dispose(&meta);
    debug_buffer_dispose(&strings);
    debug_buffer_dispose(&packages_buffer);
    debug_buffer_dispose(&frames);
    debug_buffer_dispose(&variables);
    return ok;
}

bool feng_debug_read_fd(const char *fd_path,
                        FengDebugArtifact *out_artifact,
                        char **out_error_message) {
    unsigned char *data = NULL;
    size_t size = 0U;
    FengDebugParsedSection meta = {0};
    FengDebugParsedSection strings = {0};
    FengDebugParsedSection packages = {0};
    FengDebugParsedSection frames = {0};
    FengDebugParsedSection variables = {0};
    char **loaded_strings = NULL;
    size_t loaded_string_count = 0U;

    if (out_error_message != NULL) {
        free(*out_error_message);
        *out_error_message = NULL;
    }
    if (out_artifact == NULL || fd_path == NULL) {
        debug_set_error(out_error_message, "debug sidecar path and output are required");
        return false;
    }
    memset(out_artifact, 0, sizeof(*out_artifact));
    feng_debug_info_init(&out_artifact->info);

    if (!debug_read_entire_file(fd_path, &data, &size, out_error_message) ||
        !debug_parse_sections(data,
                              size,
                              &meta,
                              &strings,
                              &packages,
                              &frames,
                              &variables,
                              out_error_message) ||
        !debug_parse_strings_section(&strings,
                                     &loaded_strings,
                                     &loaded_string_count,
                                     out_error_message) ||
        !debug_parse_meta_section(&meta,
                                  loaded_strings,
                                  loaded_string_count,
                                  out_artifact,
                                  out_error_message) ||
        !debug_parse_packages_section(&packages,
                                      loaded_strings,
                                      loaded_string_count,
                                      out_artifact,
                                      out_error_message) ||
        !debug_parse_frames_section(&frames,
                                    loaded_strings,
                                    loaded_string_count,
                                    &out_artifact->info,
                                    out_error_message) ||
        !debug_parse_variables_section(&variables,
                                       loaded_strings,
                                       loaded_string_count,
                                       &out_artifact->info,
                                       out_error_message)) {
        free(data);
        debug_free_loaded_strings(loaded_strings, loaded_string_count);
        feng_debug_artifact_dispose(out_artifact);
        return false;
    }

    free(data);
    debug_free_loaded_strings(loaded_strings, loaded_string_count);
    return true;
}

void feng_debug_artifact_dispose(FengDebugArtifact *artifact) {
    size_t index;

    if (artifact == NULL) {
        return;
    }
    free(artifact->binary_path);
    for (index = 0U; index < artifact->package_count; ++index) {
        debug_artifact_package_dispose(&artifact->packages[index]);
    }
    free(artifact->packages);
    feng_debug_info_dispose(&artifact->info);
    memset(artifact, 0, sizeof(*artifact));
}
