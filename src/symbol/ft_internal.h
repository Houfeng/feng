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

#define FENG_SYMBOL_FT_SEC_STRS 0x0001U
#define FENG_SYMBOL_FT_SEC_SYMS 0x0002U
#define FENG_SYMBOL_FT_SEC_TYPS 0x0003U
#define FENG_SYMBOL_FT_SEC_SIGS 0x0004U
#define FENG_SYMBOL_FT_SEC_PRMS 0x0005U
#define FENG_SYMBOL_FT_SEC_RELS 0x0006U
#define FENG_SYMBOL_FT_SEC_DOCS 0x0007U
#define FENG_SYMBOL_FT_SEC_ATTRS 0x0008U
#define FENG_SYMBOL_FT_SEC_SPNS 0x0010U

#define FENG_SYMBOL_FT_SEC_FLAG_REQUIRED 0x0001U
#define FENG_SYMBOL_FT_SEC_FLAG_FIXED_ENTRY 0x0002U
#define FENG_SYMBOL_FT_SEC_FLAG_SORTED 0x0004U
#define FENG_SYMBOL_FT_SEC_FLAG_WORKSPACE_ONLY 0x0008U
#define FENG_SYMBOL_FT_SEC_FLAG_IGNORABLE 0x0010U

#define FENG_SYMBOL_FT_SYM_FLAG_PUBLIC 0x0001U
#define FENG_SYMBOL_FT_SYM_FLAG_MUTABLE 0x0002U
#define FENG_SYMBOL_FT_SYM_FLAG_FIXED 0x0004U
#define FENG_SYMBOL_FT_SYM_FLAG_EXTERN 0x0008U
#define FENG_SYMBOL_FT_SYM_FLAG_BOUNDED_DECL 0x0010U
#define FENG_SYMBOL_FT_SYM_FLAG_HAS_DOC 0x0020U

#define FENG_SYMBOL_FT_SYM_KIND_MODULE 1U
#define FENG_SYMBOL_FT_SYM_KIND_TYPE 2U
#define FENG_SYMBOL_FT_SYM_KIND_SPEC 3U
#define FENG_SYMBOL_FT_SYM_KIND_FIT 4U
#define FENG_SYMBOL_FT_SYM_KIND_TOP_FN 5U
#define FENG_SYMBOL_FT_SYM_KIND_EXTERN_FN 6U
#define FENG_SYMBOL_FT_SYM_KIND_CTOR 7U
#define FENG_SYMBOL_FT_SYM_KIND_DTOR 8U
#define FENG_SYMBOL_FT_SYM_KIND_FIELD 9U
#define FENG_SYMBOL_FT_SYM_KIND_METHOD 10U
#define FENG_SYMBOL_FT_SYM_KIND_TOP_LET 11U
#define FENG_SYMBOL_FT_SYM_KIND_TOP_VAR 12U

#define FENG_SYMBOL_FT_TYPE_KIND_BUILTIN 1U
#define FENG_SYMBOL_FT_TYPE_KIND_NAMED 2U
#define FENG_SYMBOL_FT_TYPE_KIND_ARRAY 3U
#define FENG_SYMBOL_FT_TYPE_KIND_C_POINTER 4U

#define FENG_SYMBOL_FT_PARAM_FLAG_MUTABLE 0x0001U

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

typedef struct FengSymbolFtSymRecord {
    uint32_t id;
    uint32_t owner_id;
    uint32_t name_str;
    uint16_t kind;
    uint16_t flags;
    uint32_t type_ref;
    uint32_t sig_ref;
    uint32_t extra_ref;
    uint32_t doc_ref;
} FengSymbolFtSymRecord;

typedef struct FengSymbolFtTypeRecord {
    uint16_t kind;
    uint16_t reserved0;
    uint32_t string_ref;
    uint32_t inner_type_id;
    uint32_t aux;
    uint32_t aux2;
    uint32_t aux3;
} FengSymbolFtTypeRecord;

typedef struct FengSymbolFtSigRecord {
    uint32_t return_type_id;
    uint32_t first_param_index;
    uint32_t param_count;
    uint16_t call_conv;
    uint16_t reserved0;
    uint32_t abi_library_str;
} FengSymbolFtSigRecord;

typedef struct FengSymbolFtParamRecord {
    uint32_t name_str;
    uint32_t type_id;
    uint16_t flags;
    uint16_t reserved0;
} FengSymbolFtParamRecord;

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

