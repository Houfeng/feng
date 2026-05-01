#include "symbol/ft_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ReadContext {
    const unsigned char *data;
    size_t length;
    FengSymbolFtHeader header;
    const FengSymbolFtSectionEntry *strs_section;
    const FengSymbolFtSectionEntry *syms_section;
    const FengSymbolFtSectionEntry *typs_section;
    const FengSymbolFtSectionEntry *sigs_section;
    const FengSymbolFtSectionEntry *prms_section;
    const FengSymbolFtSectionEntry *rels_section;
    const FengSymbolFtSectionEntry *attrs_section;
    const FengSymbolFtSectionEntry *spns_section;
    char **strings;
    size_t string_count;
    FengSymbolTypeView **types;
    size_t type_count;
    FengSymbolDeclView **decls;
    size_t decl_count;
    uint32_t *decl_symbol_ids;
    FengSymbolModuleGraph *module;
    uint32_t module_full_name_str;
} ReadContext;

static uint16_t read_u16_le(const unsigned char *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t read_u32_le(const unsigned char *data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) | ((uint32_t)data[2] << 16U) |
           ((uint32_t)data[3] << 24U);
}

static uint64_t read_u64_le(const unsigned char *data) {
    size_t index;
    uint64_t value = 0U;

    for (index = 0U; index < 8U; ++index) {
        value |= ((uint64_t)data[index]) << (index * 8U);
    }
    return value;
}

static void read_context_dispose(ReadContext *ctx) {
    size_t index;

    if (ctx == NULL) {
        return;
    }
    if (ctx->module != NULL) {
        feng_symbol_internal_module_free(ctx->module);
        ctx->module = NULL;
    }
    for (index = 0U; index < ctx->string_count; ++index) {
        free(ctx->strings[index]);
    }
    free(ctx->strings);
    if (ctx->types != NULL) {
        for (index = 0U; index < ctx->type_count; ++index) {
            feng_symbol_internal_type_free(ctx->types[index]);
        }
    }
    free(ctx->types);
    free(ctx->decls);
    free(ctx->decl_symbol_ids);
    memset(ctx, 0, sizeof(*ctx));
}

static bool validate_range(const ReadContext *ctx,
                           uint64_t offset,
                           uint64_t size,
                           const char *path,
                           FengSymbolError *out_error) {
    if (offset > ctx->length || size > ctx->length - offset) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "malformed .ft section range exceeds file bounds");
    }
    return true;
}

static const FengSymbolFtSectionEntry *find_section(const ReadContext *ctx, uint16_t kind) {
    size_t index;
    const unsigned char *base = ctx->data + ctx->header.section_dir_offset;

    for (index = 0U; index < ctx->header.section_count; ++index) {
        const unsigned char *entry = base + index * ctx->header.section_entry_size;
        if (read_u16_le(entry) == kind) {
            return (const FengSymbolFtSectionEntry *)entry;
        }
    }
    return NULL;
}

