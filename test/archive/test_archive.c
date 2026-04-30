#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "archive/zip.h"

#define ASSERT(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1); \
        } \
    } while (0)

static char *dup_cstr(const char *text) {
    size_t length = strlen(text);
    char *out = (char *)malloc(length + 1U);

    ASSERT(out != NULL);
    memcpy(out, text, length + 1U);
    return out;
}

static char *path_join(const char *lhs, const char *rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    int need_sep = lhs_len > 0U && lhs[lhs_len - 1U] != '/';
    char *out = (char *)malloc(lhs_len + (need_sep ? 1U : 0U) + rhs_len + 1U);
    size_t cursor = 0U;

    ASSERT(out != NULL);
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

static void mkdir_p(const char *path) {
    char *mutable_path = dup_cstr(path);
    size_t index;

    for (index = 1U; mutable_path[index] != '\0'; ++index) {
        if (mutable_path[index] == '/') {
            mutable_path[index] = '\0';
            ASSERT(mkdir(mutable_path, 0775) == 0 || errno == EEXIST);
            mutable_path[index] = '/';
        }
    }
    ASSERT(mkdir(mutable_path, 0775) == 0 || errno == EEXIST);
    free(mutable_path);
}

static void write_text_file(const char *path, const char *content) {
    FILE *file = fopen(path, "wb");
    size_t length = strlen(content);

    ASSERT(file != NULL);
    ASSERT(fwrite(content, 1U, length, file) == length);
    fclose(file);
}

static char *read_file(const char *path, size_t *out_size) {
    FILE *file = fopen(path, "rb");
    long size;
    char *buffer;

    ASSERT(file != NULL);
    ASSERT(fseek(file, 0L, SEEK_END) == 0);
    size = ftell(file);
    ASSERT(size >= 0L);
    ASSERT(fseek(file, 0L, SEEK_SET) == 0);
    buffer = (char *)malloc((size_t)size + 1U);
    ASSERT(buffer != NULL);
    ASSERT(fread(buffer, 1U, (size_t)size, file) == (size_t)size);
    fclose(file);
    buffer[size] = '\0';
    *out_size = (size_t)size;
    return buffer;
}

static void remove_tree(const char *path) {
    DIR *dir = opendir(path);
    struct dirent *entry;

    if (dir == NULL) {
        ASSERT(errno == ENOENT || unlink(path) == 0 || rmdir(path) == 0);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        char *child;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        child = path_join(path, entry->d_name);
        ASSERT(lstat(child, &st) == 0);
        if (S_ISDIR(st.st_mode)) {
            remove_tree(child);
        } else {
            ASSERT(unlink(child) == 0);
        }
        free(child);
    }

    closedir(dir);
    ASSERT(rmdir(path) == 0);
}

static int find_entry_index(FengZipReader *reader,
                            const char *path,
                            FengZipEntryInfo *out_info) {
    size_t entry_count = feng_zip_reader_entry_count(reader);
    size_t index;
    char *error = NULL;

    for (index = 0U; index < entry_count; ++index) {
        FengZipEntryInfo info = {0};

        ASSERT(feng_zip_reader_entry_at(reader, index, &info, &error));
        ASSERT(error == NULL);
        if (strcmp(info.path, path) == 0) {
            if (out_info != NULL) {
                *out_info = info;
            }
            return (int)index;
        }
    }
    return -1;
}

static void test_zip_roundtrip(void) {
    char template_path[] = "/tmp/feng_zip_test_XXXXXX";
    char *root = mkdtemp(template_path);
    char *source_dir;
    char *archive_path;
    char *library_path;
    char *extract_path;
    FengZipWriter writer = {0};
    FengZipReader reader = {0};
    char *error = NULL;
    static const char *kHelloText = "hello from zip\n";
    void *loaded_data = NULL;
    size_t loaded_size = 0U;
    char *extracted_data;
    size_t extracted_size = 0U;
    FengZipEntryInfo info = {0};

    ASSERT(root != NULL);

    source_dir = path_join(root, "input/lib");
    archive_path = path_join(root, "bundle/sample.zip");
    library_path = path_join(source_dir, "libhello.a");
    extract_path = path_join(root, "output/nested/libhello.a");

    mkdir_p(source_dir);
    write_text_file(library_path, "archive-bytes");

    ASSERT(feng_zip_writer_open(archive_path, &writer, &error));
    ASSERT(error == NULL);
    ASSERT(feng_zip_writer_add_directory(&writer, "mod", &error));
    ASSERT(error == NULL);
    ASSERT(feng_zip_writer_add_bytes(&writer,
                                     "mod/hello.txt",
                                     kHelloText,
                                     strlen(kHelloText),
                                     FENG_ZIP_COMPRESSION_DEFLATE,
                                     &error));
    ASSERT(error == NULL);
    ASSERT(feng_zip_writer_add_file(&writer,
                                    "lib/libhello.a",
                                    library_path,
                                    FENG_ZIP_COMPRESSION_STORE,
                                    &error));
    ASSERT(error == NULL);
    ASSERT(feng_zip_writer_finalize(&writer, &error));
    ASSERT(error == NULL);
    feng_zip_writer_dispose(&writer);

    ASSERT(feng_zip_reader_open(archive_path, &reader, &error));
    ASSERT(error == NULL);
    ASSERT(feng_zip_reader_entry_count(&reader) == 3U);

    ASSERT(find_entry_index(&reader, "mod/", &info) >= 0);
    ASSERT(info.is_directory);

    ASSERT(find_entry_index(&reader, "mod/hello.txt", &info) >= 0);
    ASSERT(!info.is_directory);
    ASSERT(info.compression == FENG_ZIP_COMPRESSION_DEFLATE);
    ASSERT(info.uncompressed_size == strlen(kHelloText));

    ASSERT(find_entry_index(&reader, "lib/libhello.a", &info) >= 0);
    ASSERT(info.compression == FENG_ZIP_COMPRESSION_STORE);

    ASSERT(feng_zip_reader_read(&reader,
                                "mod/hello.txt",
                                &loaded_data,
                                &loaded_size,
                                &error));
    ASSERT(error == NULL);
    ASSERT(loaded_size == strlen(kHelloText));
    ASSERT(memcmp(loaded_data, kHelloText, loaded_size) == 0);
    feng_zip_free(loaded_data);

    ASSERT(feng_zip_reader_extract(&reader,
                                   "lib/libhello.a",
                                   extract_path,
                                   &error));
    ASSERT(error == NULL);
    extracted_data = read_file(extract_path, &extracted_size);
    ASSERT(extracted_size == strlen("archive-bytes"));
    ASSERT(memcmp(extracted_data, "archive-bytes", extracted_size) == 0);
    free(extracted_data);

    feng_zip_reader_dispose(&reader);

    free(extract_path);
    free(library_path);
    free(archive_path);
    free(source_dir);
    remove_tree(root);
}

static void test_zip_rejects_invalid_entry_path(void) {
    char template_path[] = "/tmp/feng_zip_invalid_XXXXXX";
    char *root = mkdtemp(template_path);
    char *archive_path;
    FengZipWriter writer = {0};
    char *error = NULL;

    ASSERT(root != NULL);

    archive_path = path_join(root, "invalid.zip");
    ASSERT(feng_zip_writer_open(archive_path, &writer, &error));
    ASSERT(error == NULL);

    ASSERT(!feng_zip_writer_add_bytes(&writer,
                                      "../bad.txt",
                                      "bad",
                                      3U,
                                      FENG_ZIP_COMPRESSION_DEFLATE,
                                      &error));
    ASSERT(error != NULL);
    ASSERT(strstr(error, "invalid segment") != NULL);

    free(error);
    feng_zip_writer_dispose(&writer);
    free(archive_path);
    remove_tree(root);
}

int main(void) {
    test_zip_roundtrip();
    test_zip_rejects_invalid_entry_path();
    fprintf(stdout, "archive tests passed\n");
    return 0;
}
