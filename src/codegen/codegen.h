/* Feng codegen — translates a semantically-validated FengProgram set into a
 * single C translation unit that links against the Feng runtime ABI.
 *
 * Scope (Phase 1A iteration 1):
 *   - module declaration
 *   - extern fn with @cdecl(<lib>) — emits a plain `extern` declaration whose
 *     C symbol is the Feng function name verbatim
 *   - top-level free fn (including `main(args: string[])` entry)
 *   - statements: block, binding (let/var), expression, return, if/else,
 *                 while, break, continue
 *   - expressions: int/bool/string/float literals, identifier, binary,
 *                  unary, call (free fn / extern fn), numeric cast
 *   - types: i8..u64, f32/f64, bool, string, void, T[] (parameter only)
 *
 * Anything outside this slice is rejected with a clear error so callers know
 * which feature is not yet generated. Subsequent iterations extend the slice.
 */
#ifndef FENG_CODEGEN_CODEGEN_H
#define FENG_CODEGEN_CODEGEN_H

#include <stdbool.h>
#include <stddef.h>

#include "semantic/semantic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengCodegenOptions {
    bool emit_line_directives;   /* if true, emit #line for source mapping */
} FengCodegenOptions;

typedef struct FengCodegenOutput {
    char  *c_source;             /* malloc'd, NUL-terminated */
    size_t c_source_length;      /* strlen(c_source) */
} FengCodegenOutput;

typedef struct FengCodegenError {
    char    *message;            /* malloc'd, owned by caller */
    FengToken token;             /* offending token (line/col only) */
    const char *path;            /* borrowed, source path */
} FengCodegenError;

bool feng_codegen_emit_program(const FengSemanticAnalysis *analysis,
                               FengCompileTarget target,
                               const FengCodegenOptions *options,
                               FengCodegenOutput *out_output,
                               FengCodegenError *out_error);

void feng_codegen_output_free(FengCodegenOutput *output);
void feng_codegen_error_free(FengCodegenError *error);

#ifdef __cplusplus
}
#endif

#endif