static bool parse_header(ReadContext *ctx,
                         const char *path,
                         const FengSymbolFtReadOptions *options,
                         FengSymbolError *out_error) {
    const unsigned char *header = ctx->data;
    uint64_t fingerprint;

    if (ctx->length < FENG_SYMBOL_FT_HEADER_SIZE) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "symbol table file is too small");
    }
    if (header[0] != FENG_SYMBOL_FT_MAGIC_0 || header[1] != FENG_SYMBOL_FT_MAGIC_1 ||
        header[2] != FENG_SYMBOL_FT_MAGIC_2 || header[3] != FENG_SYMBOL_FT_MAGIC_3) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "symbol table magic mismatch");
    }
    ctx->header.byte_order = header[4];
    ctx->header.major = header[5];
    ctx->header.minor = header[6];
    ctx->header.profile = header[7];
    ctx->header.header_size = read_u16_le(header + 0x08);
    ctx->header.section_entry_size = read_u16_le(header + 0x0A);
    ctx->header.section_count = read_u16_le(header + 0x0C);
    ctx->header.reserved0 = read_u16_le(header + 0x0E);
    ctx->header.flags = read_u32_le(header + 0x10);
    ctx->header.root_symbol_id = read_u32_le(header + 0x14);
    ctx->header.section_dir_offset = read_u64_le(header + 0x18);
    ctx->header.payload_offset = read_u64_le(header + 0x20);
    ctx->header.content_fingerprint = read_u64_le(header + 0x28);
    ctx->header.dependency_fingerprint = read_u64_le(header + 0x30);
    ctx->header.reserved1 = read_u64_le(header + 0x38);

    if (ctx->header.byte_order != FENG_SYMBOL_FT_BYTE_ORDER_LE ||
        ctx->header.major != FENG_SYMBOL_FT_VERSION_MAJOR ||
        ctx->header.header_size != FENG_SYMBOL_FT_HEADER_SIZE ||
        ctx->header.section_entry_size != FENG_SYMBOL_FT_SECTION_ENTRY_SIZE) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "unsupported .ft header shape");
    }
    if (options != NULL && options->expected_profile != 0 &&
        ctx->header.profile != (uint8_t)options->expected_profile) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "symbol table profile mismatch");
    }
    if (!validate_range(ctx,
                        ctx->header.section_dir_offset,
                        (uint64_t)ctx->header.section_count * ctx->header.section_entry_size,
                        path,
                        out_error) ||
        !validate_range(ctx,
                        ctx->header.payload_offset,
                        ctx->length - (size_t)ctx->header.payload_offset,
                        path,
                        out_error)) {
        return false;
    }
    fingerprint = feng_symbol_internal_fnv1a64(ctx->data + ctx->header.payload_offset,
                                               ctx->length - (size_t)ctx->header.payload_offset);
    if (fingerprint != ctx->header.content_fingerprint) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "symbol table payload fingerprint mismatch");
    }
    return true;
}

static bool load_required_sections(ReadContext *ctx,
                                   const char *path,
                                   FengSymbolError *out_error) {
    ctx->strs_section = find_section(ctx, FENG_SYMBOL_FT_SEC_STRS);
    ctx->syms_section = find_section(ctx, FENG_SYMBOL_FT_SEC_SYMS);
    ctx->typs_section = find_section(ctx, FENG_SYMBOL_FT_SEC_TYPS);
    ctx->sigs_section = find_section(ctx, FENG_SYMBOL_FT_SEC_SIGS);
    ctx->prms_section = find_section(ctx, FENG_SYMBOL_FT_SEC_PRMS);
    ctx->rels_section = find_section(ctx, FENG_SYMBOL_FT_SEC_RELS);
    ctx->attrs_section = find_section(ctx, FENG_SYMBOL_FT_SEC_ATTRS);
    ctx->spns_section = find_section(ctx, FENG_SYMBOL_FT_SEC_SPNS);

    if (ctx->strs_section == NULL || ctx->syms_section == NULL || ctx->typs_section == NULL ||
        ctx->sigs_section == NULL || ctx->prms_section == NULL || ctx->rels_section == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "symbol table missing required core sections");
    }
    return true;
}

static bool parse_strings(ReadContext *ctx,
                          const char *path,
                          FengSymbolError *out_error) {
    const unsigned char *base;
    uint32_t count;
    size_t index;

    count = read_u32_le((const unsigned char *)ctx->strs_section + 0x04);
    base = ctx->data + read_u64_le((const unsigned char *)ctx->strs_section + 0x08);
    if (!validate_range(ctx,
                        read_u64_le((const unsigned char *)ctx->strs_section + 0x08),
                        read_u64_le((const unsigned char *)ctx->strs_section + 0x10),
                        path,
                        out_error)) {
        return false;
    }
    ctx->strings = (char **)calloc(count + 1U, sizeof(*ctx->strings));
    if (ctx->strings == NULL && count > 0U) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory loading string table");
    }
    ctx->string_count = count;
    for (index = 0U; index < count; ++index) {
        uint32_t start = read_u32_le(base + index * 4U);
        uint32_t end = read_u32_le(base + (index + 1U) * 4U);
        uint64_t payload_bytes = read_u64_le((const unsigned char *)ctx->strs_section + 0x10);
        uint64_t header_bytes = (uint64_t)(count + 1U) * 4U;

        if (end < start || header_bytes + end > payload_bytes) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "malformed string table offsets");
        }
        ctx->strings[index + 1U] = (char *)malloc((size_t)(end - start) + 1U);
        if (ctx->strings[index + 1U] == NULL) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory loading string entry");
        }
        memcpy(ctx->strings[index + 1U], base + header_bytes + start, (size_t)(end - start));
        ctx->strings[index + 1U][end - start] = '\0';
    }
    return true;
}

