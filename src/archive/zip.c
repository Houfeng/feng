#include "archive/zip.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

typedef struct FengZipReaderImpl {
    mz_zip_archive archive;
} FengZipReaderImpl;

typedef struct FengZipWriterImpl {
    mz_zip_archive archive;
    char *archive_path;
    bool finalized;
} FengZipWriterImpl;

static bool set_errorf(char **out_error_message, const char *fmt, ...) {
    va_list args;
    va_list args_copy;
    int needed;
    char *message;

    if (out_error_message == NULL) {
        return false;
    }

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
    *out_error_message = message;
    return false;
}

static char *dup_cstr(const char *text) {
    size_t length;
    char *out;

    if (text == NULL) {
        return NULL;
    }
    length = strlen(text);
    out = (char *)malloc(length + 1U);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, length + 1U);
    return out;
}

static char *path_dirname_dup(const char *path) {
    const char *slash;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return dup_cstr(".");
    }
    if (slash == path) {
        return dup_cstr("/");
    }
    return strndup(path, (size_t)(slash - path));
}

static bool is_valid_segment(const char *segment, size_t length) {
    if (length == 0U) {
        return false;
    }
    if (length == 1U && segment[0] == '.') {
        return false;
    }
    if (length == 2U && segment[0] == '.' && segment[1] == '.') {
        return false;
    }
    return true;
}

static char *normalize_entry_path(const char *entry_path,
                                  bool directory_entry,
                                  char **out_error_message) {
    size_t input_length;
    size_t index;
    size_t cursor = 0U;
    size_t segment_start = 0U;
    char *normalized;

    if (entry_path == NULL || entry_path[0] == '\0') {
        set_errorf(out_error_message, "archive entry path must not be empty");
        return NULL;
    }
    if (entry_path[0] == '/') {
        set_errorf(out_error_message,
                   "archive entry path must be relative: %s",
                   entry_path);
        return NULL;
    }

    input_length = strlen(entry_path);
    normalized = (char *)malloc(input_length + 2U);
    if (normalized == NULL) {
        set_errorf(out_error_message, "out of memory");
        return NULL;
    }

    for (index = 0U; index < input_length; ++index) {
        char ch = entry_path[index];

        if (ch == '\\') {
            free(normalized);
            set_errorf(out_error_message,
                       "archive entry path must use '/': %s",
                       entry_path);
            return NULL;
        }
        if (ch == '/') {
            size_t segment_length = cursor - segment_start;

            if (!is_valid_segment(normalized + segment_start, segment_length)) {
                free(normalized);
                set_errorf(out_error_message,
                           "archive entry path contains invalid segment: %s",
                           entry_path);
                return NULL;
            }
            normalized[cursor++] = '/';
            segment_start = cursor;
            continue;
        }
        normalized[cursor++] = ch;
    }

    if (cursor == 0U) {
        free(normalized);
        set_errorf(out_error_message, "archive entry path must not be empty");
        return NULL;
    }

    if (normalized[cursor - 1U] == '/') {
        if (!directory_entry) {
            free(normalized);
            set_errorf(out_error_message,
                       "archive file entry path must not end with '/': %s",
                       entry_path);
            return NULL;
        }
    } else {
        size_t segment_length = cursor - segment_start;

        if (!is_valid_segment(normalized + segment_start, segment_length)) {
            free(normalized);
            set_errorf(out_error_message,
                       "archive entry path contains invalid segment: %s",
                       entry_path);
            return NULL;
        }
        if (directory_entry) {
            normalized[cursor++] = '/';
        }
    }

    normalized[cursor] = '\0';
    return normalized;
}

static bool mkdir_p(const char *path, char **out_error_message) {
    char *mutable_path;
    size_t index;

    if (path == NULL || strcmp(path, ".") == 0 || strcmp(path, "/") == 0) {
        return true;
    }

    mutable_path = dup_cstr(path);
    if (mutable_path == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }

    for (index = 1U; mutable_path[index] != '\0'; ++index) {
        if (mutable_path[index] == '/') {
            mutable_path[index] = '\0';
            if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
                int saved_errno = errno;

                free(mutable_path);
                return set_errorf(out_error_message,
                                  "failed to create directory %s: %s",
                                  path,
                                  strerror(saved_errno));
            }
            mutable_path[index] = '/';
        }
    }

    if (mkdir(mutable_path, 0775) != 0 && errno != EEXIST) {
        int saved_errno = errno;

        free(mutable_path);
        return set_errorf(out_error_message,
                          "failed to create directory %s: %s",
                          path,
                          strerror(saved_errno));
    }

    free(mutable_path);
    return true;
}

