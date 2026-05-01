#include "symbol/imported_module.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "symbol/internal.h"

/* One synthesized top-level decl built from a FengSymbolDeclView. */
typedef struct SynthDecl {
    char *name_buf;  /* owned copy of the name string */
    FengDecl decl;   /* name slices point into name_buf */
} SynthDecl;

/* One synthetic FengProgram holding the public decls of an external module. */
typedef struct SynthProgram {
    char *path_buf;
    SynthDecl *decls;
    FengDecl **decl_ptrs;
    size_t decl_count;
    FengProgram program;
} SynthProgram;

/* One cache entry per imported package module. */
typedef struct SynthModuleEntry {
    FengSlice *segments;
    char **segment_strs;
    size_t segment_count;
    SynthProgram *prog;
    const FengProgram *prog_ptr[1];
    FengSemanticModule sem_mod;
} SynthModuleEntry;

struct FengSymbolImportedModuleCache {
    const FengSymbolProvider *provider; /* borrowed */
    SynthModuleEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
};

static SynthModuleEntry *cache_append(FengSymbolImportedModuleCache *cache,
                                      const SynthModuleEntry *entry) {
    size_t new_capacity;
    SynthModuleEntry *grown;

    if (cache->entry_count == cache->entry_capacity) {
        new_capacity = cache->entry_capacity == 0U ? 4U : cache->entry_capacity * 2U;
        grown = (SynthModuleEntry *)realloc(cache->entries,
                                            new_capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        cache->entries = grown;
        cache->entry_capacity = new_capacity;
    }
    cache->entries[cache->entry_count] = *entry;
    return &cache->entries[cache->entry_count++];
}

static bool segments_match(const SynthModuleEntry *entry,
                           const FengSlice *segments,
                           size_t segment_count) {
    size_t index;

    if (entry->segment_count != segment_count) {
        return false;
    }
    for (index = 0U; index < segment_count; ++index) {
        if (entry->segments[index].length != segments[index].length ||
            memcmp(entry->segments[index].data, segments[index].data,
                   segments[index].length) != 0) {
            return false;
        }
    }
    return true;
}

static char *dup_bytes(const char *text, size_t length) {
    char *dup;

    dup = (char *)malloc(length + 1U);
    if (dup == NULL) {
        return NULL;
    }
    if (length > 0U) {
        memcpy(dup, text, length);
    }
    dup[length] = '\0';
    return dup;
}

static char *dup_cstr_or_empty(const char *text) {
    if (text == NULL) {
        return dup_bytes("", 0U);
    }
    return dup_bytes(text, strlen(text));
}

static bool clone_cstr_as_slice(const char *text, FengSlice *out_slice) {
    char *dup;
    size_t length;

    if (out_slice == NULL) {
        return false;
    }
    out_slice->data = NULL;
    out_slice->length = 0U;
    if (text == NULL) {
        return true;
    }
    length = strlen(text);
    dup = dup_bytes(text, length);
    if (dup == NULL) {
        return false;
    }
    out_slice->data = dup;
    out_slice->length = length;
    return true;
}

static void free_synthetic_type_ref(FengTypeRef *type_ref) {
    size_t index;

    if (type_ref == NULL) {
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            for (index = 0U; index < type_ref->as.named.segment_count; ++index) {
                free((void *)type_ref->as.named.segments[index].data);
            }
            free(type_ref->as.named.segments);
            break;
        case FENG_TYPE_REF_POINTER:
        case FENG_TYPE_REF_ARRAY:
            free_synthetic_type_ref(type_ref->as.inner);
            break;
    }

    free(type_ref);
}

static void free_synthetic_parameters(FengParameter *params, size_t param_count) {
    size_t index;

    if (params == NULL) {
        return;
    }
    for (index = 0U; index < param_count; ++index) {
        free((void *)params[index].name.data);
        free_synthetic_type_ref(params[index].type);
    }
    free(params);
}

static void free_synthetic_type_member(FengTypeMember *member) {
    if (member == NULL) {
        return;
    }

    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
        free((void *)member->as.field.name.data);
        free_synthetic_type_ref(member->as.field.type);
    } else {
        free((void *)member->as.callable.name.data);
        free_synthetic_parameters(member->as.callable.params, member->as.callable.param_count);
        free_synthetic_type_ref(member->as.callable.return_type);
    }

    free(member);
}