static const char *string_at(const ReadContext *ctx, uint32_t string_id) {
    if (string_id == 0U || string_id > ctx->string_count) {
        return NULL;
    }
    return ctx->strings[string_id];
}

static FengSymbolTypeView *parse_type_by_id(ReadContext *ctx,
                                            uint32_t type_id,
                                            const char *path,
                                            FengSymbolError *out_error);

static FengSymbolTypeView *parse_named_type_from_string(const char *text,
                                                        const char *path,
                                                        FengSymbolError *out_error) {
    FengSymbolTypeView *type;
    char *copy;
    char *cursor;
    size_t count = 1U;
    size_t index = 0U;

    if (text == NULL) {
        return NULL;
    }
    copy = feng_symbol_internal_dup_cstr(text);
    if (copy == NULL) {
        feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory parsing named type");
        return NULL;
    }
    for (cursor = copy; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            ++count;
        }
    }
    type = (FengSymbolTypeView *)calloc(1U, sizeof(*type));
    if (type == NULL) {
        free(copy);
        feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating named type");
        return NULL;
    }
    type->kind = FENG_SYMBOL_TYPE_KIND_NAMED;
    type->as.named.segment_count = count;
    type->as.named.segments = (char **)calloc(count, sizeof(*type->as.named.segments));
    if (type->as.named.segments == NULL) {
        free(copy);
        feng_symbol_internal_type_free(type);
        feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating named type segments");
        return NULL;
    }
    cursor = strtok(copy, ".");
    while (cursor != NULL) {
        type->as.named.segments[index] = feng_symbol_internal_dup_cstr(cursor);
        if (type->as.named.segments[index] == NULL) {
            free(copy);
            feng_symbol_internal_type_free(type);
            feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory cloning named type segment");
            return NULL;
        }
        ++index;
        cursor = strtok(NULL, ".");
    }
    free(copy);
    return type;
}

static FengSymbolTypeView *parse_type_by_id(ReadContext *ctx,
                                            uint32_t type_id,
                                            const char *path,
                                            FengSymbolError *out_error) {
    const unsigned char *base;
    const unsigned char *record;
    uint32_t count;
    FengSymbolTypeView *type;
    uint16_t kind;
    uint32_t string_ref;
    uint32_t inner_type_id;
    uint32_t aux;
    uint32_t aux2;
    uint32_t aux3;
    size_t layer;
    uint64_t bits;

    if (type_id == 0U) {
        return NULL;
    }
    if (type_id <= ctx->type_count && ctx->types[type_id - 1U] != NULL) {
        return feng_symbol_internal_type_clone(ctx->types[type_id - 1U], out_error);
    }
    count = read_u32_le((const unsigned char *)ctx->typs_section + 0x04);
    if (type_id > count) {
        feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "type id %u is out of range", type_id);
        return NULL;
    }
    base = ctx->data + read_u64_le((const unsigned char *)ctx->typs_section + 0x08);
    record = base + (size_t)(type_id - 1U) * sizeof(FengSymbolFtTypeRecord);
    kind = read_u16_le(record + 0x00);
    string_ref = read_u32_le(record + 0x04);
    inner_type_id = read_u32_le(record + 0x08);
    aux = read_u32_le(record + 0x0C);
    aux2 = read_u32_le(record + 0x10);
    aux3 = read_u32_le(record + 0x14);

    type = (FengSymbolTypeView *)calloc(1U, sizeof(*type));
    if (type == NULL) {
        return NULL;
    }
    switch (kind) {
        case FENG_SYMBOL_FT_TYPE_KIND_BUILTIN:
            type->kind = FENG_SYMBOL_TYPE_KIND_BUILTIN;
            type->as.builtin.name = feng_symbol_internal_dup_cstr(string_at(ctx, string_ref));
            break;
        case FENG_SYMBOL_FT_TYPE_KIND_NAMED:
            free(type);
            type = parse_named_type_from_string(string_at(ctx, string_ref), path, out_error);
            break;
        case FENG_SYMBOL_FT_TYPE_KIND_C_POINTER:
            type->kind = FENG_SYMBOL_TYPE_KIND_POINTER;
            type->as.pointer.inner = parse_type_by_id(ctx, inner_type_id, path, out_error);
            break;
        case FENG_SYMBOL_FT_TYPE_KIND_ARRAY:
            type->kind = FENG_SYMBOL_TYPE_KIND_ARRAY;
            type->as.array.rank = aux;
            type->as.array.element = parse_type_by_id(ctx, inner_type_id, path, out_error);
            type->as.array.layer_writable = (bool *)calloc(aux, sizeof(*type->as.array.layer_writable));
            if (aux > 0U && type->as.array.layer_writable == NULL) {
                feng_symbol_internal_type_free(type);
                feng_symbol_internal_set_error(out_error,
                                               path,
                                               (FengToken){0},
                                               "out of memory loading array mutability bitmap");
                return NULL;
            }
            bits = ((uint64_t)aux3 << 32U) | aux2;
            for (layer = 0U; layer < aux; ++layer) {
                type->as.array.layer_writable[layer] = (bits & (1ULL << layer)) != 0U;
            }
            break;
        default:
            feng_symbol_internal_type_free(type);
            feng_symbol_internal_set_error(out_error,
                                           path,
                                           (FengToken){0},
                                           "unknown type record kind %u",
                                           kind);
            return NULL;
    }
    if (type_id > ctx->type_count) {
        FengSymbolTypeView **grown = (FengSymbolTypeView **)realloc(ctx->types,
                                                                    type_id * sizeof(*grown));
        if (grown == NULL) {
            feng_symbol_internal_type_free(type);
            feng_symbol_internal_set_error(out_error,
                                           path,
                                           (FengToken){0},
                                           "out of memory caching type nodes");
            return NULL;
        }
        while (ctx->type_count < type_id) {
            grown[ctx->type_count++] = NULL;
        }
        ctx->types = grown;
    }
    ctx->types[type_id - 1U] = feng_symbol_internal_type_clone(type, out_error);
    return type;
}

