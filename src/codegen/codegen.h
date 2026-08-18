/* Feng codegen — translates a semantically-validated FengProgram set into a
 * single C translation unit that links against the Feng runtime ABI.
 *
 * Scope (Phase 1A complete):
 *   - module declaration
 *   - extern fn with @cdecl(<lib>[, <c-name>[, <fixed-count>]]) /
 *     @stdcall(<lib>[, <c-name>[, <fixed-count>]]) /
 *     @fastcall(<lib>[, <c-name>[, <fixed-count>]]) —
 *     emits an `extern` declaration whose surface types follow the C ABI path;
 *     `@stdcall` / `@fastcall` carry host calling-convention markers when the
 *     target toolchain distinguishes them; the optional second annotation
 *     argument selects the imported C symbol name and the optional third
 *     argument declares the C variadic fixed-parameter count
 *   - extern fn with @runtime — reuses the runtime contract header's
 *     declaration and emits ordinary C calls (no ABI bridge / trampoline)
 *   - top-level free fn (including `main(args: string[])` entry)
 *   - top-level let/var (module-level bindings, initialised on startup,
 *     released on shutdown)
 *   - user-defined `type` declarations: fields (let/var), zero-arg default
 *     constructor, instance methods with overload resolution (param-type
 *     mangling + FengResolvedCallable dispatch from semantic analysis)
 *   - free fn / type-method overload sets (selected per call site via
 *     FengResolvedCallable.function_decl / .member)
 *   - statements: block, binding (let/var), assignment (identifier or member),
 *                 expression, return, if/else, if-match, while, break, continue,
 *                 throw, try/catch
 *   - expressions: int/bool/string/float literals, identifier, binary,
 *                  unary, call (free fn / extern fn / method / default ctor),
 *                  member access, array literal, index, numeric cast,
 *                  if-as-expression, if-match-as-expression, .length on
 *                  string/array
 *   - types: i8..u64, f32/f64, bool, string, void, user types, T[]
 *
 * Out of scope (deferred to Phase 1B):
 *   - lambda / closure literals
 *   - for / for-in
 *   - spec / fit dispatch (specs are accepted as metadata only)
 *   - cyclic GC, finaliser resurrection
 *
 * Anything outside this slice is rejected with a clear error so callers know
 * which feature is not yet generated.
 */
#ifndef FENG_CODEGEN_CODEGEN_H
#define FENG_CODEGEN_CODEGEN_H

#include <stdbool.h>
#include <stddef.h>

#include "codegen/mapping.h"
#include "semantic/semantic.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only package symbol-table facts supplied by the outer compilation
 * driver. The implementation and user object remain opaque to core Codegen;
 * callers may adapt an in-memory symbol graph, a persisted table, or a test
 * double without exposing any symbol-module type through this interface. */
typedef struct FengCodegenPackageSymbolQuery {
    const void *user;
    /* Return whether one source declaration belongs to the stable package
     * symbol domain selected for the current compilation output. */
    bool (*contains_source_node)(const void *user, const void *source_node);
} FengCodegenPackageSymbolQuery;

typedef struct FengCodegenOptions {
    bool emit_line_directives;   /* if true, emit #line for source mapping */
    const FengCodegenMapingSourceMapping *debug_source_mappings;
    size_t debug_source_mapping_count;
    /* Borrowed for the duration of feng_codegen_emit_program(). Required by
     * provider-library emission when private compiler dependencies need a
     * stable cross-package symbol identity. */
    const FengCodegenPackageSymbolQuery *package_symbols;
} FengCodegenOptions;

typedef struct FengCodegenOutput {
    char  *c_source;             /* malloc'd, NUL-terminated */
    size_t c_source_length;      /* strlen(c_source) */
    FengCodegenMapingInfo debug_info;    /* abstract frame/variable mappings */
} FengCodegenOutput;

typedef struct FengCodegenError {
    FengToken token;             /* offending token (line/col only) */
    const char *code;            /* error code, e.g. "CE0001" */
    char    *message;            /* malloc'd, owned by caller */
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
