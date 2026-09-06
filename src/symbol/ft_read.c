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
    const FengSymbolFtSectionEntry *tseq_section;
    const FengSymbolFtSectionEntry *rels_section;
    const FengSymbolFtSectionEntry *docs_section;
    const FengSymbolFtSectionEntry *attrs_section;
    const FengSymbolFtSectionEntry *callable_deps_section;
    const FengSymbolFtSectionEntry *spns_section;
    char **strings;
    size_t string_count;
    FengSymbolTypeView **types;
    size_t type_count;
    FengSymbolDeclView **decls;
    size_t decl_count;
    uint32_t *decl_symbol_ids;
    uint32_t *decl_doc_refs;
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
    free(ctx->decl_doc_refs);
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
    ctx->tseq_section = find_section(ctx, FENG_SYMBOL_FT_SEC_TSEQ);
    ctx->rels_section = find_section(ctx, FENG_SYMBOL_FT_SEC_RELS);
    ctx->docs_section = find_section(ctx, FENG_SYMBOL_FT_SEC_DOCS);
    ctx->attrs_section = find_section(ctx, FENG_SYMBOL_FT_SEC_ATTRS);
    ctx->callable_deps_section = find_section(
        ctx, FENG_SYMBOL_FT_SEC_CALLABLE_DEPS);
    ctx->spns_section = find_section(ctx, FENG_SYMBOL_FT_SEC_SPNS);

    if (ctx->strs_section == NULL || ctx->syms_section == NULL || ctx->typs_section == NULL ||
        ctx->tseq_section == NULL || ctx->rels_section == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "symbol table missing required core sections");
    }
    if ((ctx->header.flags & FENG_SYMBOL_FT_FLAG_HAS_DOCS) != 0U && ctx->docs_section == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "symbol table declares docs payload but omits DOCS section");
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