static FengSymbolDeclKind decode_decl_kind(uint16_t kind) {
    switch (kind) {
        case FENG_SYMBOL_FT_SYM_KIND_MODULE:
            return FENG_SYMBOL_DECL_KIND_MODULE;
        case FENG_SYMBOL_FT_SYM_KIND_TYPE:
            return FENG_SYMBOL_DECL_KIND_TYPE;
        case FENG_SYMBOL_FT_SYM_KIND_SPEC:
            return FENG_SYMBOL_DECL_KIND_SPEC;
        case FENG_SYMBOL_FT_SYM_KIND_FIT:
            return FENG_SYMBOL_DECL_KIND_FIT;
        case FENG_SYMBOL_FT_SYM_KIND_TOP_FN:
        case FENG_SYMBOL_FT_SYM_KIND_EXTERN_FN:
            return FENG_SYMBOL_DECL_KIND_FUNCTION;
        case FENG_SYMBOL_FT_SYM_KIND_CTOR:
            return FENG_SYMBOL_DECL_KIND_CONSTRUCTOR;
        case FENG_SYMBOL_FT_SYM_KIND_DTOR:
            return FENG_SYMBOL_DECL_KIND_FINALIZER;
        case FENG_SYMBOL_FT_SYM_KIND_FIELD:
            return FENG_SYMBOL_DECL_KIND_FIELD;
        case FENG_SYMBOL_FT_SYM_KIND_METHOD:
            return FENG_SYMBOL_DECL_KIND_METHOD;
        case FENG_SYMBOL_FT_SYM_KIND_TOP_LET:
        case FENG_SYMBOL_FT_SYM_KIND_TOP_VAR:
            return FENG_SYMBOL_DECL_KIND_BINDING;
    }
    return FENG_SYMBOL_DECL_KIND_BINDING;
}

