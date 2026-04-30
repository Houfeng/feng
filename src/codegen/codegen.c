/* Feng codegen implementation. See codegen.h for the supported feature slice.
 *
 * Production-grade rules followed:
 *   - All I/O strings are heap-allocated and freed; no leaks on the success path.
 *   - All emit_* paths propagate errors via the codegen context; once an error
 *     is set, subsequent emits become no-ops and the entry returns false.
 *   - Generated C is portable C11; no compiler-specific extensions are emitted.
 *   - String literal allocations are deduplicated per literal site via a
 *     file-static FengString* slot, so each literal allocates exactly once.
 *   - Strings are managed objects; managed locals are released on scope exit
 *     (incl. returns and break/continue paths via cleanup-frame walking).
 */
#include "codegen/codegen.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer/token.h"
#include "parser/parser.h"

/* ===================== string buffer ===================== */

typedef struct Buf {
    char  *data;
    size_t length;
    size_t capacity;
} Buf;

static void buf_init(Buf *b) {
    b->data = NULL;
    b->length = 0;
    b->capacity = 0;
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->length = 0;
    b->capacity = 0;
}

static bool buf_reserve(Buf *b, size_t extra) {
    size_t need = b->length + extra + 1;
    if (need <= b->capacity) return true;
    size_t cap = b->capacity ? b->capacity : 64;
    while (cap < need) cap *= 2;
    char *p = realloc(b->data, cap);
    if (!p) return false;
    b->data = p;
    b->capacity = cap;
    return true;
}

static void buf_append(Buf *b, const char *s, size_t n) {
    if (!buf_reserve(b, n)) return;
    memcpy(b->data + b->length, s, n);
    b->length += n;
    b->data[b->length] = '\0';
}

static void buf_append_cstr(Buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

static void buf_append_fmt(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    if (!buf_reserve(b, (size_t)n)) { va_end(ap2); return; }
    vsnprintf(b->data + b->length, b->capacity - b->length, fmt, ap2);
    b->length += (size_t)n;
    va_end(ap2);
}

/* ===================== type kinds ===================== */

typedef enum CGTypeKind {
    CG_TYPE_UNKNOWN = 0,
    CG_TYPE_VOID,
    CG_TYPE_BOOL,
    CG_TYPE_I8, CG_TYPE_I16, CG_TYPE_I32, CG_TYPE_I64,
    CG_TYPE_U8, CG_TYPE_U16, CG_TYPE_U32, CG_TYPE_U64,
    CG_TYPE_F32, CG_TYPE_F64,
    CG_TYPE_STRING,
    CG_TYPE_ARRAY,        /* element kind held separately when needed */
    CG_TYPE_OBJECT,       /* user-defined type — Phase 1A iter 2 */
    CG_TYPE_SPEC          /* fat object-form spec value (Step 4b — value model) */
} CGTypeKind;

struct UserType;     /* forward */
struct UserSpec;     /* forward (Step 4b) */

typedef struct CGType {
    CGTypeKind kind;
    /* For arrays: element CGType (heap-owned). NULL otherwise. */
    struct CGType *element;
    /* For OBJECT: borrowed pointer to the registered UserType. NULL otherwise.
     * The UserType is owned by the CG context and outlives every CGType. */
    const struct UserType *user;
    /* For SPEC (Step 4b): borrowed pointer to the registered UserSpec. NULL
     * otherwise. The UserSpec is owned by the CG context and outlives every
     * CGType. */
    const struct UserSpec *user_spec;
} CGType;

static CGType *cgtype_new(CGTypeKind k) {
    CGType *t = calloc(1, sizeof *t);
    if (t) t->kind = k;
    return t;
}

static void cgtype_free(CGType *t) {
    if (!t) return;
    cgtype_free(t->element);
    free(t);
}

static CGType *cgtype_clone(const CGType *t) {
    if (!t) return NULL;
    CGType *c = cgtype_new(t->kind);
    if (!c) return NULL;
    c->element = cgtype_clone(t->element);
    c->user = t->user;
    c->user_spec = t->user_spec;
    return c;
}

/* Value-kind classifier for codegen-side dispatch on per-field lifetime
 * emission. Mirrors the runtime three-way classification described in
 * dev/feng-value-model-delivered.md §3.3 / §7.2:
 *
 *   CG_VK_TRIVIAL          — bit-copyable, no participation in ARC.
 *   CG_VK_MANAGED_POINTER  — single managed pointer (string / array / object).
 *   CG_VK_AGGREGATE        — by-value compound carrying one or more
 *                            FENG_SLOT_POINTER slots inside (e.g., the fat
 *                            spec value layout). No type currently classifies
 *                            here; the case is reserved for the value-model
 *                            fat-spec implementation (Step 4b) and is the
 *                            single dispatch point §7.2 mandates.
 *
 * `cgtype_is_managed` is preserved as a thin wrapper around this classifier
 * so the many ARC-emit call sites continue to read naturally; new code that
 * needs to branch by kind should call cgtype_value_kind directly. */
typedef enum CGValueKind {
    CG_VK_TRIVIAL = 0,
    CG_VK_MANAGED_POINTER,
    CG_VK_AGGREGATE
} CGValueKind;

static CGValueKind cgtype_value_kind(const CGType *t) {
    if (!t) return CG_VK_TRIVIAL;
    switch (t->kind) {
        case CG_TYPE_STRING:
        case CG_TYPE_ARRAY:
        case CG_TYPE_OBJECT:
            return CG_VK_MANAGED_POINTER;
        case CG_TYPE_SPEC:
            return CG_VK_AGGREGATE;
        default:
            return CG_VK_TRIVIAL;
    }
}

static bool cgtype_is_managed(const CGType *t) {
    return cgtype_value_kind(t) == CG_VK_MANAGED_POINTER;
}

static bool cgtype_is_aggregate(const CGType *t) {
    return cgtype_value_kind(t) == CG_VK_AGGREGATE;
}

static bool cgtype_is_integer(CGTypeKind k) {
    return k == CG_TYPE_I8 || k == CG_TYPE_I16 || k == CG_TYPE_I32 || k == CG_TYPE_I64 ||
           k == CG_TYPE_U8 || k == CG_TYPE_U16 || k == CG_TYPE_U32 || k == CG_TYPE_U64;
}

static bool cgtype_is_signed(CGTypeKind k) {
    return k == CG_TYPE_I8 || k == CG_TYPE_I16 || k == CG_TYPE_I32 || k == CG_TYPE_I64;
}

static bool cgtype_is_float(CGTypeKind k) {
    return k == CG_TYPE_F32 || k == CG_TYPE_F64;
}

static bool cgtype_is_numeric(CGTypeKind k) {
    return cgtype_is_integer(k) || cgtype_is_float(k);
}

static int cgtype_int_rank(CGTypeKind k) {
    switch (k) {
        case CG_TYPE_I8: case CG_TYPE_U8:  return 1;
        case CG_TYPE_I16: case CG_TYPE_U16: return 2;
        case CG_TYPE_I32: case CG_TYPE_U32: return 3;
        case CG_TYPE_I64: case CG_TYPE_U64: return 4;
        default: return 0;
    }
}

static const char *cgtype_to_c(CGTypeKind k) {
    switch (k) {
        case CG_TYPE_VOID: return "void";
        case CG_TYPE_BOOL: return "bool";
        case CG_TYPE_I8: return "int8_t";
        case CG_TYPE_I16: return "int16_t";
        case CG_TYPE_I32: return "int32_t";
        case CG_TYPE_I64: return "int64_t";
        case CG_TYPE_U8: return "uint8_t";
        case CG_TYPE_U16: return "uint16_t";
        case CG_TYPE_U32: return "uint32_t";
        case CG_TYPE_U64: return "uint64_t";
        case CG_TYPE_F32: return "float";
        case CG_TYPE_F64: return "double";
        case CG_TYPE_STRING: return "FengString *";
        case CG_TYPE_ARRAY: return "FengArray *";
        case CG_TYPE_OBJECT: return "void *";
        default: return "void";
    }
}

/* When generating user-type pointer references we use `<struct>` *` so the
 * field accesses are direct member loads. The generic `void *` form above is
 * used only for fall-back / unknown user types, which never reach the body.
 */
typedef struct UserField {
    char   *feng_name;     /* e.g., "name" */
    char   *c_name;        /* sanitised */
    CGType *type;          /* heap-owned */
} UserField;

typedef struct UserMethod {
    char   *feng_name;     /* e.g., "say" */
    char   *c_name;        /* mangled e.g. Feng__feng__examples__User__say */
    CGType *return_type;   /* heap-owned */
    CGType **param_types;  /* heap-owned, length param_count */
    char  **param_names;
    size_t  param_count;
    const FengTypeMember *member;
} UserMethod;

typedef struct UserType {
    char   *feng_name;
    char   *c_struct_name;     /* e.g., Feng__feng__examples__User */
    char   *c_desc_name;       /* e.g., FengTypeDesc__feng__examples__User */
    /* Symbol name of the codegen-emitted release_children callback. NULL
     * when the type has no managed fields (no callback is generated and the
     * descriptor's release_children slot is left NULL). */
    char   *c_release_children_name;
    /* User finalizer (`fn ~T()`). NULL when the type declares none.
     * c_finalizer_name is the symbol of the codegen-emitted thunk that
     * adapts the runtime FengFinalizerFn(void *self) signature to the
     * generated method body; it owns the malloc'd string. */
    const FengTypeMember *finalizer;
    char   *c_finalizer_name;
    /* Symbol name of the codegen-emitted "default zero" factory. This
     * function is only emitted when the type is eligible for default-zero
     * initialisation (no participation in an object-reference cycle). For
     * cyclic types the slot stays NULL and any binding/field that would need
     * a default zero of this type is rejected at semantic time. */
    char   *c_default_zero_name;
    UserField  *fields;
    size_t      field_count;
    UserMethod *methods;
    size_t      method_count;
    const FengDecl *decl;
} UserType;

/* Object-form spec registry (Step 4b — value model fat spec). One entry per
 * `spec` declaration. The runtime layout for a value of spec S is the
 * by-value `struct FengSpecValue__M__S { void *subject; const FengSpecWitness__M__S *witness; }`
 * (no header — see dev/feng-value-model-delivered.md §3.3 / §7.2). subject sits
 * at offset 0 so the existing pointer-cleanup chain reuses on `&value.subject`.
 *
 * Phase 4b-α covers object-form, single-program, in-module spec usage:
 *   - param/return/local of spec type
 *   - concrete object → spec coercion at call-arg position
 *   - spec method dispatch via witness->slot(subject, ...)
 *
 * Callable-form specs, spec fields inside `type`, default zero of spec, and
 * spec equality are deferred to 4b-β / 4b-γ.
 */
typedef struct UserSpecMember {
    char   *feng_name;          /* e.g., "greet" */
    char   *c_field_name;       /* sanitised — slot name in the witness struct */
    enum {
        USM_KIND_FIELD = 0,     /* spec field — getter/setter slots */
        USM_KIND_METHOD         /* spec method — single function-pointer slot */
    } kind;
    /* For FIELD: the field's declared type. For METHOD: the method return type. */
    CGType  *type;
    /* FIELD only: true when the spec declared `var` (mutable). `let` and the
     * default both record false; only `var` triggers a setter slot in the
     * witness struct (see cg_emit_user_spec_definition). */
    bool     is_var;
    /* METHOD only: declared parameter types (excluding the implicit subject). */
    CGType **param_types;
    char   **param_names;       /* informational, mirrors signature */
    size_t   param_count;
    const FengTypeMember *member;
} UserSpecMember;

typedef struct UserSpec {
    char   *feng_name;                /* e.g., "Named" */
    char   *c_value_struct_name;      /* e.g., FengSpecValue__feng__sample__Named */
    char   *c_witness_struct_name;    /* e.g., FengSpecWitness__feng__sample__Named */
    /* Value-model descriptor symbols (see dev/feng-value-model-delivered.md
     * §3, §7.2). For object-form specs the aggregate has exactly one
     * managed slot — the `subject` pointer at offset 0 of the value
     * struct. The descriptor is emitted unconditionally so per-field
     * flatten / release helpers (§7.2 / §7.4) can refer to it once a spec
     * value appears as an object field or array element. */
    char   *c_aggregate_slots_name;    /* e.g., FengSpecAggSlots__M__S */
    char   *c_aggregate_default_name;  /* e.g., FengSpecAggDefault__M__S */
    char   *c_aggregate_init_fn_name;  /* e.g., FengSpecAggInit__M__S */
    char   *c_aggregate_desc_name;     /* e.g., FengSpecAgg__M__S */
    /* Default-zero machinery (dev/feng-spec-codegen-pending.md §6). The
     * hidden subject type is a real managed object emitted purely to
     * back default spec values; the default witness reads / writes its
     * fields and returns per-type defaults from method slots. */
    char   *c_default_subject_struct_name; /* e.g., FengSpecDefault__M__S__Subject */
    char   *c_default_subject_desc_name;   /* e.g., FengSpecDefault__M__S__Subject_desc */
    char   *c_default_subject_new_name;    /* e.g., FengSpecDefault__M__S__new_subject */
    char   *c_default_witness_name;        /* e.g., FengSpecDefaultWitness__M__S */
    UserSpecMember *members;
    size_t          member_count;
    const FengDecl *decl;
} UserSpec;

/* `fit T :: S { ... }` registry (Step 4b-γ). One entry per fit declaration.
 * Codegen treats fit-body methods as if they were ordinary methods on T,
 * but mangles their C symbols with the fit's per-module index so two fits
 * for the same target T cannot collide. The witness emitter (FIT_METHOD
 * source kind) looks up the matching UserMethod by its source `member`
 * pointer to recover the symbol to call. */
typedef struct UserFit {
    const FengDecl *decl;
    const UserType *target;       /* resolved target T (named single-segment) */
    UserMethod     *methods;      /* fit-body methods, c_name mangled with index */
    size_t          method_count;
    size_t          index;        /* per-program 0-based declaration order */
    char           *c_prefix;     /* "FengFit_<idx>__<modT>__<T>" */
} UserFit;


static char *cg_user_type_c_name(const CGType *t) __attribute__((unused));
static char *cg_user_type_c_name(const CGType *t) {
    /* Caller owns the returned malloc'd string. */
    if (t->kind != CG_TYPE_OBJECT || !t->user) return strdup("void *");
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "struct %s *", t->user->c_struct_name);
    return b.data;
}

/* Emit a C type spec for a CGType into a buffer. For OBJECT we emit the full
 * struct pointer form so the generated C does not rely on `void *` casts.
 * For SPEC (Step 4b) we emit `struct FengSpecValue__M__S` (by-value, no
 * pointer star) — fat-spec values live on the stack / in argument slots and
 * are passed by value. */
static void cg_emit_c_type(Buf *b, const CGType *t) {
    if (t && t->kind == CG_TYPE_OBJECT && t->user) {
        buf_append_fmt(b, "struct %s *", t->user->c_struct_name);
        return;
    }
    if (t && t->kind == CG_TYPE_SPEC && t->user_spec) {
        buf_append_fmt(b, "struct %s", t->user_spec->c_value_struct_name);
        return;
    }
    buf_append_cstr(b, cgtype_to_c(t ? t->kind : CG_TYPE_VOID));
}

/* Heap-allocated form. Caller frees. */
static char *cg_ctype_dup(const CGType *t) {
    Buf b; buf_init(&b);
    cg_emit_c_type(&b, t);
    return b.data ? b.data : strdup("void");
}

/* ===================== local scope ===================== */

typedef struct Local {
    char     *name;     /* Feng identifier */
    char     *c_name;   /* mangled C identifier, unique within the function */
    CGType   *type;
    bool      is_param; /* parameters are not released by the frame (caller owns) */
} Local;

typedef struct Scope {
    struct Scope *parent;
    Local        *items;
    size_t        count;
    size_t        capacity;
    bool          is_loop;       /* this scope is a loop body root */
    /* If non-NULL, `continue` inside this loop emits `goto <continue_label>;`
     * instead of C `continue;`. Used by the three-clause `for` form so a
     * `continue` jumps to the update step (which lives after the body block,
     * before the next iteration of the C `for(;;)`). The string is owned by
     * the codegen instance (lives in cur_body's identifier pool) and freed
     * by the caller that created the scope. */
    const char   *continue_label;
    /* try_depth observed at the moment this scope was pushed. Used by
     * break/continue to refuse jumping across an enclosing try frame in
     * Phase 1A (where stack-based exception cleanup is not yet wired). */
    int           try_depth_at_entry;
    /* Each scope frame also holds a list of indices into items[] in original
     * insertion order; release-on-exit walks them in reverse. */
} Scope;

static Scope *scope_push(Scope *parent) {
    Scope *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->parent = parent;
    s->try_depth_at_entry = parent ? parent->try_depth_at_entry : 0;
    return s;
}

static void scope_pop_free(Scope *s) {
    if (!s) return;
    for (size_t i = 0; i < s->count; i++) {
        free(s->items[i].name);
        free(s->items[i].c_name);
        cgtype_free(s->items[i].type);
    }
    free(s->items);
    free(s);
}

static bool scope_add(Scope *s, const char *name, const char *c_name,
                      CGType *type, bool is_param) {
    if (s->count + 1 > s->capacity) {
        size_t cap = s->capacity ? s->capacity * 2 : 8;
        Local *p = realloc(s->items, cap * sizeof *p);
        if (!p) return false;
        s->items = p;
        s->capacity = cap;
    }
    Local *l = &s->items[s->count++];
    l->name = strdup(name);
    l->c_name = strdup(c_name);
    l->type = type;
    l->is_param = is_param;
    return l->name && l->c_name;
}

static const Local *scope_lookup(const Scope *s, const char *name, size_t len) {
    for (const Scope *cur = s; cur; cur = cur->parent) {
        for (size_t i = cur->count; i > 0; i--) {
            const Local *l = &cur->items[i - 1];
            if (strlen(l->name) == len && memcmp(l->name, name, len) == 0) {
                return l;
            }
        }
    }
    return NULL;
}

/* ===================== codegen context ===================== */

typedef struct ExternFn {
    char    *name;          /* Feng name, also C symbol */
    CGType **param_types;
    size_t   param_count;
    CGType  *return_type;
} ExternFn;

typedef struct FreeFn {
    char     *feng_name;
    char     *c_name;
    CGType  **param_types;
    char    **param_names;
    size_t    param_count;
    CGType   *return_type;
    const FengDecl *decl;
} FreeFn;

typedef struct ModuleBinding {
    char    *feng_name;
    char    *c_name;          /* e.g., _feng_g_examples_user — globally unique */
    CGType  *type;            /* heap-owned */
    bool     is_var;          /* false = let, true = var */
    const FengBinding *binding;
} ModuleBinding;

typedef struct CG {
    /* Output sections concatenated at the end. */
    Buf headers;        /* #include / extern decls / forward decls */
    Buf type_defs;      /* struct/enum defs (Phase 1A iter 2) */
    Buf statics;        /* static caches: literal slots */
    Buf fn_protos;      /* forward declarations for free fn / methods */
    Buf fn_defs;        /* function bodies */
    Buf witness_defs;   /* spec witness thunks + tables (Step 4b);
                           kept separate so emission from inside an
                           in-progress function body cannot splice into
                           that function's text. Concatenated after
                           fn_defs in cg_finalize. */

    /* Per-function emission state. */
    Buf      *cur_body; /* current function body buffer */
    Scope    *cur_scope;
    int       tmp_counter;
    int       label_counter;
    int       loop_depth;
    bool      in_loop_with_break;
    CGType   *cur_return_type;
    bool      cur_fn_is_main;

    /* Module info. */
    char     *module_mangle;     /* e.g., "feng__examples" */
    char     *module_dot_name;   /* e.g., "feng.examples" */

    /* Symbol tables. */
    ExternFn *externs;
    size_t    extern_count;
    size_t    extern_capacity;
    FreeFn   *free_fns;
    size_t    free_fn_count;
    size_t    free_fn_capacity;
    UserType *user_types;
    size_t    user_type_count;
    size_t    user_type_capacity;
    /* Object-form spec registry (Step 4b). Sibling of user_types. Each entry
     * holds the fat-spec value layout, witness-table layout, and member
     * descriptors used by codegen to emit coercions, dispatch, and
     * lifecycle for spec-typed values. */
    struct UserSpec *user_specs;
    size_t           user_spec_count;
    size_t           user_spec_capacity;
    /* Fit registry (Step 4b-γ). Sibling of user_specs. Each entry models
     * one `fit T :: S { ... }` so witness emission for FIT_METHOD source
     * can look up the C symbol of the fit-body method to call. */
    struct UserFit  *user_fits;
    size_t           user_fit_count;
    size_t           user_fit_capacity;
    /* Witness-table emission cache (Step 4b): one entry per (type, spec) pair
     * for which a witness instance has been emitted into fn_protos/fn_defs.
     * Avoids duplicate emission across multiple coercion sites. */
    struct {
        const struct UserType *t;
        const struct UserSpec *s;
        char *c_var;     /* e.g., "Witness__feng__sample__User__as__feng__sample__Named" */
    } *witness_tables;
    size_t witness_table_count;
    size_t witness_table_capacity;
    ModuleBinding *module_bindings;
    size_t         module_binding_count;
    size_t         module_binding_capacity;

    /* try/catch state: how many active try frames are open in the current
     * function. Used to refuse return / break / continue across them in 1A. */
    int       try_depth;

    /* String literal cache: each unique literal (by content) gets one static. */
    struct {
        char  *content;   /* raw bytes (may include NULs not supported in 1A) */
        size_t length;
        char  *c_var;     /* generated static var name */
    } *string_literals;
    size_t string_literal_count;
    size_t string_literal_capacity;

    /* Borrowed from feng_codegen_emit_program: lets descriptor emission look
     * up Phase 1B cyclicity markers without re-running SCC. */
    const FengSemanticAnalysis *analysis;

    /* Error state. */
    FengCodegenError *error;
    bool failed;
} CG;

/* Forward decls. */
static bool cg_resolve_type(CG *cg, const FengTypeRef *ref, const FengToken *fallback,
                            CGType **out_type);
static bool cg_emit_block(CG *cg, const FengBlock *block);
static bool cg_default_value_expr(CG *cg, const CGType *type,
                                  const FengToken *blame,
                                  char **out_expr);
static bool cg_emit_stmt(CG *cg, const FengStmt *stmt);
typedef struct ExprResult ExprResult;
static bool cg_emit_expr(CG *cg, const FengExpr *expr, ExprResult *out);
static void cg_release_scope(CG *cg, const Scope *scope);
static void cg_emit_cleanup_push_for_managed_local(CG *cg, const char *cname);
static void cg_emit_cleanup_push_for_aggregate_local(CG *cg, const char *cname);
static const char *cg_aggregate_field_desc_name(const CGType *t);
static void cg_emit_user_type_forward(CG *cg, const UserType *t);
static void cg_emit_user_type_definition(CG *cg, UserType *t);
static bool cg_emit_user_method(CG *cg, const UserType *t, const UserMethod *m);
static bool cg_emit_user_finalizer(CG *cg, const UserType *t);
static bool cg_ensure_witness_instance(CG *cg, const UserType *t,
                                       const UserSpec *s, FengToken blame,
                                       const char **out_var);
static size_t cg_field_managed_descriptor_count(CG *cg, const CGType *t,
                                                FengToken blame);
static bool cg_emit_field_managed_descriptors(CG *cg, Buf *td,
                                              const char *struct_name,
                                              const char *field_name,
                                              const CGType *t,
                                              FengToken blame);
static bool cg_emit_field_release(CG *cg, Buf *td, const char *field_name,
                                  const CGType *t, FengToken blame);

/* ===================== error helpers ===================== */

static char *cg_vformat(const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    if (n < 0) { va_end(ap2); return NULL; }
    char *p = malloc((size_t)n + 1);
    if (p) vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

static bool cg_fail(CG *cg, FengToken token, const char *fmt, ...) {
    if (cg->failed) return false;
    cg->failed = true;
    if (cg->error) {
        va_list ap;
        va_start(ap, fmt);
        cg->error->message = cg_vformat(fmt, ap);
        va_end(ap);
        cg->error->token = token;
    }
    return false;
}

/* ===================== mangling ===================== */

/* Replace any non-alphanumeric/underscore byte in `src` with '_' to keep C
 * identifiers valid. Caller frees. */
static char *cg_sanitize(const char *src, size_t len) {
    char *out = malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        out[i] = (isalnum(c) || c == '_') ? (char)c : '_';
    }
    out[len] = '\0';
    return out;
}

static char *cg_module_mangle(const FengSlice *segments, size_t count) {
    Buf b; buf_init(&b);
    for (size_t i = 0; i < count; i++) {
        if (i) buf_append_cstr(&b, "__");
        char *seg = cg_sanitize(segments[i].data, segments[i].length);
        if (!seg) { buf_free(&b); return NULL; }
        buf_append_cstr(&b, seg);
        free(seg);
    }
    return b.data ? b.data : strdup("");
}

static char *cg_module_dot(const FengSlice *segments, size_t count) {
    Buf b; buf_init(&b);
    for (size_t i = 0; i < count; i++) {
        if (i) buf_append_cstr(&b, ".");
        buf_append(&b, segments[i].data, segments[i].length);
    }
    return b.data ? b.data : strdup("");
}

static char *cg_fn_mangle(const char *module_mangle, const FengSlice *name) {
    Buf b; buf_init(&b);
    buf_append_cstr(&b, "feng__");
    buf_append_cstr(&b, module_mangle);
    buf_append_cstr(&b, "__");
    char *n = cg_sanitize(name->data, name->length);
    if (!n) { buf_free(&b); return NULL; }
    buf_append_cstr(&b, n);
    free(n);
    return b.data;
}

/* Encode one CGType into a compact suffix fragment used for overload-aware
 * symbol mangling. The encoding is reversible-by-collision-class only — its
 * single requirement is that two CGTypes that differ in any way produce
 * different fragments. We re-use the runtime-stable c_struct_name /
 * c_value_struct_name for user-defined types so a future ABI rename of those
 * symbols also updates every overload signature consistently. */
static bool cg_encode_type_short(const CGType *t, Buf *out) {
    if (!t) { buf_append_cstr(out, "void"); return true; }
    switch (t->kind) {
        case CG_TYPE_VOID:   buf_append_cstr(out, "void");   return true;
        case CG_TYPE_BOOL:   buf_append_cstr(out, "b");      return true;
        case CG_TYPE_I8:     buf_append_cstr(out, "i8");     return true;
        case CG_TYPE_I16:    buf_append_cstr(out, "i16");    return true;
        case CG_TYPE_I32:    buf_append_cstr(out, "i32");    return true;
        case CG_TYPE_I64:    buf_append_cstr(out, "i64");    return true;
        case CG_TYPE_U8:     buf_append_cstr(out, "u8");     return true;
        case CG_TYPE_U16:    buf_append_cstr(out, "u16");    return true;
        case CG_TYPE_U32:    buf_append_cstr(out, "u32");    return true;
        case CG_TYPE_U64:    buf_append_cstr(out, "u64");    return true;
        case CG_TYPE_F32:    buf_append_cstr(out, "f32");    return true;
        case CG_TYPE_F64:    buf_append_cstr(out, "f64");    return true;
        case CG_TYPE_STRING: buf_append_cstr(out, "s");      return true;
        case CG_TYPE_OBJECT:
            if (!t->user) { buf_append_cstr(out, "O_unknown"); return true; }
            buf_append_cstr(out, "O_");
            buf_append_cstr(out, t->user->c_struct_name);
            return true;
        case CG_TYPE_SPEC:
            if (!t->user_spec) { buf_append_cstr(out, "S_unknown"); return true; }
            buf_append_cstr(out, "S_");
            buf_append_cstr(out, t->user_spec->c_value_struct_name);
            return true;
        case CG_TYPE_ARRAY:
            buf_append_cstr(out, "A_");
            return cg_encode_type_short(t->element, out);
        default:
            buf_append_cstr(out, "X");
            return true;
    }
}

/* Build the "__from__<param-types>" suffix that disambiguates overloads.
 * docs/feng-function.md §5 / docs/feng-type.md §5 require that overloads
 * differ by parameter signature, so encoding only the parameter types is
 * sufficient (return type is not part of the overload key). The suffix is
 * applied unconditionally — even single, unambiguous declarations gain it —
 * so symbol shape stays predictable and deterministic for every callable.
 * No-arg functions encode as `__from__void`. Returns a malloc'd string;
 * caller frees. */
static char *cg_build_param_suffix(CGType *const *param_types, size_t count) {
    Buf b; buf_init(&b);
    buf_append_cstr(&b, "__from__");
    if (count == 0) {
        buf_append_cstr(&b, "void");
    } else {
        for (size_t i = 0; i < count; i++) {
            if (i) buf_append_cstr(&b, "__");
            if (!cg_encode_type_short(param_types[i], &b)) {
                buf_free(&b);
                return NULL;
            }
        }
    }
    return b.data ? b.data : strdup("__from__void");
}

/* Append the param-type suffix to an existing c_name buffer. Frees the old
 * c_name and returns the new heap-owned string. */
static char *cg_append_param_suffix(char *c_name, CGType *const *param_types,
                                    size_t count) {
    char *suffix = cg_build_param_suffix(param_types, count);
    if (!suffix) { free(c_name); return NULL; }
    Buf b; buf_init(&b);
    buf_append_cstr(&b, c_name);
    buf_append_cstr(&b, suffix);
    free(c_name);
    free(suffix);
    return b.data;
}

static char *cg_local_cname(CG *cg, const char *feng_name, size_t len) {
    char *base = cg_sanitize(feng_name, len);
    if (!base) return NULL;
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "_l_%s_%d", base, cg->tmp_counter++);
    free(base);
    return b.data;
}

static char *cg_fresh_temp(CG *cg, const char *prefix) {
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "%s%d", prefix, cg->tmp_counter++);
    return b.data;
}

/* ===================== type resolution ===================== */

typedef struct BuiltinTypeMap {
    const char *name;
    CGTypeKind  kind;
} BuiltinTypeMap;

static const BuiltinTypeMap k_builtin_types[] = {
    {"void",   CG_TYPE_VOID},
    {"bool",   CG_TYPE_BOOL},
    {"i8",     CG_TYPE_I8},   {"i16", CG_TYPE_I16},
    {"i32",    CG_TYPE_I32},  {"i64", CG_TYPE_I64},
    {"u8",     CG_TYPE_U8},   {"u16", CG_TYPE_U16},
    {"u32",    CG_TYPE_U32},  {"u64", CG_TYPE_U64},
    {"int",    CG_TYPE_I32},  {"uint", CG_TYPE_U32},
    {"f32",    CG_TYPE_F32},  {"f64", CG_TYPE_F64},
    {"float",  CG_TYPE_F32},  {"double", CG_TYPE_F64},
    {"string", CG_TYPE_STRING},
};

static const UserType *cg_find_user_type(const CG *cg, const char *name, size_t len) {
    for (size_t i = 0; i < cg->user_type_count; i++) {
        if (strlen(cg->user_types[i].feng_name) == len &&
            memcmp(cg->user_types[i].feng_name, name, len) == 0) {
            return &cg->user_types[i];
        }
    }
    return NULL;
}

static const UserSpec *cg_find_user_spec(const CG *cg, const char *name, size_t len) {
    for (size_t i = 0; i < cg->user_spec_count; i++) {
        if (strlen(cg->user_specs[i].feng_name) == len &&
            memcmp(cg->user_specs[i].feng_name, name, len) == 0) {
            return &cg->user_specs[i];
        }
    }
    return NULL;
}

static const UserType *cg_find_user_type_by_decl(const CG *cg, const FengDecl *decl) {
    for (size_t i = 0; i < cg->user_type_count; i++) {
        if (cg->user_types[i].decl == decl) return &cg->user_types[i];
    }
    return NULL;
}

