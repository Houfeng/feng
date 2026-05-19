#ifndef FENG_U8_GRAPHEME_H
#define FENG_U8_GRAPHEME_H

#include "unitypes.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void u8_grapheme_breaks(const uint8_t *s, size_t n, char *p);
const uint8_t *u8_grapheme_next(const uint8_t *s, const uint8_t *end);
const uint8_t *u8_grapheme_prev(const uint8_t *s, const uint8_t *start);

#ifdef __cplusplus
}
#endif

#endif
