#ifndef FENG_SYMBOL_EXCEPTION_IO_H
#define FENG_SYMBOL_EXCEPTION_IO_H

#include "symbol/internal.h"

/* Symbol roots preserve public declaration identity independently of graph
 * IDs. Templates retain their graph when produced by the reader. */
typedef struct FengSymbolExceptionRoot {
    uint32_t symbol;
    FengExceptionTemplate summary;
} FengSymbolExceptionRoot;

/* String-pool adapters keep all FT encoding concerns in Symbol. */
typedef uint32_t (*FengExceptionInternString)(void *user, const char *text);
typedef const char *(*FengExceptionReadString)(void *user, uint32_t id);

bool feng_symbol_exception_write(const FengSymbolExceptionRoot *roots, size_t count,
                                 FengExceptionInternString intern, void *user, unsigned char **out_data,
                                 size_t *out_size, const char *path, FengSymbolError *error);
bool feng_symbol_exception_read(const unsigned char *data, size_t size, uint32_t expected_count,
                                FengExceptionReadString string, void *user,
                                FengSymbolExceptionRoot **out_roots, size_t *out_count, const char *path,
                                FengSymbolError *error);

/* Release reader-owned roots. Writer inputs borrow their templates. */
void feng_symbol_exception_roots_free(FengSymbolExceptionRoot *roots, size_t count);

#endif