static const UserSpec *cg_find_user_spec_by_decl(const CG *cg, const FengDecl *decl) {
    for (size_t i = 0; i < cg->user_spec_count; i++) {
        if (cg->user_specs[i].decl == decl) return &cg->user_specs[i];
    }
    return NULL;
}

static const UserFit *cg_find_user_fit_by_decl(const CG *cg, const FengDecl *decl) {
    for (size_t i = 0; i < cg->user_fit_count; i++) {
        if (cg->user_fits[i].decl == decl) return &cg->user_fits[i];
    }
    return NULL;
}

static bool cg_resolve_type(CG *cg, const FengTypeRef *ref, const FengToken *fallback,
                            CGType **out_type) {
    *out_type = NULL;
    if (!ref) {
        *out_type = cgtype_new(CG_TYPE_VOID);
        return *out_type != NULL;
    }
    if (ref->kind == FENG_TYPE_REF_NAMED) {
        if (ref->as.named.segment_count != 1) {
            return cg_fail(cg, ref->token,
                "codegen: qualified type names not supported in Phase 1A");
        }
        const FengSlice *seg = &ref->as.named.segments[0];
        for (size_t i = 0; i < sizeof k_builtin_types / sizeof k_builtin_types[0]; i++) {
            const BuiltinTypeMap *m = &k_builtin_types[i];
            if (strlen(m->name) == seg->length &&
                memcmp(m->name, seg->data, seg->length) == 0) {
                *out_type = cgtype_new(m->kind);
                return *out_type != NULL;
            }
        }
        const UserType *ut = cg_find_user_type(cg, seg->data, seg->length);
        if (ut) {
            CGType *t = cgtype_new(CG_TYPE_OBJECT);
            if (!t) return false;
            t->user = ut;
            *out_type = t;
            return true;
        }
        const UserSpec *us = cg_find_user_spec(cg, seg->data, seg->length);
        if (us) {
            CGType *t = cgtype_new(CG_TYPE_SPEC);
            if (!t) return false;
            t->user_spec = us;
            *out_type = t;
            return true;
        }
        FengToken tk = fallback ? *fallback : ref->token;
        (void)tk;
        return cg_fail(cg, ref->token,
            "codegen: unknown type '%.*s'",
            (int)seg->length, seg->data);
    }
    if (ref->kind == FENG_TYPE_REF_ARRAY) {
        CGType *elem = NULL;
        if (!cg_resolve_type(cg, ref->as.inner, fallback, &elem)) return false;
        CGType *t = cgtype_new(CG_TYPE_ARRAY);
        if (!t) { cgtype_free(elem); return false; }
        t->element = elem;
        *out_type = t;
        return true;
    }
    return cg_fail(cg, ref->token,
        "codegen: pointer types not supported in Phase 1A");
}

/* ===================== string literal cache ===================== */

static const char *cg_string_literal_var(CG *cg, const char *content, size_t len) {
    for (size_t i = 0; i < cg->string_literal_count; i++) {
        if (cg->string_literals[i].length == len &&
            memcmp(cg->string_literals[i].content, content, len) == 0) {
            return cg->string_literals[i].c_var;
        }
    }
    if (cg->string_literal_count + 1 > cg->string_literal_capacity) {
        size_t cap = cg->string_literal_capacity ? cg->string_literal_capacity * 2 : 8;
        void *p = realloc(cg->string_literals, cap * sizeof *cg->string_literals);
        if (!p) return NULL;
        cg->string_literals = p;
        cg->string_literal_capacity = cap;
    }
    char *cv = NULL;
    {
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "_feng_str_lit_%zu", cg->string_literal_count);
        cv = b.data;
    }
    char *contents_copy = malloc(len + 1);
    if (!contents_copy || !cv) { free(cv); free(contents_copy); return NULL; }
    memcpy(contents_copy, content, len);
    contents_copy[len] = '\0';
    cg->string_literals[cg->string_literal_count].content = contents_copy;
    cg->string_literals[cg->string_literal_count].length = len;
    cg->string_literals[cg->string_literal_count].c_var = cv;
    cg->string_literal_count++;
    return cv;
}

/* ===================== symbol tables ===================== */

static bool cg_register_extern(CG *cg, const FengDecl *decl) {
    if (cg->extern_count + 1 > cg->extern_capacity) {
        size_t cap = cg->extern_capacity ? cg->extern_capacity * 2 : 4;
        void *p = realloc(cg->externs, cap * sizeof *cg->externs);
        if (!p) return false;
        cg->externs = p;
        cg->extern_capacity = cap;
    }
    const FengCallableSignature *sig = &decl->as.function_decl;
    ExternFn *ef = &cg->externs[cg->extern_count++];
    ef->name = strndup(sig->name.data, sig->name.length);
    ef->param_count = sig->param_count;
    ef->param_types = sig->param_count ? calloc(sig->param_count, sizeof(CGType*)) : NULL;
    for (size_t i = 0; i < sig->param_count; i++) {
        if (!cg_resolve_type(cg, sig->params[i].type, &sig->params[i].token,
                             &ef->param_types[i])) {
            return false;
        }
    }
    if (!cg_resolve_type(cg, sig->return_type, &sig->token, &ef->return_type)) {
        return false;
    }
    return true;
}

static const ExternFn *cg_find_extern(const CG *cg, const char *name, size_t len) {
    for (size_t i = 0; i < cg->extern_count; i++) {
        if (strlen(cg->externs[i].name) == len &&
            memcmp(cg->externs[i].name, name, len) == 0) {
            return &cg->externs[i];
        }
    }
    return NULL;
}

static bool cg_register_free_fn(CG *cg, const FengDecl *decl) {
    if (cg->free_fn_count + 1 > cg->free_fn_capacity) {
        size_t cap = cg->free_fn_capacity ? cg->free_fn_capacity * 2 : 4;
        void *p = realloc(cg->free_fns, cap * sizeof *cg->free_fns);
        if (!p) return false;
        cg->free_fns = p;
        cg->free_fn_capacity = cap;
    }
    const FengCallableSignature *sig = &decl->as.function_decl;
    FreeFn *f = &cg->free_fns[cg->free_fn_count++];
    f->feng_name = strndup(sig->name.data, sig->name.length);
    f->c_name = cg_fn_mangle(cg->module_mangle, &sig->name);
    f->param_count = sig->param_count;
    f->param_types = sig->param_count ? calloc(sig->param_count, sizeof(CGType*)) : NULL;
    f->param_names = sig->param_count ? calloc(sig->param_count, sizeof(char*)) : NULL;
    for (size_t i = 0; i < sig->param_count; i++) {
        if (!cg_resolve_type(cg, sig->params[i].type, &sig->params[i].token,
                             &f->param_types[i])) {
            return false;
        }
        f->param_names[i] = strndup(sig->params[i].name.data, sig->params[i].name.length);
    }
    if (!cg_resolve_type(cg, sig->return_type, &sig->token, &f->return_type)) {
        return false;
    }
    /* Append param-type suffix for overload-aware mangling. Applied
     * unconditionally so the symbol shape is the same regardless of whether
     * the function is part of an overload set today. */
    f->c_name = cg_append_param_suffix(f->c_name, f->param_types, f->param_count);
    if (!f->c_name) return false;
    f->decl = decl;
    return true;
}

static const FreeFn *cg_find_free_fn(const CG *cg, const char *name, size_t len) {
    for (size_t i = 0; i < cg->free_fn_count; i++) {
        if (strlen(cg->free_fns[i].feng_name) == len &&
            memcmp(cg->free_fns[i].feng_name, name, len) == 0) {
            return &cg->free_fns[i];
        }
    }
    return NULL;
}

/* Overload-aware lookup. Semantic analysis attaches the resolved FengDecl to
 * every call site (FengResolvedCallable.function_decl); when present, codegen
 * uses it to select the exact registered FreeFn rather than relying on
 * by-name lookup which would silently pick the first overload. */
static const FreeFn *cg_find_free_fn_by_decl(const CG *cg, const FengDecl *decl) {
    if (!decl) return NULL;
    for (size_t i = 0; i < cg->free_fn_count; i++) {
        if (cg->free_fns[i].decl == decl) return &cg->free_fns[i];
    }
    return NULL;
}

/* ----- user types ----- */

static bool cg_register_user_type_shell(CG *cg, const FengDecl *decl) {
    if (cg->user_type_count + 1 > cg->user_type_capacity) {
        size_t cap = cg->user_type_capacity ? cg->user_type_capacity * 2 : 4;
        void *p = realloc(cg->user_types, cap * sizeof *cg->user_types);
        if (!p) return false;
        cg->user_types = p;
        cg->user_type_capacity = cap;
    }
    UserType *t = &cg->user_types[cg->user_type_count++];
    memset(t, 0, sizeof *t);
    t->feng_name = strndup(decl->as.type_decl.name.data,
                           decl->as.type_decl.name.length);
    t->decl = decl;

    Buf b; buf_init(&b);
    buf_append_fmt(&b, "Feng__%s__", cg->module_mangle);
    char *san = cg_sanitize(decl->as.type_decl.name.data,
                            decl->as.type_decl.name.length);
    if (!san) { buf_free(&b); return false; }
    buf_append_cstr(&b, san);
    t->c_struct_name = b.data;

    Buf d; buf_init(&d);
    buf_append_fmt(&d, "FengTypeDesc__%s__%s", cg->module_mangle, san);
    t->c_desc_name = d.data;

    /* release_children symbol is materialised lazily in
     * cg_emit_user_type_definition: types without managed fields don't get a
     * function emitted and leave the descriptor slot NULL. */
    t->c_release_children_name = NULL;
    /* default_zero symbol is computed eagerly here so cross-type recursive
     * default-zero calls can use it before the per-type body is emitted.
     * Whether it is actually emitted (and whether it is legal to reference)
     * is decided in cg_emit_user_type_definition / cg_emit_default_value. */
    {
        Buf z; buf_init(&z);
        buf_append_fmt(&z, "%s__default_zero", t->c_struct_name);
        t->c_default_zero_name = z.data;
        if (!t->c_default_zero_name) { free(san); return false; }
    }
    free(san);
    return t->feng_name && t->c_struct_name && t->c_desc_name;
}

static bool cg_register_user_type_members(CG *cg, UserType *t) {
    const FengDecl *decl = t->decl;
    /* First pass: count fields/methods to size arrays. */
    size_t fcount = 0, mcount = 0;
    for (size_t i = 0; i < decl->as.type_decl.member_count; i++) {
        const FengTypeMember *m = decl->as.type_decl.members[i];
        if (m->kind == FENG_TYPE_MEMBER_FIELD) fcount++;
        else if (m->kind == FENG_TYPE_MEMBER_METHOD) mcount++;
        else if (m->kind == FENG_TYPE_MEMBER_CONSTRUCTOR) {
            return cg_fail(cg, m->token,
                "codegen: user-defined constructors not yet supported in Phase 1A");
        }
        else if (m->kind == FENG_TYPE_MEMBER_FINALIZER) {
            if (t->finalizer) {
                return cg_fail(cg, m->token,
                    "codegen: type already declares a finalizer");
            }
            t->finalizer = m;
        }
    }
    if (t->finalizer) {
        Buf fb; buf_init(&fb);
        buf_append_fmt(&fb, "%s__finalize", t->c_struct_name);
        t->c_finalizer_name = fb.data;
        if (!t->c_finalizer_name) return false;
    }
    t->fields = fcount ? calloc(fcount, sizeof *t->fields) : NULL;
    t->methods = mcount ? calloc(mcount, sizeof *t->methods) : NULL;
    if ((fcount && !t->fields) || (mcount && !t->methods)) return false;

    size_t fi = 0, mi = 0;
    for (size_t i = 0; i < decl->as.type_decl.member_count; i++) {
        const FengTypeMember *m = decl->as.type_decl.members[i];
        if (m->kind == FENG_TYPE_MEMBER_FIELD) {
            UserField *uf = &t->fields[fi++];
            uf->feng_name = strndup(m->as.field.name.data, m->as.field.name.length);
            uf->c_name = cg_sanitize(m->as.field.name.data, m->as.field.name.length);
            if (!uf->feng_name || !uf->c_name) return false;
            if (!cg_resolve_type(cg, m->as.field.type, &m->token, &uf->type)) return false;
            if (m->as.field.initializer) {
                return cg_fail(cg, m->token,
                    "codegen: field default initializers not yet supported in Phase 1A");
            }
        } else if (m->kind == FENG_TYPE_MEMBER_METHOD) {
            UserMethod *um = &t->methods[mi++];
            const FengCallableSignature *sig = &m->as.callable;
            um->member = m;
            um->feng_name = strndup(sig->name.data, sig->name.length);
            char *msan = cg_sanitize(sig->name.data, sig->name.length);
            if (!um->feng_name || !msan) { free(msan); return false; }
            Buf cb; buf_init(&cb);
            buf_append_fmt(&cb, "%s__%s", t->c_struct_name, msan);
            um->c_name = cb.data;
            free(msan);
            um->param_count = sig->param_count;
            um->param_types = sig->param_count ? calloc(sig->param_count, sizeof(CGType*)) : NULL;
            um->param_names = sig->param_count ? calloc(sig->param_count, sizeof(char*)) : NULL;
            for (size_t pi = 0; pi < sig->param_count; pi++) {
                if (!cg_resolve_type(cg, sig->params[pi].type, &sig->params[pi].token,
                                     &um->param_types[pi])) return false;
                um->param_names[pi] = strndup(sig->params[pi].name.data,
                                              sig->params[pi].name.length);
            }
            if (!cg_resolve_type(cg, sig->return_type, &sig->token, &um->return_type)) {
                return false;
            }
            um->c_name = cg_append_param_suffix(um->c_name,
                                                um->param_types, um->param_count);
            if (!um->c_name) return false;
        }
    }
    t->field_count = fi;
    t->method_count = mi;
    return true;
}

static const UserField *cg_user_type_field(const UserType *t, const char *name, size_t len) {
    for (size_t i = 0; i < t->field_count; i++) {
        if (strlen(t->fields[i].feng_name) == len &&
            memcmp(t->fields[i].feng_name, name, len) == 0) {
            return &t->fields[i];
        }
    }
    return NULL;
}

static const UserMethod *cg_user_type_method(const UserType *t, const char *name, size_t len) {
    for (size_t i = 0; i < t->method_count; i++) {
        if (strlen(t->methods[i].feng_name) == len &&
            memcmp(t->methods[i].feng_name, name, len) == 0) {
            return &t->methods[i];
        }
    }
    return NULL;
}

/* Overload-aware method lookup: matches by the FengTypeMember pointer that
 * semantic analysis stored on the call site. By-name lookup would silently
 * pick the first overload of `name`. */
static const UserMethod *cg_user_type_method_by_member(const UserType *t,
                                                       const FengTypeMember *m) {
    if (!m) return NULL;
    for (size_t i = 0; i < t->method_count; i++) {
        if (t->methods[i].member == m) return &t->methods[i];
    }
    return NULL;
}

/* ------------- Spec registration (Step 4b — value model) ------------- */

static bool cg_register_user_spec_shell(CG *cg, const FengDecl *decl) {
    /* Phase 4b-α only handles object-form specs. Callable-form is deferred. */
    if (decl->as.spec_decl.form != FENG_SPEC_FORM_OBJECT) {
        return cg_fail(cg, decl->token,
            "codegen: callable-form specs not yet supported (Step 4b-γ)");
    }
    if (cg->user_spec_count + 1 > cg->user_spec_capacity) {
        size_t cap = cg->user_spec_capacity ? cg->user_spec_capacity * 2 : 4;
        void *p = realloc(cg->user_specs, cap * sizeof *cg->user_specs);
        if (!p) return false;
        cg->user_specs = p;
        cg->user_spec_capacity = cap;
    }
    UserSpec *s = &cg->user_specs[cg->user_spec_count++];
    memset(s, 0, sizeof *s);
    s->decl = decl;
    s->feng_name = strndup(decl->as.spec_decl.name.data,
                           decl->as.spec_decl.name.length);
    if (!s->feng_name) return false;

    char *san = cg_sanitize(decl->as.spec_decl.name.data,
                            decl->as.spec_decl.name.length);
    if (!san) return false;

    Buf vb; buf_init(&vb);
    buf_append_fmt(&vb, "FengSpecValue__%s__%s", cg->module_mangle, san);
    s->c_value_struct_name = vb.data;

    Buf wb; buf_init(&wb);
    buf_append_fmt(&wb, "FengSpecWitness__%s__%s", cg->module_mangle, san);
    s->c_witness_struct_name = wb.data;

    /* Value-model descriptor names. Reuses the same module/sanitised name
     * mangling as the value/witness structs to keep symbol triples easy to
     * correlate when reading generated C. */
    Buf db; buf_init(&db);
    buf_append_fmt(&db, "FengSpecAgg__%s__%s", cg->module_mangle, san);
    s->c_aggregate_desc_name = db.data;

    Buf sb; buf_init(&sb);
    buf_append_fmt(&sb, "FengSpecAggSlots__%s__%s", cg->module_mangle, san);
    s->c_aggregate_slots_name = sb.data;

    Buf db2; buf_init(&db2);
    buf_append_fmt(&db2, "FengSpecAggDefault__%s__%s", cg->module_mangle, san);
    s->c_aggregate_default_name = db2.data;

    Buf ib; buf_init(&ib);
    buf_append_fmt(&ib, "FengSpecAggInit__%s__%s", cg->module_mangle, san);
    s->c_aggregate_init_fn_name = ib.data;

    Buf dssb; buf_init(&dssb);
    buf_append_fmt(&dssb, "FengSpecDefault__%s__%s__Subject",
                   cg->module_mangle, san);
    s->c_default_subject_struct_name = dssb.data;

    Buf ddb; buf_init(&ddb);
    buf_append_fmt(&ddb, "FengSpecDefault__%s__%s__Subject_desc",
                   cg->module_mangle, san);
    s->c_default_subject_desc_name = ddb.data;

    Buf dnb; buf_init(&dnb);
    buf_append_fmt(&dnb, "FengSpecDefault__%s__%s__new_subject",
                   cg->module_mangle, san);
    s->c_default_subject_new_name = dnb.data;

    Buf dwb; buf_init(&dwb);
    buf_append_fmt(&dwb, "FengSpecDefaultWitness__%s__%s",
                   cg->module_mangle, san);
    s->c_default_witness_name = dwb.data;

    free(san);
    return s->c_value_struct_name && s->c_witness_struct_name
        && s->c_aggregate_desc_name && s->c_aggregate_slots_name
        && s->c_aggregate_default_name && s->c_aggregate_init_fn_name
        && s->c_default_subject_struct_name && s->c_default_subject_desc_name
        && s->c_default_subject_new_name && s->c_default_witness_name;
}

static bool cg_register_user_spec_members(CG *cg, UserSpec *s) {
    const FengDecl *decl = s->decl;
    /* Phase 4b-α rejects parent_specs to keep witness composition out of
     * scope; per dev/feng-value-model-delivered.md the closure is intended
     * for 4b-β/γ. */
    if (decl->as.spec_decl.parent_spec_count > 0) {
        return cg_fail(cg, decl->token,
            "codegen: spec parent_specs not yet supported in Step 4b-α");
    }
    size_t mc = decl->as.spec_decl.as.object.member_count;
    s->members = mc ? calloc(mc, sizeof *s->members) : NULL;
    if (mc && !s->members) return false;
    for (size_t i = 0; i < mc; i++) {
        const FengTypeMember *m = decl->as.spec_decl.as.object.members[i];
        UserSpecMember *sm = &s->members[i];
        sm->member = m;
        if (m->kind == FENG_TYPE_MEMBER_FIELD) {
            sm->kind = USM_KIND_FIELD;
            sm->is_var = (m->as.field.mutability == FENG_MUTABILITY_VAR);
            sm->feng_name = strndup(m->as.field.name.data, m->as.field.name.length);
            sm->c_field_name = cg_sanitize(m->as.field.name.data, m->as.field.name.length);
            if (!cg_resolve_type(cg, m->as.field.type, &m->token, &sm->type)) return false;
            /* Aggregate-typed spec fields (spec inside spec) are part of the
             * 4b-γ fat-spec recursion and need recursive aggregate_assign;
             * reject early so the gap surfaces at compile time rather than
             * silently emitting a wrong thunk. */
            if (cgtype_value_kind(sm->type) == CG_VK_AGGREGATE) {
                return cg_fail(cg, m->token,
                    "codegen: spec field of aggregate type not yet supported (Step 4b-γ)");
            }
        } else if (m->kind == FENG_TYPE_MEMBER_METHOD) {
            sm->kind = USM_KIND_METHOD;
            const FengCallableSignature *sig = &m->as.callable;
            sm->feng_name = strndup(sig->name.data, sig->name.length);
            sm->c_field_name = cg_sanitize(sig->name.data, sig->name.length);
            if (!cg_resolve_type(cg, sig->return_type, &sig->token, &sm->type)) return false;
            sm->param_count = sig->param_count;
            sm->param_types = sig->param_count
                ? calloc(sig->param_count, sizeof(CGType*)) : NULL;
            sm->param_names = sig->param_count
                ? calloc(sig->param_count, sizeof(char*)) : NULL;
            for (size_t pi = 0; pi < sig->param_count; pi++) {
                if (!cg_resolve_type(cg, sig->params[pi].type,
                                     &sig->params[pi].token,
                                     &sm->param_types[pi])) return false;
                sm->param_names[pi] = strndup(sig->params[pi].name.data,
                                              sig->params[pi].name.length);
            }
        } else {
            return cg_fail(cg, m->token,
                "codegen: spec member kind not supported (Step 4b-α only handles fields/methods)");
        }
        if (!sm->feng_name || !sm->c_field_name) return false;
    }
    s->member_count = mc;
    return true;
}

static const UserSpecMember *cg_user_spec_member(const UserSpec *s,
                                                 const char *name, size_t len) {
    for (size_t i = 0; i < s->member_count; i++) {
        if (strlen(s->members[i].feng_name) == len &&
            memcmp(s->members[i].feng_name, name, len) == 0) {
            return &s->members[i];
        }
    }
    return NULL;
}

/* ------------- Fit registration (Step 4b-γ) ------------- */

/* Register a `fit T :: S { ... }` shell: resolve the target T to a UserType
 * and assign a per-program index for symbol mangling. Member resolution is
 * deferred to cg_register_user_fit_members so target T's methods (used by
 * cg_resolve_type for parameter / return types) are already populated. */
static bool cg_register_user_fit_shell(CG *cg, const FengDecl *decl) {
    if (!decl->as.fit_decl.has_body) {
        /* `fit T :: S, U;` (head-only) carries no methods to emit; semantic
         * still consumes it for satisfaction relations. Codegen registers
         * an empty entry so witness lookup by decl returns a stable handle. */
    }
    const FengTypeRef *target_ref = decl->as.fit_decl.target;
    if (!target_ref || target_ref->kind != FENG_TYPE_REF_NAMED ||
        target_ref->as.named.segment_count != 1) {
        return cg_fail(cg, decl->token,
            "codegen: only single-segment named fit targets are supported");
    }
    const FengSlice *seg = &target_ref->as.named.segments[0];
    const UserType *target = cg_find_user_type(cg, seg->data, seg->length);
    if (!target) {
        return cg_fail(cg, decl->token,
            "codegen: fit target type '%.*s' is not a known user type",
            (int)seg->length, seg->data);
    }
    if (cg->user_fit_count + 1 > cg->user_fit_capacity) {
        size_t cap = cg->user_fit_capacity ? cg->user_fit_capacity * 2 : 4;
        void *p = realloc(cg->user_fits, cap * sizeof *cg->user_fits);
        if (!p) return false;
        cg->user_fits = p;
        cg->user_fit_capacity = cap;
    }
    UserFit *uf = &cg->user_fits[cg->user_fit_count];
    memset(uf, 0, sizeof *uf);
    uf->decl = decl;
    uf->target = target;
    uf->index = cg->user_fit_count;
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "FengFit_%zu__%s", uf->index, target->c_struct_name);
    if (!b.data) return false;
    uf->c_prefix = b.data;
    cg->user_fit_count++;
    return true;
}

/* Populate the fit's UserMethod array. Field members are not legal in a fit
 * body (semantic enforces this; we double-check). Each method's c_name
 * is `<fit_prefix>__<member>` so two fits for the same T cannot collide,
 * and so a fit method does not shadow a same-named method on T itself. */
static bool cg_register_user_fit_members(CG *cg, UserFit *uf) {
    const FengDecl *decl = uf->decl;
    size_t mc = 0;
    for (size_t i = 0; i < decl->as.fit_decl.member_count; i++) {
        if (decl->as.fit_decl.members[i]->kind == FENG_TYPE_MEMBER_METHOD) mc++;
    }
    uf->methods = mc ? calloc(mc, sizeof *uf->methods) : NULL;
    if (mc && !uf->methods) return false;
    size_t mi = 0;
    for (size_t i = 0; i < decl->as.fit_decl.member_count; i++) {
        const FengTypeMember *m = decl->as.fit_decl.members[i];
        if (m->kind != FENG_TYPE_MEMBER_METHOD) {
            return cg_fail(cg, m->token,
                "codegen: only methods are supported in fit bodies");
        }
        UserMethod *um = &uf->methods[mi++];
        const FengCallableSignature *sig = &m->as.callable;
        um->member = m;
        um->feng_name = strndup(sig->name.data, sig->name.length);
        char *msan = cg_sanitize(sig->name.data, sig->name.length);
        if (!um->feng_name || !msan) { free(msan); return false; }
        Buf cb; buf_init(&cb);
        buf_append_fmt(&cb, "%s__%s", uf->c_prefix, msan);
        um->c_name = cb.data;
        free(msan);
        um->param_count = sig->param_count;
        um->param_types = sig->param_count
            ? calloc(sig->param_count, sizeof(CGType*)) : NULL;
        um->param_names = sig->param_count
            ? calloc(sig->param_count, sizeof(char*)) : NULL;
        for (size_t pi = 0; pi < sig->param_count; pi++) {
            if (!cg_resolve_type(cg, sig->params[pi].type,
                                 &sig->params[pi].token,
                                 &um->param_types[pi])) return false;
            um->param_names[pi] = strndup(sig->params[pi].name.data,
                                          sig->params[pi].name.length);
        }
        if (!cg_resolve_type(cg, sig->return_type, &sig->token,
                             &um->return_type)) return false;
        um->c_name = cg_append_param_suffix(um->c_name,
                                            um->param_types, um->param_count);
        if (!um->c_name) return false;
    }
    uf->method_count = mi;
    return true;
}

/* Forward declarations for spec value-struct + witness-struct, plus the
 * value-struct definition. The witness-struct definition is emitted later in
 * cg_emit_user_spec_definition (which needs every method's CType resolved). */
static void cg_emit_user_spec_forward(CG *cg, const UserSpec *s) {
    /* Witness struct is forward-declared here so the value struct (whose
     * `witness` field points at it) can be defined immediately. */
    buf_append_fmt(&cg->headers, "struct %s;\n", s->c_witness_struct_name);
    buf_append_fmt(&cg->headers,
        "struct %s { void *subject; const struct %s *witness; };\n",
        s->c_value_struct_name, s->c_witness_struct_name);
}

/* Emit the witness struct body. Method members get a single function-pointer
 * slot; field members get a getter slot and (for `var`) a setter slot. The
 * declaration order of slots matches the spec source so codegen iteration
 * and debugger inspection stay aligned. If a spec has no slots at all an
 * empty struct would be invalid C — emit a `_padding` byte. */
