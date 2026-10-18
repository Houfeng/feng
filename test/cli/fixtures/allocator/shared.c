/* A separate native dependency deliberately uses only standard allocation APIs. */
#include <stddef.h>
#include <stdlib.h>

/* Return storage to be released by the consumer. */
void *allocator_shared_allocate(size_t size) { return malloc(size); }

/* Release storage allocated outside this native dependency. */
void allocator_shared_free(void *pointer) { free(pointer); }