static bool parse_symbols(ReadContext *ctx,
                          const char *path,
                          FengSymbolError *out_error) {
    const unsigned char *base = ctx->data + read_u64_le((const unsigned char *)ctx->syms_section + 0x08);
    uint32_t count = read_u32_le((const unsigned char *)ctx->syms_section + 0x04);
    uint32_t symbol_index;

    ctx->decls = (FengSymbolDeclView **)calloc(count, sizeof(*ctx->decls));
    ctx->decl_symbol_ids = (uint32_t *)calloc(count, sizeof(*ctx->decl_symbol_ids));
    if ((count > 0U && ctx->decls == NULL) || (count > 0U && ctx->decl_symbol_ids == NULL)) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating symbol table views");
    }
    ctx->decl_count = count;

    for (symbol_index = 0U; symbol_index < count; ++symbol_index) {
        const unsigned char *record = base + symbol_index * sizeof(FengSymbolFtSymRecord);
        FengSymbolDeclView *decl = (FengSymbolDeclView *)calloc(1U, sizeof(*decl));
        uint32_t id = read_u32_le(record + 0x00);
        uint32_t name_str = read_u32_le(record + 0x08);
        uint16_t kind = read_u16_le(record + 0x0C);
        uint16_t flags = read_u16_le(record + 0x0E);
        uint32_t type_ref = read_u32_le(record + 0x10);
        uint32_t sig_ref = read_u32_le(record + 0x14);
        uint32_t extra_ref = read_u32_le(record + 0x18);

        if (decl == NULL) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating declaration view");
        }
        decl->kind = decode_decl_kind(kind);
        decl->visibility = (flags & FENG_SYMBOL_FT_SYM_FLAG_PUBLIC) != 0U ? FENG_VISIBILITY_PUBLIC
                                                                          : FENG_VISIBILITY_PRIVATE;
        decl->mutability = (flags & FENG_SYMBOL_FT_SYM_FLAG_MUTABLE) != 0U ? FENG_MUTABILITY_VAR
                                                                            : FENG_MUTABILITY_LET;
        decl->fixed_annotated = (flags & FENG_SYMBOL_FT_SYM_FLAG_FIXED) != 0U;
        decl->is_extern = (flags & FENG_SYMBOL_FT_SYM_FLAG_EXTERN) != 0U;
        decl->bounded_decl = (flags & FENG_SYMBOL_FT_SYM_FLAG_BOUNDED_DECL) != 0U;
        decl->has_doc = (flags & FENG_SYMBOL_FT_SYM_FLAG_HAS_DOC) != 0U;
        decl->name = feng_symbol_internal_dup_cstr(string_at(ctx, name_str));
        decl->path = ctx->module != NULL && ctx->module->primary_path != NULL
                         ? feng_symbol_internal_dup_cstr(ctx->module->primary_path)
                         : NULL;
        decl->value_type = parse_type_by_id(ctx, type_ref, path, out_error);
        decl->fit_target = decode_decl_kind(kind) == FENG_SYMBOL_DECL_KIND_MODULE
                               ? NULL
                               : parse_type_by_id(ctx, extra_ref, path, out_error);
        if ((name_str != 0U && decl->name == NULL) ||
            (type_ref != 0U && decl->value_type == NULL) ||
            (decode_decl_kind(kind) != FENG_SYMBOL_DECL_KIND_MODULE && extra_ref != 0U && decl->fit_target == NULL)) {
            free(decl->name);
            free(decl->path);
            feng_symbol_internal_type_free(decl->value_type);
            feng_symbol_internal_type_free(decl->fit_target);
            free(decl);
            return false;
        }
        if (sig_ref != 0U) {
            const unsigned char *sig_base = ctx->data + read_u64_le((const unsigned char *)ctx->sigs_section + 0x08);
            const unsigned char *sig = sig_base + (size_t)(sig_ref - 1U) * sizeof(FengSymbolFtSigRecord);
            uint32_t return_type_id = read_u32_le(sig + 0x00);
            uint32_t first_param_index = read_u32_le(sig + 0x04);
            uint32_t param_count = read_u32_le(sig + 0x08);
            uint32_t abi_library_str = read_u32_le(sig + 0x0C + 4U);
            uint32_t param_index;

            decl->calling_convention = (FengAnnotationKind)read_u16_le(sig + 0x0C);
            decl->return_type = parse_type_by_id(ctx, return_type_id, path, out_error);
            decl->abi_library = feng_symbol_internal_dup_cstr(string_at(ctx, abi_library_str));
            if ((return_type_id != 0U && decl->return_type == NULL) ||
                (abi_library_str != 0U && decl->abi_library == NULL)) {
                feng_symbol_internal_decl_free_members(decl);
                free(decl);
                return false;
            }
            for (param_index = 0U; param_index < param_count; ++param_index) {
                const unsigned char *param_base = ctx->data + read_u64_le((const unsigned char *)ctx->prms_section + 0x08);
                const unsigned char *param = param_base + (size_t)(first_param_index + param_index - 1U) * sizeof(FengSymbolFtParamRecord);
                FengSymbolParamView *grown = (FengSymbolParamView *)realloc(
                    decl->params,
                    (decl->param_count + 1U) * sizeof(*decl->params));
                uint32_t param_name_str;
                uint32_t param_type_id;
                uint16_t param_flags;

                if (grown == NULL) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory loading parameter list");
                }
                decl->params = grown;
                memset(&decl->params[decl->param_count], 0, sizeof(*decl->params));
                param_name_str = read_u32_le(param + 0x00);
                param_type_id = read_u32_le(param + 0x04);
                param_flags = read_u16_le(param + 0x08);
                decl->params[decl->param_count].name = feng_symbol_internal_dup_cstr(string_at(ctx, param_name_str));
                decl->params[decl->param_count].mutability =
                    (param_flags & FENG_SYMBOL_FT_PARAM_FLAG_MUTABLE) != 0U ? FENG_MUTABILITY_VAR
                                                                            : FENG_MUTABILITY_LET;
                decl->params[decl->param_count].type = parse_type_by_id(ctx, param_type_id, path, out_error);
                if ((param_name_str != 0U && decl->params[decl->param_count].name == NULL) ||
                    (param_type_id != 0U && decl->params[decl->param_count].type == NULL)) {
                    feng_symbol_internal_decl_free_members(decl);
                    free(decl);
                    return false;
                }
                ++decl->param_count;
            }
        }
        ctx->decls[symbol_index] = decl;
        ctx->decl_symbol_ids[symbol_index] = id;
        if (id == ctx->header.root_symbol_id) {
            ctx->module = (FengSymbolModuleGraph *)calloc(1U, sizeof(*ctx->module));
            if (ctx->module == NULL) {
                return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating module graph");
            }
            ctx->module->profile = (FengSymbolProfile)ctx->header.profile;
            ctx->module->root_decl = *decl;
            ctx->module_full_name_str = extra_ref;
            memset(decl, 0, sizeof(*decl));
            free(ctx->decls[symbol_index]);
            ctx->decls[symbol_index] = &ctx->module->root_decl;
        }
    }
    if (ctx->module == NULL) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "symbol table missing module root symbol");
    }
    return true;
}

