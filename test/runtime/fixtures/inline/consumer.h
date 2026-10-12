/* Shared declarations for two independently archived runtime consumers. */
#ifndef INLINE_TEST_CONSUMER_H
#define INLINE_TEST_CONSUMER_H
#include "feng_runtime.h"
typedef size_t (*LengthFn)(const FengArray *);
typedef void *(*DataFn)(FengArray *);
typedef void (*CheckFn)(const FengArray *, size_t);
/* Each consumer reports the canonical function addresses and executes calls. */
LengthFn consumer_a_length(void);
LengthFn consumer_b_length(void);
DataFn consumer_a_data(void);
DataFn consumer_b_data(void);
CheckFn consumer_a_check(void);
CheckFn consumer_b_check(void);
size_t consumer_a_read(FengArray *array, size_t index);
size_t consumer_b_read(FengArray *array, size_t index);
#endif
