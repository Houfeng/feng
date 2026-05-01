#ifndef FENG_CLI_PKG_BRIDGE_H
#define FENG_CLI_PKG_BRIDGE_H

/*
 * pkg_bridge — translates FengSymbolProvider public symbols into a
 * FengSemanticImportedModuleQuery that the semantic analyser can consume.
 *
 * Responsibility boundary
 * -----------------------
 *  symbol/  : reads .fb/.ft files; exposes FengSymbolProvider / FengSymbolDeclView
 *  semantic/: owns FengSemanticModule / FengSemanticImportedModuleQuery
 *  cli/pkg_bridge: bridges the two — builds synthetic FengSemanticModule objects
 *                  from provider data; belongs in CLI because it is the only
 *                  layer that holds both dependencies simultaneously.
 *
 * Lifetime
 * --------
 * 1. feng_cli_pkg_bridge_create()  – allocate; may return NULL on OOM
 * 2. feng_cli_pkg_bridge_as_query()– return query; valid while bridge is alive
 * 3. feng_cli_pkg_bridge_free()    – release all synthetic objects
 *
 * The bridge does NOT own the FengSymbolProvider; the caller owns it.
 */

#include <stddef.h>

#include "semantic/semantic.h"
#include "symbol/provider.h"

typedef struct FengCliPkgBridge FengCliPkgBridge;

/* Create a bridge backed by the given provider.
 * Returns NULL on OOM.  provider must outlive the bridge. */
FengCliPkgBridge *feng_cli_pkg_bridge_create(const FengSymbolProvider *provider);

/* Free all synthetic objects.  Safe to call with NULL. */
void feng_cli_pkg_bridge_free(FengCliPkgBridge *bridge);

/* Return a query struct whose lifetime is tied to the bridge.
 * The returned value may be passed directly to FengSemanticAnalyzeOptions. */
FengSemanticImportedModuleQuery feng_cli_pkg_bridge_as_query(FengCliPkgBridge *bridge);

#endif /* FENG_CLI_PKG_BRIDGE_H */