static FengSymbolDeclView *decl_by_symbol_id(const ReadContext *ctx, uint32_t symbol_id) {
    size_t index;

    for (index = 0U; index < ctx->decl_count; ++index) {
        if (ctx->decl_symbol_ids[index] == symbol_id) {
            return ctx->decls[index];
        }
    }
    return NULL;
}

static bool attach_decl_hierarchy(ReadContext *ctx,
                                  const char *path,
                                  FengSymbolError *out_error) {
    const unsigned char *base = ctx->data + read_u64_le((const unsigned char *)ctx->syms_section + 0x08);
    uint32_t count = read_u32_le((const unsigned char *)ctx->syms_section + 0x04);
    uint32_t symbol_index;

    for (symbol_index = 0U; symbol_index < count; ++symbol_index) {
        const unsigned char *record = base + symbol_index * sizeof(FengSymbolFtSymRecord);
        uint32_t owner_id = read_u32_le(record + 0x04);
        FengSymbolDeclView *decl = ctx->decls[symbol_index];
        FengSymbolDeclView *owner;
        FengSymbolDeclView **grown;

        if (owner_id == 0U || decl == &ctx->module->root_decl) {
            continue;
        }
        owner = decl_by_symbol_id(ctx, owner_id);
        if (owner == NULL) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "symbol owner id %u not found", owner_id);
        }
        grown = (FengSymbolDeclView **)realloc(owner->members,
                                               (owner->member_count + 1U) * sizeof(*owner->members));
        if (grown == NULL) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory linking symbol hierarchy");
        }
        owner->members = grown;
        owner->members[owner->member_count++] = decl;
        decl->owner = owner;
    }
    return true;
}

