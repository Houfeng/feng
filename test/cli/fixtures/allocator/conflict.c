/* Link-only competing allocator: this fixture must never be executed. */
#include <stddef.h>
/* Each standard entry conflicts with a forced builtin definition. */
void *malloc(size_t n) { (void)n; return NULL; }
/* A second definition of zero-initializing allocation. */
void *calloc(size_t n, size_t size) { (void)n; (void)size; return NULL; }
/* A second definition of resizing. */
void *realloc(void *p, size_t n) { (void)p; (void)n; return NULL; }
/* A second definition of deallocation. */
void free(void *p) { (void)p; }