static bool ensure_parent_directory(const char *path, char **out_error_message) {
    char *parent = path_dirname_dup(path);
    bool ok;

    if (parent == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    ok = mkdir_p(parent, out_error_message);
    free(parent);
    return ok;
}

static FengZipCompression compression_from_method(mz_uint16 method) {
    switch (method) {
        case 0:
            return FENG_ZIP_COMPRESSION_STORE;
        case MZ_DEFLATED:
            return FENG_ZIP_COMPRESSION_DEFLATE;
        default:
            return FENG_ZIP_COMPRESSION_UNKNOWN;
    }
}

static bool compression_to_flags(FengZipCompression compression,
                                 mz_uint *out_flags,
                                 char **out_error_message) {
    switch (compression) {
        case FENG_ZIP_COMPRESSION_STORE:
            *out_flags = MZ_NO_COMPRESSION;
            return true;
        case FENG_ZIP_COMPRESSION_DEFLATE:
            *out_flags = MZ_BEST_COMPRESSION;
            return true;
        default:
            return set_errorf(out_error_message,
                              "unsupported ZIP compression mode: %d",
                              (int)compression);
    }
}

static bool locate_file_index(FengZipReaderImpl *impl,
                              const char *entry_path,
                              mz_uint32 *out_file_index,
                              char **out_error_message) {
    char *normalized = normalize_entry_path(entry_path, false, out_error_message);
    bool ok = true;

    if (normalized == NULL) {
        return false;
    }
    if (!mz_zip_reader_locate_file_v2(&impl->archive,
                                      normalized,
                                      NULL,
                                      MZ_ZIP_FLAG_CASE_SENSITIVE,
                                      out_file_index)) {
        ok = set_errorf(out_error_message,
                        "archive entry not found: %s",
                        normalized);
    }
    free(normalized);
    return ok;
}

static bool get_file_stat(FengZipReaderImpl *impl,
                          size_t index,
                          mz_zip_archive_file_stat *out_stat,
                          char **out_error_message) {
    if (index >= (size_t)mz_zip_reader_get_num_files(&impl->archive)) {
        return set_errorf(out_error_message,
                          "archive entry index out of range: %zu",
                          index);
    }
    if (!mz_zip_reader_file_stat(&impl->archive, (mz_uint)index, out_stat)) {
        return set_errorf(out_error_message,
                          "failed to read archive entry metadata: %s",
                          mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }
    return true;
}

static void fill_entry_info(const mz_zip_archive_file_stat *stat,
                            FengZipEntryInfo *out_info) {
    size_t path_length = strlen(stat->m_filename);

    if (path_length >= FENG_ZIP_MAX_ENTRY_PATH) {
        path_length = FENG_ZIP_MAX_ENTRY_PATH - 1U;
    }
    memcpy(out_info->path, stat->m_filename, path_length);
    out_info->path[path_length] = '\0';
    out_info->compressed_size = (uint64_t)stat->m_comp_size;
    out_info->uncompressed_size = (uint64_t)stat->m_uncomp_size;
    out_info->external_attributes = stat->m_external_attr;
    out_info->mode_bits = (uint32_t)(stat->m_external_attr >> 16);
    out_info->compression = compression_from_method(stat->m_method);
    out_info->is_directory = stat->m_is_directory != 0;
}

bool feng_zip_reader_open(const char *archive_path,
                          FengZipReader *out_reader,
                          char **out_error_message) {
    FengZipReaderImpl *impl;

    if (out_reader == NULL) {
        return set_errorf(out_error_message, "reader output must not be null");
    }
    out_reader->impl = NULL;
    if (archive_path == NULL || archive_path[0] == '\0') {
        return set_errorf(out_error_message, "archive path must not be empty");
    }

    impl = (FengZipReaderImpl *)calloc(1U, sizeof(*impl));
    if (impl == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    mz_zip_zero_struct(&impl->archive);
    if (!mz_zip_reader_init_file(&impl->archive, archive_path, 0U)) {
        set_errorf(out_error_message,
                   "failed to open ZIP archive %s: %s",
                   archive_path,
                   mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
        free(impl);
        return false;
    }

    out_reader->impl = impl;
    return true;
}

void feng_zip_reader_dispose(FengZipReader *reader) {
    FengZipReaderImpl *impl;

    if (reader == NULL || reader->impl == NULL) {
        return;
    }
    impl = (FengZipReaderImpl *)reader->impl;
    mz_zip_reader_end(&impl->archive);
    free(impl);
    reader->impl = NULL;
}

size_t feng_zip_reader_entry_count(const FengZipReader *reader) {
    FengZipReaderImpl *impl;

    if (reader == NULL || reader->impl == NULL) {
        return 0U;
    }
    impl = (FengZipReaderImpl *)reader->impl;
    return (size_t)mz_zip_reader_get_num_files(&impl->archive);
}

bool feng_zip_reader_entry_at(const FengZipReader *reader,
                              size_t index,
                              FengZipEntryInfo *out_info,
                              char **out_error_message) {
    FengZipReaderImpl *impl;
    mz_zip_archive_file_stat stat;

    if (reader == NULL || reader->impl == NULL) {
        return set_errorf(out_error_message, "reader is not open");
    }
    if (out_info == NULL) {
        return set_errorf(out_error_message, "entry output must not be null");
    }

    impl = (FengZipReaderImpl *)reader->impl;
    if (!get_file_stat(impl, index, &stat, out_error_message)) {
        return false;
    }

    fill_entry_info(&stat, out_info);
    return true;
}

bool feng_zip_reader_read(const FengZipReader *reader,
                          const char *entry_path,
                          void **out_data,
                          size_t *out_size,
                          char **out_error_message) {
    FengZipReaderImpl *impl;
    mz_uint32 file_index;
    void *data;

    if (reader == NULL || reader->impl == NULL) {
        return set_errorf(out_error_message, "reader is not open");
    }
    if (out_data == NULL || out_size == NULL) {
        return set_errorf(out_error_message,
                          "read outputs must not be null");
    }

    *out_data = NULL;
    *out_size = 0U;
    impl = (FengZipReaderImpl *)reader->impl;
    if (!locate_file_index(impl, entry_path, &file_index, out_error_message)) {
        return false;
    }

    data = mz_zip_reader_extract_to_heap(&impl->archive, file_index, out_size, 0U);
    if (data == NULL) {
        return set_errorf(out_error_message,
                          "failed to read archive entry %s: %s",
                          entry_path,
                          mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }

    *out_data = data;
    return true;
}

bool feng_zip_reader_extract(const FengZipReader *reader,
                             const char *entry_path,
                             const char *destination_path,
                             char **out_error_message) {
    FengZipReaderImpl *impl;
    mz_uint32 file_index;
    FengZipEntryInfo info;

    if (reader == NULL || reader->impl == NULL) {
        return set_errorf(out_error_message, "reader is not open");
    }
    if (destination_path == NULL || destination_path[0] == '\0') {
        return set_errorf(out_error_message,
                          "destination path must not be empty");
    }

    impl = (FengZipReaderImpl *)reader->impl;
    if (!locate_file_index(impl, entry_path, &file_index, out_error_message)) {
        return false;
    }
    if (!feng_zip_reader_entry_at(reader,
                                  (size_t)file_index,
                                  &info,
                                  out_error_message)) {
        return false;
    }
    if (info.is_directory) {
        return set_errorf(out_error_message,
                          "cannot extract directory entry to file: %s",
                          entry_path);
    }
    if (!ensure_parent_directory(destination_path, out_error_message)) {
        return false;
    }
    if (!mz_zip_reader_extract_to_file(&impl->archive,
                                       file_index,
                                       destination_path,
                                       0U)) {
        unlink(destination_path);
        return set_errorf(out_error_message,
                          "failed to extract archive entry %s: %s",
                          entry_path,
                          mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }
    if ((info.mode_bits & 07777U) != 0U
        && chmod(destination_path, (mode_t)(info.mode_bits & 07777U)) != 0) {
        return set_errorf(out_error_message,
                          "failed to apply file mode to %s: %s",
                          destination_path,
                          strerror(errno));
    }
    return true;
}

bool feng_zip_writer_open(const char *archive_path,
                          FengZipWriter *out_writer,
                          char **out_error_message) {
    FengZipWriterImpl *impl;

    if (out_writer == NULL) {
        return set_errorf(out_error_message, "writer output must not be null");
    }
    out_writer->impl = NULL;
    if (archive_path == NULL || archive_path[0] == '\0') {
        return set_errorf(out_error_message, "archive path must not be empty");
    }
    if (!ensure_parent_directory(archive_path, out_error_message)) {
        return false;
    }

    impl = (FengZipWriterImpl *)calloc(1U, sizeof(*impl));
    if (impl == NULL) {
        return set_errorf(out_error_message, "out of memory");
    }
    impl->archive_path = dup_cstr(archive_path);
    if (impl->archive_path == NULL) {
        free(impl);
        return set_errorf(out_error_message, "out of memory");
    }

    mz_zip_zero_struct(&impl->archive);
    if (!mz_zip_writer_init_file(&impl->archive, archive_path, 0U)) {
        set_errorf(out_error_message,
                   "failed to create ZIP archive %s: %s",
                   archive_path,
                   mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
        free(impl->archive_path);
        free(impl);
        return false;
    }

    out_writer->impl = impl;
    return true;
}

bool feng_zip_writer_add_directory(FengZipWriter *writer,
                                   const char *entry_path,
                                   char **out_error_message) {
    FengZipWriterImpl *impl;
    char *normalized;
    bool ok;

    if (writer == NULL || writer->impl == NULL) {
        return set_errorf(out_error_message, "writer is not open");
    }
    normalized = normalize_entry_path(entry_path, true, out_error_message);
    if (normalized == NULL) {
        return false;
    }

    impl = (FengZipWriterImpl *)writer->impl;
    ok = mz_zip_writer_add_mem(&impl->archive,
                               normalized,
                               NULL,
                               0U,
                               MZ_NO_COMPRESSION)
         != 0;
    if (!ok) {
        set_errorf(out_error_message,
                   "failed to add ZIP directory entry %s: %s",
                   normalized,
                   mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }
    free(normalized);
    return ok;
}

bool feng_zip_writer_add_bytes(FengZipWriter *writer,
                               const char *entry_path,
                               const void *data,
                               size_t size,
                               FengZipCompression compression,
                               char **out_error_message) {
    FengZipWriterImpl *impl;
    char *normalized;
    mz_uint compression_flags;
    bool ok;

    if (writer == NULL || writer->impl == NULL) {
        return set_errorf(out_error_message, "writer is not open");
    }
    if (size > 0U && data == NULL) {
        return set_errorf(out_error_message,
                          "archive file data must not be null when size is non-zero");
    }
    if (!compression_to_flags(compression, &compression_flags, out_error_message)) {
        return false;
    }

    normalized = normalize_entry_path(entry_path, false, out_error_message);
    if (normalized == NULL) {
        return false;
    }

    impl = (FengZipWriterImpl *)writer->impl;
    ok = mz_zip_writer_add_mem(&impl->archive,
                               normalized,
                               data,
                               size,
                               compression_flags)
         != 0;
    if (!ok) {
        set_errorf(out_error_message,
                   "failed to add ZIP entry %s: %s",
                   normalized,
                   mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }
    free(normalized);
    return ok;
}

bool feng_zip_writer_add_file(FengZipWriter *writer,
                              const char *entry_path,
                              const char *source_path,
                              FengZipCompression compression,
                              char **out_error_message) {
    FengZipWriterImpl *impl;
    char *normalized;
    mz_uint compression_flags;
    bool ok;

    if (writer == NULL || writer->impl == NULL) {
        return set_errorf(out_error_message, "writer is not open");
    }
    if (source_path == NULL || source_path[0] == '\0') {
        return set_errorf(out_error_message, "source path must not be empty");
    }
    if (!compression_to_flags(compression, &compression_flags, out_error_message)) {
        return false;
    }

    normalized = normalize_entry_path(entry_path, false, out_error_message);
    if (normalized == NULL) {
        return false;
    }

    impl = (FengZipWriterImpl *)writer->impl;
    ok = mz_zip_writer_add_file(&impl->archive,
                                normalized,
                                source_path,
                                NULL,
                                0U,
                                compression_flags)
         != 0;
    if (!ok) {
        set_errorf(out_error_message,
                   "failed to add file %s as %s: %s",
                   source_path,
                   normalized,
                   mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }
    free(normalized);
    return ok;
}

bool feng_zip_writer_finalize(FengZipWriter *writer, char **out_error_message) {
    FengZipWriterImpl *impl;

    if (writer == NULL || writer->impl == NULL) {
        return set_errorf(out_error_message, "writer is not open");
    }
    impl = (FengZipWriterImpl *)writer->impl;
    if (impl->finalized) {
        return true;
    }
    if (!mz_zip_writer_finalize_archive(&impl->archive)) {
        return set_errorf(out_error_message,
                          "failed to finalize ZIP archive %s: %s",
                          impl->archive_path,
                          mz_zip_get_error_string(mz_zip_get_last_error(&impl->archive)));
    }
    impl->finalized = true;
    return true;
}

void feng_zip_writer_dispose(FengZipWriter *writer) {
    FengZipWriterImpl *impl;
    char *archive_path;
    bool remove_partial;

    if (writer == NULL || writer->impl == NULL) {
        return;
    }

    impl = (FengZipWriterImpl *)writer->impl;
    archive_path = impl->archive_path;
    remove_partial = !impl->finalized && archive_path != NULL;
    mz_zip_writer_end(&impl->archive);
    if (remove_partial) {
        unlink(archive_path);
    }
    free(archive_path);
    free(impl);
    writer->impl = NULL;
}

void feng_zip_free(void *data) {
    mz_free(data);
}
