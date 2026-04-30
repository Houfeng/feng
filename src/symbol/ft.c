#include "symbol/ft.h"

#include "symbol/ft_internal.h"

bool feng_symbol_ft_read_file(const char *path,
                              const FengSymbolFtReadOptions *options,
                              FengSymbolGraph **out_graph,
                              FengSymbolError *out_error) {
    return feng_symbol_ft_read_file_internal(path, options, out_graph, out_error);
}

bool feng_symbol_ft_write_module(const FengSymbolModuleGraph *module,
                                 FengSymbolProfile profile,
                                 const char *path,
                                 FengSymbolError *out_error) {
    return feng_symbol_ft_write_module_internal(module, profile, path, out_error);
}

