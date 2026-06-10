#ifndef FENG_PARSER_PARSER_H
#define FENG_PARSER_PARSER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "lexer/token.h"

#ifdef __cplusplus
extern "C" {
#endif /* FENG_PARSER_H */



typedef struct FengSlice {
    const char *data;
    size_t length;
} FengSlice;

typedef enum FengVisibility {
    FENG_VISIBILITY_DEFAULT = 0,
    FENG_VISIBILITY_PRIVATE,
    FENG_VISIBILITY_PUBLIC
} FengVisibility;

typedef enum FengMutability {
    FENG_MUTABILITY_DEFAULT = 0,
    FENG_MUTABILITY_LET,
    FENG_MUTABILITY_VAR
} FengMutability;

typedef enum FengTypeRefKind {
    FENG_TYPE_REF_NAMED = 0,
    FENG_TYPE_REF_POINTER,
    FENG_TYPE_REF_ARRAY
} FengTypeRefKind;

typedef struct FengTypeRef FengTypeRef;
typedef struct FengExpr FengExpr;
typedef struct FengStmt FengStmt;
typedef struct FengBlock FengBlock;
typedef struct FengDecl FengDecl;
typedef struct FengTypeMember FengTypeMember;

/* Resolution metadata attached by the semantic analyzer to call expressions.
 * Codegen and tooling consume this instead of re-running symbol lookup. */
typedef enum FengResolvedCallableKind {
    FENG_RESOLVED_CALLABLE_NONE = 0,
    FENG_RESOLVED_CALLABLE_FUNCTION,         /* free / module-level fn */
    FENG_RESOLVED_CALLABLE_TYPE_METHOD,      /* method declared in a type body */
    FENG_RESOLVED_CALLABLE_FIT_METHOD,       /* method declared in a fit body */
    FENG_RESOLVED_CALLABLE_TYPE_STATIC_METHOD, /* static method declared in a type body */
    FENG_RESOLVED_CALLABLE_FIT_STATIC_METHOD,  /* static method declared in a fit body */
    FENG_RESOLVED_CALLABLE_TYPE_CONSTRUCTOR  /* constructor of a concrete type */
} FengResolvedCallableKind;

typedef struct FengResolvedCallable {
    FengResolvedCallableKind kind;
    const FengDecl *function_decl;     /* set for FUNCTION */
    const FengDecl *owner_type_decl;   /* set for TYPE_METHOD/FIT_METHOD/TYPE_CONSTRUCTOR */
    const FengTypeMember *member;      /* set for TYPE_METHOD/FIT_METHOD/TYPE_CONSTRUCTOR */
    const FengDecl *fit_decl;          /* set for FIT_METHOD */
    /* 调用点 receiver 的具体化类型引用（含 type_args）。用于 post-pass
     * 对方法返回类型做参数代入以收集间接泛型依赖。借用 AST 生命周期。 */
    const FengTypeRef *owner_instance_type_ref;
} FengResolvedCallable;

typedef struct FengParameter {
    FengToken token;
    FengMutability mutability;
    FengSlice name;
    FengTypeRef *type;
    bool is_variadic; /* true if declared as T...; type is stored as T[] */
} FengParameter;

typedef struct FengAnnotation {
    FengToken token;
    FengSlice name;
    FengAnnotationKind builtin_kind;
    FengExpr **args;
    size_t arg_count;
} FengAnnotation;

/* A single type parameter definition, e.g. <T> or <T: Named>. */
typedef struct FengTypeParam {
    FengToken token;
    FengSlice name;          /* parameter name (e.g. "T") */
    FengTypeRef *constraint; /* NULL = unconstrained */
} FengTypeParam;

struct FengTypeRef {
    FengToken token;
    FengTypeRefKind kind;
    /* Per docs/feng-builtin-type.md §5: a writable element layer is marked
     * with `[!]` on the corresponding array layer. Only meaningful when kind ==
     * FENG_TYPE_REF_ARRAY; ignored otherwise. */
    bool array_element_writable;
    union {
        struct {
            FengSlice *segments;
            size_t segment_count;
            FengTypeRef **type_args;    /* NULL if non-generic */
            size_t type_arg_count;
        } named;
        FengTypeRef *inner;
    } as;
};

typedef struct FengObjectFieldInit {
    FengToken token;
    FengSlice name;
    FengExpr *value;
} FengObjectFieldInit;

typedef enum FengLambdaCaptureKind {
    FENG_LAMBDA_CAPTURE_LOCAL = 0, /* captured outer local binding or parameter */
    FENG_LAMBDA_CAPTURE_SELF       /* captured outer `self` reference */
} FengLambdaCaptureKind;

typedef struct FengLambdaCapture {
    FengLambdaCaptureKind kind;
    FengSlice name;             /* identifier name if LOCAL; empty when SELF */
    FengMutability mutability;  /* original binding mutability for LOCAL captures */
} FengLambdaCapture;

typedef enum FengMatchLabelKind {
    FENG_MATCH_LABEL_VALUE = 0,
    FENG_MATCH_LABEL_RANGE,
    FENG_MATCH_LABEL_TYPE
} FengMatchLabelKind;

typedef struct FengMatchLabel {
    FengToken token;
    FengMatchLabelKind kind;
    /* VALUE: literal-or-let-bound expression evaluated to a single value. */
    FengExpr *value;
    /* RANGE (closed [low, high], integer-only): low and high constant exprs. */
    FengExpr *range_low;
    FengExpr *range_high;
    /* TYPE: union member type label. For a single-segment identifier label,
     * value may also be populated so non-union match targets can continue to
     * treat the same syntax as a named constant. */
    FengTypeRef *type;
} FengMatchLabel;

typedef struct FengMatchBranch {
    FengToken token;
    FengMatchLabel *labels;
    size_t label_count;
    FengBlock *body;
} FengMatchBranch;

typedef struct FengTryCatchClause {
    FengToken token;
    FengSlice name;
    FengTypeRef *type;
    FengBlock *body;
} FengTryCatchClause;

typedef enum FengExprKind {
    FENG_EXPR_IDENTIFIER = 0,
    FENG_EXPR_SELF,
    FENG_EXPR_BOOL,
    FENG_EXPR_INTEGER,
    FENG_EXPR_FLOAT,
    FENG_EXPR_STRING,
    FENG_EXPR_ARRAY_LITERAL,
    FENG_EXPR_TUPLE_LITERAL,
    FENG_EXPR_OBJECT_LITERAL,
    FENG_EXPR_GENERIC_TARGET,
    FENG_EXPR_CALL,
    FENG_EXPR_MEMBER,
    FENG_EXPR_INDEX,
    FENG_EXPR_UNARY,
    FENG_EXPR_BINARY,
    FENG_EXPR_LAMBDA,
    FENG_EXPR_CAST,
    FENG_EXPR_IF,
    FENG_EXPR_MATCH,
    FENG_EXPR_TRY,
    FENG_EXPR_ARRAY_NEW
} FengExprKind;

struct FengExpr {
    FengToken token;
    FengExprKind kind;
    union {
        FengSlice identifier;
        bool boolean;
        int64_t integer;
        double floating;
        FengSlice string;
        struct {
            FengExpr **items;
            size_t count;
        } array_literal;
        struct {
            FengExpr **items;
            size_t count;
        } tuple_literal;
        struct {
            FengExpr *target;
            FengObjectFieldInit *fields;
            size_t field_count;
        } object_literal;
        struct {
            FengExpr *target;
            FengTypeRef **type_args;
            size_t type_arg_count;
        } generic_target;
        struct {
            FengExpr *callee;
            FengExpr **args;
            size_t arg_count;
            FengResolvedCallable resolved_callable;
            FengTypeRef **explicit_type_args;  /* for callee<T1, T2>(...) */
            size_t explicit_type_arg_count;
            bool has_explicit_type_args;
        } call;
        struct {
            FengExpr *object;
            FengSlice member;
        } member;
        struct {
            FengExpr *object;
            FengExpr *index;
        } index;
        struct {
            FengTokenKind op;
            FengExpr *operand;
        } unary;
        struct {
            FengTokenKind op;
            FengExpr *left;
            FengExpr *right;
        } binary;
        struct {
            FengParameter *params;
            size_t param_count;
            bool is_block_body;          /* false: single expression; true: block body */
            FengExpr *body;              /* body when is_block_body == false */
            FengBlock *body_block;       /* body when is_block_body == true */
            FengLambdaCapture *captures; /* filled by the semantic analyzer */
            size_t capture_count;
            bool captures_self;          /* derived: any FENG_LAMBDA_CAPTURE_SELF entry */
        } lambda;
        struct {
            FengTypeRef *type;
            FengExpr *value;
        } cast;
        struct {
            FengExpr *condition;
            FengBlock *then_block;
            FengBlock *else_block;
        } if_expr;
        struct {
            FengExpr *target;
            FengMatchBranch *branches;
            size_t branch_count;
            FengBlock *else_block;
        } match_expr;
        struct {
            FengExpr *body;
            FengTryCatchClause *clauses;
            size_t clause_count;
        } try_expr;
        struct {
            FengTypeRef *element_type;  /* the element type, e.g. MapEntry<K,V> */
            FengExpr    *size;          /* the size expression n */
        } array_new;
    } as;
};

typedef struct FengBinding {
    FengToken token;
    FengMutability mutability;
    FengSlice name;
    FengTypeRef *type;
    FengExpr *initializer;
    bool is_destructure;
    FengSlice *destructure_names;
    size_t destructure_count;
} FengBinding;

typedef struct FengIfClause {
    FengToken token;
    FengExpr *condition;
    FengBlock *block;
} FengIfClause;

typedef enum FengStmtKind {
    FENG_STMT_BLOCK = 0,
    FENG_STMT_BINDING,
    FENG_STMT_ASSIGN,
    FENG_STMT_EXPR,
    FENG_STMT_TRY,
    FENG_STMT_IF,
    FENG_STMT_MATCH,
    FENG_STMT_WHILE,
    FENG_STMT_FOR,
    FENG_STMT_RETURN,
    FENG_STMT_THROW,
    FENG_STMT_BREAK,
    FENG_STMT_CONTINUE
} FengStmtKind;

struct FengBlock {
    FengToken token;
    FengStmt **statements;
    size_t statement_count;
};

struct FengStmt {
    FengToken token;
    FengStmtKind kind;
    union {
        FengBlock *block;
        FengBinding binding;
        struct {
            FengTokenKind op;
            FengExpr *target;
            FengExpr *value;
        } assign;
        FengExpr *expr;
        struct {
            FengIfClause *clauses;
            size_t clause_count;
            FengBlock *else_block;
        } if_stmt;
        struct {
            FengExpr *target;
            FengMatchBranch *branches;
            size_t branch_count;
            FengBlock *else_block;
        } match_stmt;
        struct {
            FengExpr *condition;
            FengBlock *body;
        } while_stmt;
        struct {
            /* Common to both forms. */
            bool is_for_in;
            FengBlock *body;
            /* Three-clause form (when is_for_in == false). */
            FengStmt *init;
            FengExpr *condition;
            FengStmt *update;
            /* for/in form (when is_for_in == true). */
            FengBinding iter_binding; /* name + mutability + type (NULL until inferred) */
            FengExpr *iter_expr;
            /* Iterator protocol metadata (filled by semantic when iter_expr
             * type implements @iterable/@iterator; NULL for array for/in). */
            const FengTypeMember *iter_iterable_method;
            const FengTypeMember *iter_iterator_method;
            const FengDecl *iter_cursor_type_decl;
            const FengTypeRef *iter_cursor_type_ref;
            /* Substituted result tuple type ref (e.g. (Bool, T) with the
             * enclosing type's T).  Synthesized by the analyzer; lifetime
             * managed by FengSemanticAnalysis.synthesized_type_refs so that
             * post-analysis passes (reifiable_deps) can safely read it. */
            const FengTypeRef *iter_result_type_ref;
        } for_stmt;
        FengExpr *return_value;
        FengExpr *throw_value;
    } as;
};

typedef enum FengTypeMemberKind {
    FENG_TYPE_MEMBER_FIELD = 0,
    FENG_TYPE_MEMBER_METHOD,
    FENG_TYPE_MEMBER_CONSTRUCTOR,
    FENG_TYPE_MEMBER_FINALIZER
} FengTypeMemberKind;

typedef struct FengCallableSignature {
    FengToken token;
    FengSlice name;
    FengTypeParam *type_params; /* generic type parameters, e.g. fn f<T>(...) */
    size_t type_param_count;
    FengParameter *params;
    size_t param_count;
    FengTypeRef *return_type;
    FengBlock *body;
    FengSlice *bound_member_names; /* constructor-stage final bindings imported from .ft */
    size_t bound_member_count;
} FengCallableSignature;

typedef struct FengEnumItem {
    FengToken token;
    FengSlice name;
    bool has_explicit_value;
    int64_t explicit_value;
} FengEnumItem;

struct FengTypeMember {
    FengToken token;
    FengTypeMemberKind kind;
    FengVisibility visibility;
    bool is_static;
    FengSlice doc_comment;
    FengAnnotation *annotations;
    size_t annotation_count;
    union {
        struct {
            FengMutability mutability;
            FengSlice name;
            FengTypeRef *type;
            FengExpr *initializer;
            bool declaration_bound; /* declaration-stage final binding fact */
        } field;
        FengCallableSignature callable;
    } as;
};

typedef enum FengDeclKind {
    FENG_DECL_GLOBAL_BINDING = 0,
    FENG_DECL_TYPE,
    FENG_DECL_ENUM,
    FENG_DECL_SPEC,
    FENG_DECL_FIT,
    FENG_DECL_FUNCTION
} FengDeclKind;

typedef enum FengSpecForm {
    FENG_SPEC_FORM_OBJECT = 0,
    FENG_SPEC_FORM_CALLABLE,
    FENG_SPEC_FORM_UNION
} FengSpecForm;

typedef struct FengUseDecl {
    FengToken token;
    FengSlice *segments;
    size_t segment_count;
    FengSlice alias;
    bool has_alias;
} FengUseDecl;

struct FengDecl {
    FengToken token;
    FengDeclKind kind;
    FengVisibility visibility;
    bool is_extern;
    FengSlice doc_comment;
    FengAnnotation *annotations;
    size_t annotation_count;
    union {
        FengBinding binding;
        struct {
            FengSlice name;
            FengTypeParam *type_params; /* generic type parameters, e.g. type Box<T> */
            size_t type_param_count;
            FengTypeMember **members;
            size_t member_count;
            FengTypeRef **declared_specs;
            size_t declared_spec_count;
            bool is_tuple;
        } type_decl;
        struct {
            FengSlice name;
            FengEnumItem *items;
            size_t item_count;
        } enum_decl;
        struct {
            FengSlice name;
            FengTypeParam *type_params; /* generic type parameters, e.g. spec Reader<T> */
            size_t type_param_count;
            FengSpecForm form;
            FengTypeRef **parent_specs;
            size_t parent_spec_count;
            union {
                struct {
                    FengTypeMember **members;
                    size_t member_count;
                } object;
                struct {
                    FengParameter *params;
                    size_t param_count;
                    FengTypeRef *return_type;
                } callable;
                struct {
                    FengTypeRef **members;
                    size_t member_count;
                } union_form;
            } as;
        } spec_decl;
        struct {
            FengTypeRef *target;
            FengTypeRef **specs;
            size_t spec_count;
            FengTypeMember **members;
            size_t member_count;
            bool has_body;
        } fit_decl;
        FengCallableSignature function_decl;
    } as;
};

typedef struct FengProgram {
    const char *path;
    FengToken module_token;
    FengVisibility module_visibility;
    FengSlice *module_segments;
    size_t module_segment_count;
    FengUseDecl *uses;
    size_t use_count;
    FengDecl **declarations;
    size_t declaration_count;
} FengProgram;

typedef struct FengParseError {
    const char *message;
    FengToken token;
} FengParseError;

bool feng_parse_source(const char *source,
                       size_t length,
                       const char *path,
                       FengProgram **out_program,
                       FengParseError *out_error);

void feng_program_free(FengProgram *program);
void feng_program_dump(FILE *stream, const FengProgram *program);

#ifdef __cplusplus
}
#endif

#endif