static void cg_emit_user_spec_definition(CG *cg, const UserSpec *s) {
    Buf *td = &cg->type_defs;
    buf_append_fmt(td, "struct %s {\n", s->c_witness_struct_name);
    size_t emitted = 0;
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind == USM_KIND_METHOD) {
            buf_append_cstr(td, "    ");
            cg_emit_c_type(td, sm->type);
            buf_append_fmt(td, " (*%s)(void *_subject", sm->c_field_name);
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_cstr(td, ", ");
                cg_emit_c_type(td, sm->param_types[pi]);
            }
            buf_append_cstr(td, ");\n");
            emitted++;
        } else if (sm->kind == USM_KIND_FIELD) {
            /* Getter — borrowed return per dev/feng-spec-codegen-pending.md
             * §5.3 (witness thunks pass values borrowed). */
            buf_append_cstr(td, "    ");
            cg_emit_c_type(td, sm->type);
            buf_append_fmt(td, " (*get_%s)(void *_subject);\n", sm->c_field_name);
            emitted++;
            if (sm->is_var) {
                /* Setter — owning store via feng_assign for managed slots. */
                buf_append_cstr(td, "    void (*set_");
                buf_append_cstr(td, sm->c_field_name);
                buf_append_cstr(td, ")(void *_subject, ");
                cg_emit_c_type(td, sm->type);
                buf_append_cstr(td, " value);\n");
                emitted++;
            }
        }
    }
    if (emitted == 0) {
        buf_append_cstr(td, "    char _padding;\n");
    }
    buf_append_cstr(td, "};\n\n");

    /* ---- Value-model aggregate descriptor (dev/feng-value-model-delivered.md
     * §3, §7.2, §8.2). For object-form specs the value layout is
     * { void *subject; const Witness *witness; } — exactly one managed
     * pointer slot at offset 0 (subject). The witness pointer is a
     * non-managed reference into rodata. */
    buf_append_fmt(td,
        "static const FengManagedSlotDescriptor %s[] = {\n"
        "    { offsetof(struct %s, subject), FENG_SLOT_POINTER, NULL },\n"
        "};\n",
        s->c_aggregate_slots_name,
        s->c_value_struct_name);

    /* ---- Hidden default-subject type (dev/feng-spec-codegen-pending.md §6).
     * For each object-form spec we generate a real managed object that
     * backs default-initialised spec values: the default witness reads /
     * writes its fields, and method slots return per-type defaults. The
     * struct is opaque to user code (its name carries `Default__`). */
    buf_append_fmt(td, "struct %s {\n", s->c_default_subject_struct_name);
    buf_append_cstr(td, "    FengManagedHeader _hdr;\n");
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind != USM_KIND_FIELD) continue;
        buf_append_cstr(td, "    ");
        cg_emit_c_type(td, sm->type);
        buf_append_fmt(td, " %s;\n", sm->c_field_name);
    }
    buf_append_cstr(td, "};\n\n");

    /* Subject release_children — one drop per managed FIELD (aggregate spec
     * fields are rejected at registration, so each managed field is exactly
     * one pointer slot here). */
    bool subject_any_managed = false;
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind == USM_KIND_FIELD &&
            cgtype_value_kind(sm->type) != CG_VK_TRIVIAL) {
            subject_any_managed = true;
            break;
        }
    }
    char *subject_release_name = NULL;
    if (subject_any_managed) {
        Buf rcn; buf_init(&rcn);
        buf_append_fmt(&rcn, "%s__release_children",
                       s->c_default_subject_struct_name);
        subject_release_name = rcn.data;
        buf_append_fmt(td, "static void %s(void *_self) {\n", subject_release_name);
        buf_append_fmt(td, "    struct %s *_o = (struct %s *)_self;\n",
                       s->c_default_subject_struct_name,
                       s->c_default_subject_struct_name);
        for (size_t i = 0; i < s->member_count; i++) {
            const UserSpecMember *sm = &s->members[i];
            if (sm->kind != USM_KIND_FIELD) continue;
            if (!cg_emit_field_release(cg, td, sm->c_field_name, sm->type,
                                       s->decl->token)) {
                free(subject_release_name);
                return;
            }
        }
        buf_append_cstr(td, "}\n\n");
    }

    /* Subject managed_fields metadata for the cycle collector. */
    size_t subject_managed_count = 0U;
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind != USM_KIND_FIELD) continue;
        subject_managed_count += cg_field_managed_descriptor_count(
            cg, sm->type, s->decl->token);
    }
    if (subject_managed_count > 0U) {
        buf_append_fmt(td,
            "static const FengManagedFieldDescriptor %s__managed_fields[] = {\n",
            s->c_default_subject_struct_name);
        for (size_t i = 0; i < s->member_count; i++) {
            const UserSpecMember *sm = &s->members[i];
            if (sm->kind != USM_KIND_FIELD) continue;
            if (!cg_emit_field_managed_descriptors(cg, td,
                    s->c_default_subject_struct_name,
                    sm->c_field_name, sm->type, s->decl->token)) {
                free(subject_release_name);
                return;
            }
        }
        buf_append_cstr(td, "};\n\n");
    }

    /* Subject FengTypeDescriptor — hidden default subjects are never user
     * finalisers and conservatively marked acyclic (default zero values
     * cannot reference back into themselves: every initialiser is a fresh
     * default of its own type). */
    buf_append_fmt(td,
        "static const FengTypeDescriptor %s = {\n"
        "    .name = \"%s.%s.<spec_default>\",\n"
        "    .size = sizeof(struct %s),\n"
        "    .finalizer = NULL,\n"
        "    .release_children = %s,\n"
        "    .is_potentially_cyclic = false,\n"
        "    .managed_field_count = %zu,\n",
        s->c_default_subject_desc_name,
        cg->module_dot_name, s->feng_name,
        s->c_default_subject_struct_name,
        subject_release_name ? subject_release_name : "NULL",
        subject_managed_count);
    if (subject_managed_count > 0U) {
        buf_append_fmt(td, "    .managed_fields = %s__managed_fields,\n",
                       s->c_default_subject_struct_name);
    } else {
        buf_append_cstr(td, "    .managed_fields = NULL,\n");
    }
    buf_append_cstr(td, "};\n\n");
    free(subject_release_name);

    /* Subject factory: feng_object_new + per-field default-zero materialise.
     * Returns +1 owns_ref. */
    buf_append_fmt(td,
        "static struct %s *%s(void) {\n"
        "    struct %s *_o = (struct %s *)feng_object_new(&%s);\n",
        s->c_default_subject_struct_name, s->c_default_subject_new_name,
        s->c_default_subject_struct_name, s->c_default_subject_struct_name,
        s->c_default_subject_desc_name);
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind != USM_KIND_FIELD) continue;
        /* Trivial: feng_object_new already zeroed. */
        if (cgtype_value_kind(sm->type) == CG_VK_TRIVIAL) continue;
        char *expr = NULL;
        if (!cg_default_value_expr(cg, sm->type, &s->decl->token, &expr)) {
            free(expr);
            return;
        }
        buf_append_fmt(td, "    _o->%s = %s;\n", sm->c_field_name, expr);
        free(expr);
    }
    buf_append_cstr(td, "    return _o;\n}\n\n");

    /* Default witness thunks. Field getters / setters route through the
     * subject's own field; methods return the default value of their
     * return type. Method thunks ignore arguments (default semantics
     * cannot inspect them). */
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind == USM_KIND_FIELD) {
            buf_append_cstr(td, "static ");
            cg_emit_c_type(td, sm->type);
            buf_append_fmt(td, " %s__get_%s(void *_subject) {\n",
                           s->c_default_witness_name, sm->c_field_name);
            buf_append_fmt(td,
                "    return ((struct %s *)_subject)->%s;\n",
                s->c_default_subject_struct_name, sm->c_field_name);
            buf_append_cstr(td, "}\n");
            if (sm->is_var) {
                buf_append_fmt(td,
                    "static void %s__set_%s(void *_subject, ",
                    s->c_default_witness_name, sm->c_field_name);
                cg_emit_c_type(td, sm->type);
                buf_append_cstr(td, " value) {\n");
                if (cgtype_is_managed(sm->type)) {
                    buf_append_fmt(td,
                        "    feng_assign((void **)&((struct %s *)_subject)->%s, value);\n",
                        s->c_default_subject_struct_name, sm->c_field_name);
                } else {
                    buf_append_fmt(td,
                        "    ((struct %s *)_subject)->%s = value;\n",
                        s->c_default_subject_struct_name, sm->c_field_name);
                }
                buf_append_cstr(td, "}\n");
            }
        } else if (sm->kind == USM_KIND_METHOD) {
            buf_append_cstr(td, "static ");
            cg_emit_c_type(td, sm->type);
            buf_append_fmt(td, " %s__%s(void *_subject",
                           s->c_default_witness_name, sm->c_field_name);
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_cstr(td, ", ");
                cg_emit_c_type(td, sm->param_types[pi]);
                buf_append_fmt(td, " _p%zu", pi);
            }
            buf_append_cstr(td, ") {\n    (void)_subject;\n");
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_fmt(td, "    (void)_p%zu;\n", pi);
            }
            if (sm->type->kind == CG_TYPE_VOID) {
                buf_append_cstr(td, "}\n");
            } else {
                char *expr = NULL;
                if (!cg_default_value_expr(cg, sm->type, &s->decl->token, &expr)) {
                    free(expr);
                    return;
                }
                buf_append_fmt(td, "    return %s;\n}\n", expr);
                free(expr);
            }
        }
    }
    buf_append_cstr(td, "\n");

    /* Default witness instance — same struct layout as a (T,S) witness; only
     * the function pointers differ. */
    buf_append_fmt(td, "static const struct %s %s = {\n",
                   s->c_witness_struct_name, s->c_default_witness_name);
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind == USM_KIND_FIELD) {
            buf_append_fmt(td, "    .get_%s = &%s__get_%s,\n",
                           sm->c_field_name, s->c_default_witness_name,
                           sm->c_field_name);
            if (sm->is_var) {
                buf_append_fmt(td, "    .set_%s = &%s__set_%s,\n",
                               sm->c_field_name, s->c_default_witness_name,
                               sm->c_field_name);
            }
        } else if (sm->kind == USM_KIND_METHOD) {
            buf_append_fmt(td, "    .%s = &%s__%s,\n",
                           sm->c_field_name, s->c_default_witness_name,
                           sm->c_field_name);
        }
    }
    buf_append_cstr(td, "};\n\n");

    /* Real default-init function: allocate fresh subject (+1) and bind
     * the default witness. Per dev/feng-spec-codegen-pending.md §6.4 the
     * factory already returned an owning reference, so no extra retain. */
    buf_append_fmt(td,
        "static void %s(void *_value_out) {\n"
        "    struct %s *_v = _value_out;\n"
        "    _v->subject = (void *)%s();\n"
        "    _v->witness = &%s;\n"
        "}\n",
        s->c_aggregate_init_fn_name,
        s->c_value_struct_name,
        s->c_default_subject_new_name,
        s->c_default_witness_name);

    buf_append_fmt(td,
        "static const FengAggregateDefaultInitDescriptor %s = {\n"
        "    .kind = FENG_DEFAULT_INIT_FN,\n"
        "    .init_fn = %s,\n"
        "};\n",
        s->c_aggregate_default_name,
        s->c_aggregate_init_fn_name);

    buf_append_fmt(td,
        "static const FengAggregateValueDescriptor %s __attribute__((unused)) = {\n"
        "    .name = \"%s\",\n"
        "    .size = sizeof(struct %s),\n"
        "    .default_init = &%s,\n"
        "    .managed_slot_count = 1,\n"
        "    .managed_slots = %s,\n"
        "};\n\n",
        s->c_aggregate_desc_name,
        s->feng_name,
        s->c_value_struct_name,
        s->c_aggregate_default_name,
        s->c_aggregate_slots_name);
}

/* ----- module bindings ----- */

static bool cg_register_module_binding(CG *cg, const FengDecl *decl) {
    if (cg->module_binding_count + 1 > cg->module_binding_capacity) {
        size_t cap = cg->module_binding_capacity ? cg->module_binding_capacity * 2 : 4;
        void *p = realloc(cg->module_bindings, cap * sizeof *cg->module_bindings);
        if (!p) return false;
        cg->module_bindings = p;
        cg->module_binding_capacity = cap;
    }
    ModuleBinding *mb = &cg->module_bindings[cg->module_binding_count++];
    memset(mb, 0, sizeof *mb);
    mb->binding = &decl->as.binding;
    mb->is_var = (decl->as.binding.mutability == FENG_MUTABILITY_VAR);
    mb->feng_name = strndup(decl->as.binding.name.data, decl->as.binding.name.length);
    if (!mb->feng_name) return false;
    char *san = cg_sanitize(decl->as.binding.name.data, decl->as.binding.name.length);
    if (!san) return false;
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "_feng_g__%s__%s", cg->module_mangle, san);
    free(san);
    mb->c_name = b.data;
    if (!mb->c_name) return false;
    /* Type: explicit annotation required at module scope unless initializer
     * present. We can't fully infer from a user-type literal here without
     * doing expression emission first — which we cannot do until all types
     * are registered. So we require an explicit type annotation OR a
     * primitive/string literal initializer in 1A. */
    if (decl->as.binding.type) {
        if (!cg_resolve_type(cg, decl->as.binding.type, &decl->token, &mb->type)) {
            return false;
        }
    } else {
        const FengExpr *init = decl->as.binding.initializer;
        if (!init) {
            return cg_fail(cg, decl->token,
                "codegen: module-level binding requires an explicit type or initializer");
        }
        switch (init->kind) {
            case FENG_EXPR_BOOL:    mb->type = cgtype_new(CG_TYPE_BOOL); break;
            case FENG_EXPR_INTEGER: mb->type = cgtype_new(CG_TYPE_I32); break;
            case FENG_EXPR_FLOAT:   mb->type = cgtype_new(CG_TYPE_F64); break;
            case FENG_EXPR_STRING:  mb->type = cgtype_new(CG_TYPE_STRING); break;
            default:
                return cg_fail(cg, decl->token,
                    "codegen: module-level binding without explicit type can only be"
                    " initialised by a literal in Phase 1A");
        }
        if (!mb->type) return false;
    }
    return true;
}

static const ModuleBinding *cg_find_module_binding(const CG *cg, const char *name, size_t len) {
    for (size_t i = 0; i < cg->module_binding_count; i++) {
        if (strlen(cg->module_bindings[i].feng_name) == len &&
            memcmp(cg->module_bindings[i].feng_name, name, len) == 0) {
            return &cg->module_bindings[i];
        }
    }
    return NULL;
}

/* ===================== expression emission ===================== */

struct ExprResult {
    char   *c_expr;       /* malloc'd */
    CGType *type;         /* malloc'd */
    /* When true, the C expression evaluates to a fresh +1 reference that the
     * caller MUST either store in a slot (transferring ownership) or wrap in
     * a temporary and release after use. Only meaningful for managed types. */
    bool    owns_ref;
};

static void er_init(ExprResult *r) {
    r->c_expr = NULL;
    r->type = NULL;
    r->owns_ref = false;
}

static void er_free(ExprResult *r) {
    if (!r) return;
    free(r->c_expr);
    cgtype_free(r->type);
    r->c_expr = NULL;
    r->type = NULL;
    r->owns_ref = false;
}

/* Materialise an ExprResult into a fresh C local so its lifetime spans the
 * current statement. For managed +1 results this also schedules a release at
 * the end of the local scope by registering the local in the scope. For
 * non-managed values, it simply binds to a temp.
 *
 * Returns the C expression (identifier of the local), via *out_cexpr (malloc'd
 * by caller's responsibility — caller must free). The type is transferred to
 * the local in scope so the caller MUST NOT use r->type afterwards. */
static char *cg_materialize_to_local(CG *cg, ExprResult *r, const char *prefix) {
    char *tmp = cg_fresh_temp(cg, prefix);
    if (!tmp) return NULL;
    char *cty = cg_ctype_dup(r->type);
    buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, tmp, r->c_expr);
    free(cty);
    if (cgtype_is_managed(r->type) && r->owns_ref) {
        /* Register in scope so it gets released on scope exit. The scope
         * takes ownership of a clone so r->type remains valid for callers
         * that still need to inspect (e.g., extern arg coercions). */
        scope_add(cg->cur_scope, tmp, tmp, cgtype_clone(r->type), false);
        cg_emit_cleanup_push_for_managed_local(cg, tmp);
    } else if (cgtype_is_managed(r->type)) {
        /* Borrowed: don't register for release. */
    } else if (cgtype_is_aggregate(r->type) && r->owns_ref) {
        /* Step 4b — fat spec value carries one +1 reference on `subject`. */
        scope_add(cg->cur_scope, tmp, tmp, cgtype_clone(r->type), false);
        cg_emit_cleanup_push_for_aggregate_local(cg, tmp);
    }
    /* Free r->c_expr; r->type is preserved so caller-side decisions that
     * branch on the source type (e.g., extern STRING wrapping) keep
     * working. */
    free(r->c_expr);
    r->c_expr = strdup(tmp);
    r->owns_ref = false;
    return tmp;
}

static bool cg_emit_literal(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    if (e->kind == FENG_EXPR_BOOL) {
        out->c_expr = strdup(e->as.boolean ? "true" : "false");
        out->type = cgtype_new(CG_TYPE_BOOL);
        return out->c_expr && out->type;
    }
    if (e->kind == FENG_EXPR_INTEGER) {
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "(int64_t)INT64_C(%" PRId64 ")", e->as.integer);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_I64);
        return out->c_expr && out->type;
    }
    if (e->kind == FENG_EXPR_FLOAT) {
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "(%a)", e->as.floating);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_F64);
        return out->c_expr && out->type;
    }
    if (e->kind == FENG_EXPR_STRING) {
        /* The parser stores the raw lexeme including the surrounding quotes
         * and unprocessed escapes. Decode here. The lexer guarantees the
         * literal is well-formed: starts/ends with '"' and uses only the
         * supported escape set (\\ \" \n \r \t \0). */
        const char *raw = e->as.string.data;
        size_t rlen = e->as.string.length;
        if (rlen < 2 || raw[0] != '"' || raw[rlen - 1] != '"') {
            return cg_fail(cg, e->token, "codegen: malformed string literal");
        }
        const char *body = raw + 1;
        size_t blen = rlen - 2;
        char *decoded = malloc(blen + 1);
        if (!decoded) return cg_fail(cg, e->token, "codegen: out of memory");
        size_t di = 0;
        for (size_t i = 0; i < blen; i++) {
            char ch = body[i];
            if (ch == '\\' && i + 1 < blen) {
                char esc = body[++i];
                switch (esc) {
                    case '\\': decoded[di++] = '\\'; break;
                    case '"':  decoded[di++] = '"';  break;
                    case 'n':  decoded[di++] = '\n'; break;
                    case 'r':  decoded[di++] = '\r'; break;
                    case 't':  decoded[di++] = '\t'; break;
                    case '0':  decoded[di++] = '\0'; break;
                    default:
                        free(decoded);
                        return cg_fail(cg, e->token,
                            "codegen: unknown string escape '\\%c'", esc);
                }
            } else {
                decoded[di++] = ch;
            }
        }
        decoded[di] = '\0';
        const char *cv = cg_string_literal_var(cg, decoded, di);
        free(decoded);
        if (!cv) return cg_fail(cg, e->token, "codegen: out of memory");
        out->c_expr = strdup(cv);
        out->type = cgtype_new(CG_TYPE_STRING);
        out->owns_ref = false;   /* immortal */
        return out->c_expr && out->type;
    }
    return cg_fail(cg, e->token, "codegen: unsupported literal kind");
}

static const char *cg_binop_c(FengTokenKind op) {
    switch (op) {
        case FENG_TOKEN_PLUS: return "+";
        case FENG_TOKEN_MINUS: return "-";
        case FENG_TOKEN_STAR: return "*";
        case FENG_TOKEN_SLASH: return "/";
        case FENG_TOKEN_PERCENT: return "%";
        case FENG_TOKEN_LT: return "<";
        case FENG_TOKEN_LE: return "<=";
        case FENG_TOKEN_GT: return ">";
        case FENG_TOKEN_GE: return ">=";
        case FENG_TOKEN_EQ: return "==";
        case FENG_TOKEN_NE: return "!=";
        case FENG_TOKEN_AND_AND: return "&&";
        case FENG_TOKEN_OR_OR: return "||";
        case FENG_TOKEN_AMP: return "&";
        case FENG_TOKEN_PIPE: return "|";
        case FENG_TOKEN_CARET: return "^";
        case FENG_TOKEN_SHL: return "<<";
        case FENG_TOKEN_SHR: return ">>";
        default: return NULL;
    }
}

static bool cg_unify_numeric(CG *cg, FengToken tok, ExprResult *l, ExprResult *r,
                             CGType **out_common) {
    CGTypeKind lk = l->type->kind, rk = r->type->kind;
    if (lk == CG_TYPE_F64 || rk == CG_TYPE_F64) { *out_common = cgtype_new(CG_TYPE_F64); return true; }
    if (lk == CG_TYPE_F32 || rk == CG_TYPE_F32) { *out_common = cgtype_new(CG_TYPE_F32); return true; }
    if (cgtype_is_integer(lk) && cgtype_is_integer(rk)) {
        int lr = cgtype_int_rank(lk), rr = cgtype_int_rank(rk);
        CGTypeKind chosen = (lr >= rr) ? lk : rk;
        /* Mixed-sign promotes to signed of higher rank for Phase 1A. */
        if (cgtype_is_signed(lk) != cgtype_is_signed(rk)) {
            chosen = (cgtype_int_rank(chosen) >= 4) ? CG_TYPE_I64 : CG_TYPE_I64;
        }
        *out_common = cgtype_new(chosen);
        return true;
    }
    return cg_fail(cg, tok, "codegen: cannot apply numeric op to non-numeric operands");
}

static bool cg_emit_binary(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    ExprResult lr; ExprResult rr;
    if (!cg_emit_expr(cg, e->as.binary.left, &lr)) return false;
    if (!cg_emit_expr(cg, e->as.binary.right, &rr)) { er_free(&lr); return false; }

    /* Spec `==` / `!=` — reference-identity per dev/feng-spec-codegen-pending.md
     * §7. Semantic registers a SpecEquality sidecar entry for every binary
     * `==`/`!=` whose operands resolve to a spec; codegen consumes it and
     * lowers to a direct subject pointer compare. Both operands are fat
     * spec values (aggregates); when either side is an owns_ref temporary
     * we materialise it to a local first so its `subject` survives the
     * comparison and its scope cleanup runs the standard aggregate
     * release. */
    const FengSpecEquality *eq_site =
        cg->analysis ? feng_semantic_lookup_spec_equality(cg->analysis, e) : NULL;
    if (eq_site != NULL) {
        if (!cgtype_is_aggregate(lr.type) || !cgtype_is_aggregate(rr.type)) {
            er_free(&lr); er_free(&rr);
            return cg_fail(cg, e->token,
                "codegen: spec equality requires aggregate spec operands");
        }
        cg_materialize_to_local(cg, &lr, "_t");
        cg_materialize_to_local(cg, &rr, "_t");
        const char *cop = (eq_site->op == FENG_SPEC_EQUALITY_OP_EQ) ? "==" : "!=";
        Buf b; buf_init(&b);
        /* Cast guards against -Wparentheses-equality when the result lands
         * directly inside `if (...)`: the explicit (bool) cast prevents
         * clang from treating the `==` as a stray double-paren. */
        buf_append_fmt(&b, "(bool)(%s.subject %s %s.subject)",
                       lr.c_expr, cop, rr.c_expr);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_BOOL);
        er_free(&lr); er_free(&rr);
        return out->c_expr && out->type;
    }

    /* String concatenation: '+' on two strings. */
    if (e->as.binary.op == FENG_TOKEN_PLUS &&
        lr.type->kind == CG_TYPE_STRING && rr.type->kind == CG_TYPE_STRING) {
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "feng_string_concat(%s, %s)", lr.c_expr, rr.c_expr);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_STRING);
        out->owns_ref = true;
        er_free(&lr); er_free(&rr);
        return out->c_expr && out->type;
    }

    /* Equality on strings is not auto-defined here in 1A; require numerics. */
    const char *cop = cg_binop_c(e->as.binary.op);
    if (!cop) {
        er_free(&lr); er_free(&rr);
        return cg_fail(cg, e->token, "codegen: unsupported binary operator");
    }

    /* Logical operators: bool && bool, bool || bool. */
    if (e->as.binary.op == FENG_TOKEN_AND_AND || e->as.binary.op == FENG_TOKEN_OR_OR) {
        if (lr.type->kind != CG_TYPE_BOOL || rr.type->kind != CG_TYPE_BOOL) {
            er_free(&lr); er_free(&rr);
            return cg_fail(cg, e->token, "codegen: && / || require bool operands");
        }
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "(%s %s %s)", lr.c_expr, cop, rr.c_expr);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_BOOL);
        er_free(&lr); er_free(&rr);
        return out->c_expr && out->type;
    }

    /* Comparison: produces bool from numeric/bool operands. */
    bool is_cmp = (e->as.binary.op == FENG_TOKEN_LT || e->as.binary.op == FENG_TOKEN_LE ||
                   e->as.binary.op == FENG_TOKEN_GT || e->as.binary.op == FENG_TOKEN_GE ||
                   e->as.binary.op == FENG_TOKEN_EQ || e->as.binary.op == FENG_TOKEN_NE);
    if (is_cmp && lr.type->kind == CG_TYPE_BOOL && rr.type->kind == CG_TYPE_BOOL) {
        if (e->as.binary.op != FENG_TOKEN_EQ && e->as.binary.op != FENG_TOKEN_NE) {
            er_free(&lr); er_free(&rr);
            return cg_fail(cg, e->token, "codegen: ordering comparisons require numeric operands");
        }
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "(%s %s %s)", lr.c_expr, cop, rr.c_expr);
        out->c_expr = b.data;
        out->type = cgtype_new(CG_TYPE_BOOL);
        er_free(&lr); er_free(&rr);
        return out->c_expr && out->type;
    }

    /* Numeric arithmetic / bitwise / comparison. */
    CGType *common = NULL;
    if (!cg_unify_numeric(cg, e->token, &lr, &rr, &common)) {
        er_free(&lr); er_free(&rr); return false;
    }
    Buf b; buf_init(&b);
    const char *cty = cgtype_to_c(common->kind);
    buf_append_fmt(&b, "((%s)%s %s (%s)%s)", cty, lr.c_expr, cop, cty, rr.c_expr);
    out->c_expr = b.data;
    if (is_cmp) {
        cgtype_free(common);
        out->type = cgtype_new(CG_TYPE_BOOL);
    } else {
        out->type = common;
    }
    er_free(&lr); er_free(&rr);
    return out->c_expr && out->type;
}

static bool cg_emit_unary(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    ExprResult inner;
    if (!cg_emit_expr(cg, e->as.unary.operand, &inner)) return false;
    const char *op = NULL;
    bool require_bool = false;
    bool require_int = false;
    switch (e->as.unary.op) {
        case FENG_TOKEN_MINUS: op = "-"; break;
        case FENG_TOKEN_PLUS:  op = "+"; break;
        case FENG_TOKEN_NOT:   op = "!"; require_bool = true; break;
        case FENG_TOKEN_TILDE: op = "~"; require_int = true; break;
        default:
            er_free(&inner);
            return cg_fail(cg, e->token, "codegen: unsupported unary operator");
    }
    if (require_bool && inner.type->kind != CG_TYPE_BOOL) {
        er_free(&inner);
        return cg_fail(cg, e->token, "codegen: '!' requires bool operand");
    }
    if (require_int && !cgtype_is_integer(inner.type->kind)) {
        er_free(&inner);
        return cg_fail(cg, e->token, "codegen: '~' requires integer operand");
    }
    if (!require_bool && !require_int && !cgtype_is_numeric(inner.type->kind)) {
        er_free(&inner);
        return cg_fail(cg, e->token, "codegen: unary +/- requires numeric operand");
    }
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "(%s%s)", op, inner.c_expr);
    out->c_expr = b.data;
    out->type = inner.type;
    inner.type = NULL;
    er_free(&inner);
    return out->c_expr && out->type;
}

static bool cg_emit_identifier(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    const Local *l = scope_lookup(cg->cur_scope, e->as.identifier.data,
                                  e->as.identifier.length);
    if (l) {
        out->c_expr = strdup(l->c_name);
        out->type = cgtype_clone(l->type);
        out->owns_ref = false;  /* borrow from local slot */
        return out->c_expr && out->type;
    }
    const ModuleBinding *mb = cg_find_module_binding(cg,
        e->as.identifier.data, e->as.identifier.length);
    if (mb) {
        out->c_expr = strdup(mb->c_name);
        out->type = cgtype_clone(mb->type);
        out->owns_ref = false;  /* borrow from static slot */
        return out->c_expr && out->type;
    }
    return cg_fail(cg, e->token,
        "codegen: identifier '%.*s' not found",
        (int)e->as.identifier.length, e->as.identifier.data);
}

static bool cg_emit_call(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    /* Method call: callee is a member access. */
    if (e->as.call.callee->kind == FENG_EXPR_MEMBER) {
        const FengExpr *ma = e->as.call.callee;
        ExprResult recv;
        if (!cg_emit_expr(cg, ma->as.member.object, &recv)) return false;
        if (recv.type->kind == CG_TYPE_SPEC && recv.type->user_spec) {
            /* Step 4b — spec method dispatch via witness table. */
            const UserSpec *us = recv.type->user_spec;
            const UserSpecMember *sm = cg_user_spec_member(us,
                ma->as.member.member.data, ma->as.member.member.length);
            if (!sm || sm->kind != USM_KIND_METHOD) {
                er_free(&recv);
                return cg_fail(cg, e->token,
                    "codegen: spec '%s' has no method '%.*s'",
                    us->feng_name,
                    (int)ma->as.member.member.length, ma->as.member.member.data);
            }
            if (e->as.call.arg_count != sm->param_count) {
                er_free(&recv);
                return cg_fail(cg, e->token,
                    "codegen: wrong argument count for spec method '%s' (expected %zu, got %zu)",
                    sm->feng_name, sm->param_count, e->as.call.arg_count);
            }
            /* Materialize the receiver so .subject / .witness load exactly
             * once. Spec values are aggregates — owns_ref==true means the
             * value carries a +1 the materialised local should adopt. */
            cg_materialize_to_local(cg, &recv, "_t");
            Buf args_buf; buf_init(&args_buf);
            bool ok = true;
            for (size_t i = 0; i < e->as.call.arg_count; i++) {
                ExprResult ar;
                if (!cg_emit_expr(cg, e->as.call.args[i], &ar)) { ok = false; break; }
                if (cgtype_is_managed(ar.type) && ar.owns_ref) {
                    cg_materialize_to_local(cg, &ar, "_t");
                } else if (cgtype_is_aggregate(ar.type)) {
                    cg_materialize_to_local(cg, &ar, "_t");
                }
                buf_append_cstr(&args_buf, ", ");
                buf_append_cstr(&args_buf, ar.c_expr);
                er_free(&ar);
            }
            if (!ok) { buf_free(&args_buf); er_free(&recv); return false; }
            Buf b; buf_init(&b);
            buf_append_fmt(&b, "%s.witness->%s(%s.subject%s)",
                           recv.c_expr, sm->c_field_name, recv.c_expr,
                           args_buf.data ? args_buf.data : "");
            buf_free(&args_buf);
            out->c_expr = b.data;
            out->type = cgtype_clone(sm->type);
            out->owns_ref = cgtype_is_managed(out->type);
            er_free(&recv);
            return out->c_expr && out->type;
        }
        if (recv.type->kind != CG_TYPE_OBJECT || !recv.type->user) {
            er_free(&recv);
            return cg_fail(cg, e->token,
                "codegen: method call on non-object value");
        }
        const UserType *ut = recv.type->user;
        const FengResolvedCallable *rc = &e->as.call.resolved_callable;
        const UserMethod *um = NULL;
        if (rc->kind == FENG_RESOLVED_CALLABLE_TYPE_METHOD && rc->member) {
            um = cg_user_type_method_by_member(ut, rc->member);
        }
        if (!um) {
            um = cg_user_type_method(ut,
                ma->as.member.member.data, ma->as.member.member.length);
        }
        if (!um) {
            er_free(&recv);
            return cg_fail(cg, e->token,
                "codegen: type '%s' has no method '%.*s'",
                ut->feng_name,
                (int)ma->as.member.member.length, ma->as.member.member.data);
        }
        if (e->as.call.arg_count != um->param_count) {
            er_free(&recv);
            return cg_fail(cg, e->token,
                "codegen: wrong argument count for method '%s' (expected %zu, got %zu)",
                um->feng_name, um->param_count, e->as.call.arg_count);
        }
        /* Materialize receiver if it's a +1 owns_ref (so it lives across args). */
        if (cgtype_is_managed(recv.type) && recv.owns_ref) {
            cg_materialize_to_local(cg, &recv, "_t");
        }
        Buf args_buf; buf_init(&args_buf);
        for (size_t i = 0; i < e->as.call.arg_count; i++) {
            ExprResult ar;
            if (!cg_emit_expr(cg, e->as.call.args[i], &ar)) {
                buf_free(&args_buf); er_free(&recv); return false;
            }
            if (cgtype_is_managed(ar.type) && ar.owns_ref) {
                cg_materialize_to_local(cg, &ar, "_t");
            }
            buf_append_cstr(&args_buf, ", ");
            buf_append_cstr(&args_buf, ar.c_expr);
            er_free(&ar);
        }
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "%s(%s%s)", um->c_name, recv.c_expr,
                       args_buf.data ? args_buf.data : "");
        buf_free(&args_buf);
        out->c_expr = b.data;
        out->type = cgtype_clone(um->return_type);
        out->owns_ref = cgtype_is_managed(out->type);
        er_free(&recv);
        return out->c_expr && out->type;
    }

    if (e->as.call.callee->kind != FENG_EXPR_IDENTIFIER) {
        return cg_fail(cg, e->token,
            "codegen: only direct or method calls supported in this iteration");
    }
    const FengSlice name = e->as.call.callee->as.identifier;

    /* Default no-arg constructor for a user type: T() */
    {
        const UserType *ut = cg_find_user_type(cg, name.data, name.length);
        if (ut) {
            if (e->as.call.arg_count != 0) {
                return cg_fail(cg, e->token,
                    "codegen: type '%s' has no user-defined constructor; use object literal syntax",
                    ut->feng_name);
            }
            Buf b; buf_init(&b);
            buf_append_fmt(&b,
                "((struct %s *)feng_object_new(&%s))",
                ut->c_struct_name, ut->c_desc_name);
            out->c_expr = b.data;
            out->type = cgtype_new(CG_TYPE_OBJECT);
            if (!out->c_expr || !out->type) return false;
            out->type->user = ut;
            out->owns_ref = true;
            return true;
        }
    }

    const ExternFn *ext = cg_find_extern(cg, name.data, name.length);
    const FengResolvedCallable *rc = &e->as.call.resolved_callable;
    const FreeFn *fn = NULL;
    if (rc->kind == FENG_RESOLVED_CALLABLE_FUNCTION && rc->function_decl) {
        fn = cg_find_free_fn_by_decl(cg, rc->function_decl);
    }
    if (!fn && !ext) {
        fn = cg_find_free_fn(cg, name.data, name.length);
    }
    if (!ext && !fn) {
        return cg_fail(cg, e->token,
            "codegen: undefined function '%.*s'", (int)name.length, name.data);
    }
    size_t expected = ext ? ext->param_count : fn->param_count;
    if (e->as.call.arg_count != expected) {
        return cg_fail(cg, e->token,
            "codegen: wrong argument count for '%.*s' (expected %zu, got %zu)",
            (int)name.length, name.data, expected, e->as.call.arg_count);
    }
    /* Emit each argument; managed +1 results are lifted into a temp and
     * registered for release at scope exit (callee borrows). */
    Buf args_buf; buf_init(&args_buf);
    bool ok = true;
    for (size_t i = 0; i < e->as.call.arg_count; i++) {
        ExprResult ar;
        if (!cg_emit_expr(cg, e->as.call.args[i], &ar)) { ok = false; break; }
        CGType *expected_ty = ext ? ext->param_types[i] : fn->param_types[i];
        if (i) buf_append_cstr(&args_buf, ", ");
        if (cgtype_is_managed(ar.type) && ar.owns_ref) {
            cg_materialize_to_local(cg, &ar, "_t");
        } else if (cgtype_is_aggregate(ar.type)) {
            /* Step 4b — pass-by-value spec arg. Per the existing calling
             * convention (Local.is_param: "caller owns"), the callee borrows
             * the value; no extra retain is required. We still materialise
             * to a temp so the spec compound literal is evaluated exactly
             * once before being copied into the C arg list. When the arg
             * is +1 we register it for release (its subject lifetime is
             * owned by this scope, not the call). */
            cg_materialize_to_local(cg, &ar, "_t");
        }
        if (ext && ar.type && ar.type->kind == CG_TYPE_STRING &&
            expected_ty && expected_ty->kind == CG_TYPE_STRING) {
            buf_append_fmt(&args_buf, "feng_string_data(%s)", ar.c_expr);
        } else {
            buf_append_cstr(&args_buf, ar.c_expr);
        }
        er_free(&ar);
    }
    if (!ok) { buf_free(&args_buf); return false; }

    Buf b; buf_init(&b);
    if (ext) {
        buf_append_fmt(&b, "%s(%s)", ext->name, args_buf.data ? args_buf.data : "");
        out->type = cgtype_clone(ext->return_type);
    } else {
        buf_append_fmt(&b, "%s(%s)", fn->c_name, args_buf.data ? args_buf.data : "");
        out->type = cgtype_clone(fn->return_type);
    }
    buf_free(&args_buf);
    out->c_expr = b.data;
    /* Step 4b-γ-2 — function returns transfer +1 ownership for both managed
     * pointers and aggregate (fat-spec) values. The callee return path
     * (cg_emit_return) already retains/moves so every managed slot of the
     * returned struct carries +1; mark the rvalue accordingly so the
     * receiver (binding init / arg materialisation / further return)
     * consumes that +1 instead of double-retaining it. */
    out->owns_ref = cgtype_is_managed(out->type) || cgtype_is_aggregate(out->type);
    return out->c_expr && out->type;
}

