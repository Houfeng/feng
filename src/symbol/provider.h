#ifndef FENG_SYMBOL_PROVIDER_H
#define FENG_SYMBOL_PROVIDER_H

#include <stdbool.h>
#include <stddef.h>

#include "symbol/symbol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengSymbolProvider FengSymbolProvider;
typedef struct FengSymbolImportedModule FengSymbolImportedModule;
typedef struct FengSymbolDeclView FengSymbolDeclView;
typedef struct FengSymbolFitView FengSymbolFitView;

bool feng_symbol_provider_create(FengSymbolProvider **out_provider,
                                 FengSymbolError *out_error);

bool feng_symbol_provider_add_graph(FengSymbolProvider *provider,
                                    const FengSymbolGraph *graph,
                                    FengSymbolError *out_error);

bool feng_symbol_provider_add_ft_root(FengSymbolProvider *provider,
                                      const char *root_path,
                                      FengSymbolProfile profile,
                                      FengSymbolError *out_error);

bool feng_symbol_provider_add_bundle(FengSymbolProvider *provider,
                                     const char *bundle_path,
                                     FengSymbolError *out_error);

const FengSymbolImportedModule *feng_symbol_provider_find_module(
    const FengSymbolProvider *provider,
    const FengSlice *segments,
    size_t segment_count);

const FengSymbolDeclView *feng_symbol_module_find_public_type(
    const FengSymbolImportedModule *module,
    FengSlice name);

const FengSymbolDeclView *feng_symbol_module_find_public_spec(
    const FengSymbolImportedModule *module,
    FengSlice name);

const FengSymbolDeclView *feng_symbol_module_find_public_value(
    const FengSymbolImportedModule *module,
    FengSlice name);

size_t feng_symbol_module_public_value_count(const FengSymbolImportedModule *module,
                                             FengSlice name);

const FengSymbolDeclView *feng_symbol_module_public_value_at(
    const FengSymbolImportedModule *module,
    FengSlice name,
    size_t index);

const FengSymbolDeclView *feng_symbol_decl_find_public_member(
    const FengSymbolDeclView *owner,
    FengSlice name);

size_t feng_symbol_decl_public_member_count(const FengSymbolDeclView *owner,
                                            FengSlice name);

const FengSymbolDeclView *feng_symbol_decl_public_member_at(const FengSymbolDeclView *owner,
                                                            FengSlice name,
                                                            size_t index);

size_t feng_symbol_module_fit_count(const FengSymbolImportedModule *module);
const FengSymbolFitView *feng_symbol_module_fit_at(const FengSymbolImportedModule *module,
                                                   size_t index);

size_t feng_symbol_module_segment_count(const FengSymbolImportedModule *module);
FengSlice feng_symbol_module_segment_at(const FengSymbolImportedModule *module, size_t index);

FengSymbolDeclKind feng_symbol_decl_kind(const FengSymbolDeclView *decl);
FengSlice feng_symbol_decl_name(const FengSymbolDeclView *decl);
FengVisibility feng_symbol_decl_visibility(const FengSymbolDeclView *decl);
FengMutability feng_symbol_decl_mutability(const FengSymbolDeclView *decl);
bool feng_symbol_decl_is_extern(const FengSymbolDeclView *decl);
bool feng_symbol_decl_has_bounded_decl(const FengSymbolDeclView *decl);
const FengSymbolTypeView *feng_symbol_decl_value_type(const FengSymbolDeclView *decl);
const FengSymbolTypeView *feng_symbol_decl_return_type(const FengSymbolDeclView *decl);
const FengSymbolTypeView *feng_symbol_decl_fit_target(const FengSymbolDeclView *decl);
size_t feng_symbol_decl_param_count(const FengSymbolDeclView *decl);
FengSlice feng_symbol_decl_param_name(const FengSymbolDeclView *decl, size_t index);
FengMutability feng_symbol_decl_param_mutability(const FengSymbolDeclView *decl, size_t index);
const FengSymbolTypeView *feng_symbol_decl_param_type(const FengSymbolDeclView *decl,
                                                      size_t index);
size_t feng_symbol_decl_declared_spec_count(const FengSymbolDeclView *decl);
const FengSymbolTypeView *feng_symbol_decl_declared_spec_at(const FengSymbolDeclView *decl,
                                                            size_t index);

const FengSymbolDeclView *feng_symbol_fit_decl(const FengSymbolFitView *fit);

FengSymbolTypeKind feng_symbol_type_kind(const FengSymbolTypeView *type);
FengSlice feng_symbol_type_builtin_name(const FengSymbolTypeView *type);
size_t feng_symbol_type_segment_count(const FengSymbolTypeView *type);
FengSlice feng_symbol_type_segment_at(const FengSymbolTypeView *type, size_t index);
const FengSymbolTypeView *feng_symbol_type_inner(const FengSymbolTypeView *type);
size_t feng_symbol_type_array_rank(const FengSymbolTypeView *type);
bool feng_symbol_type_array_layer_writable(const FengSymbolTypeView *type, size_t layer_index);

void feng_symbol_provider_free(FengSymbolProvider *provider);

#ifdef __cplusplus
}
#endif

#endif /* FENG_SYMBOL_PROVIDER_H */

