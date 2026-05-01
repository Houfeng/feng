#include "cli/pkg_bridge.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "parser/parser.h"   /* FengDecl, FengProgram, FengSlice */
#include "symbol/symbol.h"   /* FengSymbolDeclView, FengSymbolImportedModule */

/* -------------------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------------- */

/* One synthesized top-level decl built from a FengSymbolDeclView. */
typedef struct SynthDecl {
    char *name_buf;  /* owned copy of the name string */
    FengDecl decl;   /* embedded; name slices point into name_buf */
} SynthDecl;

/* One synthetic FengProgram holding the public decls of an external module. */
typedef struct SynthProgram {
    SynthDecl *decls;     /* owned array */
    FengDecl **decl_ptrs; /* owned array of FengDecl* (= program.declarations) */
    size_t decl_count;
    FengProgram program;  /* embedded */
} SynthProgram;

/* One cache entry per external-package module that was requested. */
typedef struct SynthModuleEntry {
    FengSlice *segments;    /* owned array of FengSlice */
    char **segment_strs;    /* owned array of char* (backing data for each slice) */
    size_t segment_count;
    SynthProgram *prog;     /* separately heap-allocated for pointer stability */
    /* prog_ptr[0] is fixed up to &prog->program AFTER the entry is appended
     * to the ctx->entries array (which may realloc, invalidating in-array
     * self-referential pointers). */
    const FengProgram *prog_ptr[1];
    FengSemanticModule sem_mod; /* sem_mod.programs = prog_ptr (fixed after append) */
} SynthModuleEntry;

