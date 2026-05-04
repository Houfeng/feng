#include "symbol/ft_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Buffer {
    unsigned char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct StringEntry {
    char *text;
    uint32_t id;
} StringEntry;

typedef struct DeclIdMap {
    const FengSymbolDeclView *decl;
    uint32_t id;
} DeclIdMap;

typedef struct WriterContext {
    const FengSymbolModuleGraph *module;
    FengSymbolProfile profile;
    StringEntry *strings;
    size_t string_count;
    FengSymbolFtTypeRecord *types;
    size_t type_count;
    FengSymbolFtParamRecord *params;
    size_t param_count;
    FengSymbolFtSigRecord *sigs;
    size_t sig_count;
    FengSymbolFtSymRecord *syms;
    size_t sym_count;
    FengSymbolFtRelRecord *rels;
    size_t rel_count;
    FengSymbolFtDocRecord *docs;
    size_t doc_count;
    FengSymbolFtAttrRecord *attrs;
    size_t attr_count;
    FengSymbolFtSpanRecord *spans;
    size_t span_count;
    DeclIdMap *decl_ids;
    size_t decl_id_count;
    uint32_t root_symbol_id;
} WriterContext;

static void buffer_free(Buffer *buffer) {
    if (buffer == NULL) {
        return;
    }
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0U;
    buffer->capacity = 0U;
}

static bool buffer_reserve(Buffer *buffer,
                           size_t extra,
                           const char *path,
                           FengToken token,
                           FengSymbolError *out_error) {
    unsigned char *grown;
    size_t needed;
    size_t new_capacity;

    needed = buffer->length + extra;
    if (needed <= buffer->capacity) {
        return true;
    }
    new_capacity = buffer->capacity == 0U ? 256U : buffer->capacity;
    while (new_capacity < needed) {
        new_capacity *= 2U;
    }
    grown = (unsigned char *)realloc(buffer->data, new_capacity);
    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing .ft payload buffer");
    }
    buffer->data = grown;
    buffer->capacity = new_capacity;
    return true;
}

static bool buffer_append(Buffer *buffer,
                          const void *data,
                          size_t length,
                          const char *path,
                          FengToken token,
                          FengSymbolError *out_error) {
    if (!buffer_reserve(buffer, length, path, token, out_error)) {
        return false;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return true;
}

static bool buffer_append_u32(Buffer *buffer,
                              uint32_t value,
                              const char *path,
                              FengToken token,
                              FengSymbolError *out_error) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
    return buffer_append(buffer, bytes, sizeof(bytes), path, token, out_error);
}

static bool buffer_align8(Buffer *buffer,
                          const char *path,
                          FengToken token,
                          FengSymbolError *out_error) {
    static const unsigned char zeroes[8] = {0};
    size_t padding = (8U - (buffer->length % 8U)) % 8U;

    if (padding == 0U) {
        return true;
    }
    return buffer_append(buffer, zeroes, padding, path, token, out_error);
}

static void free_string_entries(StringEntry *entries, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        free(entries[index].text);
    }
    free(entries);
}

static void writer_context_dispose(WriterContext *ctx) {
    if (ctx == NULL) {
        return;
    }
    free_string_entries(ctx->strings, ctx->string_count);
    free(ctx->types);
    free(ctx->params);
    free(ctx->sigs);
    free(ctx->syms);
    free(ctx->rels);
    free(ctx->docs);
    free(ctx->attrs);
    free(ctx->spans);
    free(ctx->decl_ids);
    memset(ctx, 0, sizeof(*ctx));
}

static bool append_record(void **items,
                          size_t *count,
                          size_t item_size,
                          const void *item,
                          const char *path,
                          FengToken token,
                          FengSymbolError *out_error) {
    void *grown = realloc(*items, (*count + 1U) * item_size);

    if (grown == NULL) {
        return feng_symbol_internal_set_error(out_error, path, token, "out of memory growing .ft record table");
    }
    *items = grown;
    memcpy((unsigned char *)(*items) + (*count * item_size), item, item_size);
    ++(*count);
    return true;
}

static uint32_t writer_intern_string(WriterContext *ctx,
                                     const char *text,
                                     const char *path,
                                     FengToken token,
                                     FengSymbolError *out_error) {
    size_t index;
    StringEntry entry;

    if (text == NULL || text[0] == '\0') {
        return 0U;
    }
    for (index = 0U; index < ctx->string_count; ++index) {
        if (strcmp(ctx->strings[index].text, text) == 0) {
            return ctx->strings[index].id;
        }
    }
    entry.text = feng_symbol_internal_dup_cstr(text);
    entry.id = (uint32_t)(ctx->string_count + 1U);
    if (entry.text == NULL ||
        !append_record((void **)&ctx->strings,
                       &ctx->string_count,
                       sizeof(entry),
                       &entry,
                       path,
                       token,
                       out_error)) {
        free(entry.text);
        return 0U;
    }
    return entry.id;
}