static bool parse_module_segments(ReadContext *ctx,
                                  const char *path,
                                  FengSymbolError *out_error) {
    const char *full = ctx->module_full_name_str != 0U ? string_at(ctx, ctx->module_full_name_str) : NULL;
    const char *name = full != NULL && *full != '\0'
                           ? full
                           : (ctx->module->root_decl.name != NULL ? ctx->module->root_decl.name : "");
    char *copy = feng_symbol_internal_dup_cstr(name);
    char *cursor;
    size_t count = 1U;
    size_t index = 0U;

    if (copy == NULL) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory cloning module name");
    }
    for (cursor = copy; *cursor != '\0'; ++cursor) {
        if (*cursor == '.') {
            ++count;
        }
    }
    ctx->module->segments = (char **)calloc(count, sizeof(*ctx->module->segments));
    if (ctx->module->segments == NULL) {
        free(copy);
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating module segments");
    }
    cursor = strtok(copy, ".");
    while (cursor != NULL) {
        ctx->module->segments[index] = feng_symbol_internal_dup_cstr(cursor);
        if (ctx->module->segments[index] == NULL) {
            free(copy);
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory cloning module segment");
        }
        ++index;
        cursor = strtok(NULL, ".");
    }
    ctx->module->segment_count = index;
    free(copy);
    return true;
}

static bool parse_attrs(ReadContext *ctx,
                        const char *path,
                        FengSymbolError *out_error) {
    const unsigned char *base;
    uint32_t count;
    uint32_t index;

    if (ctx->attrs_section == NULL) {
        return true;
    }
    base = ctx->data + read_u64_le((const unsigned char *)ctx->attrs_section + 0x08);
    count = read_u32_le((const unsigned char *)ctx->attrs_section + 0x04);
    for (index = 0U; index < count; ++index) {
        const unsigned char *record = base + index * sizeof(FengSymbolFtAttrRecord);
        uint32_t symbol_id = read_u32_le(record + 0x00);
        uint16_t kind = read_u16_le(record + 0x04);
        uint32_t value0 = read_u32_le(record + 0x08);
        uint32_t value1 = read_u32_le(record + 0x0C);
        FengSymbolDeclView *decl = decl_by_symbol_id(ctx, symbol_id);
        uint32_t attr_index;

        if (decl == NULL) {
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_UNION) {
            decl->union_annotated = true;
            continue;
        }
        if (kind != FENG_SYMBOL_ATTR_DECLARED_SPECS) {
            continue;
        }
        for (attr_index = 0U; attr_index < value1; ++attr_index) {
            FengSymbolTypeView *type = parse_type_by_id(ctx, value0 + attr_index, path, out_error);
            FengSymbolTypeView **grown = (FengSymbolTypeView **)realloc(
                decl->declared_specs,
                (decl->declared_spec_count + 1U) * sizeof(*decl->declared_specs));
            if (type == NULL) {
                return false;
            }
            if (grown == NULL) {
                feng_symbol_internal_type_free(type);
                return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory loading declared spec list");
            }
            decl->declared_specs = grown;
            decl->declared_specs[decl->declared_spec_count++] = type;
        }
    }
    return true;
}

static bool parse_spans(ReadContext *ctx, const char *path, FengSymbolError *out_error) {
    const unsigned char *base;
    uint32_t count;
    uint32_t index;

    if (ctx->spns_section == NULL) {
        return true;
    }
    base = ctx->data + read_u64_le((const unsigned char *)ctx->spns_section + 0x08);
    count = read_u32_le((const unsigned char *)ctx->spns_section + 0x04);
    for (index = 0U; index < count; ++index) {
        const unsigned char *record = base + index * sizeof(FengSymbolFtSpanRecord);
        FengSymbolDeclView *decl = decl_by_symbol_id(ctx, read_u32_le(record + 0x00));
        uint32_t path_str = read_u32_le(record + 0x04);

        if (decl == NULL) {
            continue;
        }
        free(decl->path);
        decl->path = feng_symbol_internal_dup_cstr(string_at(ctx, path_str));
        decl->token.line = read_u32_le(record + 0x08);
        decl->token.column = read_u32_le(record + 0x0C);
        if (path_str != 0U && decl->path == NULL) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory loading symbol span path");
        }
    }
    ctx->module->primary_path = ctx->module->root_decl.path != NULL
                                    ? feng_symbol_internal_dup_cstr(ctx->module->root_decl.path)
                                    : NULL;
    return true;
}