static bool cg_emit_member(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    ExprResult recv;
    if (!cg_emit_expr(cg, e->as.member.object, &recv)) return false;
    if (recv.type->kind == CG_TYPE_SPEC && recv.type->user_spec) {
        /* Step 4b-β — spec field access via witness getter. */
        const UserSpec *us = recv.type->user_spec;
        const UserSpecMember *sm = cg_user_spec_member(us,
            e->as.member.member.data, e->as.member.member.length);
        if (!sm || sm->kind != USM_KIND_FIELD) {
            er_free(&recv);
            return cg_fail(cg, e->token,
                "codegen: spec '%s' has no field '%.*s'",
                us->feng_name,
                (int)e->as.member.member.length, e->as.member.member.data);
        }
        /* Materialize the receiver so .subject / .witness load exactly once. */
        cg_materialize_to_local(cg, &recv, "_t");
        Buf b; buf_init(&b);
        buf_append_fmt(&b, "%s.witness->get_%s(%s.subject)",
                       recv.c_expr, sm->c_field_name, recv.c_expr);
        out->c_expr = b.data;
        out->type = cgtype_clone(sm->type);
        out->owns_ref = false;   /* getter returns a borrow per §5.3 */
        er_free(&recv);
        return out->c_expr && out->type;
    }
    if (recv.type->kind != CG_TYPE_OBJECT || !recv.type->user) {
        er_free(&recv);
        return cg_fail(cg, e->token,
            "codegen: member access on non-object value");
    }
    const UserType *ut = recv.type->user;
    const UserField *uf = cg_user_type_field(ut,
        e->as.member.member.data, e->as.member.member.length);
    if (!uf) {
        er_free(&recv);
        return cg_fail(cg, e->token,
            "codegen: type '%s' has no field '%.*s'",
            ut->feng_name,
            (int)e->as.member.member.length, e->as.member.member.data);
    }
    /* If recv is a +1 owns_ref, materialize so its lifetime extends. */
    if (cgtype_is_managed(recv.type) && recv.owns_ref) {
        cg_materialize_to_local(cg, &recv, "_t");
    }
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "(%s)->%s", recv.c_expr, uf->c_name);
    out->c_expr = b.data;
    out->type = cgtype_clone(uf->type);
    out->owns_ref = false;   /* borrow */
    er_free(&recv);
    return out->c_expr && out->type;
}

static bool cg_emit_object_literal(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    if (e->as.object_literal.target->kind != FENG_EXPR_IDENTIFIER) {
        return cg_fail(cg, e->token,
            "codegen: only single-segment type names supported for object literals");
    }
    const FengSlice tn = e->as.object_literal.target->as.identifier;
    const UserType *ut = cg_find_user_type(cg, tn.data, tn.length);
    if (!ut) {
        return cg_fail(cg, e->token,
            "codegen: unknown type '%.*s' in object literal",
            (int)tn.length, tn.data);
    }
    /* Allocate, then assign each field. We open an inline statement block in
     * the body to compute argument expressions and then reference the result.
     * Since we need a single C expression for ExprResult, we hoist the alloc
     * + assignments into a fresh temp via cg->cur_body and return the temp
     * as the c_expr. */
    char *tmp = cg_fresh_temp(cg, "_obj");
    if (!tmp) return cg_fail(cg, e->token, "codegen: out of memory");
    buf_append_fmt(cg->cur_body,
        "    struct %s *%s = (struct %s *)feng_object_new(&%s);\n",
        ut->c_struct_name, tmp, ut->c_struct_name, ut->c_desc_name);

    /* Track which fields are assigned so we can detect missing initialisers. */
    bool *assigned = ut->field_count ? calloc(ut->field_count, sizeof *assigned) : NULL;
    if (ut->field_count && !assigned) {
        free(tmp);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    for (size_t i = 0; i < e->as.object_literal.field_count; i++) {
        const FengObjectFieldInit *fi = &e->as.object_literal.fields[i];
        size_t idx = (size_t)-1;
        for (size_t k = 0; k < ut->field_count; k++) {
            if (strlen(ut->fields[k].feng_name) == fi->name.length &&
                memcmp(ut->fields[k].feng_name, fi->name.data, fi->name.length) == 0) {
                idx = k; break;
            }
        }
        if (idx == (size_t)-1) {
            free(assigned); free(tmp);
            return cg_fail(cg, fi->token,
                "codegen: type '%s' has no field '%.*s'",
                ut->feng_name, (int)fi->name.length, fi->name.data);
        }
        if (assigned[idx]) {
            free(assigned); free(tmp);
            return cg_fail(cg, fi->token,
                "codegen: duplicate field '%s' in object literal",
                ut->fields[idx].feng_name);
        }
        assigned[idx] = true;
        ExprResult v;
        if (!cg_emit_expr(cg, fi->value, &v)) {
            free(assigned); free(tmp); return false;
        }
        const UserField *uf = &ut->fields[idx];
        if (cgtype_is_managed(uf->type)) {
            if (v.owns_ref) {
                buf_append_fmt(cg->cur_body, "    %s->%s = %s;\n",
                               tmp, uf->c_name, v.c_expr);
            } else {
                buf_append_fmt(cg->cur_body,
                    "    %s->%s = %s; feng_retain(%s->%s);\n",
                    tmp, uf->c_name, v.c_expr, tmp, uf->c_name);
            }
        } else {
            char *cty = cg_ctype_dup(uf->type);
            buf_append_fmt(cg->cur_body, "    %s->%s = (%s)(%s);\n",
                           tmp, uf->c_name, cty, v.c_expr);
            free(cty);
        }
        er_free(&v);
    }
    /* Fill any field the user omitted with that field's default zero value
     * (Feng has no `null`; see docs/feng-builtin-type.md and
     * docs/feng-type.md §5/§7). For managed fields the default expression
     * yields a +1 owned reference assigned directly into the slot. */
    for (size_t k = 0; k < ut->field_count; k++) {
        if (assigned[k]) continue;
        const UserField *uf = &ut->fields[k];
        char *def_expr = NULL;
        if (!cg_default_value_expr(cg, uf->type, &e->token, &def_expr)) {
            free(assigned); free(tmp);
            return false;
        }
        if (cgtype_is_managed(uf->type)) {
            buf_append_fmt(cg->cur_body, "    %s->%s = %s;\n",
                           tmp, uf->c_name, def_expr);
        } else {
            char *cty = cg_ctype_dup(uf->type);
            buf_append_fmt(cg->cur_body, "    %s->%s = (%s)(%s);\n",
                           tmp, uf->c_name, cty, def_expr);
            free(cty);
        }
        free(def_expr);
    }
    free(assigned);

    out->c_expr = strdup(tmp);
    out->type = cgtype_new(CG_TYPE_OBJECT);
    if (!out->c_expr || !out->type) { free(tmp); return false; }
    out->type->user = ut;
    out->owns_ref = true;
    free(tmp);
    return true;
}

/* Returns the FengTypeDescriptor C expression for an array element type, or
 * the literal "NULL" if no descriptor is meaningful (descriptor is purely
 * diagnostic in the runtime). Caller-frees via free(). */
static char *cg_array_element_descriptor(const CGType *elem) {
    Buf b; buf_init(&b);
    if (!elem) { buf_append_cstr(&b, "NULL"); return b.data; }
    switch (elem->kind) {
        case CG_TYPE_STRING: buf_append_cstr(&b, "&feng_string_descriptor"); break;
        case CG_TYPE_ARRAY:  buf_append_cstr(&b, "&feng_array_descriptor"); break;
        case CG_TYPE_OBJECT:
            if (elem->user) {
                buf_append_fmt(&b, "&%s", elem->user->c_desc_name);
            } else {
                buf_append_cstr(&b, "NULL");
            }
            break;
        default: buf_append_cstr(&b, "NULL"); break;
    }
    return b.data;
}

static bool cg_types_equal(const CGType *a, const CGType *b) {
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind == CG_TYPE_OBJECT) return a->user == b->user;
    if (a->kind == CG_TYPE_ARRAY) return cg_types_equal(a->element, b->element);
    return true;
}

static bool cg_emit_array_literal_typed(CG *cg, const FengExpr *e,
                                        const CGType *expected_elem,
                                        ExprResult *out) {
    er_init(out);
    if (e->as.array_literal.count == 0) {
        return cg_fail(cg, e->token,
            "codegen: empty array literal needs an explicit element type"
            " (not yet supported in this iteration)");
    }
    /* Evaluate every element and ensure the inferred element type is
     * uniform. We materialize +1 owners up-front so they are released
     * exactly once after the slot writes complete. */
    size_t n = e->as.array_literal.count;
    ExprResult *items = calloc(n, sizeof *items);
    if (!items) return cg_fail(cg, e->token, "codegen: out of memory");
    /* When narrowing into a nested array element, recurse with the inner
     * element type. Otherwise emit each item with the default rules. */
    bool nested_narrow = (expected_elem != NULL &&
                          expected_elem->kind == CG_TYPE_ARRAY &&
                          expected_elem->element != NULL);
    for (size_t i = 0; i < n; i++) {
        const FengExpr *item_expr = e->as.array_literal.items[i];
        bool ok;
        if (nested_narrow && item_expr->kind == FENG_EXPR_ARRAY_LITERAL) {
            ok = cg_emit_array_literal_typed(cg, item_expr,
                                             expected_elem->element, &items[i]);
        } else {
            ok = cg_emit_expr(cg, item_expr, &items[i]);
        }
        if (!ok) {
            for (size_t k = 0; k < i; k++) er_free(&items[k]);
            free(items); return false;
        }
        if (i > 0 && !cg_types_equal(items[0].type, items[i].type)) {
            for (size_t k = 0; k <= i; k++) er_free(&items[k]);
            free(items);
            return cg_fail(cg, e->as.array_literal.items[i]->token,
                "codegen: heterogeneous array literal (all elements must share a type)");
        }
        if (cgtype_is_managed(items[i].type) && items[i].owns_ref) {
            cg_materialize_to_local(cg, &items[i], "_t");
        } else if (cgtype_is_aggregate(items[i].type) && items[i].owns_ref) {
            /* Step 4b-γ — fat aggregate carries +1 on its managed slots; the
             * scope's aggregate cleanup must drive the eventual release once
             * we hand the value off to feng_aggregate_assign. */
            cg_materialize_to_local(cg, &items[i], "_t");
        }
    }
    /* Choose the slot type. If a non-array narrowing target was supplied
     * (e.g. expected i32 but items are i64 numeric literals), the slot
     * uses expected_elem so that allocation size and read access agree. */
    CGType *elem;
    if (expected_elem != NULL && !nested_narrow &&
        cgtype_is_integer(expected_elem->kind) &&
        cgtype_is_integer(items[0].type->kind)) {
        elem = cgtype_clone(expected_elem);
    } else if (nested_narrow) {
        /* Items already match expected_elem after recursive emit. */
        elem = cgtype_clone(items[0].type);
    } else {
        elem = cgtype_clone(items[0].type);
    }
    if (!elem) {
        for (size_t k = 0; k < n; k++) er_free(&items[k]);
        free(items);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    char *arr_tmp = cg_fresh_temp(cg, "_arr");
    char *slots_tmp = cg_fresh_temp(cg, "_slots");
    char *elem_cty = cg_ctype_dup(elem);
    char *desc_expr = cg_array_element_descriptor(elem);
    if (!arr_tmp || !slots_tmp || !elem_cty || !desc_expr) {
        free(arr_tmp); free(slots_tmp); free(elem_cty); free(desc_expr);
        cgtype_free(elem);
        for (size_t k = 0; k < n; k++) er_free(&items[k]);
        free(items);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    bool elem_managed = cgtype_is_managed(elem);
    bool elem_aggregate = cgtype_is_aggregate(elem);
    const char *agg_desc = elem_aggregate ? cg_aggregate_field_desc_name(elem) : NULL;
    if (elem_aggregate && agg_desc == NULL) {
        free(arr_tmp); free(slots_tmp); free(elem_cty); free(desc_expr);
        cgtype_free(elem);
        for (size_t k = 0; k < n; k++) er_free(&items[k]);
        free(items);
        return cg_fail(cg, e->token,
            "codegen: missing aggregate descriptor for spec array element");
    }
    if (elem_aggregate) {
        /* Step 4b-γ §9.6 — aggregate-element arrays must use the kinded
         * factory so the cycle collector walks each element's managed
         * slots and so default_init seeds every slot before assignment. */
        buf_append_fmt(cg->cur_body,
            "    FengArray *%s = feng_array_new_kinded("
            "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &%s, NULL, sizeof(%s), (size_t)%zu);\n",
            arr_tmp, agg_desc, elem_cty, n);
    } else {
        buf_append_fmt(cg->cur_body,
            "    FengArray *%s = feng_array_new(%s, sizeof(%s), %s, (size_t)%zu);\n",
            arr_tmp, desc_expr, elem_cty, elem_managed ? "true" : "false", n);
    }
    buf_append_fmt(cg->cur_body,
        "    %s *%s = (%s *)feng_array_data(%s);\n",
        elem_cty, slots_tmp, elem_cty, arr_tmp);
    for (size_t i = 0; i < n; i++) {
        if (elem_aggregate) {
            /* feng_aggregate_assign releases the default-init'd slot (which
             * carries +1 from feng_array_new_kinded) and retains the new
             * value's managed slots — so both owns_ref temps and borrowed
             * sources arrive in the slot with the correct refcount. The
             * source temp's eventual cleanup balances the local +1. */
            buf_append_fmt(cg->cur_body,
                "    feng_aggregate_assign(&%s[%zu], &%s, &%s);\n",
                slots_tmp, i, items[i].c_expr, agg_desc);
        } else if (elem_managed) {
            /* Slots own +1 each: retain (items already materialised). */
            buf_append_fmt(cg->cur_body,
                "    %s[%zu] = %s; feng_retain(%s[%zu]);\n",
                slots_tmp, i, items[i].c_expr, slots_tmp, i);
        } else {
            buf_append_fmt(cg->cur_body,
                "    %s[%zu] = (%s)(%s);\n",
                slots_tmp, i, elem_cty, items[i].c_expr);
        }
    }
    free(slots_tmp); free(elem_cty); free(desc_expr);
    for (size_t k = 0; k < n; k++) er_free(&items[k]);
    free(items);

    out->c_expr = strdup(arr_tmp);
    free(arr_tmp);
    out->type = cgtype_new(CG_TYPE_ARRAY);
    if (!out->c_expr || !out->type) { cgtype_free(elem); return false; }
    out->type->element = elem;
    out->owns_ref = true;
    return true;
}

static bool cg_emit_array_literal(CG *cg, const FengExpr *e, ExprResult *out) {
    return cg_emit_array_literal_typed(cg, e, NULL, out);
}

static bool cg_emit_index(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    ExprResult recv;
    if (!cg_emit_expr(cg, e->as.index.object, &recv)) return false;
    if (recv.type->kind != CG_TYPE_ARRAY || !recv.type->element) {
        er_free(&recv);
        return cg_fail(cg, e->token,
            "codegen: indexing requires an array value");
    }
    if (cgtype_is_managed(recv.type) && recv.owns_ref) {
        cg_materialize_to_local(cg, &recv, "_t");
    }
    ExprResult idx;
    if (!cg_emit_expr(cg, e->as.index.index, &idx)) {
        er_free(&recv); return false;
    }
    if (!cgtype_is_integer(idx.type->kind)) {
        er_free(&idx); er_free(&recv);
        return cg_fail(cg, e->token, "codegen: array index must be an integer");
    }
    /* Materialize index into a `size_t` local so we can both bounds-check
     * and slot-load using the same value. */
    char *idx_tmp = cg_fresh_temp(cg, "_idx");
    if (!idx_tmp) { er_free(&idx); er_free(&recv); return false; }
    buf_append_fmt(cg->cur_body, "    size_t %s = (size_t)(%s);\n",
                   idx_tmp, idx.c_expr);
    buf_append_fmt(cg->cur_body, "    feng_array_check_index(%s, %s);\n",
                   recv.c_expr, idx_tmp);

    char *elem_cty = cg_ctype_dup(recv.type->element);
    if (!elem_cty) {
        free(idx_tmp); er_free(&idx); er_free(&recv); return false;
    }
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "(((%s *)feng_array_data(%s))[%s])",
                   elem_cty, recv.c_expr, idx_tmp);
    free(elem_cty); free(idx_tmp);
    out->c_expr = b.data;
    out->type = cgtype_clone(recv.type->element);
    out->owns_ref = false;   /* borrowed */
    er_free(&idx); er_free(&recv);
    return out->c_expr && out->type;
}

static bool cg_emit_cast(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);
    CGType *target = NULL;
    if (!cg_resolve_type(cg, e->as.cast.type, &e->token, &target)) return false;
    if (!cgtype_is_numeric(target->kind) && target->kind != CG_TYPE_BOOL) {
        cgtype_free(target);
        return cg_fail(cg, e->token, "codegen: only numeric/bool casts supported in 1A iter 1");
    }
    ExprResult inner;
    if (!cg_emit_expr(cg, e->as.cast.value, &inner)) { cgtype_free(target); return false; }
    if (!cgtype_is_numeric(inner.type->kind) && inner.type->kind != CG_TYPE_BOOL) {
        er_free(&inner); cgtype_free(target);
        return cg_fail(cg, e->token, "codegen: cast operand must be numeric/bool");
    }
    Buf b; buf_init(&b);
    buf_append_fmt(&b, "((%s)%s)", cgtype_to_c(target->kind), inner.c_expr);
    out->c_expr = b.data;
    out->type = target;
    er_free(&inner);
    return out->c_expr && out->type;
}

/* ===================== if / match expression emission =====================
 *
 * Per docs/feng-flow.md §4 the `if` expression form requires every branch
 * (then, every `else if`, and a mandatory `else`) to be a block whose final
 * statement is an expression statement. The block's value is that
 * expression's value; semantic analysis has already verified all branch
 * yield types unify, so codegen is purely a lowering exercise:
 *
 *   1. Discover the result type by emitting the then-branch yield expression
 *      into a throwaway buffer/scope (probe). This is safe because the only
 *      side-effects cg_emit_expr mutates outside the buffer (string-literal
 *      cache, witness-instance table, tmp_counter) are either idempotent or
 *      monotonic.
 *   2. Allocate a slot `_ifv_N` of the result type in the OUTER scope and
 *      register it on the Feng cleanup chain (managed kinds only). Any
 *      `return`/`throw` raised inside a branch body therefore traverses
 *      `_ifv_N` exactly once via cg_release_through, keeping the runtime
 *      cleanup chain consistent.
 *   3. For each branch: push a Feng scope, emit all leading statements via
 *      cg_emit_stmt, then evaluate the yield expression and assign into
 *      `_ifv_N` with the standard +1 transfer (steal on owns_ref, retain on
 *      borrow); release the branch scope before joining.
 *
 * Aggregate (fat-spec) results require the move-by-take machinery used in
 * cg_emit_return; that path is intentionally rejected with an explicit error
 * until a smoke exercises it.
 */

static const FengExpr *cg_branch_yield_expr(const FengBlock *block) {
    if (!block || block->statement_count == 0) return NULL;
    const FengStmt *last = block->statements[block->statement_count - 1];
    if (last->kind != FENG_STMT_EXPR) return NULL;
    return last->as.expr;
}

static bool cg_emit_branch_into_slot(CG *cg,
                                     const FengBlock *block,
                                     const char *ifv_name,
                                     const CGType *result_type,
                                     bool managed,
                                     FengToken err_token) {
    Scope *bsc = scope_push(cg->cur_scope);
    if (!bsc) return cg_fail(cg, err_token, "codegen: out of memory");
    cg->cur_scope = bsc;

    bool ok = true;
    /* Leading statements (everything except the trailing yield expression). */
    for (size_t i = 0; i + 1 < block->statement_count; i++) {
        if (!cg_emit_stmt(cg, block->statements[i])) { ok = false; break; }
    }

    if (ok) {
        const FengExpr *yield =
            block->statements[block->statement_count - 1]->as.expr;
        ExprResult r;
        if (!cg_emit_expr(cg, yield, &r)) {
            ok = false;
        } else if (!cg_types_equal(result_type, r.type)) {
            cg_fail(cg, err_token,
                "codegen: if-expression branches yield mismatched types");
            er_free(&r);
            ok = false;
        } else {
            if (managed) {
                if (r.owns_ref) {
                    buf_append_fmt(cg->cur_body,
                        "        %s = %s;\n", ifv_name, r.c_expr);
                } else {
                    buf_append_fmt(cg->cur_body,
                        "        %s = %s; if (%s) feng_retain(%s);\n",
                        ifv_name, r.c_expr, ifv_name, ifv_name);
                }
            } else {
                char *cty = cg_ctype_dup(result_type);
                if (!cty) {
                    er_free(&r);
                    cg->cur_scope = bsc->parent;
                    scope_pop_free(bsc);
                    return cg_fail(cg, err_token, "codegen: out of memory");
                }
                buf_append_fmt(cg->cur_body,
                    "        %s = (%s)(%s);\n", ifv_name, cty, r.c_expr);
                free(cty);
            }
            er_free(&r);
        }
    }

    if (ok) {
        cg_release_scope(cg, bsc);
    }
    cg->cur_scope = bsc->parent;
    scope_pop_free(bsc);
    return ok;
}

/* Probe the type of a branch's yield expression by emitting it into a
 * throwaway buffer and scope. Returns a heap-owned CGType clone on success
 * or NULL on failure. Caller owns the returned CGType. */
static CGType *cg_probe_branch_yield_type(CG *cg, const FengExpr *yield) {
    Buf throwaway; buf_init(&throwaway);
    Buf *saved_body = cg->cur_body;
    cg->cur_body = &throwaway;
    Scope *probe = scope_push(cg->cur_scope);
    if (!probe) {
        cg->cur_body = saved_body;
        buf_free(&throwaway);
        return NULL;
    }
    cg->cur_scope = probe;
    ExprResult r;
    bool ok = cg_emit_expr(cg, yield, &r);
    CGType *result = ok ? cgtype_clone(r.type) : NULL;
    er_free(&r);
    cg->cur_scope = probe->parent;
    scope_pop_free(probe);
    cg->cur_body = saved_body;
    buf_free(&throwaway);
    return result;
}

static bool cg_emit_if_expr(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);

    const FengBlock *then_b = e->as.if_expr.then_block;
    const FengBlock *else_b = e->as.if_expr.else_block;
    const FengExpr *then_yield = cg_branch_yield_expr(then_b);
    const FengExpr *else_yield = cg_branch_yield_expr(else_b);
    if (!then_yield || !else_yield) {
        return cg_fail(cg, e->token,
            "codegen: if-expression branches must end with an expression statement");
    }

    CGType *result_type = cg_probe_branch_yield_type(cg, then_yield);
    if (!result_type) return false;

    if (cgtype_is_aggregate(result_type)) {
        cgtype_free(result_type);
        return cg_fail(cg, e->token,
            "codegen: if-expression of spec (aggregate) type not yet supported");
    }
    bool managed = cgtype_is_managed(result_type);

    char *cond_tmp = cg_fresh_temp(cg, "_cond");
    char *ifv = cg_fresh_temp(cg, "_ifv");
    if (!cond_tmp || !ifv) {
        free(cond_tmp); free(ifv); cgtype_free(result_type);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }

    /* Emit the condition first; the slot decl follows so a panic raised by
     * the condition expression itself never observes a dangling cleanup
     * node for `_ifv` on the runtime chain. */
    ExprResult cond;
    if (!cg_emit_expr(cg, e->as.if_expr.condition, &cond)) {
        free(cond_tmp); free(ifv); cgtype_free(result_type);
        return false;
    }
    if (cond.type->kind != CG_TYPE_BOOL) {
        er_free(&cond);
        free(cond_tmp); free(ifv); cgtype_free(result_type);
        return cg_fail(cg, e->token,
            "codegen: if-expression condition must be bool");
    }
    buf_append_fmt(cg->cur_body, "    bool %s = %s;\n", cond_tmp, cond.c_expr);
    er_free(&cond);

    char *cty = cg_ctype_dup(result_type);
    if (!cty) {
        free(cond_tmp); free(ifv); cgtype_free(result_type);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    if (managed) {
        buf_append_fmt(cg->cur_body, "    %s %s = NULL;\n", cty, ifv);
        cg_emit_cleanup_push_for_managed_local(cg, ifv);
        if (!scope_add(cg->cur_scope, ifv, ifv,
                       cgtype_clone(result_type), false)) {
            free(cty); free(cond_tmp); free(ifv); cgtype_free(result_type);
            return cg_fail(cg, e->token, "codegen: out of memory");
        }
    } else {
        buf_append_fmt(cg->cur_body, "    %s %s = (%s)0;\n", cty, ifv, cty);
    }
    free(cty);

    buf_append_fmt(cg->cur_body, "    if (%s) {\n", cond_tmp);
    if (!cg_emit_branch_into_slot(cg, then_b, ifv, result_type, managed, e->token)) {
        free(cond_tmp); free(ifv); cgtype_free(result_type);
        return false;
    }
    buf_append_cstr(cg->cur_body, "    } else {\n");
    if (!cg_emit_branch_into_slot(cg, else_b, ifv, result_type, managed, e->token)) {
        free(cond_tmp); free(ifv); cgtype_free(result_type);
        return false;
    }
    buf_append_cstr(cg->cur_body, "    }\n");

    out->c_expr = strdup(ifv);
    out->type = result_type;
    /* For managed values the OUTER Feng scope owns the +1 (the slot was
     * registered above). For non-managed values it's a plain rvalue copy.
     * In both cases the surface contract is "borrowed-from-slot": owns_ref
     * is false, callers that need to retain do so themselves. */
    out->owns_ref = false;

    free(cond_tmp);
    free(ifv);
    return out->c_expr != NULL;
}

/* Build a C boolean expression matching `target_tmp` against a single match
 * label. Label literal expressions are emitted via cg_emit_expr; per
 * docs/feng-flow.md §3 they must be compile-time constants/let-literal
 * bindings, so the emitted C is a pure value with no scope side-effects.
 * String comparison uses inline length+memcmp lowering against the runtime's
 * FengString accessors so the runtime surface stays the value-model API set.
 */
static bool cg_emit_match_label_cond(CG *cg, const char *target_tmp,
                                     CGTypeKind target_kind,
                                     const FengMatchLabel *lab, Buf *out) {
    if (lab->kind == FENG_MATCH_LABEL_VALUE) {
        ExprResult lv;
        if (!cg_emit_expr(cg, lab->value, &lv)) return false;
        if (target_kind == CG_TYPE_STRING) {
            buf_append_fmt(out,
                "(bool)(feng_string_length(%s) == feng_string_length(%s) && "
                "memcmp(feng_string_data(%s), feng_string_data(%s), "
                "feng_string_length(%s)) == 0)",
                target_tmp, lv.c_expr,
                target_tmp, lv.c_expr,
                lv.c_expr);
        } else {
            buf_append_fmt(out, "(bool)(%s == %s)", target_tmp, lv.c_expr);
        }
        er_free(&lv);
        return true;
    }
    if (lab->kind == FENG_MATCH_LABEL_RANGE) {
        if (target_kind == CG_TYPE_STRING || target_kind == CG_TYPE_BOOL) {
            return cg_fail(cg, lab->token,
                "codegen: range labels apply to integer match targets only");
        }
        ExprResult lo, hi;
        if (!cg_emit_expr(cg, lab->range_low, &lo)) return false;
        if (!cg_emit_expr(cg, lab->range_high, &hi)) {
            er_free(&lo);
            return false;
        }
        buf_append_fmt(out, "(%s >= %s && %s <= %s)",
                       target_tmp, lo.c_expr, target_tmp, hi.c_expr);
        er_free(&lo);
        er_free(&hi);
        return true;
    }
    return cg_fail(cg, lab->token, "codegen: unknown match label kind");
}

