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

/* One target platform's formal and native library staging inputs. */
typedef struct FengFbBundlePlatformArtifact {
    const char *platform;
    const char *library_path;
    const char *extlib_root;
} FengFbBundlePlatformArtifact;

typedef struct FengFbLibraryBundleSpec {
    const char *package_path;
    const char *package_name;
    const char *package_version;
    const FengFbBundlePlatformArtifact *platform_artifacts;
    size_t platform_artifact_count;
    const FengFbBundleDependency *dependencies;
    size_t dependency_count;
    /* Optional. When non-NULL, the entire directory tree at this path is
     * mirrored into the `.fb` archive under `mod/`. Only files whose name
     * ends with `.ft` are included; intermediate directories are added so
     * the bundle reflects the on-disk module layout. */
    const char *public_mod_root;
    /* Optional. Each entry mirrors one staged asset directory into the `.fb`
     * archive at the configured target directory. `entry_path` is relative to
     * the `.fb` root. */
    const FengFbBundleDirectoryEntry *asset_entries;
    size_t asset_entry_count;
} FengFbLibraryBundleSpec;

bool feng_fb_write_library_bundle(const FengFbLibraryBundleSpec *spec,
                                  char **out_error_message);

#endif /* FENG_ARCHIVE_FB_H */