static FengSymbolDeclView *decl_by_symbol_id(const ReadContext *ctx,
                                             uint32_t symbol_id);

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
    uint32_t sym_ref;
    uint32_t elem_start;
    uint32_t elem_count;
    size_t layer;

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
    sym_ref = read_u32_le(record + 0x08);
    elem_start = read_u32_le(record + 0x0C);
    elem_count = read_u32_le(record + 0x10);

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
            type->as.pointer.inner = parse_type_by_id(ctx, elem_start, path, out_error);
            break;
        case FENG_SYMBOL_FT_TYPE_KIND_ARRAY:
        {
            FengSymbolTypeView *element =
                parse_type_by_id(ctx, elem_start, path, out_error);
            size_t nested_rank = element != NULL &&
                                         element->kind == FENG_SYMBOL_TYPE_KIND_ARRAY
                                     ? element->as.array.rank
                                     : 0U;

            if (elem_start != 0U && element == NULL) {
                feng_symbol_internal_type_free(type);
                return NULL;
            }
            if ((size_t)elem_count > SIZE_MAX - nested_rank) {
                feng_symbol_internal_type_free(element);
                feng_symbol_internal_type_free(type);
                feng_symbol_internal_set_error(out_error,
                                               path,
                                               (FengToken){0},
                                               "array rank overflows host size");
                return NULL;
            }
            type->kind = FENG_SYMBOL_TYPE_KIND_ARRAY;
            type->as.array.rank = (size_t)elem_count + nested_rank;
            type->as.array.layer_writable =
                (bool *)calloc(type->as.array.rank,
                               sizeof(*type->as.array.layer_writable));
            if (type->as.array.rank > 0U && type->as.array.layer_writable == NULL) {
                feng_symbol_internal_type_free(element);
                feng_symbol_internal_type_free(type);
                feng_symbol_internal_set_error(out_error,
                                               path,
                                               (FengToken){0},
                                               "out of memory loading array mutability bitmap");
                return NULL;
            }
            /* Each record contributes its outer bitmap; consecutive ARRAY
             * child records are the .ft v1 representation of deeper ranks. */
            for (layer = 0U; layer < elem_count; ++layer) {
                type->as.array.layer_writable[layer] =
                    layer < 32U && (string_ref & ((uint32_t)1U << layer)) != 0U;
            }
            if (nested_rank > 0U) {
                memcpy(type->as.array.layer_writable + elem_count,
                       element->as.array.layer_writable,
                       nested_rank * sizeof(*type->as.array.layer_writable));
                type->as.array.element = element->as.array.element;
                element->as.array.element = NULL;
                feng_symbol_internal_type_free(element);
            } else {
                type->as.array.element = element;
            }
            break;
        }
        case FENG_SYMBOL_FT_TYPE_KIND_CALLABLE:
        case FENG_SYMBOL_FT_TYPE_KIND_SPEC_OBJECT:
        case FENG_SYMBOL_FT_TYPE_KIND_SPEC_CALLABLE:
        case FENG_SYMBOL_FT_TYPE_KIND_SPEC_UNION:
        case FENG_SYMBOL_FT_TYPE_KIND_SPEC_INTERSECTION:
            /* These node kinds are structural (not value types) and are
             * handled directly in parse_symbols.  If encountered here it
             * means a corrupt TYPS reference from a value-type context. */
            free(type);
            feng_symbol_internal_set_error(out_error,
                                           path,
                                           (FengToken){0},
                                           "type id %u (kind %u) is not a value type",
                                           type_id,
                                           (unsigned)kind);
            return NULL;
        case FENG_SYMBOL_FT_TYPE_KIND_TYPE_PARAM_REF:
            type->kind = FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF;
            type->as.type_param_ref.name = feng_symbol_internal_dup_cstr(string_at(ctx, string_ref));
            if (string_ref != 0U && type->as.type_param_ref.name == NULL) {
                free(type);
                feng_symbol_internal_set_error(out_error, path, (FengToken){0},
                                               "out of memory reading type parameter ref name");
                return NULL;
            }
            break;
        case FENG_SYMBOL_FT_TYPE_KIND_NAMED_GENERIC: {
            FengSymbolTypeView *named_base;
            size_t arg_index;

            free(type);
            named_base = parse_named_type_from_string(string_at(ctx, string_ref), path, out_error);
            if (named_base == NULL) {
                return NULL;
            }
            /* Transplant segments from NAMED into a new NAMED_GENERIC node. */
            type = (FengSymbolTypeView *)calloc(1U, sizeof(*type));
            if (type == NULL) {
                feng_symbol_internal_type_free(named_base);
                feng_symbol_internal_set_error(out_error, path, (FengToken){0},
                                               "out of memory building generic type node");
                return NULL;
            }
            type->kind = FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC;
            type->as.named_generic.segments = named_base->as.named.segments;
            type->as.named_generic.segment_count = named_base->as.named.segment_count;
            named_base->as.named.segments = NULL;
            named_base->as.named.segment_count = 0U;
            feng_symbol_internal_type_free(named_base);
            /* Read type args from TSEQ section. */
            if (elem_count > 0U) {
                type->as.named_generic.type_args =
                    (FengSymbolTypeView **)calloc(elem_count, sizeof(FengSymbolTypeView *));
                if (type->as.named_generic.type_args == NULL) {
                    feng_symbol_internal_type_free(type);
                    feng_symbol_internal_set_error(out_error, path, (FengToken){0},
                                                   "out of memory loading generic type args");
                    return NULL;
                }
                type->as.named_generic.type_arg_count = 0U;
                for (arg_index = 0U; arg_index < elem_count; ++arg_index) {
                    const unsigned char *tseq_base2;
                    uint32_t tseq_total2;
                    uint32_t tseq_id;
                    uint32_t arg_type_id;

                    tseq_total2 = read_u32_le((const unsigned char *)ctx->tseq_section + 0x04);
                    tseq_id = elem_start + (uint32_t)arg_index;
                    if (tseq_id == 0U || tseq_id > tseq_total2) {
                        feng_symbol_internal_type_free(type);
                        feng_symbol_internal_set_error(out_error, path, (FengToken){0},
                                                       "generic type TSEQ index %u out of range", tseq_id);
                        return NULL;
                    }
                    tseq_base2 = ctx->data + read_u64_le((const unsigned char *)ctx->tseq_section + 0x08);
                    arg_type_id = read_u32_le(tseq_base2 + (size_t)(tseq_id - 1U) * 12U + 0x04);
                    type->as.named_generic.type_args[arg_index] =
                        parse_type_by_id(ctx, arg_type_id, path, out_error);
                    if (arg_type_id != 0U && type->as.named_generic.type_args[arg_index] == NULL) {
                        feng_symbol_internal_type_free(type);
                        return NULL;
                    }
                    type->as.named_generic.type_arg_count = arg_index + 1U;
                }
            }
            break;
        }
        default:
            free(type);
            feng_symbol_internal_set_error(out_error,
                                           path,
                                           (FengToken){0},
                                           "unknown type record kind %u",
                                           kind);
            return NULL;
    }
    if ((type->kind == FENG_SYMBOL_TYPE_KIND_NAMED ||
         type->kind == FENG_SYMBOL_TYPE_KIND_NAMED_GENERIC ||
         type->kind == FENG_SYMBOL_TYPE_KIND_TYPE_PARAM_REF) &&
        sym_ref != 0U) {
        type->target_decl = decl_by_symbol_id(ctx, sym_ref);
        if (type->target_decl == NULL) {
            feng_symbol_internal_type_free(type);
            feng_symbol_internal_set_error(out_error,
                                           path,
                                           (FengToken){0},
                                           "type id %u refers to missing symbol %u",
                                           type_id,
                                           sym_ref);
            return NULL;
        }
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
/* Populate decl->params and decl->return_type by reading a CALLABLE or
 * SPEC_CALLABLE TYPS node and the corresponding TSEQ elements.
 * type_id == 0 is silently accepted (no signature). */
static bool parse_callable_from_type_ref(ReadContext *ctx,
                                         uint32_t type_id,
                                         FengSymbolDeclView *decl,
                                         const char *path,
                                         FengSymbolError *out_error) {
    const unsigned char *typs_base;
    const unsigned char *record;
    uint32_t typs_count;
    uint16_t kind;
    uint32_t elem_start;
    uint32_t elem_count;
    uint32_t tseq_total;
    const unsigned char *tseq_base;
    uint32_t param_index;

    if (type_id == 0U) {
        return true;
    }
    typs_count = read_u32_le((const unsigned char *)ctx->typs_section + 0x04);
    if (type_id > typs_count) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "callable type id %u is out of TYPS range",
                                              type_id);
    }
    typs_base = ctx->data + read_u64_le((const unsigned char *)ctx->typs_section + 0x08);
    record = typs_base + (size_t)(type_id - 1U) * sizeof(FengSymbolFtTypeRecord);
    kind = read_u16_le(record + 0x00);
    elem_start = read_u32_le(record + 0x0C);
    elem_count = read_u32_le(record + 0x10);

    if (kind != FENG_SYMBOL_FT_TYPE_KIND_CALLABLE &&
        kind != FENG_SYMBOL_FT_TYPE_KIND_SPEC_CALLABLE) {
        if (kind == FENG_SYMBOL_FT_TYPE_KIND_SPEC_OBJECT) {
            return true; /* object-form spec: no callable info */
        }
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "type_ref %u for callable symbol has unexpected kind %u",
                                              type_id,
                                              (unsigned)kind);
    }
    if (elem_count == 0U) {
        return true; /* no elements at all */
    }

    tseq_total = read_u32_le((const unsigned char *)ctx->tseq_section + 0x04);
    if (elem_start + elem_count > tseq_total) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "callable TSEQ range [%u, %u) exceeds TSEQ section size %u",
                                              elem_start,
                                              elem_start + elem_count,
                                              tseq_total);
    }
    tseq_base = ctx->data + read_u64_le((const unsigned char *)ctx->tseq_section + 0x08);

    /* First (elem_count - 1) elements are parameters */
    for (param_index = 0U; param_index < elem_count - 1U; ++param_index) {
        const unsigned char *elem =
            tseq_base + (size_t)(elem_start + param_index) * sizeof(FengSymbolFtTseqRecord);
        FengSymbolParamView *grown;
        uint32_t name_str = read_u32_le(elem + 0x00);
        uint32_t type_id_elem = read_u32_le(elem + 0x04);
        uint16_t flags = read_u16_le(elem + 0x08);

        grown = (FengSymbolParamView *)realloc(decl->params,
                                               (decl->param_count + 1U) * sizeof(*decl->params));
        if (grown == NULL) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "out of memory loading parameter list");
        }
        decl->params = grown;
        memset(&decl->params[decl->param_count], 0, sizeof(*decl->params));
        decl->params[decl->param_count].name =
            feng_symbol_internal_dup_cstr(string_at(ctx, name_str));
        decl->params[decl->param_count].mutability =
            (flags & FENG_SYMBOL_FT_TSEQ_FLAG_VAR) != 0U ? FENG_MUTABILITY_VAR
                                                          : FENG_MUTABILITY_LET;
        decl->params[decl->param_count].is_variadic =
            (flags & FENG_SYMBOL_FT_TSEQ_FLAG_VARIADIC) != 0U;
        decl->params[decl->param_count].type =
            parse_type_by_id(ctx, type_id_elem, path, out_error);
        if ((name_str != 0U && decl->params[decl->param_count].name == NULL) ||
            (type_id_elem != 0U && decl->params[decl->param_count].type == NULL)) {
            return false;
        }
        ++decl->param_count;
    }

    /* Last element is the return type (name_str = 0) */
    {
        const unsigned char *elem =
            tseq_base + (size_t)(elem_start + elem_count - 1U) * sizeof(FengSymbolFtTseqRecord);
        uint32_t return_type_id = read_u32_le(elem + 0x04);

        decl->return_type = parse_type_by_id(ctx, return_type_id, path, out_error);
        if (return_type_id != 0U && decl->return_type == NULL) {
            return false;
        }
    }
    return true;
}