static bool cg_emit_match_expr(CG *cg, const FengExpr *e, ExprResult *out) {
    er_init(out);

    if (!e->as.match_expr.else_block) {
        return cg_fail(cg, e->token,
            "codegen: match expression requires an else branch");
    }
    const FengExpr *else_yield =
        cg_branch_yield_expr(e->as.match_expr.else_block);
    if (!else_yield) {
        return cg_fail(cg, e->token,
            "codegen: match expression else branch must end with an expression statement");
    }
    for (size_t i = 0; i < e->as.match_expr.branch_count; i++) {
        if (!cg_branch_yield_expr(e->as.match_expr.branches[i].body)) {
            return cg_fail(cg, e->token,
                "codegen: match branch must end with an expression statement");
        }
    }

    CGType *result_type = cg_probe_branch_yield_type(cg, else_yield);
    if (!result_type) return false;
    if (cgtype_is_aggregate(result_type)) {
        cgtype_free(result_type);
        return cg_fail(cg, e->token,
            "codegen: match expression of spec (aggregate) type not yet supported");
    }
    bool managed = cgtype_is_managed(result_type);

    /* Materialise the target so it is evaluated exactly once and (for managed
     * targets like `string`) stays alive across every label comparison. */
    ExprResult tgt;
    if (!cg_emit_expr(cg, e->as.match_expr.target, &tgt)) {
        cgtype_free(result_type);
        return false;
    }
    CGTypeKind tk = tgt.type->kind;
    if (tk != CG_TYPE_BOOL && tk != CG_TYPE_STRING && !cgtype_is_integer(tk)) {
        er_free(&tgt);
        cgtype_free(result_type);
        return cg_fail(cg, e->token,
            "codegen: match target must be integer, bool, or string");
    }
    /* materialize_to_local registers managed +1 results into the current
     * scope and emits the cleanup_push, so the target survives every
     * comparison and is released at scope exit alongside other locals. */
    char *tgt_tmp = cg_materialize_to_local(cg, &tgt, "_mt");
    if (!tgt_tmp) {
        er_free(&tgt);
        cgtype_free(result_type);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    er_free(&tgt);

    char *ifv = cg_fresh_temp(cg, "_ifv");
    if (!ifv) {
        free(tgt_tmp); cgtype_free(result_type);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    char *cty = cg_ctype_dup(result_type);
    if (!cty) {
        free(ifv); free(tgt_tmp); cgtype_free(result_type);
        return cg_fail(cg, e->token, "codegen: out of memory");
    }
    if (managed) {
        buf_append_fmt(cg->cur_body, "    %s %s = NULL;\n", cty, ifv);
        cg_emit_cleanup_push_for_managed_local(cg, ifv);
        if (!scope_add(cg->cur_scope, ifv, ifv,
                       cgtype_clone(result_type), false)) {
            free(cty); free(ifv); free(tgt_tmp); cgtype_free(result_type);
            return cg_fail(cg, e->token, "codegen: out of memory");
        }
    } else {
        buf_append_fmt(cg->cur_body, "    %s %s = (%s)0;\n", cty, ifv, cty);
    }
    free(cty);

    bool first_branch = true;
    for (size_t i = 0; i < e->as.match_expr.branch_count; i++) {
        const FengMatchBranch *br = &e->as.match_expr.branches[i];
        if (br->label_count == 0) {
            free(ifv); free(tgt_tmp); cgtype_free(result_type);
            return cg_fail(cg, br->token,
                "codegen: match branch has no labels");
        }
        Buf cond; buf_init(&cond);
        bool cond_ok = true;
        for (size_t li = 0; li < br->label_count; li++) {
            if (li) buf_append_cstr(&cond, " || ");
            if (!cg_emit_match_label_cond(cg, tgt_tmp, tk, &br->labels[li], &cond)) {
                cond_ok = false;
                break;
            }
        }
        if (!cond_ok) {
            buf_free(&cond);
            free(ifv); free(tgt_tmp); cgtype_free(result_type);
            return false;
        }
        if (first_branch) {
            buf_append_fmt(cg->cur_body, "    if (%s) {\n", cond.data);
            first_branch = false;
        } else {
            buf_append_fmt(cg->cur_body, "    } else if (%s) {\n", cond.data);
        }
        buf_free(&cond);
        if (!cg_emit_branch_into_slot(cg, br->body, ifv, result_type, managed,
                                      e->token)) {
            free(ifv); free(tgt_tmp); cgtype_free(result_type);
            return false;
        }
    }
    if (first_branch) {
        /* No value branches — degenerate to a plain block running else only.
         * Wrap in a C block so the assignment-driven slot setup is consistent. */
        buf_append_cstr(cg->cur_body, "    {\n");
    } else {
        buf_append_cstr(cg->cur_body, "    } else {\n");
    }
    if (!cg_emit_branch_into_slot(cg, e->as.match_expr.else_block, ifv,
                                  result_type, managed, e->token)) {
        free(ifv); free(tgt_tmp); cgtype_free(result_type);
        return false;
    }
    buf_append_cstr(cg->cur_body, "    }\n");

    out->c_expr = strdup(ifv);
    out->type = result_type;
    out->owns_ref = false;

    free(tgt_tmp);
    free(ifv);
    return out->c_expr != NULL;
}

static bool cg_emit_expr(CG *cg, const FengExpr *e, ExprResult *out) {
    if (cg->failed) return false;
    bool ok;
    switch (e->kind) {
        case FENG_EXPR_BOOL:
        case FENG_EXPR_INTEGER:
        case FENG_EXPR_FLOAT:
        case FENG_EXPR_STRING:
            ok = cg_emit_literal(cg, e, out); break;
        case FENG_EXPR_IDENTIFIER:
            ok = cg_emit_identifier(cg, e, out); break;
        case FENG_EXPR_SELF: {
            const Local *l = scope_lookup(cg->cur_scope, "self", 4);
            if (!l) {
                return cg_fail(cg, e->token,
                    "codegen: 'self' used outside of method body");
            }
            er_init(out);
            out->c_expr = strdup(l->c_name);
            out->type = cgtype_clone(l->type);
            out->owns_ref = false;
            ok = out->c_expr && out->type;
            break;
        }
        case FENG_EXPR_BINARY:        ok = cg_emit_binary(cg, e, out); break;
        case FENG_EXPR_UNARY:         ok = cg_emit_unary(cg, e, out); break;
        case FENG_EXPR_CALL:          ok = cg_emit_call(cg, e, out); break;
        case FENG_EXPR_MEMBER:        ok = cg_emit_member(cg, e, out); break;
        case FENG_EXPR_OBJECT_LITERAL:ok = cg_emit_object_literal(cg, e, out); break;
        case FENG_EXPR_ARRAY_LITERAL: ok = cg_emit_array_literal(cg, e, out); break;
        case FENG_EXPR_INDEX:         ok = cg_emit_index(cg, e, out); break;
        case FENG_EXPR_CAST:          ok = cg_emit_cast(cg, e, out); break;
        case FENG_EXPR_IF:            ok = cg_emit_if_expr(cg, e, out); break;
        case FENG_EXPR_MATCH:         ok = cg_emit_match_expr(cg, e, out); break;
        default:
            return cg_fail(cg, e->token,
                "codegen: expression kind not yet supported in this iteration");
    }
    if (!ok) return false;

    /* Step 4b — apply spec coercion if the analyzer marked this expression as
     * a coercion site. For object-form, we wrap the produced object reference
     * into a fat-spec value `{ .subject = expr, .witness = &Witness }`. */
    const FengSpecCoercionSite *cs =
        feng_semantic_lookup_spec_coercion_site(cg->analysis, e);
    if (cs && cs->form == FENG_SPEC_COERCION_FORM_OBJECT) {
        if (!out->type || out->type->kind != CG_TYPE_OBJECT || !out->type->user) {
            return cg_fail(cg, e->token,
                "codegen: spec coercion source must be an object value");
        }
        const UserType *src_t = cg_find_user_type_by_decl(cg, cs->src_type_decl);
        const UserSpec *tgt_s = cg_find_user_spec_by_decl(cg, cs->target_spec_decl);
        if (!src_t || !tgt_s) {
            return cg_fail(cg, e->token,
                "codegen: spec coercion references decl outside current module (Step 4b-α only handles single-module)");
        }
        const char *witness_var = NULL;
        if (!cg_ensure_witness_instance(cg, src_t, tgt_s, e->token, &witness_var)) {
            return false;
        }
        /* Materialise the source so the subject expression evaluates exactly
         * once, then wrap it. The fat-spec value owns the +1 on `subject`. */
        cg_materialize_to_local(cg, out, "_t");
        Buf b; buf_init(&b);
        buf_append_fmt(&b,
            "((struct %s){ .subject = (void *)%s, .witness = &%s })",
            tgt_s->c_value_struct_name, out->c_expr, witness_var);
        free(out->c_expr); out->c_expr = b.data;
        cgtype_free(out->type);
        out->type = cgtype_new(CG_TYPE_SPEC);
        if (!out->type) return false;
        out->type->user_spec = tgt_s;
        /* The materialised local already retains subject; the spec value
         * borrows that reference, so the value itself is NOT owns_ref — the
         * receiving slot must retain `subject` if it wants to outlive the
         * source local's scope. */
        out->owns_ref = false;
    } else if (cs && cs->form == FENG_SPEC_COERCION_FORM_CALLABLE) {
        return cg_fail(cg, e->token,
            "codegen: callable-form spec coercion not yet supported (Step 4b-γ)");
    }
    return true;
}

/* ===================== statement emission ===================== */

static void cg_release_scope(CG *cg, const Scope *scope) {
    /* Walk in reverse insertion order to mirror C destruction. Each managed
     * non-param local was paired with a feng_cleanup_push at declaration; we
     * must pop the chain in strict LIFO order, then release+NULL the slot so
     * any later throw-driven walk skips it. */
    for (size_t i = scope->count; i > 0; i--) {
        const Local *l = &scope->items[i - 1];
        if (l->is_param) continue;
        if (cgtype_is_managed(l->type)) {
            buf_append_fmt(cg->cur_body,
                           "    feng_cleanup_pop(); feng_release(%s); %s = NULL;\n",
                           l->c_name, l->c_name);
        } else if (cgtype_is_aggregate(l->type)) {
            /* Step 4b-β — drop the 4b-α subject-shortcut and route through
             * the value-model aggregate API. The cleanup chain still holds
             * `&local.subject` because the only managed slot of an
             * object-form spec value is subject (single FENG_SLOT_POINTER).
             * Pop first so a panic raised inside the release walk does not
             * re-enter the same slot, then walk every managed slot via the
             * descriptor, then zero `subject` to keep the post-release
             * invariant aligned with the managed-local path. */
            const char *desc = cg_aggregate_field_desc_name(l->type);
            if (!desc) {
                /* unreachable today: AGGREGATE means object-form spec. */
                buf_append_fmt(cg->cur_body,
                    "    feng_panic(\"codegen: missing aggregate descriptor for %s\");\n",
                    l->c_name);
                continue;
            }
            buf_append_fmt(cg->cur_body,
                "    feng_cleanup_pop(); feng_aggregate_release(&%s, &%s); %s.subject = NULL;\n",
                l->c_name, desc, l->c_name);
        }
    }
}

/* Release all scopes from cg->cur_scope down to (but not including) `stop`.
 * Used by return / break / continue. */
static void cg_release_through(CG *cg, const Scope *stop) {
    for (const Scope *s = cg->cur_scope; s && s != stop; s = s->parent) {
        cg_release_scope(cg, s);
    }
}

/* Register a managed local on the per-thread cleanup chain so a throw passing
 * through the current scope releases it. The companion feng_cleanup_pop is
 * emitted by cg_release_scope. The companion node lives on the C stack right
 * next to the local so its lifetime matches. */
static void cg_emit_cleanup_push_for_managed_local(CG *cg, const char *cname) {
    buf_append_fmt(cg->cur_body,
                   "    FengCleanupNode _cu_%s; feng_cleanup_push(&_cu_%s, (void **)&%s);\n",
                   cname, cname, cname);
}

/* Step 4b — register the managed subject slot of a fat spec local on the
 * cleanup chain. The fat-spec ABI (codegen.h §value-model) places `subject`
 * at offset 0, but we still address it explicitly so a future ABI change
 * (extra header word) breaks loudly here. */
static void cg_emit_cleanup_push_for_aggregate_local(CG *cg, const char *cname) {
    buf_append_fmt(cg->cur_body,
                   "    FengCleanupNode _cu_%s; feng_cleanup_push(&_cu_%s, (void **)&%s.subject);\n",
                   cname, cname, cname);
}

/* ---------- default-zero emission ----------
 *
 * Feng has no `null`. Every binding without an explicit initializer (and
 * every object-literal field omitted by the user) takes the type's default
 * zero value, see docs/feng-builtin-type.md and docs/feng-type.md §5/§7.
 *
 * The string default is the process-wide IMMORTAL singleton produced by
 * feng_string_default(); the array default is a freshly allocated empty
 * FengArray with the right element descriptor; the object default is a
 * recursive zero instance produced by the per-type Feng__M__T__default_zero()
 * factory emitted alongside the type. Cyclic object types (self- or
 * mutually-referential) cannot have a finite default zero and must be
 * rejected at compile time. */

/* Recursive predicate: T is default-zero-safe iff T itself is acyclic AND
 * every managed object field of T is itself default-zero-safe. The recursion
 * terminates on the acyclic-types DAG. `visited` tracks the current DFS path
 * to keep the algorithm robust even if the cyclicity marker is somehow
 * inconsistent (defensive — feng_semantic_type_is_potentially_cyclic should
 * already catch true cycles). */
static bool cg_user_type_dz_safe_dfs(CG *cg, const UserType *t,
                                     const UserType **stack, size_t depth,
                                     size_t cap) {
    if (feng_semantic_type_is_potentially_cyclic(cg->analysis, t->decl)) return false;
    for (size_t i = 0; i < depth; i++) if (stack[i] == t) return false;
    if (depth + 1 > cap) return false;  /* defensive */
    stack[depth] = t;
    for (size_t i = 0; i < t->field_count; i++) {
        const CGType *ft = t->fields[i].type;
        if (ft->kind != CG_TYPE_OBJECT || !ft->user) continue;
        if (!cg_user_type_dz_safe_dfs(cg, ft->user, stack, depth + 1, cap)) {
            return false;
        }
    }
    return true;
}

static bool cg_user_type_is_default_zero_safe(CG *cg, const UserType *t) {
    /* Cap at user_type_count: any deeper path must contain a repeat, which
     * the cycle marker would already have flagged. */
    size_t cap = cg->user_type_count + 1;
    const UserType **stack = calloc(cap, sizeof *stack);
    if (!stack) return false;
    bool ok = cg_user_type_dz_safe_dfs(cg, t, stack, 0, cap);
    free(stack);
    return ok;
}

/* Emit a C expression that produces the default-zero value of `type` and
 * write it to *out_expr (caller-frees). For managed types the expression
 * always carries a freshly-owned reference (+1 owns_ref semantics), with the
 * exception of the IMMORTAL string singleton — it is also safe to treat as
 * "owns_ref" because feng_release/feng_retain are no-ops on IMMORTAL refs.
 *
 * Returns false (and reports through cg_fail) when `type` is not eligible
 * for default-zero (cyclic object type, or a type containing one). */
static bool cg_default_value_expr(CG *cg, const CGType *type,
                                  const FengToken *blame,
                                  char **out_expr) {
    Buf b; buf_init(&b);
    switch (type->kind) {
        case CG_TYPE_BOOL:
            buf_append_cstr(&b, "false"); break;
        case CG_TYPE_I8: case CG_TYPE_I16: case CG_TYPE_I32: case CG_TYPE_I64:
        case CG_TYPE_U8: case CG_TYPE_U16: case CG_TYPE_U32: case CG_TYPE_U64:
            buf_append_cstr(&b, "0"); break;
        case CG_TYPE_F32: case CG_TYPE_F64:
            buf_append_cstr(&b, "0.0"); break;
        case CG_TYPE_STRING:
            buf_append_cstr(&b, "feng_string_default()"); break;
        case CG_TYPE_ARRAY: {
            char *desc = cg_array_element_descriptor(type->element);
            /* Use full C type (including pointer star for managed elements)
             * so sizeof matches the per-slot storage actually allocated by
             * feng_array_new — see cg_emit_array_literal_typed for the
             * canonical pattern. */
            char *elem_cty = cg_ctype_dup(type->element);
            bool em = type->element ? cgtype_is_managed(type->element) : false;
            bool ea = type->element ? cgtype_is_aggregate(type->element) : false;
            if (ea) {
                /* Step 4b-γ §9.6 — aggregate-element arrays must encode the
                 * AGGREGATE element kind even at length 0 so the cycle
                 * collector tags the array correctly when later resized. */
                const char *agg_desc = cg_aggregate_field_desc_name(type->element);
                if (agg_desc == NULL) {
                    free(desc); free(elem_cty); buf_free(&b);
                    return cg_fail(cg, blame ? *blame : (FengToken){0},
                        "codegen: missing aggregate descriptor for spec array default-zero");
                }
                buf_append_fmt(&b,
                    "feng_array_new_kinded("
                    "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &%s, NULL, sizeof(%s), (size_t)0)",
                    agg_desc, elem_cty ? elem_cty : "void *");
            } else {
                buf_append_fmt(&b, "feng_array_new(%s, sizeof(%s), %s, (size_t)0)",
                               desc ? desc : "NULL",
                               elem_cty ? elem_cty : "void *",
                               em ? "true" : "false");
            }
            free(desc);
            free(elem_cty);
            break;
        }
        case CG_TYPE_OBJECT: {
            if (!type->user) {
                buf_free(&b);
                return cg_fail(cg, blame ? *blame : (FengToken){0},
                    "codegen: cannot default-zero an unresolved object type");
            }
            if (!cg_user_type_is_default_zero_safe(cg, type->user)) {
                buf_free(&b);
                return cg_fail(cg, blame ? *blame : (FengToken){0},
                    "codegen: type '%s' contains reference cycles and has no default zero value; provide an explicit initializer",
                    type->user->feng_name);
            }
            buf_append_fmt(&b, "%s()", type->user->c_default_zero_name);
            break;
        }
        default:
            buf_free(&b);
            return cg_fail(cg, blame ? *blame : (FengToken){0},
                "codegen: cannot produce default value for this type");
    }
    *out_expr = b.data ? b.data : strdup("0");
    return *out_expr != NULL;
}

static bool cg_emit_binding(CG *cg, const FengStmt *stmt) {
    const FengBinding *b = &stmt->as.binding;
    /* Determine type: explicit annotation else inferred from initializer. */
    CGType *decl_type = NULL;
    if (b->type) {
        if (!cg_resolve_type(cg, b->type, &b->token, &decl_type)) return false;
    }
    ExprResult init;
    bool has_init = b->initializer != NULL;
    if (has_init) {
        /* If the binding declares an array type and the initializer is an
         * array literal, narrow the literal's element type to the declared
         * one so allocation slot size matches subsequent reads. */
        bool ok;
        if (decl_type != NULL && decl_type->kind == CG_TYPE_ARRAY &&
            decl_type->element != NULL &&
            b->initializer->kind == FENG_EXPR_ARRAY_LITERAL) {
            ok = cg_emit_array_literal_typed(cg, b->initializer,
                                             decl_type->element, &init);
        } else {
            ok = cg_emit_expr(cg, b->initializer, &init);
        }
        if (!ok) {
            cgtype_free(decl_type); return false;
        }
        if (!decl_type) {
            decl_type = cgtype_clone(init.type);
        }
    } else {
        if (!decl_type) {
            return cg_fail(cg, b->token,
                "codegen: binding without type or initializer not supported");
        }
    }
    char *cname = cg_local_cname(cg, b->name.data, b->name.length);
    if (!cname) { if (has_init) er_free(&init); cgtype_free(decl_type); return false; }

    char *cty = cg_ctype_dup(decl_type);
    if (has_init) {
        if (cgtype_is_managed(decl_type)) {
            if (init.owns_ref) {
                /* Take the +1 directly. */
                buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, cname, init.c_expr);
            } else {
                /* Borrowed; retain into our slot. */
                buf_append_fmt(cg->cur_body, "    %s %s = %s; feng_retain(%s);\n",
                               cty, cname, init.c_expr, cname);
            }
        } else if (cgtype_is_aggregate(decl_type)) {
            /* Step 4b-β — fat spec value: by-value struct copy carries the
             * subject reference. If the producer was +1 (e.g., wrapped object
             * literal), take it directly; otherwise route through the
             * value-model aggregate retain so every managed slot of the
             * descriptor gets bumped (today subject only, future descriptors
             * may carry more). */
            if (init.owns_ref) {
                buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, cname, init.c_expr);
            } else {
                const char *desc = cg_aggregate_field_desc_name(decl_type);
                if (!desc) {
                    er_free(&init);
                    free(cty); free(cname); cgtype_free(decl_type);
                    return cg_fail(cg, b->token,
                        "codegen: missing aggregate descriptor for spec local");
                }
                buf_append_fmt(cg->cur_body,
                               "    %s %s = %s; feng_aggregate_retain(&%s, &%s);\n",
                               cty, cname, init.c_expr, cname, desc);
            }
        } else {
            buf_append_fmt(cg->cur_body, "    %s %s = (%s)(%s);\n",
                           cty, cname, cty, init.c_expr);
        }
        er_free(&init);
    } else {
        /* No initializer: emit the type's default zero value. Per
         * docs/feng-builtin-type.md & docs/feng-type.md §5/§7 every Feng
         * type has a finite default zero (Feng has no `null`). For managed
         * types the resulting reference is owned by this slot and joins the
         * cleanup chain just like an explicit initializer. */
        if (cgtype_is_aggregate(decl_type)) {
            /* Step 4b-β — spec default zero. The aggregate descriptor's
             * default_init function allocates a fresh subject and binds the
             * default witness; the resulting struct already carries a +1
             * reference on subject. */
            const char *desc = cg_aggregate_field_desc_name(decl_type);
            if (!desc) {
                free(cname); free(cty); cgtype_free(decl_type);
                return cg_fail(cg, b->token,
                    "codegen: missing aggregate descriptor for spec default-init");
            }
            buf_append_fmt(cg->cur_body,
                "    %s %s; feng_aggregate_default_init(&%s, &%s);\n",
                cty, cname, cname, desc);
        } else {
            char *def_expr = NULL;
            if (!cg_default_value_expr(cg, decl_type, &b->token, &def_expr)) {
                free(cname); free(cty); cgtype_free(decl_type); return false;
            }
            buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, cname, def_expr);
            free(def_expr);
        }
    }
    free(cty);

    if (!scope_add(cg->cur_scope, /*feng*/ "_unused_internal_name__", cname,
                   decl_type, false)) {
        free(cname); return false;
    }
    /* Replace the placeholder with the real Feng name. */
    Local *added = &cg->cur_scope->items[cg->cur_scope->count - 1];
    free(added->name);
    added->name = strndup(b->name.data, b->name.length);
    if (cgtype_is_managed(decl_type)) {
        cg_emit_cleanup_push_for_managed_local(cg, cname);
    } else if (cgtype_is_aggregate(decl_type)) {
        cg_emit_cleanup_push_for_aggregate_local(cg, cname);
    }
    free(cname);
    return true;
}

static bool cg_emit_assign(CG *cg, const FengStmt *stmt) {
    const FengExpr *target = stmt->as.assign.target;
    if (target->kind == FENG_EXPR_INDEX) {
        ExprResult recv;
        if (!cg_emit_expr(cg, target->as.index.object, &recv)) return false;
        if (recv.type->kind != CG_TYPE_ARRAY || !recv.type->element) {
            er_free(&recv);
            return cg_fail(cg, stmt->token,
                "codegen: indexed assignment requires an array value");
        }
        if (cgtype_is_managed(recv.type) && recv.owns_ref) {
            cg_materialize_to_local(cg, &recv, "_t");
        }
        ExprResult ix;
        if (!cg_emit_expr(cg, target->as.index.index, &ix)) {
            er_free(&recv); return false;
        }
        if (!cgtype_is_integer(ix.type->kind)) {
            er_free(&ix); er_free(&recv);
            return cg_fail(cg, stmt->token,
                "codegen: array index must be an integer");
        }
        char *idx_tmp = cg_fresh_temp(cg, "_idx");
        if (!idx_tmp) { er_free(&ix); er_free(&recv); return false; }
        buf_append_fmt(cg->cur_body, "    size_t %s = (size_t)(%s);\n",
                       idx_tmp, ix.c_expr);
        buf_append_fmt(cg->cur_body, "    feng_array_check_index(%s, %s);\n",
                       recv.c_expr, idx_tmp);
        ExprResult v;
        if (!cg_emit_expr(cg, stmt->as.assign.value, &v)) {
            free(idx_tmp); er_free(&ix); er_free(&recv); return false;
        }
        char *elem_cty = cg_ctype_dup(recv.type->element);
        if (!elem_cty) {
            free(idx_tmp); er_free(&v); er_free(&ix); er_free(&recv); return false;
        }
        if (cgtype_is_managed(recv.type->element)) {
            if (v.owns_ref) {
                buf_append_fmt(cg->cur_body,
                    "    { %s *_slots = (%s *)feng_array_data(%s);"
                    " void *_old = _slots[%s]; _slots[%s] = %s;"
                    " feng_release(_old); }\n",
                    elem_cty, elem_cty, recv.c_expr, idx_tmp, idx_tmp, v.c_expr);
            } else {
                buf_append_fmt(cg->cur_body,
                    "    feng_assign((void**)&((%s *)feng_array_data(%s))[%s], %s);\n",
                    elem_cty, recv.c_expr, idx_tmp, v.c_expr);
            }
        } else if (cgtype_is_aggregate(recv.type->element)) {
            /* Step 4b-γ §9.6 — aggregate slot write goes through
             * feng_aggregate_assign so the slot's old subject is released
             * and the new subject's managed slots get the right retain.
             * For owns_ref temps we still let aggregate cleanup at scope
             * exit drain the source's +1, mirroring local-binding semantics. */
            const char *agg_desc = cg_aggregate_field_desc_name(recv.type->element);
            if (agg_desc == NULL) {
                free(elem_cty); free(idx_tmp);
                er_free(&v); er_free(&ix); er_free(&recv);
                return cg_fail(cg, stmt->token,
                    "codegen: missing aggregate descriptor for spec array element write");
            }
            if (v.owns_ref) {
                cg_materialize_to_local(cg, &v, "_t");
            }
            buf_append_fmt(cg->cur_body,
                "    feng_aggregate_assign(&((%s *)feng_array_data(%s))[%s], &%s, &%s);\n",
                elem_cty, recv.c_expr, idx_tmp, v.c_expr, agg_desc);
        } else {
            buf_append_fmt(cg->cur_body,
                "    ((%s *)feng_array_data(%s))[%s] = (%s)(%s);\n",
                elem_cty, recv.c_expr, idx_tmp, elem_cty, v.c_expr);
        }
        free(elem_cty); free(idx_tmp);
        er_free(&v); er_free(&ix); er_free(&recv);
        return true;
    }
    if (target->kind == FENG_EXPR_MEMBER) {
        ExprResult recv;
        if (!cg_emit_expr(cg, target->as.member.object, &recv)) return false;
        if (recv.type->kind == CG_TYPE_SPEC && recv.type->user_spec) {
            /* Step 4b-β — spec field write via witness setter. */
            const UserSpec *us = recv.type->user_spec;
            const UserSpecMember *sm = cg_user_spec_member(us,
                target->as.member.member.data,
                target->as.member.member.length);
            if (!sm || sm->kind != USM_KIND_FIELD) {
                er_free(&recv);
                return cg_fail(cg, stmt->token,
                    "codegen: spec '%s' has no field '%.*s'",
                    us->feng_name,
                    (int)target->as.member.member.length,
                    target->as.member.member.data);
            }
            if (!sm->is_var) {
                er_free(&recv);
                return cg_fail(cg, stmt->token,
                    "codegen: spec field '%s' is not declared `var`",
                    sm->feng_name);
            }
            cg_materialize_to_local(cg, &recv, "_t");
            ExprResult v;
            if (!cg_emit_expr(cg, stmt->as.assign.value, &v)) {
                er_free(&recv); return false;
            }
            /* Setter takes ownership semantics inside the thunk (managed →
             * feng_assign). For an owns_ref RHS we must pass it as +1 and
             * the setter's feng_assign will retain+release; therefore we
             * release the +1 ourselves after the call to balance to a net
             * +0 store. For a borrow RHS we just pass through. */
            if (cgtype_is_managed(sm->type) && v.owns_ref) {
                buf_append_fmt(cg->cur_body,
                    "    { ");
                cg_emit_c_type(cg->cur_body, sm->type);
                buf_append_fmt(cg->cur_body,
                    " _v = %s; %s.witness->set_%s(%s.subject, _v); feng_release(_v); }\n",
                    v.c_expr, recv.c_expr, sm->c_field_name, recv.c_expr);
            } else {
                buf_append_fmt(cg->cur_body,
                    "    %s.witness->set_%s(%s.subject, %s);\n",
                    recv.c_expr, sm->c_field_name, recv.c_expr, v.c_expr);
            }
            er_free(&v);
            er_free(&recv);
            return true;
        }
        if (recv.type->kind != CG_TYPE_OBJECT || !recv.type->user) {
            er_free(&recv);
            return cg_fail(cg, stmt->token,
                "codegen: member assignment on non-object value");
        }
        const UserType *ut = recv.type->user;
        const UserField *uf = cg_user_type_field(ut,
            target->as.member.member.data, target->as.member.member.length);
        if (!uf) {
            er_free(&recv);
            return cg_fail(cg, stmt->token,
                "codegen: type '%s' has no field '%.*s'",
                ut->feng_name,
                (int)target->as.member.member.length,
                target->as.member.member.data);
        }
        if (cgtype_is_managed(recv.type) && recv.owns_ref) {
            cg_materialize_to_local(cg, &recv, "_t");
        }
        ExprResult v;
        if (!cg_emit_expr(cg, stmt->as.assign.value, &v)) {
            er_free(&recv); return false;
        }
        if (cgtype_is_managed(uf->type)) {
            if (v.owns_ref) {
                buf_append_fmt(cg->cur_body,
                    "    { void *_old = (%s)->%s; (%s)->%s = %s; feng_release(_old); }\n",
                    recv.c_expr, uf->c_name, recv.c_expr, uf->c_name, v.c_expr);
            } else {
                buf_append_fmt(cg->cur_body,
                    "    feng_assign((void**)&(%s)->%s, %s);\n",
                    recv.c_expr, uf->c_name, v.c_expr);
            }
        } else {
            char *cty = cg_ctype_dup(uf->type);
            buf_append_fmt(cg->cur_body, "    (%s)->%s = (%s)(%s);\n",
                           recv.c_expr, uf->c_name, cty, v.c_expr);
            free(cty);
        }
        er_free(&v);
        er_free(&recv);
        return true;
    }
    if (target->kind != FENG_EXPR_IDENTIFIER) {
        return cg_fail(cg, stmt->token,
            "codegen: only identifier or member assignments supported in this iteration");
    }
    const FengSlice n = target->as.identifier;
    const Local *l = scope_lookup(cg->cur_scope, n.data, n.length);
    if (!l) {
        const ModuleBinding *mb = cg_find_module_binding(cg, n.data, n.length);
        if (!mb) {
            return cg_fail(cg, stmt->token,
                "codegen: assignment to undefined identifier '%.*s'",
                (int)n.length, n.data);
        }
        if (!mb->is_var) {
            return cg_fail(cg, stmt->token,
                "codegen: cannot assign to immutable module binding '%s'",
                mb->feng_name);
        }
        ExprResult v;
        if (!cg_emit_expr(cg, stmt->as.assign.value, &v)) return false;
        if (cgtype_is_managed(mb->type)) {
            if (v.owns_ref) {
                buf_append_fmt(cg->cur_body,
                    "    { void *_old = %s; %s = %s; feng_release(_old); }\n",
                    mb->c_name, mb->c_name, v.c_expr);
            } else {
                buf_append_fmt(cg->cur_body,
                    "    feng_assign((void**)&%s, %s);\n", mb->c_name, v.c_expr);
            }
        } else {
            char *cty = cg_ctype_dup(mb->type);
            buf_append_fmt(cg->cur_body, "    %s = (%s)(%s);\n",
                           mb->c_name, cty, v.c_expr);
            free(cty);
        }
        er_free(&v);
        return true;
    }
    ExprResult v;
    if (!cg_emit_expr(cg, stmt->as.assign.value, &v)) return false;
    if (cgtype_is_managed(l->type)) {
        if (v.owns_ref) {
            /* Release old, take +1. */
            buf_append_fmt(cg->cur_body,
                "    { void *_old = %s; %s = %s; feng_release(_old); }\n",
                l->c_name, l->c_name, v.c_expr);
        } else {
            buf_append_fmt(cg->cur_body,
                "    feng_assign((void**)&%s, %s);\n", l->c_name, v.c_expr);
        }
    } else {
        char *cty = cg_ctype_dup(l->type);
        buf_append_fmt(cg->cur_body, "    %s = (%s)(%s);\n",
                       l->c_name, cty, v.c_expr);
        free(cty);
    }
    er_free(&v);
    return true;
}