static char *join_type_segments(const FengSymbolTypeView *type) {
    size_t total = 0U;
    size_t index;
    char *out;
    size_t cursor = 0U;

    if (type == NULL || type->kind != FENG_SYMBOL_TYPE_KIND_NAMED) {
        return NULL;
    }
    for (index = 0U; index < type->as.named.segment_count; ++index) {
        total += strlen(type->as.named.segments[index]);
    }
    total += type->as.named.segment_count > 0U ? type->as.named.segment_count - 1U : 0U;
    out = (char *)malloc(total + 1U);
    if (out == NULL) {
        return NULL;
    }
    for (index = 0U; index < type->as.named.segment_count; ++index) {
        if (index > 0U) {
            out[cursor++] = '.';
        }
        memcpy(out + cursor,
               type->as.named.segments[index],
               strlen(type->as.named.segments[index]));
        cursor += strlen(type->as.named.segments[index]);
    }
    out[cursor] = '\0';
    return out;
}

static uint32_t writer_serialize_type(WriterContext *ctx,
                                      const FengSymbolTypeView *type,
                                      const char *path,
                                      FengToken token,
                                      FengSymbolError *out_error) {
    FengSymbolFtTypeRecord record;
    char *joined_name;
    uint64_t mutability_bits;
    size_t layer_index;

    if (type == NULL) {
        return 0U;
    }
    memset(&record, 0, sizeof(record));
    switch (type->kind) {
        case FENG_SYMBOL_TYPE_KIND_BUILTIN:
            record.kind = FENG_SYMBOL_FT_TYPE_KIND_BUILTIN;
            record.string_ref = writer_intern_string(ctx, type->as.builtin.name, path, token, out_error);
            if (type->as.builtin.name != NULL && record.string_ref == 0U) {
                return 0U;
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_NAMED:
            joined_name = join_type_segments(type);
            if (joined_name == NULL) {
                feng_symbol_internal_set_error(out_error, path, token, "out of memory serializing named type");
                return 0U;
            }
            record.kind = FENG_SYMBOL_FT_TYPE_KIND_NAMED;
            record.string_ref = writer_intern_string(ctx, joined_name, path, token, out_error);
            record.aux = (uint32_t)type->as.named.segment_count;
            free(joined_name);
            if (record.string_ref == 0U) {
                return 0U;
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_POINTER:
            record.kind = FENG_SYMBOL_FT_TYPE_KIND_C_POINTER;
            record.inner_type_id = writer_serialize_type(ctx,
                                                         type->as.pointer.inner,
                                                         path,
                                                         token,
                                                         out_error);
            if (type->as.pointer.inner != NULL && record.inner_type_id == 0U) {
                return 0U;
            }
            break;

        case FENG_SYMBOL_TYPE_KIND_ARRAY:
            if (type->as.array.rank > 64U) {
                feng_symbol_internal_set_error(out_error,
                                               path,
                                               token,
                                               "array rank %zu exceeds current .ft v1 bitmap capacity",
                                               type->as.array.rank);
                return 0U;
            }
            record.kind = FENG_SYMBOL_FT_TYPE_KIND_ARRAY;
            record.inner_type_id = writer_serialize_type(ctx,
                                                         type->as.array.element,
                                                         path,
                                                         token,
                                                         out_error);
            if (type->as.array.element != NULL && record.inner_type_id == 0U) {
                return 0U;
            }
            record.aux = (uint32_t)type->as.array.rank;
            mutability_bits = 0U;
            for (layer_index = 0U; layer_index < type->as.array.rank; ++layer_index) {
                if (type->as.array.layer_writable[layer_index]) {
                    mutability_bits |= (1ULL << layer_index);
                }
            }
            record.aux2 = (uint32_t)(mutability_bits & 0xFFFFFFFFULL);
            record.aux3 = (uint32_t)((mutability_bits >> 32U) & 0xFFFFFFFFULL);
            break;

        case FENG_SYMBOL_TYPE_KIND_INVALID:
        default:
            return 0U;
    }
    if (!append_record((void **)&ctx->types,
                       &ctx->type_count,
                       sizeof(record),
                       &record,
                       path,
                       token,
                       out_error)) {
        return 0U;
    }
    return (uint32_t)ctx->type_count;
}

static uint32_t writer_serialize_signature(WriterContext *ctx,
                                           const FengSymbolDeclView *decl,
                                           const char *path,
                                           FengToken token,
                                           FengSymbolError *out_error) {
    FengSymbolFtSigRecord record;
    size_t param_index;

    memset(&record, 0, sizeof(record));
    record.return_type_id = writer_serialize_type(ctx, decl->return_type, path, token, out_error);
    if (decl->return_type != NULL && record.return_type_id == 0U) {
        return 0U;
    }
    record.first_param_index = decl->param_count > 0U ? (uint32_t)(ctx->param_count + 1U) : 0U;
    record.param_count = (uint32_t)decl->param_count;
    record.call_conv = (uint16_t)decl->calling_convention;
    record.abi_library_str = writer_intern_string(ctx, decl->abi_library, path, token, out_error);
    if (decl->abi_library != NULL && record.abi_library_str == 0U) {
        return 0U;
    }

    for (param_index = 0U; param_index < decl->param_count; ++param_index) {
        FengSymbolFtParamRecord param_record;

        memset(&param_record, 0, sizeof(param_record));
        param_record.name_str = writer_intern_string(ctx,
                                                     decl->params[param_index].name,
                                                     path,
                                                     decl->params[param_index].token,
                                                     out_error);
        param_record.type_id = writer_serialize_type(ctx,
                                                     decl->params[param_index].type,
                                                     path,
                                                     decl->params[param_index].token,
                                                     out_error);
        param_record.flags = decl->params[param_index].mutability == FENG_MUTABILITY_VAR
                                 ? FENG_SYMBOL_FT_PARAM_FLAG_MUTABLE
                                 : 0U;
        if ((decl->params[param_index].name != NULL && param_record.name_str == 0U) ||
            (decl->params[param_index].type != NULL && param_record.type_id == 0U) ||
            !append_record((void **)&ctx->params,
                           &ctx->param_count,
                           sizeof(param_record),
                           &param_record,
                           path,
                           decl->params[param_index].token,
                           out_error)) {
            return 0U;
        }
    }

    if (!append_record((void **)&ctx->sigs,
                       &ctx->sig_count,
                       sizeof(record),
                       &record,
                       path,
                       token,
                       out_error)) {
        return 0U;
    }
    return (uint32_t)ctx->sig_count;
}

static bool writer_append_decl_id(WriterContext *ctx,
                                  const FengSymbolDeclView *decl,
                                  uint32_t id,
                                  const char *path,
                                  FengToken token,
                                  FengSymbolError *out_error) {
    DeclIdMap entry;

    entry.decl = decl;
    entry.id = id;
    return append_record((void **)&ctx->decl_ids,
                         &ctx->decl_id_count,
                         sizeof(entry),
                         &entry,
                         path,
                         token,
                         out_error);
}

static uint32_t writer_find_decl_id(const WriterContext *ctx, const FengSymbolDeclView *decl) {
    size_t index;

    for (index = 0U; index < ctx->decl_id_count; ++index) {
        if (ctx->decl_ids[index].decl == decl) {
            return ctx->decl_ids[index].id;
        }
    }
    return 0U;
}

static bool writer_should_export_decl(FengSymbolProfile profile, const FengSymbolDeclView *decl) {
    return profile == FENG_SYMBOL_PROFILE_WORKSPACE_CACHE ||
           (decl != NULL && decl->visibility == FENG_VISIBILITY_PUBLIC);
}

static uint16_t writer_symbol_kind(const FengSymbolDeclView *decl) {
    switch (decl->kind) {
        case FENG_SYMBOL_DECL_KIND_MODULE:
            return FENG_SYMBOL_FT_SYM_KIND_MODULE;
        case FENG_SYMBOL_DECL_KIND_TYPE:
            return FENG_SYMBOL_FT_SYM_KIND_TYPE;
        case FENG_SYMBOL_DECL_KIND_SPEC:
            return FENG_SYMBOL_FT_SYM_KIND_SPEC;
        case FENG_SYMBOL_DECL_KIND_FIT:
            return FENG_SYMBOL_FT_SYM_KIND_FIT;
        case FENG_SYMBOL_DECL_KIND_FUNCTION:
            return decl->is_extern ? FENG_SYMBOL_FT_SYM_KIND_EXTERN_FN : FENG_SYMBOL_FT_SYM_KIND_TOP_FN;
        case FENG_SYMBOL_DECL_KIND_BINDING:
            return decl->mutability == FENG_MUTABILITY_VAR ? FENG_SYMBOL_FT_SYM_KIND_TOP_VAR
                                                           : FENG_SYMBOL_FT_SYM_KIND_TOP_LET;
        case FENG_SYMBOL_DECL_KIND_FIELD:
            return FENG_SYMBOL_FT_SYM_KIND_FIELD;
        case FENG_SYMBOL_DECL_KIND_METHOD:
            return FENG_SYMBOL_FT_SYM_KIND_METHOD;
        case FENG_SYMBOL_DECL_KIND_CONSTRUCTOR:
            return FENG_SYMBOL_FT_SYM_KIND_CTOR;
        case FENG_SYMBOL_DECL_KIND_FINALIZER:
            return FENG_SYMBOL_FT_SYM_KIND_DTOR;
    }
    return 0U;
}

static uint16_t writer_symbol_flags(const FengSymbolDeclView *decl) {
    uint16_t flags = 0U;

    if (decl->visibility == FENG_VISIBILITY_PUBLIC) {
        flags |= FENG_SYMBOL_FT_SYM_FLAG_PUBLIC;
    }
    if (decl->mutability == FENG_MUTABILITY_VAR) {
        flags |= FENG_SYMBOL_FT_SYM_FLAG_MUTABLE;
    }
    if (decl->fixed_annotated) {
        flags |= FENG_SYMBOL_FT_SYM_FLAG_FIXED;
    }
    if (decl->is_extern) {
        flags |= FENG_SYMBOL_FT_SYM_FLAG_EXTERN;
    }
    if (decl->bounded_decl) {
        flags |= FENG_SYMBOL_FT_SYM_FLAG_BOUNDED_DECL;
    }
    if (decl->has_doc && decl->doc != NULL && decl->doc[0] != '\0') {
        flags |= FENG_SYMBOL_FT_SYM_FLAG_HAS_DOC;
    }
    return flags;
}

static uint32_t writer_emit_doc(WriterContext *ctx,
                                const FengSymbolDeclView *decl,
                                uint32_t symbol_id,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    FengSymbolFtDocRecord record;

    if (decl == NULL || !decl->has_doc || decl->doc == NULL || decl->doc[0] == '\0') {
        return 0U;
    }

    memset(&record, 0, sizeof(record));
    record.id = (uint32_t)(ctx->doc_count + 1U);
    record.symbol_id = symbol_id;
    record.doc_str_id = writer_intern_string(ctx, decl->doc, path, token, out_error);
    if (record.doc_str_id == 0U) {
        return 0U;
    }
    if (!append_record((void **)&ctx->docs,
                       &ctx->doc_count,
                       sizeof(record),
                       &record,
                       path,
                       token,
                       out_error)) {
        return 0U;
    }
    return record.id;
}

static bool writer_emit_decl_attrs(WriterContext *ctx,
                                   const FengSymbolDeclView *decl,
                                   uint32_t symbol_id,
                                   const char *path,
                                   FengToken token,
                                   FengSymbolError *out_error) {
    if (decl->declared_spec_count > 0U) {
        FengSymbolFtAttrRecord attr;
        size_t index;
        uint32_t first_type_id = (uint32_t)(ctx->type_count + 1U);

        memset(&attr, 0, sizeof(attr));
        for (index = 0U; index < decl->declared_spec_count; ++index) {
            if (writer_serialize_type(ctx, decl->declared_specs[index], path, token, out_error) == 0U) {
                return false;
            }
        }
        attr.symbol_id = symbol_id;
        attr.kind = (uint16_t)FENG_SYMBOL_ATTR_DECLARED_SPECS;
        attr.value0 = first_type_id;
        attr.value1 = (uint32_t)decl->declared_spec_count;
        if (!append_record((void **)&ctx->attrs,
                           &ctx->attr_count,
                           sizeof(attr),
                           &attr,
                           path,
                           token,
                           out_error)) {
            return false;
        }
    }
    if (decl->union_annotated) {
        FengSymbolFtAttrRecord attr;

        memset(&attr, 0, sizeof(attr));
        attr.symbol_id = symbol_id;
        attr.kind = (uint16_t)FENG_SYMBOL_ATTR_UNION;
        if (!append_record((void **)&ctx->attrs,
                           &ctx->attr_count,
                           sizeof(attr),
                           &attr,
                           path,
                           token,
                           out_error)) {
            return false;
        }
    }
    return true;
}

static bool writer_emit_span(WriterContext *ctx,
                             const FengSymbolDeclView *decl,
                             uint32_t symbol_id,
                             const char *path,
                             FengToken token,
                             FengSymbolError *out_error) {
    FengSymbolFtSpanRecord span;
    const char *decl_path;

    if (ctx->profile != FENG_SYMBOL_PROFILE_WORKSPACE_CACHE) {
        return true;
    }
    decl_path = decl->path != NULL ? decl->path : ctx->module->primary_path;
    memset(&span, 0, sizeof(span));
    span.symbol_id = symbol_id;
    span.path_str = writer_intern_string(ctx, decl_path, path, token, out_error);
    span.start_line = decl->token.line;
    span.start_column = decl->token.column;
    span.end_line = decl->token.line;
    span.end_column = decl->token.column + (decl->token.length > 0U ? (uint32_t)decl->token.length - 1U : 0U);
    if (decl_path != NULL && span.path_str == 0U) {
        return false;
    }
    return append_record((void **)&ctx->spans,
                         &ctx->span_count,
                         sizeof(span),
                         &span,
                         path,
                         token,
                         out_error);
}

static bool writer_collect_decl(WriterContext *ctx,
                                const FengSymbolDeclView *decl,
                                uint32_t owner_id,
                                const char *path,
                                FengSymbolError *out_error) {
    FengSymbolFtSymRecord record;
    uint32_t symbol_id;
    size_t index;
    char *full_module_name = NULL;

    memset(&record, 0, sizeof(record));
    symbol_id = (uint32_t)(ctx->sym_count + 1U);
    record.id = symbol_id;
    record.owner_id = owner_id;
    record.name_str = writer_intern_string(ctx, decl->name, path, decl->token, out_error);
    record.kind = writer_symbol_kind(decl);
    record.flags = writer_symbol_flags(decl);
    if (decl->name != NULL && record.name_str == 0U) {
        return false;
    }
    switch (decl->kind) {
        case FENG_SYMBOL_DECL_KIND_BINDING:
        case FENG_SYMBOL_DECL_KIND_FIELD:
            record.type_ref = writer_serialize_type(ctx, decl->value_type, path, decl->token, out_error);
            if (decl->value_type != NULL && record.type_ref == 0U) {
                return false;
            }
            break;

        case FENG_SYMBOL_DECL_KIND_FUNCTION:
        case FENG_SYMBOL_DECL_KIND_METHOD:
        case FENG_SYMBOL_DECL_KIND_CONSTRUCTOR:
        case FENG_SYMBOL_DECL_KIND_FINALIZER:
            record.sig_ref = writer_serialize_signature(ctx, decl, path, decl->token, out_error);
            if (record.sig_ref == 0U) {
                return false;
            }
            break;

        case FENG_SYMBOL_DECL_KIND_SPEC:
            if (decl->param_count > 0U || decl->return_type != NULL) {
                record.sig_ref = writer_serialize_signature(ctx, decl, path, decl->token, out_error);
                if (record.sig_ref == 0U) {
                    return false;
                }
            }
            break;

        case FENG_SYMBOL_DECL_KIND_FIT:
            record.extra_ref = writer_serialize_type(ctx, decl->fit_target, path, decl->token, out_error);
            if (decl->fit_target != NULL && record.extra_ref == 0U) {
                return false;
            }
            break;

        case FENG_SYMBOL_DECL_KIND_MODULE:
            if (ctx->module->segment_count > 0U) {
                size_t total = 0U;
                size_t seg_index;
                size_t cursor = 0U;

                for (seg_index = 0U; seg_index < ctx->module->segment_count; ++seg_index) {
                    total += strlen(ctx->module->segments[seg_index]);
                }
                total += ctx->module->segment_count - 1U;
                full_module_name = (char *)malloc(total + 1U);
                if (full_module_name == NULL) {
                    return feng_symbol_internal_set_error(out_error, path, decl->token, "out of memory serializing module name");
                }
                for (seg_index = 0U; seg_index < ctx->module->segment_count; ++seg_index) {
                    size_t seg_len = strlen(ctx->module->segments[seg_index]);
                    if (seg_index > 0U) {
                        full_module_name[cursor++] = '.';
                    }
                    memcpy(full_module_name + cursor, ctx->module->segments[seg_index], seg_len);
                    cursor += seg_len;
                }
                full_module_name[cursor] = '\0';
                record.extra_ref = writer_intern_string(ctx, full_module_name, path, decl->token, out_error);
                free(full_module_name);
                if (record.extra_ref == 0U) {
                    return false;
                }
            }
            break;

        case FENG_SYMBOL_DECL_KIND_TYPE:
        default:
            break;
    }
    record.doc_ref = writer_emit_doc(ctx, decl, symbol_id, path, decl->token, out_error);
    if (decl->has_doc && decl->doc != NULL && decl->doc[0] != '\0' && record.doc_ref == 0U) {
        return false;
    }
    if (!append_record((void **)&ctx->syms,
                       &ctx->sym_count,
                       sizeof(record),
                       &record,
                       path,
                       decl->token,
                       out_error) ||
        !writer_append_decl_id(ctx, decl, symbol_id, path, decl->token, out_error) ||
        !writer_emit_decl_attrs(ctx, decl, symbol_id, path, decl->token, out_error) ||
        !writer_emit_span(ctx, decl, symbol_id, path, decl->token, out_error)) {
        return false;
    }
    if (decl->kind == FENG_SYMBOL_DECL_KIND_MODULE) {
        ctx->root_symbol_id = symbol_id;
    }

    for (index = 0U; index < decl->member_count; ++index) {
        if (!writer_should_export_decl(ctx->profile, decl->members[index])) {
            continue;
        }
        if (!writer_collect_decl(ctx, decl->members[index], symbol_id, path, out_error)) {
            return false;
        }
    }
    return true;
}

static bool writer_collect_relations(WriterContext *ctx,
                                     const char *path,
                                     FengSymbolError *out_error) {
    size_t index;

    for (index = 0U; index < ctx->module->relation_count; ++index) {
        const FengSymbolRelation *relation = &ctx->module->relations[index];
        FengSymbolFtRelRecord record;

        record.kind = (uint16_t)relation->kind;
        record.reserved0 = 0U;
        record.left_symbol_id = writer_find_decl_id(ctx, relation->left);
        record.right_symbol_id = writer_find_decl_id(ctx, relation->right);
        record.owner_symbol_id = writer_find_decl_id(ctx, relation->owner);
        if (record.left_symbol_id == 0U || record.right_symbol_id == 0U) {
            continue;
        }
        if (!append_record((void **)&ctx->rels,
                           &ctx->rel_count,
                           sizeof(record),
                           &record,
                           path,
                           ctx->module->root_decl.token,
                           out_error)) {
            return false;
        }
    }
    return true;
}

static int compare_cstr_ptrs(const void *lhs, const void *rhs) {
    const char *const *a = (const char *const *)lhs;
    const char *const *b = (const char *const *)rhs;

    return strcmp(*a, *b);
}

static uint64_t writer_dependency_fingerprint(const FengSymbolModuleGraph *module) {
    size_t index;
    uint64_t hash = 14695981039346656037ULL;
    char **sorted;

    if (module->use_count == 0U) {
        return hash;
    }
    sorted = (char **)calloc(module->use_count, sizeof(*sorted));
    if (sorted == NULL) {
        return hash;
    }
    for (index = 0U; index < module->use_count; ++index) {
        sorted[index] = module->uses[index];
    }
    qsort(sorted, module->use_count, sizeof(*sorted), compare_cstr_ptrs);
    for (index = 0U; index < module->use_count; ++index) {
        hash = feng_symbol_internal_fnv1a64_extend(hash, sorted[index], strlen(sorted[index]));
        hash = feng_symbol_internal_fnv1a64_extend(hash, "\n", 1U);
    }
    free(sorted);
    return hash;
}

static bool build_strings_section(const WriterContext *ctx,
                                  Buffer *buffer,
                                  const char *path,
                                  FengToken token,
                                  FengSymbolError *out_error) {
    uint32_t offset = 0U;
    size_t index;

    for (index = 0U; index < ctx->string_count + 1U; ++index) {
        if (!buffer_append_u32(buffer, offset, path, token, out_error)) {
            return false;
        }
        if (index < ctx->string_count) {
            offset += (uint32_t)strlen(ctx->strings[index].text);
        }
    }
    for (index = 0U; index < ctx->string_count; ++index) {
        if (!buffer_append(buffer,
                           ctx->strings[index].text,
                           strlen(ctx->strings[index].text),
                           path,
                           token,
                           out_error)) {
            return false;
        }
    }
    return true;
}

static bool build_fixed_section(Buffer *buffer,
                                const void *items,
                                size_t count,
                                size_t item_size,
                                const char *path,
                                FengToken token,
                                FengSymbolError *out_error) {
    return count == 0U || buffer_append(buffer, items, count * item_size, path, token, out_error);
}

static void write_u16_le(unsigned char *out, uint16_t value) {
    out[0] = (unsigned char)(value & 0xFFU);
    out[1] = (unsigned char)((value >> 8U) & 0xFFU);
}

static void write_u32_le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xFFU);
    out[1] = (unsigned char)((value >> 8U) & 0xFFU);
    out[2] = (unsigned char)((value >> 16U) & 0xFFU);
    out[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void write_u64_le(unsigned char *out, uint64_t value) {
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        out[index] = (unsigned char)((value >> (index * 8U)) & 0xFFU);
    }
}

static bool write_header_and_sections(FILE *file,
                                      FengSymbolFtHeader header,
                                      const FengSymbolFtSectionEntry *sections,
                                      size_t section_count,
                                      const Buffer *payload,
                                      const char *path,
                                      FengSymbolError *out_error) {
    unsigned char header_bytes[FENG_SYMBOL_FT_HEADER_SIZE];
    size_t index;

    memset(header_bytes, 0, sizeof(header_bytes));
    header_bytes[0] = FENG_SYMBOL_FT_MAGIC_0;
    header_bytes[1] = FENG_SYMBOL_FT_MAGIC_1;
    header_bytes[2] = FENG_SYMBOL_FT_MAGIC_2;
    header_bytes[3] = FENG_SYMBOL_FT_MAGIC_3;
    header_bytes[4] = FENG_SYMBOL_FT_BYTE_ORDER_LE;
    header_bytes[5] = FENG_SYMBOL_FT_VERSION_MAJOR;
    header_bytes[6] = FENG_SYMBOL_FT_VERSION_MINOR;
    header_bytes[7] = (unsigned char)header.profile;
    write_u16_le(header_bytes + 0x08, header.header_size);
    write_u16_le(header_bytes + 0x0A, header.section_entry_size);
    write_u16_le(header_bytes + 0x0C, header.section_count);
    write_u16_le(header_bytes + 0x0E, header.reserved0);
    write_u32_le(header_bytes + 0x10, header.flags);
    write_u32_le(header_bytes + 0x14, header.root_symbol_id);
    write_u64_le(header_bytes + 0x18, header.section_dir_offset);
    write_u64_le(header_bytes + 0x20, header.payload_offset);
    write_u64_le(header_bytes + 0x28, header.content_fingerprint);
    write_u64_le(header_bytes + 0x30, header.dependency_fingerprint);
    write_u64_le(header_bytes + 0x38, header.reserved1);

    if (fwrite(header_bytes, 1U, sizeof(header_bytes), file) != sizeof(header_bytes)) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to write .ft header");
    }

    for (index = 0U; index < section_count; ++index) {
        unsigned char entry_bytes[FENG_SYMBOL_FT_SECTION_ENTRY_SIZE];

        memset(entry_bytes, 0, sizeof(entry_bytes));
        write_u16_le(entry_bytes + 0x00, sections[index].kind);
        write_u16_le(entry_bytes + 0x02, sections[index].flags);
        write_u32_le(entry_bytes + 0x04, sections[index].count);
        write_u64_le(entry_bytes + 0x08, sections[index].offset);
        write_u64_le(entry_bytes + 0x10, sections[index].size);
        write_u32_le(entry_bytes + 0x18, sections[index].entry_size);
        write_u32_le(entry_bytes + 0x1C, sections[index].reserved);
        if (fwrite(entry_bytes, 1U, sizeof(entry_bytes), file) != sizeof(entry_bytes)) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to write .ft section directory");
        }
    }

    if (payload->length > 0U && fwrite(payload->data, 1U, payload->length, file) != payload->length) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to write .ft payload");
    }
    return true;
}