static bool parse_union_members_from_type_ref(ReadContext *ctx,
                                              uint32_t type_id,
                                              FengSymbolDeclView *decl,
                                              const char *path,
                                              FengSymbolError *out_error) {
    const unsigned char *typs_base;
    const unsigned char *record;
    const unsigned char *tseq_base;
    uint32_t typs_total;
    uint32_t tseq_total;
    uint16_t kind;
    uint32_t elem_start;
    uint32_t elem_count;

    if (type_id == 0U || decl == NULL) {
        return true;
    }

    typs_total = read_u32_le((const unsigned char *)ctx->typs_section + 0x04);
    if (type_id > typs_total) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "spec type_ref %u is out of range",
                                              type_id);
    }
    typs_base = ctx->data + read_u64_le((const unsigned char *)ctx->typs_section + 0x08);
    record = typs_base + (size_t)(type_id - 1U) * sizeof(FengSymbolFtTypeRecord);
    kind = read_u16_le(record + 0x00);
    elem_start = read_u32_le(record + 0x0C);
    elem_count = read_u32_le(record + 0x10);

    if (kind != FENG_SYMBOL_FT_TYPE_KIND_SPEC_UNION) {
        return false;
    }
    if (elem_count == 0U) {
        return true;
    }

    tseq_total = read_u32_le((const unsigned char *)ctx->tseq_section + 0x04);
    if (elem_start + elem_count > tseq_total) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "union member TSEQ range [%u, %u) exceeds TSEQ section size %u",
                                              elem_start,
                                              elem_start + elem_count,
                                              tseq_total);
    }
    decl->union_members = (FengSymbolTypeView **)calloc(elem_count, sizeof(*decl->union_members));
    if (decl->union_members == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "out of memory loading union member list");
    }

    tseq_base = ctx->data + read_u64_le((const unsigned char *)ctx->tseq_section + 0x08);
    for (uint32_t member_index = 0U; member_index < elem_count; ++member_index) {
        const unsigned char *elem =
            tseq_base + (size_t)(elem_start + member_index) * sizeof(FengSymbolFtTseqRecord);
        uint32_t member_type_id = read_u32_le(elem + 0x04);

        decl->union_members[member_index] = parse_type_by_id(ctx, member_type_id, path, out_error);
        if (member_type_id != 0U && decl->union_members[member_index] == NULL) {
            return false;
        }
        decl->union_member_count = (size_t)member_index + 1U;
    }
    return true;
}