static bool parse_relations(ReadContext *ctx,
                            const char *path,
                            FengSymbolError *out_error) {
    const unsigned char *base = ctx->data + read_u64_le((const unsigned char *)ctx->rels_section + 0x08);
    uint32_t count = read_u32_le((const unsigned char *)ctx->rels_section + 0x04);
    uint32_t index;

    if (count == 0U) {
        return true;
    }
    ctx->module->relations = (FengSymbolRelation *)calloc(count, sizeof(*ctx->module->relations));
    if (ctx->module->relations == NULL) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory loading relation table");
    }
    for (index = 0U; index < count; ++index) {
        const unsigned char *record = base + index * sizeof(FengSymbolFtRelRecord);
        ctx->module->relations[index].kind = (FengSymbolRelationKind)read_u16_le(record + 0x00);
        ctx->module->relations[index].left = decl_by_symbol_id(ctx, read_u32_le(record + 0x04));
        ctx->module->relations[index].right = decl_by_symbol_id(ctx, read_u32_le(record + 0x08));
        ctx->module->relations[index].owner = decl_by_symbol_id(ctx, read_u32_le(record + 0x0C));
    }
    ctx->module->relation_count = count;
    return true;
}

bool feng_symbol_ft_read_bytes_internal(const void *data,
                                        size_t length,
                                        const char *source_name,
                                        const FengSymbolFtReadOptions *options,
                                        FengSymbolGraph **out_graph,
                                        FengSymbolError *out_error) {
    ReadContext ctx;
    FengSymbolGraph *graph = NULL;
    FengSymbolModuleGraph *module_clone = NULL;
    const unsigned char *bytes = (const unsigned char *)data;

    if (out_graph == NULL || (bytes == NULL && length > 0U)) {
        return false;
    }
    *out_graph = NULL;
    memset(&ctx, 0, sizeof(ctx));

    ctx.data = bytes;
    ctx.length = length;

    if (!parse_header(&ctx, source_name, options, out_error) ||
        !load_required_sections(&ctx, source_name, out_error) ||
        !parse_strings(&ctx, source_name, out_error) ||
        !parse_symbols(&ctx, source_name, out_error) ||
        !attach_decl_hierarchy(&ctx, source_name, out_error) ||
        !parse_module_segments(&ctx, source_name, out_error) ||
        !parse_attrs(&ctx, source_name, out_error) ||
        !parse_spans(&ctx, source_name, out_error) ||
        !parse_relations(&ctx, source_name, out_error)) {
        read_context_dispose(&ctx);
        return false;
    }

    module_clone = feng_symbol_internal_module_clone(ctx.module,
                                                     (FengSymbolProfile)ctx.header.profile,
                                                     out_error);
    if (module_clone == NULL) {
        read_context_dispose(&ctx);
        return false;
    }

    graph = (FengSymbolGraph *)calloc(1U, sizeof(*graph));
    if (graph == NULL) {
        feng_symbol_internal_module_free(module_clone);
        read_context_dispose(&ctx);
        return feng_symbol_internal_set_error(out_error,
                                              source_name,
                                              (FengToken){0},
                                              "out of memory allocating symbol graph");
    }
    if (!feng_symbol_internal_graph_append_module(graph, module_clone, out_error)) {
        feng_symbol_internal_module_free(module_clone);
        feng_symbol_graph_free(graph);
        read_context_dispose(&ctx);
        return false;
    }

    *out_graph = graph;
    read_context_dispose(&ctx);
    return true;
}

bool feng_symbol_ft_read_file_internal(const char *path,
                                       const FengSymbolFtReadOptions *options,
                                       FengSymbolGraph **out_graph,
                                       FengSymbolError *out_error) {
    FILE *file = NULL;
    long length;
    unsigned char *data = NULL;
    bool ok;

    if (out_graph == NULL || path == NULL) {
        return false;
    }
    *out_graph = NULL;

    file = fopen(path, "rb");
    if (file == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "failed to open symbol table '%s': %s",
                                              path,
                                              strerror(errno));
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to seek symbol table");
    }
    length = ftell(file);
    if (length < 0L) {
        fclose(file);
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to measure symbol table size");
    }
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to rewind symbol table");
    }
    data = (unsigned char *)malloc((size_t)length);
    if (data == NULL && length > 0L) {
        fclose(file);
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory reading symbol table");
    }
    if ((size_t)length > 0U && fread(data, 1U, (size_t)length, file) != (size_t)length) {
        free(data);
        fclose(file);
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "failed to read symbol table bytes");
    }
    fclose(file);

    ok = feng_symbol_ft_read_bytes_internal(data, (size_t)length, path, options, out_graph, out_error);
    free(data);
    return ok;
}


