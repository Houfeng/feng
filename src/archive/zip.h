#ifndef FENG_ARCHIVE_ZIP_H
#define FENG_ARCHIVE_ZIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FENG_ZIP_MAX_ENTRY_PATH 512U

typedef enum FengZipCompression {
    FENG_ZIP_COMPRESSION_UNKNOWN = -1,
    FENG_ZIP_COMPRESSION_STORE = 0,
    FENG_ZIP_COMPRESSION_DEFLATE = 1
} FengZipCompression;

typedef struct FengZipEntryInfo {
    char path[FENG_ZIP_MAX_ENTRY_PATH];
    uint64_t compressed_size;
    uint64_t uncompressed_size;
    uint32_t external_attributes;
    uint32_t mode_bits;
    FengZipCompression compression;
    bool is_directory;
} FengZipEntryInfo;

typedef struct FengZipReader {
    void *impl;
} FengZipReader;

typedef struct FengZipWriter {
    void *impl;
} FengZipWriter;

bool feng_zip_reader_open(const char *archive_path,
                          FengZipReader *out_reader,
                          char **out_error_message);

void feng_zip_reader_dispose(FengZipReader *reader);

size_t feng_zip_reader_entry_count(const FengZipReader *reader);

bool feng_zip_reader_entry_at(const FengZipReader *reader,
                              size_t index,
                              FengZipEntryInfo *out_info,
                              char **out_error_message);

bool feng_zip_reader_read(const FengZipReader *reader,
                          const char *entry_path,
                          void **out_data,
                          size_t *out_size,
                          char **out_error_message);

bool feng_zip_reader_extract(const FengZipReader *reader,
                             const char *entry_path,
                             const char *destination_path,
                             char **out_error_message);

bool feng_zip_writer_open(const char *archive_path,
                          FengZipWriter *out_writer,
                          char **out_error_message);

bool feng_zip_writer_add_directory(FengZipWriter *writer,
                                   const char *entry_path,
                                   char **out_error_message);

bool feng_zip_writer_add_bytes(FengZipWriter *writer,
                               const char *entry_path,
                               const void *data,
                               size_t size,
                               FengZipCompression compression,
                               char **out_error_message);

bool feng_zip_writer_add_file(FengZipWriter *writer,
                              const char *entry_path,
                              const char *source_path,
                              FengZipCompression compression,
                              char **out_error_message);

bool feng_zip_writer_finalize(FengZipWriter *writer, char **out_error_message);

void feng_zip_writer_dispose(FengZipWriter *writer);

void feng_zip_free(void *data);

#endif /* FENG_ARCHIVE_ZIP_H */