static bool cg_emit_return(CG *cg, const FengStmt *stmt) {
    if (cg->try_depth > 0) {
        return cg_fail(cg, stmt->token,
            "codegen: 'return' inside try/catch/finally is not yet supported in Phase 1A");
    }
    if (!cg->cur_return_type ||
        cg->cur_return_type->kind == CG_TYPE_VOID) {
        if (stmt->as.return_value) {
            return cg_fail(cg, stmt->token,
                "codegen: void function cannot return a value");
        }
        cg_release_through(cg, NULL);
        if (cg->cur_fn_is_main) {
            buf_append_cstr(cg->cur_body, "    return;\n");
        } else {
            buf_append_cstr(cg->cur_body, "    return;\n");
        }
        return true;
    }
    if (!stmt->as.return_value) {
        return cg_fail(cg, stmt->token,
            "codegen: non-void function must return a value");
    }
    ExprResult r;
    if (!cg_emit_expr(cg, stmt->as.return_value, &r)) return false;
    /* Non-managed: emit cleanup then return. Managed: must transfer +1 out:
     * - if r.owns_ref, store in temp, release scopes, return temp.
     * - else (borrowed), retain, release scopes, return retained. */
    if (cgtype_is_managed(r.type)) {
        char *tmp = cg_fresh_temp(cg, "_ret");
        char *cty = cg_ctype_dup(r.type);
        if (!r.owns_ref) {
            buf_append_fmt(cg->cur_body, "    %s %s = %s; feng_retain(%s);\n",
                           cty, tmp, r.c_expr, tmp);
        } else {
            buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, tmp, r.c_expr);
        }
        free(cty);
        cg_release_through(cg, NULL);
        buf_append_fmt(cg->cur_body, "    return %s;\n", tmp);
        free(tmp);
    } else if (cgtype_is_aggregate(r.type)) {
        /* Step 4b-γ-2 — fat spec value: transfer +1 on every managed slot
         * out of the function. Two source flavours:
         *   • borrowed (owns_ref=false): bump every managed slot via
         *     feng_aggregate_retain so the caller's _ret carries +1
         *     independent of the source's lifetime.
         *   • +1 rvalue (owns_ref=true, e.g. another spec-returning call,
         *     a coerced object, a default-init binding): hoist into a
         *     scope-tracked temp via cg_materialize_to_local (so its
         *     subject lifetime is anchored, including across panics) and
         *     then move the bytes through feng_aggregate_take. take()
         *     nulls the source's managed slots so the cleanup-chain
         *     release at scope exit is a well-defined no-op (per
         *     feng_aggregate.c §take), which is exactly the documented
         *     spec-return move semantics (dev/feng-spec-codegen-pending
         *     §13.3.γ-2). _ret itself is null-initialised before take so
         *     take's "release dst slots first" precondition holds.
         */
        const char *desc = cg_aggregate_field_desc_name(r.type);
        if (!desc) {
            er_free(&r);
            return cg_fail(cg, stmt->token,
                "codegen: missing aggregate descriptor for spec return");
        }
        char *tmp = cg_fresh_temp(cg, "_ret");
        char *cty = cg_ctype_dup(r.type);
        if (!r.owns_ref) {
            buf_append_fmt(cg->cur_body,
                "    %s %s = %s; feng_aggregate_retain(&%s, &%s);\n",
                cty, tmp, r.c_expr, tmp, desc);
        } else {
            cg_materialize_to_local(cg, &r, "_t");
            buf_append_fmt(cg->cur_body,
                "    %s %s; memset(&%s, 0, sizeof %s);"
                " feng_aggregate_take(&%s, &%s, &%s);\n",
                cty, tmp, tmp, tmp, tmp, r.c_expr, desc);
        }
        free(cty);
        cg_release_through(cg, NULL);
        buf_append_fmt(cg->cur_body, "    return %s;\n", tmp);
        free(tmp);
    } else {
        char *tmp = cg_fresh_temp(cg, "_ret");
        char *cty = cg_ctype_dup(r.type);
        buf_append_fmt(cg->cur_body, "    %s %s = (%s)(%s);\n",
                       cty, tmp, cty, r.c_expr);
        free(cty);
        cg_release_through(cg, NULL);
        buf_append_fmt(cg->cur_body, "    return %s;\n", tmp);
        free(tmp);
    }
    er_free(&r);
    return true;
}

static bool cg_emit_if(CG *cg, const FengStmt *stmt) {
    for (size_t i = 0; i < stmt->as.if_stmt.clause_count; i++) {
        const FengIfClause *c = &stmt->as.if_stmt.clauses[i];
        ExprResult cond;
        if (!cg_emit_expr(cg, c->condition, &cond)) return false;
        if (cond.type->kind != CG_TYPE_BOOL) {
            er_free(&cond);
            return cg_fail(cg, c->token, "codegen: if condition must be bool");
        }
        buf_append_fmt(cg->cur_body, "    %sif (%s) {\n",
                       i == 0 ? "" : "else ", cond.c_expr);
        er_free(&cond);
        if (!cg_emit_block(cg, c->block)) return false;
        buf_append_cstr(cg->cur_body, "    }\n");
    }
    if (stmt->as.if_stmt.else_block) {
        buf_append_cstr(cg->cur_body, "    else {\n");
        if (!cg_emit_block(cg, stmt->as.if_stmt.else_block)) return false;
        buf_append_cstr(cg->cur_body, "    }\n");
    }
    return true;
}

static bool cg_emit_while(CG *cg, const FengStmt *stmt) {
    /* Emit as `for (;;)` so we can re-evaluate the condition each iter
     * inside a scope that releases temporaries from condition eval. */
    buf_append_cstr(cg->cur_body, "    for (;;) {\n");
    /* Condition scope: any temporaries from cond eval get released here. */
    Scope *cond_scope = scope_push(cg->cur_scope);
    if (!cond_scope) return cg_fail(cg, stmt->token, "codegen: out of memory");
    cg->cur_scope = cond_scope;
    ExprResult cond;
    if (!cg_emit_expr(cg, stmt->as.while_stmt.condition, &cond)) {
        cg->cur_scope = cond_scope->parent;
        scope_pop_free(cond_scope);
        return false;
    }
    if (cond.type->kind != CG_TYPE_BOOL) {
        er_free(&cond);
        cg->cur_scope = cond_scope->parent;
        scope_pop_free(cond_scope);
        return cg_fail(cg, stmt->token, "codegen: while condition must be bool");
    }
    char *cond_tmp = cg_fresh_temp(cg, "_cond");
    buf_append_fmt(cg->cur_body, "        bool %s = %s;\n", cond_tmp, cond.c_expr);
    er_free(&cond);
    cg_release_scope(cg, cond_scope);
    cg->cur_scope = cond_scope->parent;
    scope_pop_free(cond_scope);
    buf_append_fmt(cg->cur_body, "        if (!%s) break;\n", cond_tmp);
    free(cond_tmp);

    cg->loop_depth++;
    Scope *body_scope = scope_push(cg->cur_scope);
    if (!body_scope) { cg->loop_depth--; return cg_fail(cg, stmt->token, "codegen: out of memory"); }
    body_scope->is_loop = true;
    cg->cur_scope = body_scope;
    if (!cg_emit_block(cg, stmt->as.while_stmt.body)) {
        cg->cur_scope = body_scope->parent;
        scope_pop_free(body_scope);
        cg->loop_depth--;
        return false;
    }
    cg_release_scope(cg, body_scope);
    cg->cur_scope = body_scope->parent;
    scope_pop_free(body_scope);
    cg->loop_depth--;
    buf_append_cstr(cg->cur_body, "    }\n");
    return true;
}

/* `for` statement — supports both forms documented in docs/feng-flow.md §6:
 *
 *   1. Three-clause:  for [init]; [cond]; [update] { body }
 *   2. for/in       :  for let|var IT in SEQ { body }
 *
 * Compilation strategy (production-grade, mirroring `while` for ARC and
 * cleanup-chain bookkeeping; jumps are routed through goto-labels so a
 * `continue` runs the update step before re-entering the loop):
 *
 *   {                              // outer scope
 *       <init or seq materialise>  // lives until loop exits
 *       for (;;) {
 *           bool _cond_N;
 *           { <cond eval>; _cond_N = ...; }   // condition scope (release temps)
 *           if (!_cond_N) break;
 *           <body>                 // body scope = loop scope; continue_label set
 *           _cont_N: ;             // continue lands here
 *           { <update> }           // update scope (release temps)
 *       }
 *   }
 *
 * For `for/in`, the iteration sequence is materialised once into the outer
 * scope so its +1 reference is held for the whole loop. Each iteration the
 * body scope re-declares the iter binding, retaining the array slot value
 * if it is managed; the per-iter release happens through the body scope's
 * normal release path (and through the break/continue path via the
 * inclusive release added in cg_emit_break_continue).
 *
 * Limitations honoured here:
 *   - The semantic analyzer (analyzer.c) restricts the for/in sequence to
 *     `T[]` / `T[]!` and ensures the iter binding's mutability matches use.
 *   - try-frame crossing is rejected by cg_emit_break_continue (Phase 1A).
 */
static bool cg_emit_for_three(CG *cg, const FengStmt *stmt) {
    int id = cg->label_counter++;
    char cont_label[64];
    snprintf(cont_label, sizeof cont_label, "_cont_%d", id);

    buf_append_cstr(cg->cur_body, "    {\n");

    /* Outer scope: holds the optional `for (init; ...)` binding. The init
     * may be a binding or an assignment / expression statement. We push a
     * scope so binding-form inits are released when the loop exits, and so
     * a binding declared here is invisible outside the for. */
    Scope *outer_scope = scope_push(cg->cur_scope);
    if (!outer_scope) return cg_fail(cg, stmt->token, "codegen: out of memory");
    cg->cur_scope = outer_scope;

    if (stmt->as.for_stmt.init != NULL) {
        if (!cg_emit_stmt(cg, stmt->as.for_stmt.init)) {
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return false;
        }
    }

    buf_append_cstr(cg->cur_body, "    for (;;) {\n");

    /* Condition: optional. Empty condition means "always true" per spec. */
    if (stmt->as.for_stmt.condition != NULL) {
        Scope *cond_scope = scope_push(cg->cur_scope);
        if (!cond_scope) {
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return cg_fail(cg, stmt->token, "codegen: out of memory");
        }
        cg->cur_scope = cond_scope;
        ExprResult cond;
        if (!cg_emit_expr(cg, stmt->as.for_stmt.condition, &cond)) {
            cg->cur_scope = cond_scope->parent;
            scope_pop_free(cond_scope);
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return false;
        }
        if (cond.type->kind != CG_TYPE_BOOL) {
            er_free(&cond);
            cg->cur_scope = cond_scope->parent;
            scope_pop_free(cond_scope);
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return cg_fail(cg, stmt->token,
                "codegen: for condition must be bool");
        }
        char *cond_tmp = cg_fresh_temp(cg, "_fcond");
        buf_append_fmt(cg->cur_body, "        bool %s = %s;\n",
                       cond_tmp, cond.c_expr);
        er_free(&cond);
        cg_release_scope(cg, cond_scope);
        cg->cur_scope = cond_scope->parent;
        scope_pop_free(cond_scope);
        buf_append_fmt(cg->cur_body, "        if (!%s) break;\n", cond_tmp);
        free(cond_tmp);
    }

    /* Body: loop scope, with continue_label so `continue` jumps to update. */
    cg->loop_depth++;
    Scope *body_scope = scope_push(cg->cur_scope);
    if (!body_scope) {
        cg->loop_depth--;
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }
    body_scope->is_loop = true;
    /* Stash label in a stable buffer associated with cur_body — we use the
     * outer scope as the lifetime anchor by allocating on the heap and
     * tracking via the items list: simpler to embed the literal in body_scope
     * via a strdup that we free after the loop closes. */
    char *cont_label_owned = strdup(cont_label);
    body_scope->continue_label = cont_label_owned;
    cg->cur_scope = body_scope;
    if (!cg_emit_block(cg, stmt->as.for_stmt.body)) {
        cg->cur_scope = body_scope->parent;
        body_scope->continue_label = NULL;
        scope_pop_free(body_scope);
        free(cont_label_owned);
        cg->loop_depth--;
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return false;
    }
    cg_release_scope(cg, body_scope);
    cg->cur_scope = body_scope->parent;
    body_scope->continue_label = NULL;
    scope_pop_free(body_scope);
    free(cont_label_owned);
    cg->loop_depth--;

    /* Continue label: lands here after body, before update. */
    buf_append_fmt(cg->cur_body, "        %s: ;\n", cont_label);

    /* Update: optional. Wrapped in its own scope so any temporaries from
     * the update statement are released before the next iteration. */
    if (stmt->as.for_stmt.update != NULL) {
        Scope *upd_scope = scope_push(cg->cur_scope);
        if (!upd_scope) {
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return cg_fail(cg, stmt->token, "codegen: out of memory");
        }
        cg->cur_scope = upd_scope;
        if (!cg_emit_stmt(cg, stmt->as.for_stmt.update)) {
            cg->cur_scope = upd_scope->parent;
            scope_pop_free(upd_scope);
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return false;
        }
        cg_release_scope(cg, upd_scope);
        cg->cur_scope = upd_scope->parent;
        scope_pop_free(upd_scope);
    }

    buf_append_cstr(cg->cur_body, "    }\n");

    /* Outer scope cleanup (init binding release). */
    cg_release_scope(cg, outer_scope);
    cg->cur_scope = outer_scope->parent;
    scope_pop_free(outer_scope);
    buf_append_cstr(cg->cur_body, "    }\n");
    return true;
}

static bool cg_emit_for_in(CG *cg, const FengStmt *stmt) {
    int id = cg->label_counter++;
    char cont_label[64];
    snprintf(cont_label, sizeof cont_label, "_cont_%d", id);

    buf_append_cstr(cg->cur_body, "    {\n");

    /* Outer scope: holds the materialised iteration sequence (so its +1
     * reference outlives the loop) and the loop index. */
    Scope *outer_scope = scope_push(cg->cur_scope);
    if (!outer_scope) return cg_fail(cg, stmt->token, "codegen: out of memory");
    cg->cur_scope = outer_scope;

    ExprResult seq;
    if (!cg_emit_expr(cg, stmt->as.for_stmt.iter_expr, &seq)) {
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return false;
    }
    if (seq.type->kind != CG_TYPE_ARRAY || seq.type->element == NULL) {
        er_free(&seq);
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token,
            "codegen: for/in sequence must be an array");
    }
    /* Stash the element type before materialise possibly invalidates seq.type's
     * lifetime via cgtype clone semantics. cg_materialize_to_local hands
     * ownership of the type to the scope's Local. */
    CGType *element_type = cgtype_clone(seq.type->element);
    if (!element_type) {
        er_free(&seq);
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }
    /* Materialise the sequence into a local that owns +1 if it's an owning
     * temp; if it was borrowed (e.g. a bound var read), we still hold the
     * borrow for the loop's lifetime — but to be safe across mutation we
     * retain into a fresh slot. */
    char *seq_tmp;
    if (seq.owns_ref) {
        seq_tmp = cg_materialize_to_local(cg, &seq, "_fseq");
    } else {
        /* Borrowed: retain into a managed local so a subsequent re-assignment
         * to the source variable does not drop the array we are iterating. */
        seq_tmp = cg_fresh_temp(cg, "_fseq");
        if (seq_tmp) {
            buf_append_fmt(cg->cur_body,
                "    FengArray *%s = %s; feng_retain(%s);\n",
                seq_tmp, seq.c_expr, seq_tmp);
            scope_add(cg->cur_scope, seq_tmp, seq_tmp,
                      cgtype_clone(seq.type), false);
            cg_emit_cleanup_push_for_managed_local(cg, seq_tmp);
        }
        er_free(&seq);
    }
    if (!seq_tmp) {
        cgtype_free(element_type);
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }

    char *idx_var = cg_fresh_temp(cg, "_fidx");
    buf_append_fmt(cg->cur_body, "    size_t %s = 0;\n", idx_var);

    buf_append_cstr(cg->cur_body, "    for (;;) {\n");
    buf_append_fmt(cg->cur_body,
                   "        if (%s >= feng_array_length(%s)) break;\n",
                   idx_var, seq_tmp);

    /* Body scope: loop scope, hosts the per-iteration iter binding. */
    cg->loop_depth++;
    Scope *body_scope = scope_push(cg->cur_scope);
    if (!body_scope) {
        free(idx_var); free(seq_tmp); cgtype_free(element_type);
        cg->loop_depth--;
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }
    body_scope->is_loop = true;
    char *cont_label_owned = strdup(cont_label);
    body_scope->continue_label = cont_label_owned;
    cg->cur_scope = body_scope;

    /* Declare the iter binding for this iteration. It joins body_scope so
     * the existing release path (normal-end + break/continue inclusive
     * release) handles managed/aggregate ARC correctly. */
    const FengBinding *ib = &stmt->as.for_stmt.iter_binding;
    char *iter_cname = cg_local_cname(cg, ib->name.data, ib->name.length);
    if (!iter_cname) {
        free(cont_label_owned);
        body_scope->continue_label = NULL;
        cg->cur_scope = body_scope->parent;
        scope_pop_free(body_scope);
        free(idx_var); free(seq_tmp); cgtype_free(element_type);
        cg->loop_depth--;
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }
    char *elem_cty = cg_ctype_dup(element_type);
    /* Slot read: ((T*)feng_array_data(seq))[idx]. */
    if (cgtype_is_managed(element_type)) {
        buf_append_fmt(cg->cur_body,
            "        %s %s = ((%s *)feng_array_data(%s))[%s]; feng_retain(%s);\n",
            elem_cty, iter_cname, elem_cty, seq_tmp, idx_var, iter_cname);
    } else if (cgtype_is_aggregate(element_type)) {
        const char *desc = cg_aggregate_field_desc_name(element_type);
        if (!desc) {
            free(elem_cty); free(iter_cname); free(cont_label_owned);
            body_scope->continue_label = NULL;
            cg->cur_scope = body_scope->parent;
            scope_pop_free(body_scope);
            free(idx_var); free(seq_tmp); cgtype_free(element_type);
            cg->loop_depth--;
            cg_release_scope(cg, outer_scope);
            cg->cur_scope = outer_scope->parent;
            scope_pop_free(outer_scope);
            return cg_fail(cg, stmt->token,
                "codegen: missing aggregate descriptor for spec for/in element");
        }
        buf_append_fmt(cg->cur_body,
            "        %s %s = ((%s *)feng_array_data(%s))[%s]; feng_aggregate_retain(&%s, &%s);\n",
            elem_cty, iter_cname, elem_cty, seq_tmp, idx_var, iter_cname, desc);
    } else {
        buf_append_fmt(cg->cur_body,
            "        %s %s = ((%s *)feng_array_data(%s))[%s];\n",
            elem_cty, iter_cname, elem_cty, seq_tmp, idx_var);
    }
    free(elem_cty);
    /* Register iter binding in body scope under its Feng name and arrange
     * cleanup. We mirror cg_emit_binding's tail. */
    if (!scope_add(cg->cur_scope, "_unused_internal_name__", iter_cname,
                   cgtype_clone(element_type), false)) {
        free(iter_cname); free(cont_label_owned);
        body_scope->continue_label = NULL;
        cg->cur_scope = body_scope->parent;
        scope_pop_free(body_scope);
        free(idx_var); free(seq_tmp); cgtype_free(element_type);
        cg->loop_depth--;
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }
    Local *added = &cg->cur_scope->items[cg->cur_scope->count - 1];
    free(added->name);
    added->name = strndup(ib->name.data, ib->name.length);
    if (cgtype_is_managed(element_type)) {
        cg_emit_cleanup_push_for_managed_local(cg, iter_cname);
    } else if (cgtype_is_aggregate(element_type)) {
        cg_emit_cleanup_push_for_aggregate_local(cg, iter_cname);
    }
    free(iter_cname);

    if (!cg_emit_block(cg, stmt->as.for_stmt.body)) {
        body_scope->continue_label = NULL;
        cg->cur_scope = body_scope->parent;
        scope_pop_free(body_scope);
        free(cont_label_owned);
        free(idx_var); free(seq_tmp); cgtype_free(element_type);
        cg->loop_depth--;
        cg_release_scope(cg, outer_scope);
        cg->cur_scope = outer_scope->parent;
        scope_pop_free(outer_scope);
        return false;
    }
    cg_release_scope(cg, body_scope);
    body_scope->continue_label = NULL;
    cg->cur_scope = body_scope->parent;
    scope_pop_free(body_scope);
    free(cont_label_owned);
    cg->loop_depth--;

    /* Continue label: lands here after body, before index advance. */
    buf_append_fmt(cg->cur_body, "        %s: ;\n", cont_label);
    buf_append_fmt(cg->cur_body, "        %s++;\n", idx_var);
    buf_append_cstr(cg->cur_body, "    }\n");

    free(idx_var); free(seq_tmp); cgtype_free(element_type);

    cg_release_scope(cg, outer_scope);
    cg->cur_scope = outer_scope->parent;
    scope_pop_free(outer_scope);
    buf_append_cstr(cg->cur_body, "    }\n");
    return true;
}

static bool cg_emit_for(CG *cg, const FengStmt *stmt) {
    if (stmt->as.for_stmt.is_for_in) {
        return cg_emit_for_in(cg, stmt);
    }
    return cg_emit_for_three(cg, stmt);
}

static bool cg_emit_break_continue(CG *cg, const FengStmt *stmt, bool is_break) {
    if (cg->loop_depth == 0) {
        return cg_fail(cg, stmt->token,
            "codegen: '%s' outside of loop", is_break ? "break" : "continue");
    }
    /* Release scopes up to (but not including) the nearest loop scope. */
    const Scope *stop = NULL;
    for (const Scope *s = cg->cur_scope; s; s = s->parent) {
        if (s->is_loop) { stop = s; break; }
    }
    /* Refuse to jump across an enclosing try frame: longjmp-based unwind is
     * not wired into break/continue paths yet (Phase 1A limitation). */
    if (stop != NULL && cg->try_depth > stop->try_depth_at_entry) {
        return cg_fail(cg, stmt->token,
            "codegen: '%s' that crosses a try/catch/finally is not yet supported in Phase 1A",
            is_break ? "break" : "continue");
    }
    cg_release_through(cg, stop);
    /* Also release the loop scope itself: the C `break;` / `continue;` (or
     * the `goto _cont_N;` for three-clause `for`) jumps past the body's
     * closing brace, which is where the body-scope releases would otherwise
     * be emitted. Without this, any managed local declared at body-level
     * would leak (and unbalance the per-thread cleanup chain) on the
     * break / continue path. The normal end-of-iteration release path is
     * mutually exclusive with these jumps, so the slot is released exactly
     * once on every control-flow path. */
    if (stop != NULL) {
        cg_release_scope(cg, stop);
    }
    if (!is_break && stop != NULL && stop->continue_label != NULL) {
        buf_append_fmt(cg->cur_body, "    goto %s;\n", stop->continue_label);
    } else {
        buf_append_fmt(cg->cur_body, "    %s;\n", is_break ? "break" : "continue");
    }
    return true;
}

static bool cg_emit_expr_stmt(CG *cg, const FengStmt *stmt) {
    /* Open a fresh inner scope so any +1 temporaries from the expression
     * are released at the statement boundary. */
    Scope *st_scope = scope_push(cg->cur_scope);
    if (!st_scope) return cg_fail(cg, stmt->token, "codegen: out of memory");
    cg->cur_scope = st_scope;
    buf_append_cstr(cg->cur_body, "    {\n");
    ExprResult r;
    if (!cg_emit_expr(cg, stmt->as.expr, &r)) {
        cg->cur_scope = st_scope->parent;
        scope_pop_free(st_scope);
        return false;
    }
    /* Materialise managed +1 results into a temp so they're released. */
    if (cgtype_is_managed(r.type) && r.owns_ref) {
        cg_materialize_to_local(cg, &r, "_t");
    } else {
        buf_append_fmt(cg->cur_body, "    (void)(%s);\n", r.c_expr);
    }
    er_free(&r);
    cg_release_scope(cg, st_scope);
    cg->cur_scope = st_scope->parent;
    scope_pop_free(st_scope);
    buf_append_cstr(cg->cur_body, "    }\n");
    return true;
}

/* throw <expr>;
 *
 * Phase 1A only supports managed payloads (string / array / object) — the
 * runtime carries the value as `void *` plus an `is_managed` flag, and
 * Phase 1A has no boxing path for unmanaged scalars. Codegen takes a +1
 * ownership of the value and hands it to `feng_exception_throw`, which
 * `longjmp`s to the nearest pushed frame and is marked `noreturn` so the
 * C compiler treats subsequent code as unreachable. */
static bool cg_emit_throw(CG *cg, const FengStmt *stmt) {
    if (stmt->as.throw_value == NULL) {
        return cg_fail(cg, stmt->token,
            "codegen: 'throw' requires a value");
    }
    ExprResult r;
    if (!cg_emit_expr(cg, stmt->as.throw_value, &r)) return false;
    if (!cgtype_is_managed(r.type)) {
        er_free(&r);
        return cg_fail(cg, stmt->token,
            "codegen: throwing non-managed values is not yet supported in Phase 1A");
    }
    char *tmp = cg_fresh_temp(cg, "_thr");
    char *cty = cg_ctype_dup(r.type);
    if (r.owns_ref) {
        buf_append_fmt(cg->cur_body, "    %s %s = %s;\n", cty, tmp, r.c_expr);
    } else {
        buf_append_fmt(cg->cur_body, "    %s %s = %s; feng_retain(%s);\n",
                       cty, tmp, r.c_expr, tmp);
    }
    free(cty);
    er_free(&r);
    buf_append_fmt(cg->cur_body,
                   "    feng_exception_throw((void *)%s, 1);\n", tmp);
    free(tmp);
    return true;
}

/* try { ... } [ catch { ... } ] [ finally { ... } ]
 *
 * Compilation strategy (Phase 1A, single-frame, no rethrow-from-catch):
 *
 *   {
 *       FengExceptionFrame _exc_frame_N;
 *       volatile int       _setjmp_N;
 *       feng_exception_push(&_exc_frame_N);
 *       _setjmp_N = setjmp(_exc_frame_N.jb);
 *       if (_setjmp_N == 0) {
 *           // try body — Feng scope releases happen on this normal path
 *           feng_exception_pop();
 *       } else {
 *           feng_exception_pop();
 *           // catch body (if present); release exception value at end
 *       }
 *       // finally body (if present)
 *       // if no catch and exception fired, rethrow into outer frame
 *   }
 *
 * Limitations recorded for follow-up phases:
 *  - `return` from inside try and `break`/`continue` crossing a try frame
 *    are rejected by codegen (see cg_emit_return / cg_emit_break_continue).
 *
 * ARC on the throw path:
 *  Every managed local registers itself on the per-thread cleanup chain at
 *  declaration (see cg_emit_cleanup_push_for_managed_local). On longjmp,
 *  feng_exception_throw walks the chain down to the frame's snapshot,
 *  releasing each live slot. Locals declared between the try-frame push and
 *  the throw site are therefore released exactly once.
 */
static bool cg_emit_try(CG *cg, const FengStmt *stmt) {
    const FengBlock *try_block     = stmt->as.try_stmt.try_block;
    const FengBlock *catch_block   = stmt->as.try_stmt.catch_block;
    const FengBlock *finally_block = stmt->as.try_stmt.finally_block;

    if (try_block == NULL) {
        return cg_fail(cg, stmt->token,
            "codegen: 'try' requires a body block");
    }

    int id = cg->label_counter++;
    char frame[64];
    char setjmp_var[64];
    snprintf(frame, sizeof frame, "_exc_frame_%d", id);
    snprintf(setjmp_var, sizeof setjmp_var, "_exc_setjmp_%d", id);

    buf_append_cstr(cg->cur_body, "    {\n");
    buf_append_fmt(cg->cur_body, "        FengExceptionFrame %s;\n", frame);
    buf_append_fmt(cg->cur_body, "        volatile int %s;\n", setjmp_var);
    buf_append_fmt(cg->cur_body, "        feng_exception_push(&%s);\n", frame);
    buf_append_fmt(cg->cur_body, "        %s = setjmp(%s.jb);\n",
                   setjmp_var, frame);
    buf_append_fmt(cg->cur_body, "        if (%s == 0) {\n", setjmp_var);

    cg->try_depth++;
    Scope *try_scope = scope_push(cg->cur_scope);
    if (!try_scope) {
        cg->try_depth--;
        return cg_fail(cg, stmt->token, "codegen: out of memory");
    }
    try_scope->try_depth_at_entry = cg->try_depth;
    cg->cur_scope = try_scope;
    bool ok = cg_emit_block(cg, try_block);
    if (ok) cg_release_scope(cg, try_scope);
    cg->cur_scope = try_scope->parent;
    scope_pop_free(try_scope);
    cg->try_depth--;
    if (!ok) return false;

    buf_append_cstr(cg->cur_body, "            feng_exception_pop();\n");
    buf_append_cstr(cg->cur_body, "        } else {\n");
    buf_append_cstr(cg->cur_body, "            feng_exception_pop();\n");

    if (catch_block != NULL) {
        Scope *catch_scope = scope_push(cg->cur_scope);
        if (!catch_scope) {
            return cg_fail(cg, stmt->token, "codegen: out of memory");
        }
        cg->cur_scope = catch_scope;
        bool cok = cg_emit_block(cg, catch_block);
        if (cok) cg_release_scope(cg, catch_scope);
        cg->cur_scope = catch_scope->parent;
        scope_pop_free(catch_scope);
        if (!cok) return false;
        /* Caught: drop the +1 reference the throw site retained. */
        buf_append_fmt(cg->cur_body,
                       "            if (%s.is_managed && %s.value) feng_release(%s.value);\n",
                       frame, frame, frame);
    }

    buf_append_cstr(cg->cur_body, "        }\n");

    if (finally_block != NULL) {
        Scope *fin_scope = scope_push(cg->cur_scope);
        if (!fin_scope) {
            return cg_fail(cg, stmt->token, "codegen: out of memory");
        }
        cg->cur_scope = fin_scope;
        bool fok = cg_emit_block(cg, finally_block);
        if (fok) cg_release_scope(cg, fin_scope);
        cg->cur_scope = fin_scope->parent;
        scope_pop_free(fin_scope);
        if (!fok) return false;
    }

    if (catch_block == NULL) {
        /* No catch: re-throw to outer frame after finally has run. */
        buf_append_fmt(cg->cur_body,
                       "        if (%s != 0) feng_exception_throw(%s.value, %s.is_managed);\n",
                       setjmp_var, frame, frame);
    }

    buf_append_cstr(cg->cur_body, "    }\n");
    return true;
}

static bool cg_emit_stmt(CG *cg, const FengStmt *stmt) {
    if (cg->failed) return false;
    switch (stmt->kind) {
        case FENG_STMT_BLOCK: {
            buf_append_cstr(cg->cur_body, "    {\n");
            Scope *s = scope_push(cg->cur_scope);
            if (!s) return cg_fail(cg, stmt->token, "codegen: out of memory");
            cg->cur_scope = s;
            bool ok = cg_emit_block(cg, stmt->as.block);
            cg_release_scope(cg, s);
            cg->cur_scope = s->parent;
            scope_pop_free(s);
            buf_append_cstr(cg->cur_body, "    }\n");
            return ok;
        }
        case FENG_STMT_BINDING:  return cg_emit_binding(cg, stmt);
        case FENG_STMT_ASSIGN:   return cg_emit_assign(cg, stmt);
        case FENG_STMT_EXPR:     return cg_emit_expr_stmt(cg, stmt);
        case FENG_STMT_RETURN:   return cg_emit_return(cg, stmt);
        case FENG_STMT_IF:       return cg_emit_if(cg, stmt);
        case FENG_STMT_WHILE:    return cg_emit_while(cg, stmt);
        case FENG_STMT_FOR:      return cg_emit_for(cg, stmt);
        case FENG_STMT_BREAK:    return cg_emit_break_continue(cg, stmt, true);
        case FENG_STMT_CONTINUE: return cg_emit_break_continue(cg, stmt, false);
        case FENG_STMT_THROW:    return cg_emit_throw(cg, stmt);
        case FENG_STMT_TRY:      return cg_emit_try(cg, stmt);
        default:
            return cg_fail(cg, stmt->token,
                "codegen: statement kind not yet supported in this iteration");
    }
}