/* The bridge object. */
struct FengCliPkgBridge {
    const FengSymbolProvider *provider; /* not owned */
    SynthModuleEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Append a copy of *e to the bridge's entries array.
 * Returns a stable pointer to the new element, or NULL on OOM. */
static SynthModuleEntry *bridge_append(FengCliPkgBridge *b,
                                       const SynthModuleEntry *e) {
    size_t new_capacity;
    SynthModuleEntry *grown;

    if (b->entry_count == b->entry_capacity) {
        new_capacity = b->entry_capacity == 0U ? 4U : b->entry_capacity * 2U;
        grown = (SynthModuleEntry *)realloc(b->entries,
                                            new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        b->entries = grown;
        b->entry_capacity = new_capacity;
    }
    b->entries[b->entry_count] = *e;
    return &b->entries[b->entry_count++];
}

/* Free an in-progress entry that was never appended (or a partially-built one
 * that failed mid-construction). */
static void entry_free_partial(SynthModuleEntry *e) {
    size_t j;

    if (e->prog != NULL) {
        for (j = 0U; j < e->prog->decl_count; ++j) {
            free(e->prog->decls[j].name_buf);
        }
        free(e->prog->decls);
        free(e->prog->decl_ptrs);
        free(e->prog);
    }
    if (e->segment_strs != NULL) {
        for (j = 0U; j < e->segment_count; ++j) {
            free(e->segment_strs[j]);
        }
        free(e->segment_strs);
    }
    free(e->segments);
}

/* Build and cache a synthetic FengSemanticModule for the given path.
 * Returns a cached pointer if already built, NULL on OOM or not-found. */
static const FengSemanticModule *bridge_get_module(const void *user,
                                                   const FengSlice *segments,
                                                   size_t segment_count) {
    FengCliPkgBridge *b = (FengCliPkgBridge *)user;
    const FengSymbolImportedModule *imp_mod;
    SynthModuleEntry entry;
    SynthModuleEntry *ep;
    size_t i;
    size_t decl_count;

    if (b == NULL || b->provider == NULL) {
        return NULL;
    }

    /* Cache look-up. */
    for (i = 0U; i < b->entry_count; ++i) {
        const SynthModuleEntry *e = &b->entries[i];
        size_t k;
        bool match;

        if (e->segment_count != segment_count) {
            continue;
        }
        match = true;
        for (k = 0U; k < segment_count; ++k) {
            if (e->segments[k].length != segments[k].length ||
                memcmp(e->segments[k].data, segments[k].data,
                       segments[k].length) != 0) {
                match = false;
                break;
            }
        }
        if (match) {
            return &b->entries[i].sem_mod;
        }
    }

    imp_mod = feng_symbol_provider_find_module(b->provider, segments, segment_count);
    if (imp_mod == NULL) {
        return NULL;
    }

    memset(&entry, 0, sizeof(entry));

    /* --- copy path segments --- */
    entry.segment_count = segment_count;
    if (segment_count > 0U) {
        entry.segment_strs = (char **)calloc(segment_count, sizeof(char *));
        entry.segments = (FengSlice *)calloc(segment_count, sizeof(FengSlice));
        if (entry.segment_strs == NULL || entry.segments == NULL) {
            goto fail;
        }
        for (i = 0U; i < segment_count; ++i) {
            size_t len = segments[i].length;

            entry.segment_strs[i] = (char *)malloc(len + 1U);
            if (entry.segment_strs[i] == NULL) {
                goto fail;
            }
            memcpy(entry.segment_strs[i], segments[i].data, len);
            entry.segment_strs[i][len] = '\0';
            entry.segments[i].data = entry.segment_strs[i];
            entry.segments[i].length = len;
        }
    }

    /* --- build synthetic program with public decls --- */
    decl_count = feng_symbol_module_public_decl_count(imp_mod);
    entry.prog = (SynthProgram *)calloc(1U, sizeof(*entry.prog));
    if (entry.prog == NULL) {
        goto fail;
    }
    if (decl_count > 0U) {
        entry.prog->decls = (SynthDecl *)calloc(decl_count, sizeof(SynthDecl));
        entry.prog->decl_ptrs = (FengDecl **)calloc(decl_count, sizeof(FengDecl *));
        if (entry.prog->decls == NULL || entry.prog->decl_ptrs == NULL) {
            goto fail;
        }
    }

    for (i = 0U; i < decl_count; ++i) {
        const FengSymbolDeclView *sv = feng_symbol_module_public_decl_at(imp_mod, i);
        FengDeclKind feng_kind;
        FengSlice name_slice;
        FengSlice name;
        size_t name_len;
        SynthDecl *sd;

        switch (feng_symbol_decl_kind(sv)) {
            case FENG_SYMBOL_DECL_KIND_TYPE:     feng_kind = FENG_DECL_TYPE;           break;
            case FENG_SYMBOL_DECL_KIND_SPEC:     feng_kind = FENG_DECL_SPEC;           break;
            case FENG_SYMBOL_DECL_KIND_FUNCTION: feng_kind = FENG_DECL_FUNCTION;       break;
            case FENG_SYMBOL_DECL_KIND_BINDING:  feng_kind = FENG_DECL_GLOBAL_BINDING; break;
            default:
                /* FIT, FIELD, METHOD, CONSTRUCTOR, FINALIZER, MODULE:
                 * not top-level importable names in Phase 1; skip. */
                continue;
        }

        name_slice = feng_symbol_decl_name(sv);
        name_len = name_slice.length;

        sd = &entry.prog->decls[entry.prog->decl_count];
        memset(sd, 0, sizeof(*sd));
        sd->name_buf = (char *)malloc(name_len + 1U);
        if (sd->name_buf == NULL) {
            goto fail;
        }
        memcpy(sd->name_buf, name_slice.data, name_len);
        sd->name_buf[name_len] = '\0';

        name.data = sd->name_buf;
        name.length = name_len;

        sd->decl.kind = feng_kind;
        sd->decl.visibility = FENG_VISIBILITY_PUBLIC;
        switch (feng_kind) {
            case FENG_DECL_TYPE:           sd->decl.as.type_decl.name     = name; break;
            case FENG_DECL_SPEC:           sd->decl.as.spec_decl.name     = name; break;
            case FENG_DECL_FUNCTION:       sd->decl.as.function_decl.name = name; break;
            case FENG_DECL_GLOBAL_BINDING: sd->decl.as.binding.name       = name; break;
            default: break;
        }

        entry.prog->decl_ptrs[entry.prog->decl_count] = &sd->decl;
        ++entry.prog->decl_count;
    }

    entry.prog->program.path = "";
    entry.prog->program.module_segments = entry.segments;
    entry.prog->program.module_segment_count = entry.segment_count;
    entry.prog->program.module_visibility = FENG_VISIBILITY_PUBLIC;
    entry.prog->program.declarations = entry.prog->decl_ptrs;
    entry.prog->program.declaration_count = entry.prog->decl_count;

    /* sem_mod.programs must point to prog_ptr[0] which holds &prog->program.
     * Fix up AFTER appending, because the append may realloc b->entries,
     * invalidating any pre-append pointer into the array. */
    entry.sem_mod.segments = entry.segments;
    entry.sem_mod.segment_count = entry.segment_count;
    entry.sem_mod.visibility = FENG_VISIBILITY_PUBLIC;
    entry.sem_mod.program_count = 1U;
    entry.sem_mod.program_capacity = 1U;
    entry.sem_mod.origin = FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE;
    /* sem_mod.programs and prog_ptr fixed below after append */

    ep = bridge_append(b, &entry);
    if (ep == NULL) {
        goto fail;
    }
    ep->prog_ptr[0] = &ep->prog->program;
    ep->sem_mod.programs = ep->prog_ptr;

    return &ep->sem_mod;

fail:
    entry_free_partial(&entry);
    return NULL;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

FengCliPkgBridge *feng_cli_pkg_bridge_create(const FengSymbolProvider *provider) {
    FengCliPkgBridge *b = (FengCliPkgBridge *)calloc(1U, sizeof(*b));
    if (b == NULL) {
        return NULL;
    }
    b->provider = provider;
    return b;
}

void feng_cli_pkg_bridge_free(FengCliPkgBridge *bridge) {
    size_t i;

    if (bridge == NULL) {
        return;
    }
    for (i = 0U; i < bridge->entry_count; ++i) {
        entry_free_partial(&bridge->entries[i]);
    }
    free(bridge->entries);
    free(bridge);
}

FengSemanticImportedModuleQuery feng_cli_pkg_bridge_as_query(FengCliPkgBridge *bridge) {
    FengSemanticImportedModuleQuery q = {0};
    if (bridge != NULL) {
        q.user = bridge;
        q.get_module = bridge_get_module;
    }
    return q;
}