static void free_synthetic_decl_payload(FengDecl *decl) {
    size_t index;

    if (decl == NULL) {
        return;
    }

    switch (decl->kind) {
        case FENG_DECL_GLOBAL_BINDING:
            free_synthetic_type_ref(decl->as.binding.type);
            break;
        case FENG_DECL_TYPE:
            for (index = 0U; index < decl->as.type_decl.member_count; ++index) {
                free_synthetic_type_member(decl->as.type_decl.members[index]);
            }
            free(decl->as.type_decl.members);
            for (index = 0U; index < decl->as.type_decl.declared_spec_count; ++index) {
                free_synthetic_type_ref(decl->as.type_decl.declared_specs[index]);
            }
            free(decl->as.type_decl.declared_specs);
            break;
        case FENG_DECL_SPEC:
            for (index = 0U; index < decl->as.spec_decl.parent_spec_count; ++index) {
                free_synthetic_type_ref(decl->as.spec_decl.parent_specs[index]);
            }
            free(decl->as.spec_decl.parent_specs);
            if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                for (index = 0U; index < decl->as.spec_decl.as.object.member_count; ++index) {
                    free_synthetic_type_member(decl->as.spec_decl.as.object.members[index]);
                }
                free(decl->as.spec_decl.as.object.members);
            } else {
                free_synthetic_parameters(decl->as.spec_decl.as.callable.params,
                                          decl->as.spec_decl.as.callable.param_count);
                free_synthetic_type_ref(decl->as.spec_decl.as.callable.return_type);
            }
            break;
        case FENG_DECL_FIT:
            free_synthetic_type_ref(decl->as.fit_decl.target);
            for (index = 0U; index < decl->as.fit_decl.spec_count; ++index) {
                free_synthetic_type_ref(decl->as.fit_decl.specs[index]);
            }
            free(decl->as.fit_decl.specs);
            for (index = 0U; index < decl->as.fit_decl.member_count; ++index) {
                free_synthetic_type_member(decl->as.fit_decl.members[index]);
            }
            free(decl->as.fit_decl.members);
            break;
        case FENG_DECL_FUNCTION:
            free_synthetic_parameters(decl->as.function_decl.params,
                                      decl->as.function_decl.param_count);
            free_synthetic_type_ref(decl->as.function_decl.return_type);
            break;
    }
}

static FengTypeRef *synthesize_type_ref(const FengSymbolTypeView *symbol_type) {
    FengTypeRef *type_ref;
    size_t index;

    if (symbol_type == NULL) {
        return NULL;
    }

    switch (symbol_type->kind) {
        case FENG_SYMBOL_TYPE_KIND_BUILTIN:
            type_ref = (FengTypeRef *)calloc(1U, sizeof(*type_ref));
            if (type_ref == NULL) {
                return NULL;
            }
            type_ref->kind = FENG_TYPE_REF_NAMED;
            type_ref->as.named.segment_count = 1U;
            type_ref->as.named.segments = (FengSlice *)calloc(1U, sizeof(FengSlice));
            if (type_ref->as.named.segments == NULL ||
                !clone_cstr_as_slice(symbol_type->as.builtin.name, &type_ref->as.named.segments[0])) {
                free_synthetic_type_ref(type_ref);
                return NULL;
            }
            return type_ref;

        case FENG_SYMBOL_TYPE_KIND_NAMED:
            type_ref = (FengTypeRef *)calloc(1U, sizeof(*type_ref));
            if (type_ref == NULL) {
                return NULL;
            }
            type_ref->kind = FENG_TYPE_REF_NAMED;
            type_ref->as.named.segment_count = symbol_type->as.named.segment_count;
            if (type_ref->as.named.segment_count > 0U) {
                type_ref->as.named.segments = (FengSlice *)calloc(
                    type_ref->as.named.segment_count, sizeof(FengSlice));
                if (type_ref->as.named.segments == NULL) {
                    free_synthetic_type_ref(type_ref);
                    return NULL;
                }
                for (index = 0U; index < type_ref->as.named.segment_count; ++index) {
                    if (!clone_cstr_as_slice(symbol_type->as.named.segments[index],
                                             &type_ref->as.named.segments[index])) {
                        free_synthetic_type_ref(type_ref);
                        return NULL;
                    }
                }
            }
            return type_ref;

        case FENG_SYMBOL_TYPE_KIND_POINTER:
            type_ref = (FengTypeRef *)calloc(1U, sizeof(*type_ref));
            if (type_ref == NULL) {
                return NULL;
            }
            type_ref->kind = FENG_TYPE_REF_POINTER;
            type_ref->as.inner = synthesize_type_ref(symbol_type->as.pointer.inner);
            if (symbol_type->as.pointer.inner != NULL && type_ref->as.inner == NULL) {
                free_synthetic_type_ref(type_ref);
                return NULL;
            }
            return type_ref;

        case FENG_SYMBOL_TYPE_KIND_ARRAY: {
            FengTypeRef *inner;

            if (symbol_type->as.array.rank == 0U) {
                return NULL;
            }
            inner = synthesize_type_ref(symbol_type->as.array.element);
            if (symbol_type->as.array.element != NULL && inner == NULL) {
                return NULL;
            }
            for (index = symbol_type->as.array.rank; index > 0U; --index) {
                FengTypeRef *wrapper = (FengTypeRef *)calloc(1U, sizeof(*wrapper));
                if (wrapper == NULL) {
                    free_synthetic_type_ref(inner);
                    return NULL;
                }
                wrapper->kind = FENG_TYPE_REF_ARRAY;
                wrapper->array_element_writable =
                    symbol_type->as.array.layer_writable[index - 1U];
                wrapper->as.inner = inner;
                inner = wrapper;
            }
            return inner;
        }

        case FENG_SYMBOL_TYPE_KIND_INVALID:
        default:
            break;
    }

    return NULL;
}

