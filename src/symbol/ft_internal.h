#ifndef FENG_SYMBOL_FT_INTERNAL_H
#define FENG_SYMBOL_FT_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "symbol/ft.h"
#include "symbol/internal.h"

#define FENG_SYMBOL_FT_MAGIC_0 'F'
#define FENG_SYMBOL_FT_MAGIC_1 'S'
#define FENG_SYMBOL_FT_MAGIC_2 'T'
#define FENG_SYMBOL_FT_MAGIC_3 '1'

#define FENG_SYMBOL_FT_BYTE_ORDER_LE 0x01U
#define FENG_SYMBOL_FT_VERSION_MAJOR 0x01U
#define FENG_SYMBOL_FT_VERSION_MINOR 0x00U

#define FENG_SYMBOL_FT_HEADER_SIZE 64U
#define FENG_SYMBOL_FT_SECTION_ENTRY_SIZE 32U

#define FENG_SYMBOL_FT_FLAG_HAS_DOCS 0x00000001U
#define FENG_SYMBOL_FT_FLAG_HAS_SPANS 0x00000002U
#define FENG_SYMBOL_FT_FLAG_HAS_USES 0x00000004U
#define FENG_SYMBOL_FT_FLAG_HAS_META 0x00000008U
#define FENG_SYMBOL_FT_FLAG_HAS_ATTRS 0x00000010U

#define FENG_SYMBOL_FT_SEC_STRS  0x0001U
#define FENG_SYMBOL_FT_SEC_SYMS  0x0002U
#define FENG_SYMBOL_FT_SEC_TYPS  0x0003U
#define FENG_SYMBOL_FT_SEC_TSEQ  0x0004U
#define FENG_SYMBOL_FT_SEC_RELS  0x0005U
#define FENG_SYMBOL_FT_SEC_DOCS  0x0006U
#define FENG_SYMBOL_FT_SEC_ATTRS 0x0007U
#define FENG_SYMBOL_FT_SEC_SPNS  0x0010U

#define FENG_SYMBOL_FT_SEC_FLAG_REQUIRED 0x0001U
#define FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY 0x0002U
#define FENG_SYMBOL_FT_SEC_FLAG_SORTED 0x0004U
#define FENG_SYMBOL_FT_SEC_FLAG_WORKSPACE_ONLY 0x0008U
#define FENG_SYMBOL_FT_SEC_FLAG_IGNORABLE 0x0010U

#define FENG_SYMBOL_FT_SYM_FLAG_PUBLIC 0x0001U
#define FENG_SYMBOL_FT_SYM_FLAG_MUTABLE 0x0002U
#define FENG_SYMBOL_FT_SYM_FLAG_ABI 0x0004U
#define FENG_SYMBOL_FT_SYM_FLAG_EXTERN 0x0008U
#define FENG_SYMBOL_FT_SYM_FLAG_BOUNDED_DECL 0x0010U
#define FENG_SYMBOL_FT_SYM_FLAG_HAS_DOC 0x0020U

#define FENG_SYMBOL_FT_SYM_KIND_MODULE     1U
#define FENG_SYMBOL_FT_SYM_KIND_TYPE       2U
#define FENG_SYMBOL_FT_SYM_KIND_SPEC       3U
#define FENG_SYMBOL_FT_SYM_KIND_FIT        4U
#define FENG_SYMBOL_FT_SYM_KIND_TOP_FN     5U
#define FENG_SYMBOL_FT_SYM_KIND_EXTERN_FN  6U
#define FENG_SYMBOL_FT_SYM_KIND_CTOR       7U
#define FENG_SYMBOL_FT_SYM_KIND_DTOR       8U
#define FENG_SYMBOL_FT_SYM_KIND_FIELD      9U
#define FENG_SYMBOL_FT_SYM_KIND_METHOD     10U
#define FENG_SYMBOL_FT_SYM_KIND_TOP_LET    11U
#define FENG_SYMBOL_FT_SYM_KIND_TOP_VAR    12U
#define FENG_SYMBOL_FT_SYM_KIND_ENUM       13U
#define FENG_SYMBOL_FT_SYM_KIND_ENUM_ITEM  14U
#define FENG_SYMBOL_FT_SYM_KIND_TYPE_PARAM 15U

#define FENG_SYMBOL_FT_TYPE_KIND_BUILTIN       1U
#define FENG_SYMBOL_FT_TYPE_KIND_NAMED         2U
#define FENG_SYMBOL_FT_TYPE_KIND_ARRAY         3U
#define FENG_SYMBOL_FT_TYPE_KIND_C_POINTER     4U
#define FENG_SYMBOL_FT_TYPE_KIND_TYPE_PARAM_REF 5U
#define FENG_SYMBOL_FT_TYPE_KIND_NAMED_GENERIC  6U
#define FENG_SYMBOL_FT_TYPE_KIND_CALLABLE       7U
#define FENG_SYMBOL_FT_TYPE_KIND_SPEC_OBJECT    8U
#define FENG_SYMBOL_FT_TYPE_KIND_SPEC_CALLABLE  9U

/* TSEQ element flags: bit 0 = mutable (var) parameter, bit 1 = variadic T... parameter */
#define FENG_SYMBOL_FT_TSEQ_FLAG_VAR 0x0001U
#define FENG_SYMBOL_FT_TSEQ_FLAG_VARIADIC 0x0002U