static bool parse_intersection_members_from_type_ref(ReadContext *ctx,
                                                     uint32_t type_id,
                                                     FengSymbolDeclView *decl,
                                                     const char *path,
                                                     FengSymbolError *out_error) {
    const unsigned char *typs_base;
    const unsigned char *record;
    const unsigned char *tseq_base;
    uint32_t typs_total;
    uint32_t tseq_total;
    uint16_t kind;
    uint32_t elem_start;
    uint32_t elem_count;

    if (type_id == 0U || decl == NULL) {
        return true;
    }

    typs_total = read_u32_le((const unsigned char *)ctx->typs_section + 0x04);
    if (type_id > typs_total) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "spec type_ref %u is out of range",
                                              type_id);
    }
    typs_base = ctx->data + read_u64_le((const unsigned char *)ctx->typs_section + 0x08);
    record = typs_base + (size_t)(type_id - 1U) * sizeof(FengSymbolFtTypeRecord);
    kind = read_u16_le(record + 0x00);
    elem_start = read_u32_le(record + 0x0C);
    elem_count = read_u32_le(record + 0x10);

    if (kind != FENG_SYMBOL_FT_TYPE_KIND_SPEC_INTERSECTION) {
        return false;
    }
    if (elem_count == 0U) {
        return true;
    }

    tseq_total = read_u32_le((const unsigned char *)ctx->tseq_section + 0x04);
    if (elem_start + elem_count > tseq_total) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "intersection member TSEQ range [%u, %u) exceeds TSEQ section size %u",
                                              elem_start,
                                              elem_start + elem_count,
                                              tseq_total);
    }
    decl->intersection_members = (FengSymbolTypeView **)calloc(elem_count, sizeof(*decl->intersection_members));
    if (decl->intersection_members == NULL) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "out of memory allocating intersection member list (%u entries)",
                                              elem_count);
    }
    tseq_base = ctx->data + read_u64_le((const unsigned char *)ctx->tseq_section + 0x08);
    for (uint32_t member_index = 0U; member_index < elem_count; ++member_index) {
        const unsigned char *tseq_record = tseq_base + (size_t)(elem_start + member_index) * sizeof(FengSymbolFtTseqRecord);
        uint32_t member_type_id = read_u32_le(tseq_record + 0x04);

        decl->intersection_members[member_index] = parse_type_by_id(ctx, member_type_id, path, out_error);
        if (member_type_id != 0U && decl->intersection_members[member_index] == NULL) {
            return false;
        }
        decl->intersection_member_count = (size_t)member_index + 1U;
    }
    return true;
}

static bool parse_spec_from_type_ref(ReadContext *ctx,
                                     uint32_t type_id,
                                     FengSymbolDeclView *decl,
                                     const char *path,
                                     FengSymbolError *out_error) {
    const unsigned char *typs_base;
    const unsigned char *record;
    uint32_t typs_total;
    uint16_t kind;

    if (type_id == 0U || decl == NULL) {
        return true;
    }

    typs_total = read_u32_le((const unsigned char *)ctx->typs_section + 0x04);
    if (type_id > typs_total) {
        return feng_symbol_internal_set_error(out_error,
                                              path,
                                              (FengToken){0},
                                              "spec type_ref %u is out of range",
                                              type_id);
    }
    typs_base = ctx->data + read_u64_le((const unsigned char *)ctx->typs_section + 0x08);
    record = typs_base + (size_t)(type_id - 1U) * sizeof(FengSymbolFtTypeRecord);
    kind = read_u16_le(record + 0x00);

    if (kind == FENG_SYMBOL_FT_TYPE_KIND_SPEC_UNION) {
        return parse_union_members_from_type_ref(ctx, type_id, decl, path, out_error);
    }
    if (kind == FENG_SYMBOL_FT_TYPE_KIND_SPEC_INTERSECTION) {
        return parse_intersection_members_from_type_ref(ctx, type_id, decl, path, out_error);
    }
    return parse_callable_from_type_ref(ctx, type_id, decl, path, out_error);
}