bool feng_symbol_ft_write_module_internal(const FengSymbolModuleGraph *module,
                                          FengSymbolProfile profile,
                                          const char *path,
                                          FengSymbolError *out_error) {
    WriterContext ctx;
    Buffer strings = {0};
    Buffer syms = {0};
    Buffer typs = {0};
    Buffer sigs = {0};
    Buffer prms = {0};
    Buffer rels = {0};
    Buffer docs = {0};
    Buffer attrs = {0};
    Buffer spans = {0};
    Buffer payload = {0};
    FengSymbolFtSectionEntry sections[9];
    size_t section_count = 0U;
    FengSymbolFtHeader header;
    FILE *file = NULL;

    if (module == NULL || path == NULL) {
        return false;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.module = module;
    ctx.profile = profile;

    if (!writer_collect_decl(&ctx, &module->root_decl, 0U, path, out_error) ||
        !writer_collect_relations(&ctx, path, out_error) ||
        !build_strings_section(&ctx, &strings, path, module->root_decl.token, out_error) ||
        !build_fixed_section(&syms, ctx.syms, ctx.sym_count, sizeof(*ctx.syms), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&typs, ctx.types, ctx.type_count, sizeof(*ctx.types), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&sigs, ctx.sigs, ctx.sig_count, sizeof(*ctx.sigs), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&prms, ctx.params, ctx.param_count, sizeof(*ctx.params), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&rels, ctx.rels, ctx.rel_count, sizeof(*ctx.rels), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&docs, ctx.docs, ctx.doc_count, sizeof(*ctx.docs), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&attrs, ctx.attrs, ctx.attr_count, sizeof(*ctx.attrs), path, module->root_decl.token, out_error) ||
        !build_fixed_section(&spans, ctx.spans, ctx.span_count, sizeof(*ctx.spans), path, module->root_decl.token, out_error)) {
        goto cleanup;
    }

    memset(&header, 0, sizeof(header));
    header.profile = (uint8_t)profile;
    header.header_size = FENG_SYMBOL_FT_HEADER_SIZE;
    header.section_entry_size = FENG_SYMBOL_FT_SECTION_ENTRY_SIZE;
    header.root_symbol_id = ctx.root_symbol_id;
    header.section_dir_offset = FENG_SYMBOL_FT_HEADER_SIZE;
    header.payload_offset = FENG_SYMBOL_FT_HEADER_SIZE;

    if (!buffer_align8(&payload, path, module->root_decl.token, out_error)) {
        goto cleanup;
    }

#define APPEND_SECTION(kind_value, flags_value, count_value, entry_size_value, buffer_ptr) \
    do { \
        if ((buffer_ptr)->length > 0U || (kind_value) <= FENG_SYMBOL_FT_SEC_RELS) { \
            if (!buffer_align8(&payload, path, module->root_decl.token, out_error)) { \
                goto cleanup; \
            } \
            sections[section_count].offset = header.payload_offset + (uint64_t)payload.length; \
            if (!buffer_append(&payload, (buffer_ptr)->data, (buffer_ptr)->length, path, module->root_decl.token, out_error)) { \
                goto cleanup; \
            } \
            sections[section_count].kind = (kind_value); \
            sections[section_count].flags = (flags_value); \
            sections[section_count].count = (count_value); \
            sections[section_count].size = (buffer_ptr)->length; \
            sections[section_count].entry_size = (entry_size_value); \
            sections[section_count].reserved = 0U; \
            ++section_count; \
        } \
    } while (0)

    header.payload_offset = FENG_SYMBOL_FT_HEADER_SIZE +
                            (uint64_t)(FENG_SYMBOL_FT_SECTION_ENTRY_SIZE *
                                       (6U + (ctx.doc_count > 0U ? 1U : 0U) +
                                        (ctx.attr_count > 0U ? 1U : 0U) +
                                        (profile == FENG_SYMBOL_PROFILE_WORKSPACE_CACHE && ctx.span_count > 0U ? 1U : 0U)));
    APPEND_SECTION(FENG_SYMBOL_FT_SEC_STRS,
                   FENG_SYMBOL_FT_SEC_FLAG_REQUIRED | FENG_SYMBOL_FT_SEC_FLAG_SORTED,
                   (uint32_t)ctx.string_count,
                   0U,
                   &strings);
    APPEND_SECTION(FENG_SYMBOL_FT_SEC_SYMS,
                   FENG_SYMBOL_FT_SEC_FLAG_REQUIRED | FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY | FENG_SYMBOL_FT_SEC_FLAG_SORTED,
                   (uint32_t)ctx.sym_count,
                   (uint32_t)sizeof(FengSymbolFtSymRecord),
                   &syms);
    APPEND_SECTION(FENG_SYMBOL_FT_SEC_TYPS,
                   FENG_SYMBOL_FT_SEC_FLAG_REQUIRED | FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY,
                   (uint32_t)ctx.type_count,
                   (uint32_t)sizeof(FengSymbolFtTypeRecord),
                   &typs);
    APPEND_SECTION(FENG_SYMBOL_FT_SEC_SIGS,
                   FENG_SYMBOL_FT_SEC_FLAG_REQUIRED | FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY,
                   (uint32_t)ctx.sig_count,
                   (uint32_t)sizeof(FengSymbolFtSigRecord),
                   &sigs);
    APPEND_SECTION(FENG_SYMBOL_FT_SEC_PRMS,
                   FENG_SYMBOL_FT_SEC_FLAG_REQUIRED | FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY,
                   (uint32_t)ctx.param_count,
                   (uint32_t)sizeof(FengSymbolFtParamRecord),
                   &prms);
    APPEND_SECTION(FENG_SYMBOL_FT_SEC_RELS,
                   FENG_SYMBOL_FT_SEC_FLAG_REQUIRED | FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY,
                   (uint32_t)ctx.rel_count,
                   (uint32_t)sizeof(FengSymbolFtRelRecord),
                   &rels);
    if (ctx.doc_count > 0U) {
        header.flags |= FENG_SYMBOL_FT_FLAG_HAS_DOCS;
        APPEND_SECTION(FENG_SYMBOL_FT_SEC_DOCS,
                       FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY | FENG_SYMBOL_FT_SEC_FLAG_IGNORABLE,
                       (uint32_t)ctx.doc_count,
                       (uint32_t)sizeof(FengSymbolFtDocRecord),
                       &docs);
    }
    if (ctx.attr_count > 0U) {
        header.flags |= FENG_SYMBOL_FT_FLAG_HAS_ATTRS;
        APPEND_SECTION(FENG_SYMBOL_FT_SEC_ATTRS,
                       FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY | FENG_SYMBOL_FT_SEC_FLAG_IGNORABLE,
                       (uint32_t)ctx.attr_count,
                       (uint32_t)sizeof(FengSymbolFtAttrRecord),
                       &attrs);
    }
    if (profile == FENG_SYMBOL_PROFILE_WORKSPACE_CACHE && ctx.span_count > 0U) {
        header.flags |= FENG_SYMBOL_FT_FLAG_HAS_SPANS;
        APPEND_SECTION(FENG_SYMBOL_FT_SEC_SPNS,
                       FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY | FENG_SYMBOL_FT_SEC_FLAG_WORKSPACE_ONLY |
                           FENG_SYMBOL_FT_SEC_FLAG_IGNORABLE,
                       (uint32_t)ctx.span_count,
                       (uint32_t)sizeof(FengSymbolFtSpanRecord),
                       &spans);
    }

#undef APPEND_SECTION

    header.section_count = (uint16_t)section_count;
    header.content_fingerprint = feng_symbol_internal_fnv1a64(payload.data, payload.length);
    header.dependency_fingerprint = profile == FENG_SYMBOL_PROFILE_WORKSPACE_CACHE
                                        ? writer_dependency_fingerprint(module)
                                        : 0U;

    file = fopen(path, "wb");
    if (file == NULL) {
        feng_symbol_internal_set_error(out_error,
                                       path,
                                       module->root_decl.token,
                                       "failed to open '%s' for write: %s",
                                       path,
                                       strerror(errno));
        goto cleanup;
    }

    if (!write_header_and_sections(file,
                                   header,
                                   sections,
                                   section_count,
                                   &payload,
                                   path,
                                   out_error)) {
        goto cleanup;
    }

    fclose(file);
    file = NULL;
    writer_context_dispose(&ctx);
    buffer_free(&strings);
    buffer_free(&syms);
    buffer_free(&typs);
    buffer_free(&sigs);
    buffer_free(&prms);
    buffer_free(&rels);
    buffer_free(&docs);
    buffer_free(&attrs);
    buffer_free(&spans);
    buffer_free(&payload);
    return true;

cleanup:
    if (file != NULL) {
        fclose(file);
    }
    writer_context_dispose(&ctx);
    buffer_free(&strings);
    buffer_free(&syms);
    buffer_free(&typs);
    buffer_free(&sigs);
    buffer_free(&prms);
    buffer_free(&rels);
    buffer_free(&attrs);
    buffer_free(&spans);
    buffer_free(&payload);
    return false;
}


