#ifndef FENG_ARCHIVE_FB_H
#define FENG_ARCHIVE_FB_H

#include <stdbool.h>
#include <stddef.h>

typedef struct FengFbBundleDependency {
    const char *name;
    const char *version;
} FengFbBundleDependency;

typedef struct FengFbBundleDirectoryEntry {
    const char *entry_path;
    const char *source_root;
} FengFbBundleDirectoryEntry;

typedef struct FengFbLibraryBundleSpec {
    const char *package_path;
    const char *package_name;
    const char *package_version;
    const char *library_path;
    const FengFbBundleDependency *dependencies;
    size_t dependency_count;
    /* Optional. When non-NULL, the entire directory tree at this path is
     * mirrored into the `.fb` archive under `mod/`. Only files whose name
     * ends with `.ft` are included; intermediate directories are added so
     * the bundle reflects the on-disk module layout. */
    const char *public_mod_root;
    /* Optional. When non-NULL, mirrors the entire directory tree at this path
     * into the `.fb` archive under `extlib/`. The source root is expected to
     * contain per-platform subdirectories such as `<host-target>/`. */
    const char *extlib_root;
    /* Optional. Each entry mirrors one staged asset directory into the `.fb`
     * archive at the configured target directory. `entry_path` is relative to
     * the `.fb` root. */
    const FengFbBundleDirectoryEntry *asset_entries;
    size_t asset_entry_count;
} FengFbLibraryBundleSpec;

const char *feng_fb_host_static_library_suffix(void);

char *feng_fb_host_static_library_file_name(const char *library_name);

bool feng_fb_is_host_static_library_path(const char *path);

bool feng_fb_host_static_library_matches_name(const char *path,
                                              const char *library_name);

bool feng_fb_detect_host_target(char **out_host_target, char **out_error_message);

bool feng_fb_write_library_bundle(const FengFbLibraryBundleSpec *spec,
                                  char **out_error_message);

#endif /* FENG_ARCHIVE_FB_H */