typedef struct FengSymbolFtHeader {
    uint8_t magic[4];
    uint8_t byte_order;
    uint8_t major;
    uint8_t minor;
    uint8_t profile;
    uint16_t header_size;
    uint16_t section_entry_size;
    uint16_t section_count;
    uint16_t reserved0;
    uint32_t flags;
    uint32_t root_symbol_id;
    uint64_t section_dir_offset;
    uint64_t payload_offset;
    uint64_t content_fingerprint;
    uint64_t dependency_fingerprint;
    uint64_t reserved1;
} FengSymbolFtHeader;

typedef struct FengSymbolFtSectionEntry {
    uint16_t kind;
    uint16_t flags;
    uint32_t count;
    uint64_t offset;
    uint64_t size;
    uint32_t entry_size;
    uint32_t reserved;
} FengSymbolFtSectionEntry;

/* SYMS record: 28 bytes.  type_ref for callable kinds points to a
 * CALLABLE/SPEC_OBJECT/SPEC_CALLABLE TYPS node; for binding/field it
 * points to the value TYPS node; 0 otherwise. */
typedef struct FengSymbolFtSymRecord {
    uint32_t id;        /* 1-based symbol id */
    uint32_t owner_id;  /* parent symbol id, 0 for module root */
    uint32_t name_str;  /* string id of the declaration name */
    uint16_t kind;      /* FT_SYM_KIND_* */
    uint16_t flags;     /* FT_SYM_FLAG_* */
    uint32_t type_ref;  /* TYPS.id (callable: CALLABLE node; binding/field: value type) */
    uint32_t extra_ref; /* module: full-name string id; fit: target TYPS.id; else 0 */
    uint32_t doc_ref;   /* DOC record id, 0 if none */
} FengSymbolFtSymRecord;

/* TYPS record: 24 bytes.
 * Field usage by kind:
 *   BUILTIN:       string_ref=type-name, others=0
 *   NAMED:         string_ref=dot-joined-name, sym_ref=decl-sym-id (0 if unknown), others=0
 *   ARRAY:         string_ref=mutability-bitmap(u32,bit-i=layer-i-writable),
 *                  elem_start=element-TYPS.id, elem_count=rank, sym_ref/reserved1=0
 *   C_POINTER:     elem_start=inner-TYPS.id, others=0
 *   CALLABLE:      elem_start=TSEQ-start(0-based), elem_count=param_count+1, others=0
 *   SPEC_OBJECT:   sym_ref=spec-sym-id, others=0
 *   SPEC_CALLABLE: sym_ref=spec-sym-id, elem_start=TSEQ-start, elem_count=param_count+1
 *   NAMED_GENERIC: string_ref=base-name, sym_ref=decl-sym-id,
 *                  elem_start=TSEQ-start, elem_count=type-arg-count
 *   TYPE_PARAM_REF:string_ref=param-name, sym_ref=type-param-sym-id, others=0 */
typedef struct FengSymbolFtTypeRecord {
    uint16_t kind;        /* FT_TYPE_KIND_* */
    uint16_t reserved0;   /* must be 0 */
    uint32_t string_ref;  /* string id or bitmap (see above) */
    uint32_t sym_ref;     /* sym id reference (see above) */
    uint32_t elem_start;  /* TSEQ start index (0-based) or element TYPS.id */
    uint32_t elem_count;  /* element count or rank */
    uint32_t reserved1;   /* must be 0 */
} FengSymbolFtTypeRecord;

/* TSEQ record: 12 bytes.  Callable parameter list followed by the return
 * type slot (name_str=0, flags=0).  Shared by all CALLABLE and
 * SPEC_CALLABLE TYPS nodes. */
typedef struct FengSymbolFtTseqRecord {
    uint32_t name_str;  /* param name string id; 0 for the return-type slot */
    uint32_t type_id;   /* TYPS.id of the element type; 0 = void/none */
    uint16_t flags;     /* FT_TSEQ_FLAG_*; 0 for the return slot */
    uint16_t reserved0; /* must be 0 */
} FengSymbolFtTseqRecord;

typedef struct FengSymbolFtRelRecord {
    uint16_t kind;
    uint16_t reserved0;
    uint32_t left_symbol_id;
    uint32_t right_symbol_id;
    uint32_t owner_symbol_id;
} FengSymbolFtRelRecord;

typedef struct FengSymbolFtDocRecord {
    uint32_t id;
    uint32_t symbol_id;
    uint32_t doc_str_id;
} FengSymbolFtDocRecord;

typedef struct FengSymbolFtAttrRecord {
    uint32_t symbol_id;
    uint16_t kind;
    uint16_t reserved0;
    uint32_t value0;
    uint32_t value1;
    uint32_t value2;
} FengSymbolFtAttrRecord;

typedef struct FengSymbolFtSpanRecord {
    uint32_t symbol_id;
    uint32_t path_str;
    uint32_t start_line;
    uint32_t start_column;
    uint32_t end_line;
    uint32_t end_column;
} FengSymbolFtSpanRecord;

bool feng_symbol_ft_write_module_internal(const FengSymbolModuleGraph *module,
                                          FengSymbolProfile profile,
                                          const char *path,
                                          FengSymbolError *out_error);

bool feng_symbol_ft_read_file_internal(const char *path,
                                       const FengSymbolFtReadOptions *options,
                                       FengSymbolGraph **out_graph,
                                       FengSymbolError *out_error);

bool feng_symbol_ft_read_bytes_internal(const void *data,
                                        size_t length,
                                        const char *source_name,
                                        const FengSymbolFtReadOptions *options,
                                        FengSymbolGraph **out_graph,
                                        FengSymbolError *out_error);

#endif /* FENG_SYMBOL_FT_INTERNAL_H */