static bool cg_emit_block(CG *cg, const FengBlock *block) {
    for (size_t i = 0; i < block->statement_count; i++) {
        if (!cg_emit_stmt(cg, block->statements[i])) return false;
    }
    return true;
}

/* ===================== top-level emission ===================== */

static bool cg_emit_extern_decl(CG *cg, const FengDecl *decl) {
    if (!cg_register_extern(cg, decl)) return false;
    const ExternFn *ef = &cg->externs[cg->extern_count - 1];
    /* Emit `extern <ret> name(<params>);` Strings map to `const char *`. */
    Buf *h = &cg->headers;
    const char *ret_c = (ef->return_type->kind == CG_TYPE_STRING)
                          ? "const char *"
                          : cgtype_to_c(ef->return_type->kind);
    buf_append_fmt(h, "extern %s %s(", ret_c, ef->name);
    if (ef->param_count == 0) {
        buf_append_cstr(h, "void");
    } else {
        for (size_t i = 0; i < ef->param_count; i++) {
            if (i) buf_append_cstr(h, ", ");
            const char *pty = (ef->param_types[i]->kind == CG_TYPE_STRING)
                                ? "const char *"
                                : cgtype_to_c(ef->param_types[i]->kind);
            buf_append_cstr(h, pty);
        }
    }
    buf_append_cstr(h, ");\n");
    return true;
}

static bool cg_check_main_signature(CG *cg, const FreeFn *fn) {
    if (fn->return_type->kind != CG_TYPE_VOID &&
        fn->return_type->kind != CG_TYPE_I32) {
        return cg_fail(cg, fn->decl->token,
            "codegen: main must return void or i32");
    }
    if (fn->param_count != 1 ||
        fn->param_types[0]->kind != CG_TYPE_ARRAY ||
        !fn->param_types[0]->element ||
        fn->param_types[0]->element->kind != CG_TYPE_STRING) {
        return cg_fail(cg, fn->decl->token,
            "codegen: main must have signature (args: string[])");
    }
    return true;
}

static bool cg_emit_function(CG *cg, const FengDecl *decl, bool is_main) {
    if (!cg_register_free_fn(cg, decl)) return false;
    FreeFn *fn = &cg->free_fns[cg->free_fn_count - 1];
    cg->cur_fn_is_main = is_main;
    cg->cur_return_type = fn->return_type;

    if (is_main && !cg_check_main_signature(cg, fn)) return false;

    /* Forward proto. */
    Buf *p = &cg->fn_protos;
    buf_append_cstr(p, "static ");
    cg_emit_c_type(p, fn->return_type);
    buf_append_fmt(p, " %s(", fn->c_name);
    if (fn->param_count == 0) buf_append_cstr(p, "void");
    for (size_t i = 0; i < fn->param_count; i++) {
        if (i) buf_append_cstr(p, ", ");
        cg_emit_c_type(p, fn->param_types[i]);
        buf_append_fmt(p, " %s",
            fn->param_names[i] ? fn->param_names[i] : "_p");
    }
    buf_append_cstr(p, ");\n");

    /* Body. */
    Buf *body = &cg->fn_defs;
    cg->cur_body = body;
    buf_append_cstr(body, "static ");
    cg_emit_c_type(body, fn->return_type);
    buf_append_fmt(body, " %s(", fn->c_name);
    if (fn->param_count == 0) buf_append_cstr(body, "void");
    Scope *fn_scope = scope_push(NULL);
    if (!fn_scope) return cg_fail(cg, decl->token, "codegen: out of memory");
    cg->cur_scope = fn_scope;
    cg->tmp_counter = 0;
    cg->loop_depth = 0;
    cg->try_depth = 0;

    for (size_t i = 0; i < fn->param_count; i++) {
        if (i) buf_append_cstr(body, ", ");
        cg_emit_c_type(body, fn->param_types[i]);
        buf_append_fmt(body, " %s",
            fn->param_names[i] ? fn->param_names[i] : "_p");
        CGType *pt = cgtype_clone(fn->param_types[i]);
        scope_add(fn_scope, fn->param_names[i] ? fn->param_names[i] : "_p",
                  fn->param_names[i] ? fn->param_names[i] : "_p", pt, true);
    }
    buf_append_cstr(body, ") {\n");

    /* Suppress -Wunused-parameter for parameters that the body may not
     * reference. The cast is a no-op at runtime. */
    for (size_t i = 0; i < fn->param_count; i++) {
        const char *pn = fn->param_names[i] ? fn->param_names[i] : "_p";
        buf_append_fmt(body, "    (void)%s;\n", pn);
    }

    if (!cg_emit_block(cg, decl->as.function_decl.body)) {
        cg->cur_scope = NULL;
        scope_pop_free(fn_scope);
        return false;
    }

    /* Implicit fall-off cleanup + return for void/main. */
    cg_release_scope(cg, fn_scope);
    if (fn->return_type->kind == CG_TYPE_VOID) {
        buf_append_cstr(body, "    return;\n");
    } else if (is_main && fn->return_type->kind == CG_TYPE_I32) {
        buf_append_cstr(body, "    return 0;\n");
    } else {
        /* Non-void without explicit return is rejected by semantic; emit
         * abort as defensive measure. */
        buf_append_cstr(body, "    feng_panic(\"function reached end without return\");\n");
    }
    buf_append_cstr(body, "}\n\n");
    cg->cur_scope = NULL;
    scope_pop_free(fn_scope);
    cg->cur_body = NULL;
    cg->cur_return_type = NULL;
    cg->cur_fn_is_main = false;
    return true;
}

/* ===================== driver ===================== */

static bool slice_eq(FengSlice s, const char *cstr) {
    size_t n = strlen(cstr);
    return s.length == n && memcmp(s.data, cstr, n) == 0;
}

static bool cg_emit_module_header(CG *cg, const FengProgram *prog) {
    free(cg->module_mangle);
    free(cg->module_dot_name);
    cg->module_mangle = cg_module_mangle(prog->module_segments,
                                         prog->module_segment_count);
    cg->module_dot_name = cg_module_dot(prog->module_segments,
                                        prog->module_segment_count);
    if (!cg->module_mangle || !cg->module_dot_name) {
        return cg_fail(cg, prog->module_token, "codegen: out of memory");
    }
    return true;
}

/* ----- Spec witness instance emission (Step 4b — value model §6.5/§8.2) ----- */

/* Look up an already-emitted (T, S) witness table; returns NULL on miss. */
static const char *cg_witness_table_lookup(const CG *cg, const UserType *t,
                                           const UserSpec *s) {
    for (size_t i = 0; i < cg->witness_table_count; i++) {
        if (cg->witness_tables[i].t == t && cg->witness_tables[i].s == s) {
            return cg->witness_tables[i].c_var;
        }
    }
    return NULL;
}

/* Ensure a witness table for (T, S) has been materialised in fn_protos /
 * fn_defs and write its C variable name to *out_var (borrowed pointer into
 * the cache). The witness contents are read from the analysis sidecar
 * (feng_semantic_lookup_spec_witness, populated on-demand at coercion sites
 * during semantic analysis). 4b-α only supports own-method-sourced slots; any
 * fit-sourced or unresolved slot is reported as an error here. */
static bool cg_ensure_witness_instance(CG *cg, const UserType *t,
                                       const UserSpec *s, FengToken blame,
                                       const char **out_var) {
    const char *cached = cg_witness_table_lookup(cg, t, s);
    if (cached) { *out_var = cached; return true; }

    const FengSpecWitness *witness = feng_semantic_lookup_spec_witness(
        cg->analysis, t->decl, s->decl);
    if (!witness) {
        return cg_fail(cg, blame,
            "codegen: internal: spec witness for (%s, %s) not computed by analyzer",
            t->feng_name, s->feng_name);
    }

    /* Emit one thunk per spec method member (in spec member order). Field
     * accessor thunks are deferred to 4b-β. */
    char *t_san = cg_sanitize(t->feng_name, strlen(t->feng_name));
    char *s_san = cg_sanitize(s->feng_name, strlen(s->feng_name));
    if (!t_san || !s_san) { free(t_san); free(s_san); return false; }

    /* Per-thunk symbol prefix scoped by both the implementing type and the
     * target spec to keep multiple (T, S) pairings non-colliding. */
    Buf prefix; buf_init(&prefix);
    buf_append_fmt(&prefix, "FengSpecThunk__%s__%s__as__%s__%s",
                   cg->module_mangle, t_san, cg->module_mangle, s_san);

    /* Iterate spec members in declared order — matches witness->members[] order
     * from feng_semantic_spec_witness_append_member. Methods get a single
     * thunk; fields get a getter and (for `var`) a setter. */
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (i >= witness->member_count) {
            buf_free(&prefix); free(t_san); free(s_san);
            return cg_fail(cg, blame,
                "codegen: internal: witness slot count mismatch for (%s, %s)",
                t->feng_name, s->feng_name);
        }
        const FengSpecWitnessMember *wm = &witness->members[i];
        if (!wm->impl_member) {
            buf_free(&prefix); free(t_san); free(s_san);
            return cg_fail(cg, blame,
                "codegen: type '%s' is missing an implementation for spec '%s' member '%s'",
                t->feng_name, s->feng_name, sm->feng_name);
        }
        if (wm->source_kind == FENG_SPEC_WITNESS_SOURCE_FIT_METHOD) {
            /* Step 4b-γ — fit-provided method. The fit body emits the
             * implementation as `<fit_prefix>__<member>(struct T *self, ...)`
             * (see cg_register_user_fit_members / cg_emit_user_fit_methods);
             * the witness thunk just adapts the void* subject and forwards. */
            if (sm->kind != USM_KIND_METHOD) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: spec field '%s' cannot be satisfied by a fit method",
                    sm->feng_name);
            }
            const UserFit *uf = cg_find_user_fit_by_decl(cg, wm->via_fit_decl);
            if (!uf || uf->target != t) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: internal: fit decl for spec '%s' member '%s' not registered for type '%s'",
                    s->feng_name, sm->feng_name, t->feng_name);
            }
            const UserMethod *fm = NULL;
            for (size_t k = 0; k < uf->method_count; k++) {
                if (uf->methods[k].member == wm->impl_member) {
                    fm = &uf->methods[k]; break;
                }
            }
            if (!fm) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: internal: fit method '%s' not found in fit body for type '%s'",
                    sm->feng_name, t->feng_name);
            }
            Buf *fp = &cg->fn_protos;
            buf_append_cstr(fp, "static ");
            cg_emit_c_type(fp, sm->type);
            buf_append_fmt(fp, " %s__%s(void *_subject", prefix.data, sm->c_field_name);
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_cstr(fp, ", ");
                cg_emit_c_type(fp, sm->param_types[pi]);
                buf_append_fmt(fp, " p%zu", pi);
            }
            buf_append_cstr(fp, ");\n");

            Buf *fd = &cg->witness_defs;
            buf_append_cstr(fd, "static ");
            cg_emit_c_type(fd, sm->type);
            buf_append_fmt(fd, " %s__%s(void *_subject", prefix.data, sm->c_field_name);
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_cstr(fd, ", ");
                cg_emit_c_type(fd, sm->param_types[pi]);
                buf_append_fmt(fd, " p%zu", pi);
            }
            buf_append_cstr(fd, ") {\n");
            if (sm->type->kind == CG_TYPE_VOID) {
                buf_append_fmt(fd, "    %s((struct %s *)_subject", fm->c_name, t->c_struct_name);
            } else {
                buf_append_fmt(fd, "    return %s((struct %s *)_subject",
                               fm->c_name, t->c_struct_name);
            }
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_fmt(fd, ", p%zu", pi);
            }
            buf_append_cstr(fd, ");\n}\n\n");
            continue;
        }
        if (sm->kind == USM_KIND_METHOD) {
            if (wm->source_kind != FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_METHOD) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: spec method '%s' must be implemented by a method on '%s' (Step 4b-α)",
                    sm->feng_name, t->feng_name);
            }
            const UserMethod *um = cg_user_type_method(t, sm->feng_name,
                                                       strlen(sm->feng_name));
            if (!um) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: internal: type '%s' has no method '%s' to satisfy spec '%s'",
                    t->feng_name, sm->feng_name, s->feng_name);
            }
            /* Forward declaration — body is emitted in cg_emit_user_method. */
            Buf *fp = &cg->fn_protos;
            buf_append_cstr(fp, "static ");
            cg_emit_c_type(fp, sm->type);
            buf_append_fmt(fp, " %s__%s(void *_subject", prefix.data, sm->c_field_name);
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_cstr(fp, ", ");
                cg_emit_c_type(fp, sm->param_types[pi]);
                buf_append_fmt(fp, " p%zu", pi);
            }
            buf_append_cstr(fp, ");\n");

            /* Body — emitted into witness_defs to avoid splicing into whatever
             * function body cur_body currently points at. */
            Buf *fd = &cg->witness_defs;
            buf_append_cstr(fd, "static ");
            cg_emit_c_type(fd, sm->type);
            buf_append_fmt(fd, " %s__%s(void *_subject", prefix.data, sm->c_field_name);
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_cstr(fd, ", ");
                cg_emit_c_type(fd, sm->param_types[pi]);
                buf_append_fmt(fd, " p%zu", pi);
            }
            buf_append_cstr(fd, ") {\n");
            if (sm->type->kind == CG_TYPE_VOID) {
                buf_append_fmt(fd, "    %s((struct %s *)_subject", um->c_name, t->c_struct_name);
            } else {
                buf_append_fmt(fd, "    return %s((struct %s *)_subject",
                               um->c_name, t->c_struct_name);
            }
            for (size_t pi = 0; pi < sm->param_count; pi++) {
                buf_append_fmt(fd, ", p%zu", pi);
            }
            buf_append_cstr(fd, ");\n}\n\n");
        } else if (sm->kind == USM_KIND_FIELD) {
            /* Step 4b-β — TYPE_OWN_FIELD witness. The implementing field
             * lives directly on T; locate it by Feng name to recover the
             * sanitised C field identifier. */
            if (wm->source_kind != FENG_SPEC_WITNESS_SOURCE_TYPE_OWN_FIELD) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: spec field '%s' must be satisfied by a field on '%s'",
                    sm->feng_name, t->feng_name);
            }
            const UserField *uf = cg_user_type_field(t, sm->feng_name,
                                                     strlen(sm->feng_name));
            if (!uf) {
                buf_free(&prefix); free(t_san); free(s_san);
                return cg_fail(cg, blame,
                    "codegen: internal: type '%s' has no field '%s' to satisfy spec '%s'",
                    t->feng_name, sm->feng_name, s->feng_name);
            }

            /* Getter: return ((struct T*)_subject)->c_field. The thunk
             * emits a borrow — caller is responsible for any retain. */
            Buf *fd = &cg->witness_defs;
            buf_append_cstr(fd, "static ");
            cg_emit_c_type(fd, sm->type);
            buf_append_fmt(fd, " %s__get_%s(void *_subject) {\n",
                           prefix.data, sm->c_field_name);
            buf_append_fmt(fd, "    return ((struct %s *)_subject)->%s;\n",
                           t->c_struct_name, uf->c_name);
            buf_append_cstr(fd, "}\n\n");

            Buf *fp = &cg->fn_protos;
            buf_append_cstr(fp, "static ");
            cg_emit_c_type(fp, sm->type);
            buf_append_fmt(fp, " %s__get_%s(void *_subject);\n",
                           prefix.data, sm->c_field_name);

            if (sm->is_var) {
                /* Setter: managed slots route through feng_assign so the
                 * old reference is released and the new one retained
                 * atomically; trivial slots use a direct store. */
                buf_append_fmt(fd, "static void %s__set_%s(void *_subject, ",
                               prefix.data, sm->c_field_name);
                cg_emit_c_type(fd, sm->type);
                buf_append_cstr(fd, " value) {\n");
                if (cgtype_is_managed(sm->type)) {
                    buf_append_fmt(fd,
                        "    feng_assign((void **)&((struct %s *)_subject)->%s, value);\n",
                        t->c_struct_name, uf->c_name);
                } else {
                    buf_append_fmt(fd,
                        "    ((struct %s *)_subject)->%s = value;\n",
                        t->c_struct_name, uf->c_name);
                }
                buf_append_cstr(fd, "}\n\n");

                buf_append_fmt(fp, "static void %s__set_%s(void *_subject, ",
                               prefix.data, sm->c_field_name);
                cg_emit_c_type(fp, sm->type);
                buf_append_cstr(fp, " value);\n");
            }
        }
    }

    /* Witness table instance. */
    Buf var; buf_init(&var);
    buf_append_fmt(&var, "FengWitness__%s__%s__as__%s__%s",
                   cg->module_mangle, t_san, cg->module_mangle, s_san);

    Buf *fd = &cg->witness_defs;
    buf_append_fmt(fd, "static const struct %s %s = {\n",
                   s->c_witness_struct_name, var.data);
    /* Forward-declare the table in fn_protos so any function body referencing
     * it (emitted into fn_defs before witness_defs in cg_finalize) sees a
     * declaration first. */
    buf_append_fmt(&cg->fn_protos, "static const struct %s %s;\n",
                   s->c_witness_struct_name, var.data);
    for (size_t i = 0; i < s->member_count; i++) {
        const UserSpecMember *sm = &s->members[i];
        if (sm->kind == USM_KIND_METHOD) {
            buf_append_fmt(fd, "    .%s = &%s__%s,\n",
                           sm->c_field_name, prefix.data, sm->c_field_name);
        } else if (sm->kind == USM_KIND_FIELD) {
            buf_append_fmt(fd, "    .get_%s = &%s__get_%s,\n",
                           sm->c_field_name, prefix.data, sm->c_field_name);
            if (sm->is_var) {
                buf_append_fmt(fd, "    .set_%s = &%s__set_%s,\n",
                               sm->c_field_name, prefix.data, sm->c_field_name);
            }
        }
    }
    buf_append_cstr(fd, "};\n\n");

    /* Cache. */
    if (cg->witness_table_count + 1 > cg->witness_table_capacity) {
        size_t cap = cg->witness_table_capacity ? cg->witness_table_capacity * 2 : 4;
        void *p = realloc(cg->witness_tables, cap * sizeof *cg->witness_tables);
        if (!p) { buf_free(&prefix); buf_free(&var); free(t_san); free(s_san); return false; }
        cg->witness_tables = p;
        cg->witness_table_capacity = cap;
    }
    cg->witness_tables[cg->witness_table_count].t = t;
    cg->witness_tables[cg->witness_table_count].s = s;
    cg->witness_tables[cg->witness_table_count].c_var = var.data;
    *out_var = var.data;
    cg->witness_table_count++;

    buf_free(&prefix); free(t_san); free(s_san);
    return true;
}

/* --- Multi-program emission helpers (P3) ---------------------------------
 *
 * The pipeline below is structured so that every name minted by Pass 1/1.5/
 * 2.7/2b/4 uses the OWNING program's module mangle (set via
 * `cg_emit_module_header`), while passes that operate on already-registered
 * shells (member registration and emission) are program-agnostic and run
 * once across the global cg arrays.
 */

static bool cg_pass_register_type_shells(CG *cg, const FengProgram *prog) {
    if (!cg_emit_module_header(cg, prog)) return false;
    for (size_t i = 0; i < prog->declaration_count; i++) {
        const FengDecl *d = prog->declarations[i];
        if (d->kind == FENG_DECL_TYPE) {
            if (!cg_register_user_type_shell(cg, d)) return false;
        }
    }
    /* Pass 1.5: register every spec SHELL (Step 4b). Specs may appear before
     * or after the types that implement them; registering shells first lets
     * cg_resolve_type see both kinds during member-pass type resolution. */
    for (size_t i = 0; i < prog->declaration_count; i++) {
        const FengDecl *d = prog->declarations[i];
        if (d->kind == FENG_DECL_SPEC) {
            if (!cg_register_user_spec_shell(cg, d)) return false;
        }
    }
    return true;
}

static bool cg_pass_register_fit_shells(CG *cg, const FengProgram *prog) {
    if (!cg_emit_module_header(cg, prog)) return false;
    for (size_t i = 0; i < prog->declaration_count; i++) {
        const FengDecl *d = prog->declarations[i];
        if (d->kind == FENG_DECL_FIT) {
            if (!cg_register_user_fit_shell(cg, d)) return false;
        }
    }
    return true;
}

static bool cg_pass_register_module_bindings(CG *cg, const FengProgram *prog) {
    if (!cg_emit_module_header(cg, prog)) return false;
    for (size_t i = 0; i < prog->declaration_count; i++) {
        const FengDecl *d = prog->declarations[i];
        if (d->kind != FENG_DECL_GLOBAL_BINDING) continue;
        if (d->is_extern) {
            return cg_fail(cg, d->token,
                "codegen: extern module-level bindings not supported in Phase 1A");
        }
        if (!cg_register_module_binding(cg, d)) return false;
        const ModuleBinding *mb = &cg->module_bindings[cg->module_binding_count - 1];
        char *cty = cg_ctype_dup(mb->type);
        if (!cty) return cg_fail(cg, d->token, "codegen: out of memory");
        if (cgtype_is_managed(mb->type)) {
            buf_append_fmt(&cg->statics, "static %s %s = NULL;\n", cty, mb->c_name);
        } else {
            buf_append_fmt(&cg->statics, "static %s %s = 0;\n", cty, mb->c_name);
        }
        free(cty);
    }
    return true;
}

static bool cg_pass_emit_decls(CG *cg, const FengProgram *prog,
                               FengCompileTarget target) {
    if (!cg_emit_module_header(cg, prog)) return false;
    /* Pass 4: walk top-level decls in source order for externs / functions /
     * methods. Type decls themselves emit only their methods here. */
    for (size_t i = 0; i < prog->declaration_count; i++) {
        const FengDecl *d = prog->declarations[i];
        switch (d->kind) {
            case FENG_DECL_GLOBAL_BINDING:
                /* Already registered + storage emitted in Pass 2b; the
                 * initializer runs from the main wrapper. */
                continue;
            case FENG_DECL_TYPE: {
                /* Find the registered UserType by AST identity. */
                const UserType *ut = NULL;
                for (size_t k = 0; k < cg->user_type_count; k++) {
                    if (cg->user_types[k].decl == d) { ut = &cg->user_types[k]; break; }
                }
                if (!ut) return cg_fail(cg, d->token, "codegen: internal: type not registered");
                for (size_t mi = 0; mi < ut->method_count; mi++) {
                    if (!cg_emit_user_method(cg, ut, &ut->methods[mi])) return false;
                }
                if (!cg_emit_user_finalizer(cg, ut)) return false;
                break;
            }
            case FENG_DECL_SPEC:
                continue;
            case FENG_DECL_FIT: {
                /* Step 4b-γ — emit each fit-body method as if it were an
                 * ordinary method on the target type. The fit-mangled
                 * c_name (set in cg_register_user_fit_members) keeps the
                 * symbol disjoint from T's own methods and from sibling
                 * fits for the same T. Witness thunks generated by
                 * cg_ensure_witness_instance call this c_name directly. */
                const UserFit *uf = cg_find_user_fit_by_decl(cg, d);
                if (!uf) return cg_fail(cg, d->token,
                    "codegen: internal: fit not registered");
                for (size_t mi = 0; mi < uf->method_count; mi++) {
                    if (!cg_emit_user_method(cg, uf->target, &uf->methods[mi])) return false;
                }
                break;
            }
            case FENG_DECL_FUNCTION:
                if (d->is_extern) {
                    if (!cg_emit_extern_decl(cg, d)) return false;
                } else {
                    bool is_main = (target == FENG_COMPILE_TARGET_BIN) &&
                                   slice_eq(d->as.function_decl.name, "main");
                    if (!cg_emit_function(cg, d, is_main)) return false;
                }
                break;
        }
    }
    return true;
}

/* Drive every pass across the full program set in deterministic
 * (module, program) order. Passes that mint cross-module-mangled symbols
 * (shells, fits, module bindings, decl emission) re-anchor `cg->module_mangle`
 * via cg_emit_module_header before processing each program; passes that walk
 * already-registered shells (member registration, type/spec body emission)
 * are program-agnostic and run once over the global cg arrays. */
static bool cg_emit_all_programs(CG *cg,
                                 const FengProgram *const *programs,
                                 size_t program_count,
                                 FengCompileTarget target) {
    /* Pass 1 + 1.5: register type and spec shells per program. */
    for (size_t p = 0; p < program_count; p++) {
        if (!cg_pass_register_type_shells(cg, programs[p])) return false;
    }
    /* Pass 2: register fields/methods (uses cg_resolve_type which now sees
     * every shell, regardless of owning program). */
    for (size_t i = 0; i < cg->user_type_count; i++) {
        if (!cg_register_user_type_members(cg, &cg->user_types[i])) return false;
    }
    /* Pass 2.5: register spec members. */
    for (size_t i = 0; i < cg->user_spec_count; i++) {
        if (!cg_register_user_spec_members(cg, &cg->user_specs[i])) return false;
    }
    /* Pass 2.7: register fit shells (mangled per owning program), then
     * fit members (program-agnostic). */
    for (size_t p = 0; p < program_count; p++) {
        if (!cg_pass_register_fit_shells(cg, programs[p])) return false;
    }
    for (size_t i = 0; i < cg->user_fit_count; i++) {
        if (!cg_register_user_fit_members(cg, &cg->user_fits[i])) return false;
    }
    /* Pass 2b: register module-level let/var bindings + emit static storage. */
    for (size_t p = 0; p < program_count; p++) {
        if (!cg_pass_register_module_bindings(cg, programs[p])) return false;
    }
    /* Pass 3 + 3.5a/3.5b: emit type forwards/defs and spec forwards/defs.
     * These walk the global cg shell arrays, so a single global pass suffices. */
    for (size_t i = 0; i < cg->user_type_count; i++) {
        cg_emit_user_type_forward(cg, &cg->user_types[i]);
    }
    for (size_t i = 0; i < cg->user_spec_count; i++) {
        cg_emit_user_spec_forward(cg, &cg->user_specs[i]);
    }
    for (size_t i = 0; i < cg->user_type_count; i++) {
        cg_emit_user_type_definition(cg, &cg->user_types[i]);
    }
    for (size_t i = 0; i < cg->user_spec_count; i++) {
        cg_emit_user_spec_definition(cg, &cg->user_specs[i]);
    }
    /* Pass 4: per-program decl emission (externs / functions / methods /
     * finalizers / fit method bodies). */
    for (size_t p = 0; p < program_count; p++) {
        if (!cg_pass_emit_decls(cg, programs[p], target)) return false;
    }
    return true;
}


static void cg_emit_string_literal_table(CG *cg) {
    Buf *s = &cg->statics;
    for (size_t i = 0; i < cg->string_literal_count; i++) {
        buf_append_fmt(s, "static FengString *%s = NULL;\n", cg->string_literals[i].c_var);
    }
}

static void cg_emit_string_literal_init(CG *cg, Buf *body) {
    for (size_t i = 0; i < cg->string_literal_count; i++) {
        buf_append_fmt(body,
            "    %s = feng_string_literal(", cg->string_literals[i].c_var);
        buf_append_cstr(body, "\"");
        const char *p = cg->string_literals[i].content;
        size_t n = cg->string_literals[i].length;
        for (size_t j = 0; j < n; j++) {
            unsigned char c = (unsigned char)p[j];
            switch (c) {
                case '\\': buf_append_cstr(body, "\\\\"); break;
                case '"':  buf_append_cstr(body, "\\\""); break;
                case '\n': buf_append_cstr(body, "\\n"); break;
                case '\r': buf_append_cstr(body, "\\r"); break;
                case '\t': buf_append_cstr(body, "\\t"); break;
                default:
                    if (c < 0x20 || c == 0x7f) buf_append_fmt(body, "\\x%02x", c);
                    else { char ch = (char)c; buf_append(body, &ch, 1); }
            }
        }
        buf_append_fmt(body, "\", %zu);\n", cg->string_literals[i].length);
    }
}

static bool cg_emit_module_binding_init(CG *cg, const ModuleBinding *mb) {
    const FengExpr *init = mb->binding->initializer;
    if (!init) {
        /* No initializer: assign the type's default zero. For managed
         * types the slot now owns a +1 reference (the static was zero-initialised
         * to NULL/0 in pass 2b). */
        char *def_expr = NULL;
        if (!cg_default_value_expr(cg, mb->type, &mb->binding->token, &def_expr)) {
            return false;
        }
        if (cgtype_is_managed(mb->type)) {
            buf_append_fmt(cg->cur_body, "    %s = %s;\n", mb->c_name, def_expr);
        } else {
            char *cty = cg_ctype_dup(mb->type);
            buf_append_fmt(cg->cur_body, "    %s = (%s)(%s);\n",
                           mb->c_name, cty, def_expr);
            free(cty);
        }
        free(def_expr);
        return true;
    }

    /* Use a transient scope so any temporaries from the expression get
     * released after the assignment. */
    Scope *fn_scope = scope_push(NULL);
    if (!fn_scope) return cg_fail(cg, mb->binding->token,
                                  "codegen: out of memory");
    cg->cur_scope = fn_scope;
    /* Caller is responsible for setting cg->cur_body to the destination
     * buffer (typically a side buffer that is later spliced into main()). */
    cg->cur_return_type = NULL;
    cg->cur_fn_is_main = false;

    buf_append_cstr(cg->cur_body, "    {\n");
    ExprResult r;
    if (!cg_emit_expr(cg, init, &r)) {
        cg->cur_scope = NULL; scope_pop_free(fn_scope);
        cg->cur_body = NULL;
        return false;
    }
    /* Type compatibility: require kinds to match (and for OBJECT, same user
     * type). Numeric narrowing/widening is not auto-applied at module scope
     * to avoid silent surprises. */
    bool compatible = (r.type->kind == mb->type->kind);
    if (compatible && mb->type->kind == CG_TYPE_OBJECT) {
        compatible = (r.type->user == mb->type->user);
    }
    if (compatible && mb->type->kind == CG_TYPE_ARRAY) {
        compatible = (r.type->element && mb->type->element &&
                      r.type->element->kind == mb->type->element->kind);
    }
    if (!compatible) {
        er_free(&r);
        cg_release_scope(cg, fn_scope);
        buf_append_cstr(cg->cur_body, "    }\n");
        cg->cur_scope = NULL; scope_pop_free(fn_scope);
        cg->cur_body = NULL;
        return cg_fail(cg, mb->binding->token,
            "codegen: module binding '%s' initializer type does not match its declared type",
            mb->feng_name);
    }
    if (cgtype_is_managed(mb->type)) {
        if (r.owns_ref) {
            buf_append_fmt(cg->cur_body, "        %s = %s;\n", mb->c_name, r.c_expr);
        } else {
            buf_append_fmt(cg->cur_body,
                "        %s = %s; feng_retain(%s);\n",
                mb->c_name, r.c_expr, mb->c_name);
        }
    } else {
        char *cty = cg_ctype_dup(mb->type);
        buf_append_fmt(cg->cur_body, "        %s = (%s)(%s);\n",
                       mb->c_name, cty, r.c_expr);
        free(cty);
    }
    er_free(&r);
    cg_release_scope(cg, fn_scope);
    buf_append_cstr(cg->cur_body, "    }\n");
    cg->cur_scope = NULL; scope_pop_free(fn_scope);
    cg->cur_body = NULL;
    return true;
}