static FengSymbolDeclKind decode_decl_kind(uint16_t kind) {
    switch (kind) {
        case FENG_SYMBOL_FT_SYM_KIND_MODULE:
            return FENG_SYMBOL_DECL_KIND_MODULE;
        case FENG_SYMBOL_FT_SYM_KIND_TYPE:
            return FENG_SYMBOL_DECL_KIND_TYPE;
        case FENG_SYMBOL_FT_SYM_KIND_ENUM:
            return FENG_SYMBOL_DECL_KIND_ENUM;
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
        case FENG_SYMBOL_FT_SYM_KIND_ENUM_ITEM:
            return FENG_SYMBOL_DECL_KIND_ENUM_ITEM;
        case FENG_SYMBOL_FT_SYM_KIND_TYPE_PARAM:
            return FENG_SYMBOL_DECL_KIND_TYPE_PARAM;
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
    ctx->decl_doc_refs = (uint32_t *)calloc(count, sizeof(*ctx->decl_doc_refs));
    if ((count > 0U && ctx->decls == NULL) || (count > 0U && ctx->decl_symbol_ids == NULL) ||
        (count > 0U && ctx->decl_doc_refs == NULL)) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating symbol table views");
    }
    ctx->decl_count = count;

    for (symbol_index = 0U; symbol_index < count; ++symbol_index) {
        /* New FengSymbolFtSymRecord layout (28 bytes, sig_ref removed):
         *   0x00 id, 0x04 owner_id, 0x08 name_str, 0x0C kind, 0x0E flags,
         *   0x10 type_ref, 0x14 extra_ref, 0x18 doc_ref */
        const unsigned char *record = base + symbol_index * sizeof(FengSymbolFtSymRecord);
        FengSymbolDeclView *decl = (FengSymbolDeclView *)calloc(1U, sizeof(*decl));
        uint32_t id = read_u32_le(record + 0x00);
        uint32_t name_str = read_u32_le(record + 0x08);
        uint16_t kind = read_u16_le(record + 0x0C);
        uint16_t flags = read_u16_le(record + 0x0E);
        uint32_t extra_ref = read_u32_le(record + 0x14);
        uint32_t doc_ref = read_u32_le(record + 0x18);
        FengSymbolDeclKind decl_kind;

        if (decl == NULL) {
            return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating declaration view");
        }
        decl_kind = decode_decl_kind(kind);
        decl->kind = decl_kind;
        decl->ft_symbol_id = id;
        decl->visibility = (flags & FENG_SYMBOL_FT_SYM_FLAG_PUBLIC) != 0U ? FENG_VISIBILITY_PUBLIC
                                                                          : FENG_VISIBILITY_PRIVATE;
        decl->mutability = (flags & FENG_SYMBOL_FT_SYM_FLAG_MUTABLE) != 0U ? FENG_MUTABILITY_VAR
                                                                            : FENG_MUTABILITY_LET;
        decl->abi_annotated = (flags & FENG_SYMBOL_FT_SYM_FLAG_ABI) != 0U;
        decl->is_extern = (flags & FENG_SYMBOL_FT_SYM_FLAG_EXTERN) != 0U;
        decl->bounded_decl = (flags & FENG_SYMBOL_FT_SYM_FLAG_BOUNDED_DECL) != 0U;
        decl->has_doc = (flags & FENG_SYMBOL_FT_SYM_FLAG_HAS_DOC) != 0U;
        decl->is_tuple = (flags & FENG_SYMBOL_FT_SYM_FLAG_TUPLE_DECL) != 0U;
        decl->is_value = (flags & FENG_SYMBOL_FT_SYM_FLAG_IS_VALUE) != 0U;
        decl->name = feng_symbol_internal_dup_cstr(string_at(ctx, name_str));
        decl->path = ctx->module != NULL && ctx->module->primary_path != NULL
                         ? feng_symbol_internal_dup_cstr(ctx->module->primary_path)
                         : NULL;

        if (kind == FENG_SYMBOL_FT_SYM_KIND_SPEC) {
            uint16_t form_bits = flags & FENG_SYMBOL_FT_SYM_FLAG_SPEC_FORM_MASK;

            if (form_bits == FENG_SYMBOL_FT_SYM_FLAG_SPEC_FORM_UNION) {
                decl->spec_form = FENG_SPEC_FORM_UNION;
            } else if (form_bits == FENG_SYMBOL_FT_SYM_FLAG_SPEC_FORM_INTERSECTION) {
                decl->spec_form = FENG_SPEC_FORM_INTERSECTION;
            } else if (form_bits == FENG_SYMBOL_FT_SYM_FLAG_SPEC_FORM_CALLABLE) {
                decl->spec_form = FENG_SPEC_FORM_CALLABLE;
            } else {
                decl->spec_form = FENG_SPEC_FORM_OBJECT;
            }
        }

        if (decl_kind == FENG_SYMBOL_DECL_KIND_ENUM_ITEM) {
            decl->enum_item_ordinal = extra_ref;
        }
        if (name_str != 0U && decl->name == NULL) {
            feng_symbol_internal_decl_free_members(decl);
            free(decl);
            return false;
        }

        ctx->decls[symbol_index] = decl;
        ctx->decl_symbol_ids[symbol_index] = id;
        ctx->decl_doc_refs[symbol_index] = doc_ref;
        if (id == ctx->header.root_symbol_id) {
            ctx->module = (FengSymbolModuleGraph *)calloc(1U, sizeof(*ctx->module));
            if (ctx->module == NULL) {
                return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "out of memory allocating module graph");
            }
            ctx->module->profile = (FengSymbolProfile)ctx->header.profile;
            ctx->module->root_decl = *decl;
            ctx->module->visibility = ctx->module->root_decl.visibility;
            ctx->module_full_name_str = extra_ref;
            memset(decl, 0, sizeof(*decl));
            free(ctx->decls[symbol_index]);
            ctx->decls[symbol_index] = &ctx->module->root_decl;
        }
    }
    if (ctx->module == NULL) {
        return feng_symbol_internal_set_error(out_error, path, (FengToken){0}, "symbol table missing module root symbol");
    }

    /*
     * Phase two: every declaration id is now registered, so type sym_ref
     * values can be restored without depending on declaration order.
     */
    for (symbol_index = 0U; symbol_index < count; ++symbol_index) {
        const unsigned char *record =
            base + symbol_index * sizeof(FengSymbolFtSymRecord);
        FengSymbolDeclView *decl = ctx->decls[symbol_index];
        uint16_t kind = read_u16_le(record + 0x0C);
        uint32_t type_ref = read_u32_le(record + 0x10);
        uint32_t extra_ref = read_u32_le(record + 0x14);
        bool is_callable_kind =
            kind == FENG_SYMBOL_FT_SYM_KIND_TOP_FN ||
            kind == FENG_SYMBOL_FT_SYM_KIND_EXTERN_FN ||
            kind == FENG_SYMBOL_FT_SYM_KIND_METHOD ||
            kind == FENG_SYMBOL_FT_SYM_KIND_CTOR ||
            kind == FENG_SYMBOL_FT_SYM_KIND_DTOR;

        if (kind == FENG_SYMBOL_FT_SYM_KIND_SPEC) {
            if (!parse_spec_from_type_ref(ctx,
                                          type_ref,
                                          decl,
                                          path,
                                          out_error)) {
                return false;
            }
        } else if (is_callable_kind) {
            if (!parse_callable_from_type_ref(ctx,
                                              type_ref,
                                              decl,
                                              path,
                                              out_error)) {
                return false;
            }
        } else if (decl->kind == FENG_SYMBOL_DECL_KIND_BINDING ||
                   decl->kind == FENG_SYMBOL_DECL_KIND_FIELD ||
                   decl->kind == FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
            decl->value_type = parse_type_by_id(ctx,
                                               type_ref,
                                               path,
                                               out_error);
            if (type_ref != 0U && decl->value_type == NULL) {
                return false;
            }
        }

        if (decl->kind == FENG_SYMBOL_DECL_KIND_FIT) {
            decl->fit_target = parse_type_by_id(ctx,
                                               extra_ref,
                                               path,
                                               out_error);
            if (extra_ref != 0U && decl->fit_target == NULL) {
                return false;
            }
        }
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

static bool parse_docs(ReadContext *ctx,
                       const char *path,
                       FengSymbolError *out_error) {
    const unsigned char *base;
    uint32_t count;
    size_t decl_index;

    if (ctx->docs_section == NULL) {
        for (decl_index = 0U; decl_index < ctx->decl_count; ++decl_index) {
            if (ctx->decl_doc_refs[decl_index] != 0U ||
                (ctx->decls[decl_index] != NULL && ctx->decls[decl_index]->has_doc)) {
                return feng_symbol_internal_set_error(out_error,
                                                      path,
                                                      (FengToken){0},
                                                      "symbol table declaration marked with docs but DOCS payload is missing");
            }
        }
        return true;
    }

    if (!validate_range(ctx,
                        read_u64_le((const unsigned char *)ctx->docs_section + 0x08),
                        read_u64_le((const unsigned char *)ctx->docs_section + 0x10),
                        path,
                        out_error)) {
        return false;
    }

    base = ctx->data + read_u64_le((const unsigned char *)ctx->docs_section + 0x08);
    count = read_u32_le((const unsigned char *)ctx->docs_section + 0x04);

    for (decl_index = 0U; decl_index < ctx->decl_count; ++decl_index) {
        FengSymbolDeclView *decl = ctx->decls[decl_index];
        uint32_t doc_ref = ctx->decl_doc_refs[decl_index];
        size_t doc_index;
        const unsigned char *matched = NULL;
        const char *doc_text;

        if ((decl->has_doc && doc_ref == 0U) || (!decl->has_doc && doc_ref != 0U)) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "symbol table doc flag/reference mismatch for symbol %u",
                                                  ctx->decl_symbol_ids[decl_index]);
        }
        if (doc_ref == 0U) {
            continue;
        }

        for (doc_index = 0U; doc_index < count; ++doc_index) {
            const unsigned char *record = base + doc_index * sizeof(FengSymbolFtDocRecord);
            if (read_u32_le(record + 0x00) == doc_ref) {
                matched = record;
                break;
            }
        }
        if (matched == NULL) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "doc record %u is missing",
                                                  doc_ref);
        }
        if (read_u32_le(matched + 0x04) != ctx->decl_symbol_ids[decl_index]) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "doc record %u points at the wrong symbol",
                                                  doc_ref);
        }

        doc_text = string_at(ctx, read_u32_le(matched + 0x08));
        if (doc_text == NULL) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "doc record %u refers to a missing string",
                                                  doc_ref);
        }
        decl->doc = feng_symbol_internal_dup_cstr(doc_text);
        if (decl->doc == NULL) {
            return feng_symbol_internal_set_error(out_error,
                                                  path,
                                                  (FengToken){0},
                                                  "out of memory loading doc payload for symbol %u",
                                                  ctx->decl_symbol_ids[decl_index]);
        }
        decl->has_doc = true;
    }

    return true;
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

    /* Compute type_param_count for each decl from its TYPE_PARAM children. */
    for (symbol_index = 0U; symbol_index < count; ++symbol_index) {
        FengSymbolDeclView *decl = ctx->decls[symbol_index];
        size_t tp_count = 0U;
        size_t mi;

        if (decl == NULL) {
            continue;
        }
        for (mi = 0U; mi < decl->member_count; ++mi) {
            if (decl->members[mi] != NULL &&
                decl->members[mi]->kind == FENG_SYMBOL_DECL_KIND_TYPE_PARAM) {
                ++tp_count;
            }
        }
        decl->type_param_count = tp_count;
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
        if (kind == FENG_SYMBOL_ATTR_CALL_CONV) {
            decl->calling_convention = (FengAnnotationKind)value0;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_ABI_LIBRARY) {
            free(decl->abi_library);
            decl->abi_library = feng_symbol_internal_dup_cstr(string_at(ctx, value0));
            if (value0 != 0U && decl->abi_library == NULL) {
                return feng_symbol_internal_set_error(out_error,
                                                      path,
                                                      (FengToken){0},
                                                      "out of memory loading abi_library string");
            }
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_ABI_SYMBOL) {
            free(decl->abi_symbol);
            decl->abi_symbol = feng_symbol_internal_dup_cstr(string_at(ctx, value0));
            if (value0 != 0U && decl->abi_symbol == NULL) {
                return feng_symbol_internal_set_error(out_error,
                                                      path,
                                                      (FengToken){0},
                                                      "out of memory loading abi_symbol string");
            }
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_ABI_FIXED_PARAM_COUNT) {
            decl->abi_fixed_param_count = (size_t)value0;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_ENUM_ITEM_VALUE) {
            decl->enum_item_value = (int64_t)(int32_t)value0;
            decl->has_enum_item_value = true;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_STATIC_MEMBER) {
            decl->is_static = true;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_ITERABLE_METHOD) {
            decl->is_iterable = true;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_ITERATOR_METHOD) {
            decl->is_iterator = true;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_MIXABLE_METHOD) {
            decl->is_mixable = true;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_REIFIABLE_AGGREGATE_DEP) {
            FengSymbolTypeView *type = parse_type_by_id(ctx, value0, path, out_error);
            FengSymbolTypeView **grown;

            if (type == NULL) {
                return false;
            }
            grown = (FengSymbolTypeView **)realloc(
                decl->reifiable_agg_deps,
                (decl->reifiable_agg_dep_count + 1U) * sizeof(*decl->reifiable_agg_deps));
            if (grown == NULL) {
                feng_symbol_internal_type_free(type);
                return feng_symbol_internal_set_error(out_error, path, (FengToken){0},
                                                      "out of memory loading reifiable agg dep");
            }
            decl->reifiable_agg_deps = grown;
            decl->reifiable_agg_deps[decl->reifiable_agg_dep_count++] = type;
            continue;
        }
        if (kind == FENG_SYMBOL_ATTR_REIFIABLE_MANAGED_DEP) {
            FengSymbolTypeView *type = parse_type_by_id(ctx, value0, path, out_error);
            FengSymbolTypeView **grown;

            if (type == NULL) {
                return false;
            }
            grown = (FengSymbolTypeView **)realloc(
                decl->reifiable_type_deps,
                (decl->reifiable_type_dep_count + 1U) * sizeof(*decl->reifiable_type_deps));
            if (grown == NULL) {
                feng_symbol_internal_type_free(type);
                return feng_symbol_internal_set_error(out_error, path, (FengToken){0},
                                                      "out of memory loading reifiable type dep");
            }
            decl->reifiable_type_deps = grown;
            decl->reifiable_type_deps[decl->reifiable_type_dep_count++] = type;
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

/* Return whether one callable dependency kind has a complete FT restoration
 * path in the current reader. Keep this explicit so newly added callable
 * categories cannot be accepted without matching import reconstruction. */
static bool callable_dependency_kind_is_supported(uint16_t kind) {
    switch ((FengResolvedCallableKind)kind) {
        case FENG_RESOLVED_CALLABLE_FUNCTION:
        case FENG_RESOLVED_CALLABLE_TYPE_METHOD:
        case FENG_RESOLVED_CALLABLE_FIT_METHOD:
        case FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD:
        case FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD:
        case FENG_RESOLVED_CALLABLE_SPEC_METHOD:
        case FENG_RESOLVED_CALLABLE_SPEC_STATIC_METHOD:
            return true;

        case FENG_RESOLVED_CALLABLE_NONE:
        case FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR:
        default:
            return false;
    }
}

/* Restore direct callable dependencies and their caller-view type arguments. */
static bool parse_callable_dependencies(ReadContext *ctx,
                                        const char *path,
                                        FengSymbolError *out_error) {
    const unsigned char *section;
    const unsigned char *base;
    const unsigned char *tseq_base;
    uint64_t section_offset;
    uint64_t section_size;
    uint32_t count;
    uint32_t tseq_total;
    uint32_t index;
    const char *current_module_name;

    if (ctx->callable_deps_section == NULL) {
        return true;
    }
    section = (const unsigned char *)ctx->callable_deps_section;
    section_offset = read_u64_le(section + 0x08);
    section_size = read_u64_le(section + 0x10);
    count = read_u32_le(section + 0x04);
    if (read_u32_le(section + 0x18) !=
            sizeof(FengSymbolFtCallableDepRecord) ||
        section_size !=
            (uint64_t)count * sizeof(FengSymbolFtCallableDepRecord) ||
        !validate_range(ctx,
                        section_offset,
                        section_size,
                        path,
                        out_error)) {
        return feng_symbol_internal_set_error(
            out_error,
            path,
            (FengToken){0},
            "malformed callable dependency section");
    }
    base = ctx->data + section_offset;
    tseq_total = read_u32_le(
        (const unsigned char *)ctx->tseq_section + 0x04);
    tseq_base = ctx->data + read_u64_le(
        (const unsigned char *)ctx->tseq_section + 0x08);
    current_module_name = string_at(ctx, ctx->module_full_name_str);

    for (index = 0U; index < count; ++index) {
        const unsigned char *record =
            base + (size_t)index * sizeof(FengSymbolFtCallableDepRecord);
        uint32_t caller_symbol_id = read_u32_le(record + 0x00);
        uint32_t target_module_str = read_u32_le(record + 0x04);
        uint32_t target_symbol_id = read_u32_le(record + 0x08);
        uint16_t kind = read_u16_le(record + 0x0C);
        uint16_t purpose = read_u16_le(record + 0x0E);
        uint32_t owner_instance_type_id = read_u32_le(record + 0x10);
        uint32_t callable_arg_start = read_u32_le(record + 0x14);
        uint32_t callable_arg_count = read_u32_le(record + 0x18);
        uint32_t target_callable_type_id = read_u32_le(record + 0x1C);
        const char *target_module_name = string_at(ctx, target_module_str);
        FengSymbolDeclView *caller = decl_by_symbol_id(ctx, caller_symbol_id);
        FengSymbolCallableDepView *grown;
        FengSymbolCallableDepView *dependency;
        uint32_t arg_index;

        if (caller == NULL || target_module_str == 0U ||
            target_module_name == NULL || target_symbol_id == 0U ||
            !callable_dependency_kind_is_supported(kind) ||
            purpose > FENG_SYMBOL_CALLABLE_DEP_CALLABLE_VALUE ||
            callable_arg_start > tseq_total ||
            callable_arg_count > tseq_total - callable_arg_start) {
            return feng_symbol_internal_set_error(
                out_error,
                path,
                (FengToken){0},
                "invalid callable dependency record %u",
                index);
        }
        grown = (FengSymbolCallableDepView *)realloc(
            caller->reifiable_callable_deps,
            (caller->reifiable_callable_dep_count + 1U) * sizeof(*grown));
        if (grown == NULL) {
            return feng_symbol_internal_set_error(
                out_error,
                path,
                (FengToken){0},
                "out of memory loading callable dependency");
        }
        caller->reifiable_callable_deps = grown;
        dependency = &caller->reifiable_callable_deps[
            caller->reifiable_callable_dep_count];
        memset(dependency, 0, sizeof(*dependency));
        ++caller->reifiable_callable_dep_count;
        dependency->kind = (FengResolvedCallableKind)kind;
        dependency->purpose =
            (FengSymbolCallableDepPurpose)purpose;
        dependency->target_module_name =
            feng_symbol_internal_dup_cstr(target_module_name);
        dependency->target_symbol_id = target_symbol_id;
        dependency->owner_instance_type = parse_type_by_id(
            ctx, owner_instance_type_id, path, out_error);
        dependency->target_callable_type = parse_type_by_id(
            ctx, target_callable_type_id, path, out_error);
        if (dependency->target_module_name == NULL ||
            (owner_instance_type_id != 0U &&
             dependency->owner_instance_type == NULL) ||
            (target_callable_type_id != 0U &&
             dependency->target_callable_type == NULL)) {
            free(dependency->target_module_name);
            dependency->target_module_name = NULL;
            feng_symbol_internal_type_free(
                dependency->owner_instance_type);
            dependency->owner_instance_type = NULL;
            feng_symbol_internal_type_free(
                dependency->target_callable_type);
            dependency->target_callable_type = NULL;
            return false;
        }
        if (current_module_name != NULL &&
            strcmp(current_module_name, target_module_name) == 0) {
            dependency->local_target_decl =
                decl_by_symbol_id(ctx, target_symbol_id);
            if (dependency->local_target_decl == NULL) {
                return feng_symbol_internal_set_error(
                    out_error,
                    path,
                    (FengToken){0},
                    "local callable dependency target %u was not found",
                    target_symbol_id);
            }
        }
        if (callable_arg_count > 0U) {
            dependency->callable_type_args =
                (FengSymbolTypeView **)calloc(
                    callable_arg_count,
                    sizeof(*dependency->callable_type_args));
            if (dependency->callable_type_args == NULL) {
                return feng_symbol_internal_set_error(
                    out_error,
                    path,
                    (FengToken){0},
                    "out of memory loading callable dependency arguments");
            }
            dependency->callable_type_arg_count = callable_arg_count;
            for (arg_index = 0U; arg_index < callable_arg_count; ++arg_index) {
                const unsigned char *arg_record =
                    tseq_base +
                    (size_t)(callable_arg_start + arg_index) *
                        sizeof(FengSymbolFtTseqRecord);
                uint32_t type_id = read_u32_le(arg_record + 0x04);

                dependency->callable_type_args[arg_index] =
                    parse_type_by_id(ctx, type_id, path, out_error);
                if (type_id != 0U &&
                    dependency->callable_type_args[arg_index] == NULL) {
                    return false;
                }
            }
        }
    }
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
        !parse_docs(&ctx, source_name, out_error) ||
        !attach_decl_hierarchy(&ctx, source_name, out_error) ||
        !parse_module_segments(&ctx, source_name, out_error) ||
        !parse_attrs(&ctx, source_name, out_error) ||
        !parse_callable_dependencies(&ctx, source_name, out_error) ||
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