static FengParameter *synthesize_parameters(const FengSymbolDeclView *symbol_decl,
                                            size_t *out_count) {
    FengParameter *params;
    size_t index;

    if (out_count == NULL) {
        return NULL;
    }
    *out_count = 0U;
    if (symbol_decl == NULL || symbol_decl->param_count == 0U) {
        return NULL;
    }

    params = (FengParameter *)calloc(symbol_decl->param_count, sizeof(*params));
    if (params == NULL) {
        return NULL;
    }
    for (index = 0U; index < symbol_decl->param_count; ++index) {
        params[index].token = symbol_decl->params[index].token;
        params[index].mutability = symbol_decl->params[index].mutability;
        if (!clone_cstr_as_slice(symbol_decl->params[index].name, &params[index].name)) {
            free_synthetic_parameters(params, symbol_decl->param_count);
            return NULL;
        }
        params[index].type = synthesize_type_ref(symbol_decl->params[index].type);
        if (symbol_decl->params[index].type != NULL && params[index].type == NULL) {
            free_synthetic_parameters(params, symbol_decl->param_count);
            return NULL;
        }
    }
    *out_count = symbol_decl->param_count;
    return params;
}

static FengTypeRef **synthesize_type_ref_list(FengSymbolTypeView *const *types,
                                              size_t type_count) {
    FengTypeRef **refs;
    size_t index;
    size_t cleanup_index;

    if (type_count == 0U) {
        return NULL;
    }
    refs = (FengTypeRef **)calloc(type_count, sizeof(*refs));
    if (refs == NULL) {
        return NULL;
    }
    for (index = 0U; index < type_count; ++index) {
        refs[index] = synthesize_type_ref(types[index]);
        if (types[index] != NULL && refs[index] == NULL) {
            for (cleanup_index = 0U; cleanup_index < type_count; ++cleanup_index) {
                free_synthetic_type_ref(refs[cleanup_index]);
            }
            free(refs);
            return NULL;
        }
    }
    return refs;
}

static bool synthesize_callable_signature(FengCallableSignature *signature,
                                          const FengSymbolDeclView *symbol_decl,
                                          FengSlice name) {
    signature->token = symbol_decl->token;
    signature->name = name;
    signature->params = synthesize_parameters(symbol_decl, &signature->param_count);
    if (symbol_decl->param_count > 0U && signature->params == NULL) {
        return false;
    }
    signature->return_type = synthesize_type_ref(symbol_decl->return_type);
    if (symbol_decl->return_type != NULL && signature->return_type == NULL) {
        free_synthetic_parameters(signature->params, signature->param_count);
        signature->params = NULL;
        signature->param_count = 0U;
        return false;
    }
    signature->body = NULL;
    return true;
}

