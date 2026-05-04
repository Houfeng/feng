#ifndef FENG_SYMBOL_INTERNAL_H
#define FENG_SYMBOL_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "symbol/provider.h"

typedef enum FengSymbolRelationKind {
    FENG_SYMBOL_RELATION_TYPE_IMPLEMENTS_SPEC = 1,
    FENG_SYMBOL_RELATION_FIT_IMPLEMENTS_SPEC = 2,
    FENG_SYMBOL_RELATION_FIT_EXTENDS_TYPE = 3,
    FENG_SYMBOL_RELATION_CTOR_BINDS_MEMBER = 4
} FengSymbolRelationKind;

typedef enum FengSymbolAttrKind {
    FENG_SYMBOL_ATTR_DECLARED_SPECS = 1,
    FENG_SYMBOL_ATTR_UNION = 2,
    FENG_SYMBOL_ATTR_CALL_CONV = 3,
    FENG_SYMBOL_ATTR_ABI_LIBRARY = 4
} FengSymbolAttrKind;

typedef struct FengSymbolParamView {
    FengToken token;
    char *name;
    FengMutability mutability;
    FengSymbolTypeView *type;
} FengSymbolParamView;

struct FengSymbolTypeView {
    FengSymbolTypeKind kind;
    union {
        struct {
            char *name;
        } builtin;
        struct {
            char **segments;
            size_t segment_count;
        } named;
        struct {
            FengSymbolTypeView *inner;
        } pointer;
        struct {
            FengSymbolTypeView *element;
            bool *layer_writable;
            size_t rank;
        } array;
    } as;
};

struct FengSymbolDeclView {
    FengSymbolDeclKind kind;
    FengVisibility visibility;
    FengMutability mutability;
    bool is_extern;
    bool fixed_annotated;
    bool bounded_decl;
    bool union_annotated;
    bool has_doc;
    FengAnnotationKind calling_convention;
    char *abi_library;
    char *doc;
    char *name;
    char *path;
    FengToken token;
    FengSymbolTypeView *value_type;
    FengSymbolTypeView *return_type;
    FengSymbolParamView *params;
    size_t param_count;
    FengSymbolTypeView **declared_specs;
    size_t declared_spec_count;
    struct FengSymbolDeclView **members;
    size_t member_count;
    struct FengSymbolDeclView *owner;
    FengSymbolTypeView *fit_target;
};

typedef struct FengSymbolRelation {
    FengSymbolRelationKind kind;
    FengSymbolDeclView *left;
    FengSymbolDeclView *right;
    FengSymbolDeclView *owner;
} FengSymbolRelation;

struct FengSymbolModuleGraph {
    FengSymbolProfile profile;
    FengVisibility visibility;
    char **segments;
    size_t segment_count;
    char *primary_path;
    char **uses;
    size_t use_count;
    FengSymbolDeclView root_decl;
    FengSymbolRelation *relations;
    size_t relation_count;
};

struct FengSymbolGraph {
    FengSymbolModuleGraph **modules;
    size_t module_count;
};

struct FengSymbolFitView {
    const FengSymbolDeclView *decl;
};

struct FengSymbolImportedModule {
    FengSymbolModuleGraph *module;
    FengSymbolProfile profile;
    char *source_path;
    FengSymbolFitView *fits;
    size_t fit_count;
};

struct FengSymbolProvider {
    FengSymbolImportedModule *modules;
    size_t module_count;
    size_t module_capacity;
};

char *feng_symbol_internal_dup_cstr(const char *text);
char *feng_symbol_internal_dup_slice(FengSlice slice);
bool feng_symbol_internal_slice_equals(FengSlice lhs, FengSlice rhs);
bool feng_symbol_internal_set_error(FengSymbolError *error,
                                    const char *path,
                                    FengToken token,
                                    const char *fmt,
                                    ...);

uint64_t feng_symbol_internal_fnv1a64(const void *data, size_t length);
uint64_t feng_symbol_internal_fnv1a64_extend(uint64_t seed, const void *data, size_t length);

void feng_symbol_internal_type_free(FengSymbolTypeView *type);
void feng_symbol_internal_decl_free_members(FengSymbolDeclView *decl);
FengSymbolTypeView *feng_symbol_internal_type_clone(const FengSymbolTypeView *type,
                                                    FengSymbolError *out_error);
FengSymbolDeclView *feng_symbol_internal_decl_clone(const FengSymbolDeclView *decl,
                                                    FengSymbolError *out_error);
FengSymbolModuleGraph *feng_symbol_internal_module_clone(const FengSymbolModuleGraph *module,
                                                         FengSymbolProfile profile,
                                                         FengSymbolError *out_error);
void feng_symbol_internal_module_free(FengSymbolModuleGraph *module);
void feng_symbol_internal_imported_module_free(FengSymbolImportedModule *module);
bool feng_symbol_internal_imported_module_init_fit_views(FengSymbolImportedModule *module,
                                                         FengSymbolError *out_error);

bool feng_symbol_internal_graph_append_module(FengSymbolGraph *graph,
                                              FengSymbolModuleGraph *module,
                                              FengSymbolError *out_error);

#endif /* FENG_SYMBOL_INTERNAL_H */