static bool cg_emit_main_wrapper(CG *cg, const FreeFn *main_fn) {
    Buf *b = &cg->fn_defs;
    /* Emit module-binding initialisers into a side buffer first so that any
     * string literals embedded in their expressions get interned BEFORE we
     * write the string-literal init prelude. The captured body is then
     * appended after the literal init below. */
    Buf module_init_body;
    buf_init(&module_init_body);
    Buf *prev_body = cg->cur_body;
    cg->cur_body = &module_init_body;
    for (size_t i = 0; i < cg->module_binding_count; i++) {
        if (!cg_emit_module_binding_init(cg, &cg->module_bindings[i])) {
            buf_free(&module_init_body);
            cg->cur_body = prev_body;
            return false;
        }
    }
    cg->cur_body = prev_body;

    buf_append_cstr(b,
        "int main(int argc, char **argv) {\n");
    /* Initialise string literal slots once. */
    cg_emit_string_literal_init(cg, b);
    buf_append_cstr(b,
        "    FengArray *_args = feng_array_new(&feng_string_descriptor, sizeof(FengString *), true, (size_t)argc);\n"
        "    FengString **_slots = (FengString **)feng_array_data(_args);\n"
        "    for (int _i = 0; _i < argc; _i++) {\n"
        "        _slots[_i] = feng_string_literal(argv[_i], strlen(argv[_i]));\n"
        "    }\n");
    /* Now splice in the previously captured module-binding init code. */
    if (module_init_body.length > 0) {
        buf_append(b, module_init_body.data, module_init_body.length);
    }
    buf_free(&module_init_body);
    if (main_fn->return_type->kind == CG_TYPE_I32) {
        buf_append_fmt(b, "    int32_t _rc = %s(_args);\n", main_fn->c_name);
    } else {
        buf_append_fmt(b, "    %s(_args);\n", main_fn->c_name);
    }
    /* Release globals so leak-checkers stay quiet. */
    for (size_t i = 0; i < cg->module_binding_count; i++) {
        const ModuleBinding *mb = &cg->module_bindings[i];
        if (cgtype_is_managed(mb->type)) {
            buf_append_fmt(b, "    feng_release(%s); %s = NULL;\n",
                           mb->c_name, mb->c_name);
        }
    }
    buf_append_cstr(b, "    feng_release(_args);\n");
    buf_append_cstr(b,
        "    feng_runtime_shutdown();\n");
    if (main_fn->return_type->kind == CG_TYPE_I32) {
        buf_append_cstr(b, "    return (int)_rc;\n}\n");
    } else {
        buf_append_cstr(b, "    return 0;\n}\n");
    }
    return true;
}

/* ===================== USER TYPE EMISSION (Phase 1A iter 2a) ===================== */

/* --- per-field lifetime / metadata helpers --------------------------------
 *
 * The three helpers below centralise the "how does this field participate in
 * ARC + cycle metadata" decision so cg_emit_user_type_definition can iterate
 * fields without re-deriving the rule each time. Each helper dispatches on
 * cgtype_value_kind(field_type):
 *
 *   TRIVIAL          — contributes nothing.
 *   MANAGED_POINTER  — one managed_fields[] entry; one feng_release() call.
 *   AGGREGATE        — flattened per dev/feng-value-model-delivered.md §7.2:
 *                      one descriptor per FENG_SLOT_POINTER slot inside the
 *                      aggregate (with offset = field_offset + slot_offset),
 *                      plus a feng_aggregate_release() call on the field
 *                      address. The currently-supported aggregate is the
 *                      object-form fat spec, whose layout has a single
 *                      managed `subject` pointer at slot offset 0.
 */

/* For aggregate field types, returns the user-facing
 * FengAggregateValueDescriptor symbol name. Today only object-form spec
 * values are aggregate; new aggregate kinds (tuple, value-struct) must
 * extend this dispatch. */
static const char *cg_aggregate_field_desc_name(const CGType *t) {
    if (!t || t->kind != CG_TYPE_SPEC || t->user_spec == NULL) {
        return NULL;
    }
    return t->user_spec->c_aggregate_desc_name;
}

/* Returns the number of FENG_SLOT_POINTER slots produced by flattening the
 * aggregate type. Mirrors aggregate_for_each_pointer_slot semantics; for
 * spec values this is always 1 (the subject pointer). */
static size_t cg_aggregate_pointer_slot_count(const CGType *t) {
    if (!t || t->kind != CG_TYPE_SPEC || t->user_spec == NULL) {
        return 0U;
    }
    return 1U;
}

/* Walks the aggregate type's pointer slots and emits one
 * FengManagedFieldDescriptor row per slot at offset
 * `field_base_offsetof_expr + slot_offset_within_aggregate`. The
 * `field_base_offsetof_expr` is a complete C expression yielding the
 * field's offset in the enclosing struct (e.g.,
 * `offsetof(struct Foo, bar)`). For spec values we reuse the value
 * struct's own `offsetof(..., subject)` so the slot offset stays in sync
 * with the layout the C compiler picked. */
static void cg_emit_aggregate_pointer_slot_rows(Buf *td,
                                                const char *field_base_offsetof_expr,
                                                const CGType *t) {
    /* Spec: single subject pointer at offset 0 of the value struct. */
    if (t && t->kind == CG_TYPE_SPEC && t->user_spec) {
        buf_append_fmt(td,
            "    { %s + offsetof(struct %s, subject), NULL },\n",
            field_base_offsetof_expr,
            t->user_spec->c_value_struct_name);
    }
}

static size_t cg_field_managed_descriptor_count(CG *cg, const CGType *t,
                                                FengToken err_token) {
    switch (cgtype_value_kind(t)) {
        case CG_VK_TRIVIAL:         return 0U;
        case CG_VK_MANAGED_POINTER: return 1U;
        case CG_VK_AGGREGATE: {
            size_t n = cg_aggregate_pointer_slot_count(t);
            if (n == 0U) {
                (void)cg_fail(cg, err_token,
                    "codegen: aggregate field has no flatten rule (unknown aggregate kind)");
            }
            return n;
        }
    }
    return 0U;
}

static bool cg_emit_field_managed_descriptors(CG *cg, Buf *td,
                                              const char *struct_name,
                                              const char *field_c_name,
                                              const CGType *ft,
                                              FengToken err_token) {
    switch (cgtype_value_kind(ft)) {
        case CG_VK_TRIVIAL:
            return true;
        case CG_VK_MANAGED_POINTER:
            buf_append_fmt(td, "    { offsetof(struct %s, %s), ",
                           struct_name, field_c_name);
            switch (ft->kind) {
                case CG_TYPE_STRING:
                    buf_append_cstr(td, "&feng_string_descriptor");
                    break;
                case CG_TYPE_ARRAY:
                    buf_append_cstr(td, "&feng_array_descriptor");
                    break;
                case CG_TYPE_OBJECT:
                    if (ft->user) {
                        buf_append_fmt(td, "&%s", ft->user->c_desc_name);
                    } else {
                        buf_append_cstr(td, "NULL");
                    }
                    break;
                default:
                    buf_append_cstr(td, "NULL");
                    break;
            }
            buf_append_cstr(td, " },\n");
            return true;
        case CG_VK_AGGREGATE: {
            if (cg_aggregate_pointer_slot_count(ft) == 0U) {
                return cg_fail(cg, err_token,
                    "codegen: aggregate field has no flatten rule (unknown aggregate kind)");
            }
            char field_base[256];
            int n = snprintf(field_base, sizeof(field_base),
                             "offsetof(struct %s, %s)",
                             struct_name, field_c_name);
            if (n < 0 || (size_t)n >= sizeof(field_base)) {
                return cg_fail(cg, err_token,
                    "codegen: aggregate field offset expression exceeds buffer");
            }
            cg_emit_aggregate_pointer_slot_rows(td, field_base, ft);
            return true;
        }
    }
    return true;
}

static bool cg_emit_field_release(CG *cg, Buf *td,
                                  const char *field_c_name,
                                  const CGType *ft,
                                  FengToken err_token) {
    switch (cgtype_value_kind(ft)) {
        case CG_VK_TRIVIAL:
            return true;
        case CG_VK_MANAGED_POINTER:
            buf_append_fmt(td, "    feng_release(_o->%s);\n", field_c_name);
            return true;
        case CG_VK_AGGREGATE: {
            const char *desc = cg_aggregate_field_desc_name(ft);
            if (desc == NULL) {
                return cg_fail(cg, err_token,
                    "codegen: aggregate field has no descriptor symbol (unknown aggregate kind)");
            }
            buf_append_fmt(td,
                "    feng_aggregate_release(&_o->%s, &%s);\n",
                field_c_name, desc);
            return true;
        }
    }
    return true;
}

/* Emit `struct Feng__mod__T;` and the descriptor extern into `headers` so
 * cross references compile in any order. */
static void cg_emit_user_type_forward(CG *cg, const UserType *t) {
    buf_append_fmt(&cg->headers, "struct %s;\n", t->c_struct_name);
    buf_append_fmt(&cg->headers, "extern const FengTypeDescriptor %s;\n",
                   t->c_desc_name);
}

/* Emit struct body, finalizer, and descriptor into `type_defs`. */
static void cg_emit_user_type_definition(CG *cg, UserType *t) {
    Buf *td = &cg->type_defs;
    buf_append_fmt(td, "struct %s {\n", t->c_struct_name);
    buf_append_cstr(td, "    FengManagedHeader _hdr;\n");
    for (size_t i = 0; i < t->field_count; i++) {
        buf_append_cstr(td, "    ");
        cg_emit_c_type(td, t->fields[i].type);
        buf_append_fmt(td, " %s;\n", t->fields[i].c_name);
    }
    buf_append_cstr(td, "};\n\n");

    /* release_children: codegen-emitted callback that drops every managed
     * field of an instance. Only emitted when the type actually holds at
     * least one managed reference; otherwise the descriptor's
     * .release_children slot is left NULL so the runtime can skip the call.
     * This is a separate concept from the user finalizer (which lives in
     * descriptor.finalizer); see docs/feng-lifetime.md §11/§13.2 and the
     * FengReleaseChildrenFn typedef in feng_runtime.h.
     *
     * Per-field dispatch goes through cg_emit_field_release so the AGGREGATE
     * branch (Step 4b) can hook in without touching this driver. */
    bool any_managed = false;
    for (size_t i = 0; i < t->field_count; i++) {
        if (cgtype_value_kind(t->fields[i].type) != CG_VK_TRIVIAL) {
            any_managed = true;
            break;
        }
    }
    /* UserType is allocated via calloc by the type registry; the cast is safe
     * because we own the only writer of this field. */
    if (any_managed) {
        Buf rcn; buf_init(&rcn);
        buf_append_fmt(&rcn, "%s__release_children", t->c_struct_name);
        t->c_release_children_name = rcn.data;

        buf_append_fmt(td, "static void %s(void *_self) {\n",
                       t->c_release_children_name);
        buf_append_fmt(td, "    struct %s *_o = (struct %s *)_self;\n",
                       t->c_struct_name, t->c_struct_name);
        for (size_t i = 0; i < t->field_count; i++) {
            if (!cg_emit_field_release(cg, td, t->fields[i].c_name,
                                       t->fields[i].type, t->decl->token)) {
                return;
            }
        }
        buf_append_cstr(td, "}\n\n");
    }

    /* User finalizer (`fn ~T()`). The thunk body is emitted later in Pass 4
     * — alongside methods — so it can resolve module-level externs and free
     * functions; here we only emit a forward declaration so the descriptor
     * below can take its address. The runtime invokes this through
     * descriptor.finalizer with `void *self` pointing at the
     * FengManagedHeader-prefixed instance. Per docs/feng-lifetime.md §13.2,
     * any uncaught exception escaping the body is a deterministic crash; that
     * barrier is enforced one layer up by feng_finalizer_invoke
     * (src/runtime/feng_object.c). */
    if (t->finalizer) {
        buf_append_fmt(td, "static void %s(void *_self);\n\n",
                       t->c_finalizer_name);
    }

    /* Phase 1B managed-field metadata: enumerate every slot of this type that
     * holds a managed reference. The cycle collector reads this table to do
     * non-mutating object traversal during trial deletion. The deterministic
     * ARC release path does NOT consult it; the user finalizer above remains
     * the sole owner of per-field release ordering.
     *
     * Per-field dispatch goes through cg_field_managed_descriptor_count and
     * cg_emit_field_managed_descriptors so AGGREGATE field flattening (Step
     * 4b) can be added in one place without re-walking this driver. */
    size_t managed_count = 0U;
    for (size_t i = 0; i < t->field_count; i++) {
        managed_count += cg_field_managed_descriptor_count(cg, t->fields[i].type,
                                                           t->decl->token);
    }
    if (managed_count > 0U) {
        buf_append_fmt(td,
            "static const FengManagedFieldDescriptor %s__managed_fields[] = {\n",
            t->c_desc_name);
        for (size_t i = 0; i < t->field_count; i++) {
            if (!cg_emit_field_managed_descriptors(cg, td, t->c_struct_name,
                                                   t->fields[i].c_name,
                                                   t->fields[i].type,
                                                   t->decl->token)) {
                return;
            }
        }
        buf_append_cstr(td, "};\n\n");
    }

    /* Look up Phase 1B cyclicity marker for this type. Acyclic types yield
     * zero collector overhead at runtime: feng_release skips the candidate
     * buffer entirely when descriptor->is_potentially_cyclic == false. */
    bool is_cyclic = feng_semantic_type_is_potentially_cyclic(cg->analysis, t->decl);

    buf_append_fmt(td,
        "const FengTypeDescriptor %s = {\n"
        "    .name = \"%s.%s\",\n"
        "    .size = sizeof(struct %s),\n"
        "    .finalizer = %s,\n"
        "    .release_children = %s,\n"
        "    .is_potentially_cyclic = %s,\n"
        "    .managed_field_count = %zu,\n",
        t->c_desc_name,
        cg->module_dot_name, t->feng_name,
        t->c_struct_name,
        t->c_finalizer_name ? t->c_finalizer_name : "NULL",
        t->c_release_children_name ? t->c_release_children_name : "NULL",
        is_cyclic ? "true" : "false",
        managed_count);
    if (managed_count > 0U) {
        buf_append_fmt(td,
            "    .managed_fields = %s__managed_fields,\n",
            t->c_desc_name);
    } else {
        buf_append_cstr(td, "    .managed_fields = NULL,\n");
    }
    buf_append_cstr(td, "};\n\n");

    /* Default-zero factory: per docs/feng-type.md §5/§7 every non-cyclic
     * user object type has a recursive default zero instance. The factory
     * allocates a fresh object via feng_object_new (which zeroes the struct
     * memory and runs no constructor) and then materialises non-zero
     * defaults for each managed field (string/array/object). Numeric/bool
     * fields stay 0 from the calloc inside feng_object_new. The returned
     * reference is owned by the caller (+1).
     *
     * Cyclic types intentionally have no factory; cg_default_value_expr
     * rejects them and the c_default_zero_name slot is freed below. */
    if (cg_user_type_is_default_zero_safe(cg, t)) {
        /* Forward declare into headers so default_zero callers in any pass
         * (including this same type's own field defaults) can reference it
         * regardless of source order. */
        buf_append_fmt(&cg->headers,
            "struct %s *%s(void);\n",
            t->c_struct_name, t->c_default_zero_name);
        buf_append_fmt(td,
            "struct %s *%s(void) {\n"
            "    struct %s *_o = (struct %s *)feng_object_new(&%s);\n",
            t->c_struct_name, t->c_default_zero_name,
            t->c_struct_name, t->c_struct_name, t->c_desc_name);
        for (size_t i = 0; i < t->field_count; i++) {
            const CGType *ft = t->fields[i].type;
            switch (ft->kind) {
                case CG_TYPE_STRING:
                    buf_append_fmt(td,
                        "    _o->%s = feng_string_default();\n",
                        t->fields[i].c_name);
                    break;
                case CG_TYPE_ARRAY: {
                    char *desc = cg_array_element_descriptor(ft->element);
                    char *elem_cty = cg_ctype_dup(ft->element);
                    bool em = ft->element ? cgtype_is_managed(ft->element) : false;
                    bool ea = ft->element ? cgtype_is_aggregate(ft->element) : false;
                    if (ea) {
                        const char *agg_desc = cg_aggregate_field_desc_name(ft->element);
                        if (agg_desc != NULL) {
                            buf_append_fmt(td,
                                "    _o->%s = feng_array_new_kinded("
                                "FENG_VALUE_AGGREGATE_WITH_MANAGED_SLOTS, &%s, NULL, sizeof(%s), (size_t)0);\n",
                                t->fields[i].c_name, agg_desc,
                                elem_cty ? elem_cty : "void *");
                        } else {
                            buf_append_fmt(td,
                                "    _o->%s = feng_array_new(NULL, sizeof(%s), false, (size_t)0);\n",
                                t->fields[i].c_name,
                                elem_cty ? elem_cty : "void *");
                        }
                    } else {
                        buf_append_fmt(td,
                            "    _o->%s = feng_array_new(%s, sizeof(%s), %s, (size_t)0);\n",
                            t->fields[i].c_name, desc ? desc : "NULL",
                            elem_cty ? elem_cty : "void *", em ? "true" : "false");
                    }
                    free(desc);
                    free(elem_cty);
                    break;
                }
                case CG_TYPE_OBJECT:
                    if (ft->user) {
                        buf_append_fmt(td,
                            "    _o->%s = %s();\n",
                            t->fields[i].c_name,
                            ft->user->c_default_zero_name);
                    }
                    break;
                default:
                    /* Numeric/bool: feng_object_new already zeroed. */
                    break;
            }
        }
        buf_append_cstr(td, "    return _o;\n}\n\n");
    } else {
        free(t->c_default_zero_name);
        t->c_default_zero_name = NULL;
    }
}

/* Emit a method body. Mirrors cg_emit_function but with a leading `self`
 * parameter typed as `struct T *`. */
static bool cg_emit_user_method(CG *cg, const UserType *t, const UserMethod *m) {
    Buf *p = &cg->fn_protos;
    buf_append_cstr(p, "static ");
    cg_emit_c_type(p, m->return_type);
    buf_append_fmt(p, " %s(struct %s *self", m->c_name, t->c_struct_name);
    for (size_t i = 0; i < m->param_count; i++) {
        buf_append_cstr(p, ", ");
        cg_emit_c_type(p, m->param_types[i]);
        buf_append_fmt(p, " %s", m->param_names[i] ? m->param_names[i] : "_p");
    }
    buf_append_cstr(p, ");\n");

    Buf *body = &cg->fn_defs;
    cg->cur_body = body;
    cg->cur_return_type = m->return_type;
    cg->cur_fn_is_main = false;
    buf_append_cstr(body, "static ");
    cg_emit_c_type(body, m->return_type);
    buf_append_fmt(body, " %s(struct %s *self", m->c_name, t->c_struct_name);

    Scope *fn_scope = scope_push(NULL);
    if (!fn_scope) return cg_fail(cg, m->member->token, "codegen: out of memory");
    cg->cur_scope = fn_scope;
    cg->tmp_counter = 0;
    cg->loop_depth = 0;
    cg->try_depth = 0;
    /* Register self as a borrowed param so cg_release_scope skips it. */
    {
        CGType *self_t = cgtype_new(CG_TYPE_OBJECT);
        if (!self_t) {
            cg->cur_scope = NULL; scope_pop_free(fn_scope);
            return false;
        }
        self_t->user = t;
        scope_add(fn_scope, "self", "self", self_t, true);
    }
    for (size_t i = 0; i < m->param_count; i++) {
        buf_append_cstr(body, ", ");
        cg_emit_c_type(body, m->param_types[i]);
        const char *pn = m->param_names[i] ? m->param_names[i] : "_p";
        buf_append_fmt(body, " %s", pn);
        CGType *pt = cgtype_clone(m->param_types[i]);
        scope_add(fn_scope, pn, pn, pt, true);
    }
    buf_append_cstr(body, ") {\n");
    buf_append_cstr(body, "    (void)self;\n");
    for (size_t i = 0; i < m->param_count; i++) {
        const char *pn = m->param_names[i] ? m->param_names[i] : "_p";
        buf_append_fmt(body, "    (void)%s;\n", pn);
    }
    if (!cg_emit_block(cg, m->member->as.callable.body)) {
        cg->cur_scope = NULL; scope_pop_free(fn_scope);
        cg->cur_body = NULL; cg->cur_return_type = NULL;
        return false;
    }
    cg_release_scope(cg, fn_scope);
    if (m->return_type->kind == CG_TYPE_VOID) {
        buf_append_cstr(body, "    return;\n");
    } else {
        buf_append_cstr(body,
            "    feng_panic(\"method reached end without return\");\n");
    }
    buf_append_cstr(body, "}\n\n");
    cg->cur_scope = NULL; scope_pop_free(fn_scope);
    cg->cur_body = NULL;
    cg->cur_return_type = NULL;
    return true;
}

/* Emit the user finalizer thunk body. Mirrors cg_emit_user_method but with
 * the runtime FengFinalizerFn(void *self) entry signature and a synthetic
 * `self` binding. The forward declaration is emitted earlier in
 * cg_emit_user_type_definition so the descriptor can take the symbol's
 * address before the body exists. */
static bool cg_emit_user_finalizer(CG *cg, const UserType *t) {
    if (!t->finalizer) return true;
    const FengTypeMember *fm = t->finalizer;
    Buf *body = &cg->fn_defs;
    buf_append_fmt(body, "static void %s(void *_self) {\n",
                   t->c_finalizer_name);
    buf_append_fmt(body, "    struct %s *self = (struct %s *)_self;\n",
                   t->c_struct_name, t->c_struct_name);
    buf_append_cstr(body, "    (void)self;\n");

    cg->cur_body = body;
    CGType *void_t = cgtype_new(CG_TYPE_VOID);
    if (!void_t) return cg_fail(cg, fm->token, "codegen: out of memory");
    cg->cur_return_type = void_t;
    cg->cur_fn_is_main = false;
    cg->tmp_counter = 0;
    cg->loop_depth = 0;
    cg->try_depth = 0;

    Scope *fn_scope = scope_push(NULL);
    if (!fn_scope) {
        cgtype_free(void_t);
        cg->cur_body = NULL; cg->cur_return_type = NULL;
        return cg_fail(cg, fm->token, "codegen: out of memory");
    }
    cg->cur_scope = fn_scope;
    {
        CGType *self_t = cgtype_new(CG_TYPE_OBJECT);
        if (!self_t) {
            cg->cur_scope = NULL; scope_pop_free(fn_scope);
            cgtype_free(void_t);
            cg->cur_body = NULL; cg->cur_return_type = NULL;
            return false;
        }
        self_t->user = t;
        scope_add(fn_scope, "self", "self", self_t, true);
    }
    if (!cg_emit_block(cg, fm->as.callable.body)) {
        cg->cur_scope = NULL; scope_pop_free(fn_scope);
        cgtype_free(void_t);
        cg->cur_body = NULL; cg->cur_return_type = NULL;
        return false;
    }
    cg_release_scope(cg, fn_scope);
    buf_append_cstr(body, "    return;\n}\n\n");

    cg->cur_scope = NULL; scope_pop_free(fn_scope);
    cgtype_free(void_t);
    cg->cur_body = NULL;
    cg->cur_return_type = NULL;
    return true;
}

static const FreeFn *cg_lookup_main(const CG *cg) {
    for (size_t i = 0; i < cg->free_fn_count; i++) {
        if (strcmp(cg->free_fns[i].feng_name, "main") == 0) return &cg->free_fns[i];
    }
    return NULL;
}

static char *cg_finalize(CG *cg) {
    Buf out; buf_init(&out);
    buf_append_cstr(&out,
        "/* Feng generated code — do not edit. */\n"
        "#include <stdbool.h>\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <string.h>\n"
        "#include \"runtime/feng_runtime.h\"\n\n");
    if (cg->headers.length) buf_append(&out, cg->headers.data, cg->headers.length);
    buf_append_cstr(&out, "\n");
    if (cg->type_defs.length) buf_append(&out, cg->type_defs.data, cg->type_defs.length);
    buf_append_cstr(&out, "\n");
    if (cg->statics.length) buf_append(&out, cg->statics.data, cg->statics.length);
    buf_append_cstr(&out, "\n");
    if (cg->fn_protos.length) buf_append(&out, cg->fn_protos.data, cg->fn_protos.length);
    buf_append_cstr(&out, "\n");
    if (cg->fn_defs.length) buf_append(&out, cg->fn_defs.data, cg->fn_defs.length);
    if (cg->witness_defs.length) buf_append(&out, cg->witness_defs.data, cg->witness_defs.length);
    return out.data;
}

static void cg_dispose(CG *cg) {
    buf_free(&cg->headers);
    buf_free(&cg->type_defs);
    buf_free(&cg->statics);
    buf_free(&cg->fn_protos);
    buf_free(&cg->fn_defs);
    buf_free(&cg->witness_defs);
    free(cg->module_mangle);
    free(cg->module_dot_name);
    for (size_t i = 0; i < cg->extern_count; i++) {
        free(cg->externs[i].name);
        for (size_t j = 0; j < cg->externs[i].param_count; j++)
            cgtype_free(cg->externs[i].param_types[j]);
        free(cg->externs[i].param_types);
        cgtype_free(cg->externs[i].return_type);
    }
    free(cg->externs);
    for (size_t i = 0; i < cg->free_fn_count; i++) {
        free(cg->free_fns[i].feng_name);
        free(cg->free_fns[i].c_name);
        for (size_t j = 0; j < cg->free_fns[i].param_count; j++) {
            cgtype_free(cg->free_fns[i].param_types[j]);
            free(cg->free_fns[i].param_names[j]);
        }
        free(cg->free_fns[i].param_types);
        free(cg->free_fns[i].param_names);
        cgtype_free(cg->free_fns[i].return_type);
    }
    free(cg->free_fns);
    for (size_t i = 0; i < cg->user_type_count; i++) {
        UserType *ut = &cg->user_types[i];
        free(ut->feng_name);
        free(ut->c_struct_name);
        free(ut->c_desc_name);
        free(ut->c_release_children_name);
        free(ut->c_finalizer_name);
        free(ut->c_default_zero_name);
        for (size_t j = 0; j < ut->field_count; j++) {
            free(ut->fields[j].feng_name);
            free(ut->fields[j].c_name);
            cgtype_free(ut->fields[j].type);
        }
        free(ut->fields);
        for (size_t j = 0; j < ut->method_count; j++) {
            UserMethod *um = &ut->methods[j];
            free(um->feng_name);
            free(um->c_name);
            cgtype_free(um->return_type);
            for (size_t k = 0; k < um->param_count; k++) {
                cgtype_free(um->param_types[k]);
                free(um->param_names[k]);
            }
            free(um->param_types);
            free(um->param_names);
        }
        free(ut->methods);
    }
    free(cg->user_types);
    for (size_t i = 0; i < cg->user_spec_count; i++) {
        UserSpec *us = &cg->user_specs[i];
        free(us->feng_name);
        free(us->c_value_struct_name);
        free(us->c_witness_struct_name);
        free(us->c_aggregate_desc_name);
        free(us->c_aggregate_slots_name);
        free(us->c_aggregate_default_name);
        free(us->c_aggregate_init_fn_name);
        free(us->c_default_subject_struct_name);
        free(us->c_default_subject_desc_name);
        free(us->c_default_subject_new_name);
        free(us->c_default_witness_name);
        for (size_t j = 0; j < us->member_count; j++) {
            UserSpecMember *sm = &us->members[j];
            free(sm->feng_name);
            free(sm->c_field_name);
            cgtype_free(sm->type);
            for (size_t k = 0; k < sm->param_count; k++) {
                cgtype_free(sm->param_types[k]);
                free(sm->param_names[k]);
            }
            free(sm->param_types);
            free(sm->param_names);
        }
        free(us->members);
    }
    free(cg->user_specs);
    for (size_t i = 0; i < cg->user_fit_count; i++) {
        UserFit *uf = &cg->user_fits[i];
        for (size_t j = 0; j < uf->method_count; j++) {
            UserMethod *um = &uf->methods[j];
            free(um->feng_name);
            free(um->c_name);
            cgtype_free(um->return_type);
            for (size_t k = 0; k < um->param_count; k++) {
                cgtype_free(um->param_types[k]);
                free(um->param_names[k]);
            }
            free(um->param_types);
            free(um->param_names);
        }
        free(uf->methods);
        free(uf->c_prefix);
    }
    free(cg->user_fits);
    for (size_t i = 0; i < cg->witness_table_count; i++) {
        free(cg->witness_tables[i].c_var);
    }
    free(cg->witness_tables);
    for (size_t i = 0; i < cg->module_binding_count; i++) {
        free(cg->module_bindings[i].feng_name);
        free(cg->module_bindings[i].c_name);
        cgtype_free(cg->module_bindings[i].type);
    }
    free(cg->module_bindings);
    for (size_t i = 0; i < cg->string_literal_count; i++) {
        free(cg->string_literals[i].content);
        free(cg->string_literals[i].c_var);
    }
    free(cg->string_literals);
}

bool feng_codegen_emit_program(const FengSemanticAnalysis *analysis,
                               FengCompileTarget target,
                               const FengCodegenOptions *options,
                               FengCodegenOutput *out_output,
                               FengCodegenError *out_error) {
    (void)options;
    if (!analysis || !out_output) return false;
    if (out_error) {
        out_error->message = NULL;
        out_error->path = NULL;
        memset(&out_error->token, 0, sizeof out_error->token);
    }
    /* Collect every program in deterministic (module, program) order. */
    size_t program_total = 0;
    for (size_t i = 0; i < analysis->module_count; i++) {
        program_total += analysis->modules[i].program_count;
    }
    CG cg = {0};
    cg.error = out_error;
    cg.analysis = analysis;
    if (program_total == 0) {
        cg_fail(&cg, (FengToken){0}, "codegen: no programs to compile");
        cg_dispose(&cg);
        return false;
    }

    const FengProgram **programs = calloc(program_total, sizeof(*programs));
    if (!programs) {
        cg_fail(&cg, (FengToken){0}, "codegen: out of memory collecting programs");
        cg_dispose(&cg);
        return false;
    }
    size_t cursor = 0;
    for (size_t i = 0; i < analysis->module_count; i++) {
        for (size_t j = 0; j < analysis->modules[i].program_count; j++) {
            programs[cursor++] = analysis->modules[i].programs[j];
        }
    }

    if (!cg_emit_all_programs(&cg, programs, program_total, target)) {
        free(programs);
        cg_dispose(&cg);
        return false;
    }

    if (target == FENG_COMPILE_TARGET_BIN) {
        const FreeFn *main_fn = cg_lookup_main(&cg);
        if (!main_fn) {
            cg_fail(&cg, programs[0]->module_token,
                    "codegen: bin target requires `main` function");
            free(programs);
            cg_dispose(&cg);
            return false;
        }
        if (!cg_emit_main_wrapper(&cg, main_fn)) {
            free(programs);
            cg_dispose(&cg);
            return false;
        }
    }

    free(programs);

    /* Emit the string literal storage table after all bodies are generated
     * so that strings interned during module-binding initialisers are
     * captured. The init prelude inside `main()` references these slots. */
    cg_emit_string_literal_table(&cg);

    char *src = cg_finalize(&cg);
    if (!src) {
        cg_fail(&cg, (FengToken){0}, "codegen: out of memory finalizing output");
        cg_dispose(&cg);
        return false;
    }
    out_output->c_source = src;
    out_output->c_source_length = strlen(src);
    cg_dispose(&cg);
    return true;
}

void feng_codegen_output_free(FengCodegenOutput *output) {
    if (!output) return;
    free(output->c_source);
    output->c_source = NULL;
    output->c_source_length = 0;
}

void feng_codegen_error_free(FengCodegenError *error) {
    if (!error) return;
    free(error->message);
    error->message = NULL;
}