static FengTypeMember *synthesize_type_member(const FengSymbolDeclView *member_decl) {
    FengTypeMember *member;
    FengSlice member_name = {0};

    if (member_decl == NULL) {
        return NULL;
    }

    member = (FengTypeMember *)calloc(1U, sizeof(*member));
    if (member == NULL) {
        return NULL;
    }
    member->token = member_decl->token;
    member->visibility = member_decl->visibility;

    switch (member_decl->kind) {
        case FENG_SYMBOL_DECL_KIND_FIELD:
            member->kind = FENG_TYPE_MEMBER_FIELD;
            member->as.field.mutability = member_decl->mutability;
            if (!clone_cstr_as_slice(member_decl->name, &member->as.field.name)) {
                free_synthetic_type_member(member);
                return NULL;
            }
            member->as.field.type = synthesize_type_ref(member_decl->value_type);
            if (member_decl->value_type != NULL && member->as.field.type == NULL) {
                free_synthetic_type_member(member);
                return NULL;
            }
            member->as.field.initializer = NULL;
            return member;

        case FENG_SYMBOL_DECL_KIND_METHOD:
            member->kind = FENG_TYPE_MEMBER_METHOD;
            break;
        case FENG_SYMBOL_DECL_KIND_CONSTRUCTOR:
            member->kind = FENG_TYPE_MEMBER_CONSTRUCTOR;
            break;
        case FENG_SYMBOL_DECL_KIND_FINALIZER:
            member->kind = FENG_TYPE_MEMBER_FINALIZER;
            break;
        default:
            free(member);
            return NULL;
    }

    if (!clone_cstr_as_slice(member_decl->name, &member_name) ||
        !synthesize_callable_signature(&member->as.callable, member_decl, member_name)) {
        free((void *)member_name.data);
        free(member);
        return NULL;
    }
    return member;
}

static FengTypeMember **synthesize_type_members(const FengSymbolDeclView *symbol_decl,
                                                size_t *out_count) {
    FengTypeMember **members;
    size_t index;
    size_t cleanup_index;

    if (out_count == NULL) {
        return NULL;
    }
    *out_count = 0U;
    if (symbol_decl == NULL || symbol_decl->member_count == 0U) {
        return NULL;
    }

    members = (FengTypeMember **)calloc(symbol_decl->member_count, sizeof(*members));
    if (members == NULL) {
        return NULL;
    }
    for (index = 0U; index < symbol_decl->member_count; ++index) {
        members[index] = synthesize_type_member(symbol_decl->members[index]);
        if (members[index] == NULL) {
            for (cleanup_index = 0U; cleanup_index < symbol_decl->member_count; ++cleanup_index) {
                free_synthetic_type_member(members[cleanup_index]);
            }
            free(members);
            return NULL;
        }
    }
    *out_count = symbol_decl->member_count;
    return members;
}

