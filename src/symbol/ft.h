#ifndef FENG_SYMBOL_FT_H
#define FENG_SYMBOL_FT_H

#include <stddef.h>

#include "symbol/symbol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengSymbolFtReadOptions {
    FengSymbolProfile expected_profile;
} FengSymbolFtReadOptions;

bool feng_symbol_ft_read_file(const char *path,
                              const FengSymbolFtReadOptions *options,
                              FengSymbolGraph **out_graph,
                              FengSymbolError *out_error);

bool feng_symbol_ft_read_bytes(const void *data,
                               size_t length,
                               const char *source_name,
                               const FengSymbolFtReadOptions *options,
                               FengSymbolGraph **out_graph,
                               FengSymbolError *out_error);

bool feng_symbol_ft_write_module(const FengSymbolModuleGraph *module,
                                 FengSymbolProfile profile,
                                 const char *path,
                                 FengSymbolError *out_error);

#ifdef __cplusplus
}
#endif

#endif /* FENG_SYMBOL_FT_H */
