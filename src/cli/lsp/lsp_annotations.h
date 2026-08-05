/* lsp_annotations.h - LSP annotation completion data.
 *
 * This file declares the builtin annotation set used by the LSP completion
 * engine.  The engine iterates the table without hard-coding any annotation
 * text in C logic.
 *
 * The label strings intentionally duplicate the compiler internal annotation
 * table (feng_builtin_annotations()): the two tables serve different purposes
 * (semantic analysis vs. LSP completion) and are kept independent on purpose. */

#ifndef FENG_LSP_ANNOTATIONS_H
#define FENG_LSP_ANNOTATIONS_H

#include <stddef.h>

/* Single annotation completion item. */
typedef struct {
    const char *label;   /* annotation text, e.g. "abi" */
    const char *detail;  /* human-readable description, e.g. "ABI annotation" */
} LspAnnotationItem;

/* Builtin annotation table (9 items). */
static const LspAnnotationItem BUILTIN_ANNOTATIONS[] = {
    { "abi",      "ABI annotation" },
    { "cdecl",    "C calling convention" },
    { "stdcall",  "StdCall calling convention" },
    { "fastcall", "FastCall calling convention" },
    { "runtime",  "runtime annotation" },
    { "iterable", "iterable annotation" },
    { "iterator", "iterator annotation" },
    { "value",    "value type annotation" },
    { "mixable",  "mixable static method annotation" },
};

static const size_t BUILTIN_ANNOTATION_COUNT =
    sizeof(BUILTIN_ANNOTATIONS) / sizeof(BUILTIN_ANNOTATIONS[0]);

#endif /* FENG_LSP_ANNOTATIONS_H */