static bool synthesize_decl_from_symbol(SynthDecl *synth_decl,
                                        const FengSymbolDeclView *symbol_decl) {
    FengSlice name;

    if (synth_decl == NULL || symbol_decl == NULL || symbol_decl->name == NULL) {
        return false;
    }

    memset(synth_decl, 0, sizeof(*synth_decl));
    synth_decl->name_buf = dup_bytes(symbol_decl->name, strlen(symbol_decl->name));
    if (synth_decl->name_buf == NULL) {
        return false;
    }

    name.data = synth_decl->name_buf;
    name.length = strlen(symbol_decl->name);
    synth_decl->decl.token = symbol_decl->token;
    synth_decl->decl.visibility = symbol_decl->visibility;
    synth_decl->decl.is_extern = symbol_decl->is_extern;

    switch (symbol_decl->kind) {
        case FENG_SYMBOL_DECL_KIND_TYPE:
            synth_decl->decl.kind = FENG_DECL_TYPE;
            synth_decl->decl.as.type_decl.name = name;
            synth_decl->decl.as.type_decl.members =
                synthesize_type_members(symbol_decl, &synth_decl->decl.as.type_decl.member_count);
            if (symbol_decl->member_count > 0U && synth_decl->decl.as.type_decl.members == NULL) {
                break;
            }
            synth_decl->decl.as.type_decl.declared_specs =
                synthesize_type_ref_list(symbol_decl->declared_specs,
                                         symbol_decl->declared_spec_count);
            if (symbol_decl->declared_spec_count > 0U &&
                synth_decl->decl.as.type_decl.declared_specs == NULL) {
                break;
            }
            synth_decl->decl.as.type_decl.declared_spec_count = symbol_decl->declared_spec_count;
            return true;

        case FENG_SYMBOL_DECL_KIND_SPEC:
            synth_decl->decl.kind = FENG_DECL_SPEC;
            synth_decl->decl.as.spec_decl.name = name;
            synth_decl->decl.as.spec_decl.parent_specs =
                synthesize_type_ref_list(symbol_decl->declared_specs,
                                         symbol_decl->declared_spec_count);
            if (symbol_decl->declared_spec_count > 0U &&
                synth_decl->decl.as.spec_decl.parent_specs == NULL) {
                break;
            }
            synth_decl->decl.as.spec_decl.parent_spec_count = symbol_decl->declared_spec_count;
            if (symbol_decl->member_count > 0U ||
                (symbol_decl->return_type == NULL && symbol_decl->param_count == 0U)) {
                synth_decl->decl.as.spec_decl.form = FENG_SPEC_FORM_OBJECT;
                synth_decl->decl.as.spec_decl.as.object.members =
                    synthesize_type_members(symbol_decl,
                                            &synth_decl->decl.as.spec_decl.as.object.member_count);
                if (symbol_decl->member_count > 0U &&
                    synth_decl->decl.as.spec_decl.as.object.members == NULL) {
                    break;
                }
            } else {
                synth_decl->decl.as.spec_decl.form = FENG_SPEC_FORM_CALLABLE;
                synth_decl->decl.as.spec_decl.as.callable.params =
                    synthesize_parameters(symbol_decl,
                                          &synth_decl->decl.as.spec_decl.as.callable.param_count);
                if (symbol_decl->param_count > 0U &&
                    synth_decl->decl.as.spec_decl.as.callable.params == NULL) {
                    break;
                }
                synth_decl->decl.as.spec_decl.as.callable.return_type =
                    synthesize_type_ref(symbol_decl->return_type);
                if (symbol_decl->return_type != NULL &&
                    synth_decl->decl.as.spec_decl.as.callable.return_type == NULL) {
                    break;
                }
            }
            return true;

        case FENG_SYMBOL_DECL_KIND_FUNCTION:
            synth_decl->decl.kind = FENG_DECL_FUNCTION;
            if (!synthesize_callable_signature(&synth_decl->decl.as.function_decl,
                                               symbol_decl,
                                               name)) {
                break;
            }
            return true;

        case FENG_SYMBOL_DECL_KIND_BINDING:
            synth_decl->decl.kind = FENG_DECL_GLOBAL_BINDING;
            synth_decl->decl.as.binding.token = symbol_decl->token;
            synth_decl->decl.as.binding.mutability = symbol_decl->mutability;
            synth_decl->decl.as.binding.name = name;
            synth_decl->decl.as.binding.type = synthesize_type_ref(symbol_decl->value_type);
            if (symbol_decl->value_type != NULL && synth_decl->decl.as.binding.type == NULL) {
                break;
            }
            synth_decl->decl.as.binding.initializer = NULL;
            return true;

        default:
            break;
    }

    free_synthetic_decl_payload(&synth_decl->decl);
    free(synth_decl->name_buf);
    memset(synth_decl, 0, sizeof(*synth_decl));
    return false;
}

static void entry_free_partial(SynthModuleEntry *entry) {
    size_t index;

    if (entry->prog != NULL) {
        for (index = 0U; index < entry->prog->decl_count; ++index) {
            free_synthetic_decl_payload(&entry->prog->decls[index].decl);
            free(entry->prog->decls[index].name_buf);
        }
        free(entry->prog->path_buf);
        free(entry->prog->decls);
        free(entry->prog->decl_ptrs);
        free(entry->prog);
    }
    if (entry->segment_strs != NULL) {
        for (index = 0U; index < entry->segment_count; ++index) {
            free(entry->segment_strs[index]);
        }
        free(entry->segment_strs);
    }
    free(entry->segments);
}

