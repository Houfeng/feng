#ifndef FENG_INTRINSIC_H
#define FENG_INTRINSIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Intrinsic native APIs are compiler-shipped helpers exposed through the
 * ordinary extern-ABI surface. They are intentionally separate from the
 * runtime object ABI. */
int64_t feng_string_utf8_length(uint8_t *value);

#ifdef __cplusplus
}
#endif

#endif
