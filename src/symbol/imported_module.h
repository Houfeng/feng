#ifndef FENG_SYMBOL_IMPORTED_MODULE_H
#define FENG_SYMBOL_IMPORTED_MODULE_H

#include <stddef.h>

#include "semantic/semantic.h"
#include "symbol/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengSymbolImportedModuleCache FengSymbolImportedModuleCache;

/* Build a cache that lazily synthesizes external FengSemanticModule objects
 * from provider-backed public symbol views. The provider must outlive all
 * get_module() calls made through the returned query; once a module has been
 * synthesized, the cache owns the backing FengDecl/FengProgram objects. */
FengSymbolImportedModuleCache *feng_symbol_imported_module_cache_create(
    const FengSymbolProvider *provider);

/* Release all synthesized FengDecl/FengProgram/FengSemanticModule storage.
 * Safe to call with NULL. */
void feng_symbol_imported_module_cache_free(FengSymbolImportedModuleCache *cache);

/* Return a query struct whose lifetime is tied to the cache. */
FengSemanticImportedModuleQuery feng_symbol_imported_module_cache_as_query(
    FengSymbolImportedModuleCache *cache);

#ifdef __cplusplus
}
#endif

#endif /* FENG_SYMBOL_IMPORTED_MODULE_H */
