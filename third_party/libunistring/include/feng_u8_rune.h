#ifndef FENG_U8_RUNE_H
#define FENG_U8_RUNE_H

#include "unitypes.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t u8_mbsnlen(const uint8_t *s, size_t n);
const uint8_t *u8_next(ucs4_t *puc, const uint8_t *s);
const uint8_t *u8_prev(ucs4_t *puc, const uint8_t *s, const uint8_t *start);

#ifdef __cplusplus
}
#endif

#endif
