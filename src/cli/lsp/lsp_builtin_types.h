/* lsp_builtin_types.h - LSP builtin type completion and hover data.
 *
 * This file declares the builtin type and type alias sets used by the LSP
 * completion and hover engines.  The engines iterate the tables without
 * hard-coding any type text in C logic.
 *
 * The label strings intentionally duplicate the compiler internal type table
 * (codegen.c k_builtin_types / runtime.c builtin_name_for_type_identifier):
 * the two tables serve different purposes (code generation / semantic analysis
 * vs. LSP completion and hover) and are kept independent on purpose. */

#ifndef FENG_LSP_BUILTIN_TYPES_H
#define FENG_LSP_BUILTIN_TYPES_H

#include <stddef.h>

/* Single builtin type completion/hover item. */
typedef struct {
    const char *label;  /* type name, e.g. "i32" */
    const char *detail; /* human-readable description */
} LspBuiltinTypeItem;

/* Single type alias completion/hover item. */
typedef struct {
    const char *label;     /* alias name, e.g. "int" */
    const char *canonical; /* target type name, e.g. "i32" */
    const char *detail;    /* human-readable description */
} LspBuiltinTypeAliasItem;

/* Builtin type table (12 items). */
static const LspBuiltinTypeItem BUILTIN_TYPES[] = {
    { "bool",   "boolean type" },
    { "i8",     "8-bit signed integer" },
    { "i16",    "16-bit signed integer" },
    { "i32",    "32-bit signed integer" },
    { "i64",    "64-bit signed integer" },
    { "u8",     "8-bit unsigned integer" },
    { "u16",    "16-bit unsigned integer" },
    { "u32",    "32-bit unsigned integer" },
    { "u64",    "64-bit unsigned integer" },
    { "f32",    "32-bit floating point" },
    { "f64",    "64-bit floating point" },
    { "string", "string type" },
};

static const size_t BUILTIN_TYPE_COUNT =
    sizeof(BUILTIN_TYPES) / sizeof(BUILTIN_TYPES[0]);

/* Builtin type alias table (5 items).
 * int is platform-dependent (i32 on 32-bit, i64 on 64-bit platforms).
 * long, byte, float, double are fixed aliases. */
static const LspBuiltinTypeAliasItem BUILTIN_TYPE_ALIASES[] = {
    { "int",    "i32", "platform-dependent integer alias (i32 or i64)" },
    { "long",   "i64", "alias for i64" },
    { "byte",   "u8",  "alias for u8" },
    { "float",  "f32", "alias for f32" },
    { "double", "f64", "alias for f64" },
};

static const size_t BUILTIN_TYPE_ALIAS_COUNT =
    sizeof(BUILTIN_TYPE_ALIASES) / sizeof(BUILTIN_TYPE_ALIASES[0]);

#endif /* FENG_LSP_BUILTIN_TYPES_H */