static const FengSemanticModule *cache_get_module(const void *user,
                                                  const FengSlice *segments,
                                                  size_t segment_count) {
    FengSymbolImportedModuleCache *cache = (FengSymbolImportedModuleCache *)user;
    const FengSymbolImportedModule *imp_mod;
    SynthModuleEntry entry;
    SynthModuleEntry *appended;
    size_t index;
    size_t decl_count;

    if (cache == NULL || cache->provider == NULL) {
        return NULL;
    }

    for (index = 0U; index < cache->entry_count; ++index) {
        if (segments_match(&cache->entries[index], segments, segment_count)) {
            return &cache->entries[index].sem_mod;
        }
    }

    imp_mod = feng_symbol_provider_find_module(cache->provider, segments, segment_count);
    if (imp_mod == NULL) {
        return NULL;
    }

    memset(&entry, 0, sizeof(entry));
    entry.segment_count = segment_count;
    if (segment_count > 0U) {
        entry.segment_strs = (char **)calloc(segment_count, sizeof(char *));
        entry.segments = (FengSlice *)calloc(segment_count, sizeof(FengSlice));
        if (entry.segment_strs == NULL || entry.segments == NULL) {
            goto fail;
        }
        for (index = 0U; index < segment_count; ++index) {
            size_t length = segments[index].length;

            entry.segment_strs[index] = (char *)malloc(length + 1U);
            if (entry.segment_strs[index] == NULL) {
                goto fail;
            }
            memcpy(entry.segment_strs[index], segments[index].data, length);
            entry.segment_strs[index][length] = '\0';
            entry.segments[index].data = entry.segment_strs[index];
            entry.segments[index].length = length;
        }
    }

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

    for (index = 0U; index < decl_count; ++index) {
        const FengSymbolDeclView *symbol_decl = feng_symbol_module_public_decl_at(imp_mod, index);
        SynthDecl *synth_decl;

        switch (symbol_decl->kind) {
            case FENG_SYMBOL_DECL_KIND_TYPE:
            case FENG_SYMBOL_DECL_KIND_SPEC:
            case FENG_SYMBOL_DECL_KIND_FUNCTION:
            case FENG_SYMBOL_DECL_KIND_BINDING:
                break;
            default:
                continue;
        }

        synth_decl = &entry.prog->decls[entry.prog->decl_count];
        if (!synthesize_decl_from_symbol(synth_decl, symbol_decl)) {
            goto fail;
        }

        entry.prog->decl_ptrs[entry.prog->decl_count] = &synth_decl->decl;
        ++entry.prog->decl_count;
    }

    entry.prog->path_buf = dup_cstr_or_empty(imp_mod->source_path);
    if (entry.prog->path_buf == NULL) {
        goto fail;
    }
    entry.prog->program.path = entry.prog->path_buf;
    entry.prog->program.module_segments = entry.segments;
    entry.prog->program.module_segment_count = entry.segment_count;
    entry.prog->program.module_visibility = imp_mod->module->visibility;
    entry.prog->program.declarations = entry.prog->decl_ptrs;
    entry.prog->program.declaration_count = entry.prog->decl_count;

    entry.sem_mod.segments = entry.segments;
    entry.sem_mod.segment_count = entry.segment_count;
    entry.sem_mod.visibility = imp_mod->module->visibility;
    entry.sem_mod.program_count = 1U;
    entry.sem_mod.program_capacity = 1U;
    entry.sem_mod.origin = FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE;

    appended = cache_append(cache, &entry);
    if (appended == NULL) {
        goto fail;
    }
    appended->prog_ptr[0] = &appended->prog->program;
    appended->sem_mod.programs = appended->prog_ptr;
    return &appended->sem_mod;

fail:
    entry_free_partial(&entry);
    return NULL;
}

FengSymbolImportedModuleCache *feng_symbol_imported_module_cache_create(
    const FengSymbolProvider *provider) {
    FengSymbolImportedModuleCache *cache;

    cache = (FengSymbolImportedModuleCache *)calloc(1U, sizeof(*cache));
    if (cache == NULL) {
        return NULL;
    }
    cache->provider = provider;
    return cache;
}

void feng_symbol_imported_module_cache_free(FengSymbolImportedModuleCache *cache) {
    size_t index;

    if (cache == NULL) {
        return;
    }
    for (index = 0U; index < cache->entry_count; ++index) {
        entry_free_partial(&cache->entries[index]);
    }
    free(cache->entries);
    free(cache);
}

FengSemanticImportedModuleQuery feng_symbol_imported_module_cache_as_query(
    FengSymbolImportedModuleCache *cache) {
    FengSemanticImportedModuleQuery query = {0};

    if (cache != NULL) {
        query.user = cache;
        query.get_module = cache_get_module;
    }
    return query;
}
