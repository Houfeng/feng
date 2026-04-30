#ifndef FENG_ARCHIVE_FB_H
#define FENG_ARCHIVE_FB_H

#include <stdbool.h>

typedef struct FengFbLibraryBundleSpec {
    const char *package_path;
    const char *package_name;
    const char *package_version;
    const char *library_path;
} FengFbLibraryBundleSpec;

bool feng_fb_detect_host_target(char **out_host_target, char **out_error_message);

bool feng_fb_write_library_bundle(const FengFbLibraryBundleSpec *spec,
                                  char **out_error_message);

#endif /* FENG_ARCHIVE_FB_H */
