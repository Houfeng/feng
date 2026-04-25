#include "parser/parser.h"

#include <stdio.h>

static void dump_indent(FILE *stream, int indent) {
    int index;

    for (index = 0; index < indent; ++index) {
        fputs("  ", stream);
    }
}



static void dump_slice(FILE *stream, FengSlice slice) {
    size_t index;

    for (index = 0U; index < slice.length; ++index) {
        fputc((int)slice.data[index], stream);
    }
}

static void dump_path(FILE *stream, FengSlice *segments, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (index != 0U) {
            fputc('.', stream);
        }
        dump_slice(stream, segments[index]);
    }
}

static const char *visibility_name(FengVisibility visibility) {
    switch (visibility) {
        case FENG_VISIBILITY_PRIVATE:
            return "pr";
        case FENG_VISIBILITY_PUBLIC:
            return "pu";
        case FENG_VISIBILITY_DEFAULT:
        default:
            return "default";
    }
}

static const char *mutability_name(FengMutability mutability) {
    switch (mutability) {
        case FENG_MUTABILITY_LET:
            return "let";
        case FENG_MUTABILITY_VAR:
            return "var";
        case FENG_MUTABILITY_DEFAULT:
        default:
            return "default";
    }
}

static void dump_type_ref(FILE *stream, const FengTypeRef *type_ref) {
    if (type_ref == NULL) {
        fputs("<inferred>", stream);
        return;
    }

    switch (type_ref->kind) {
        case FENG_TYPE_REF_NAMED:
            dump_path(stream, type_ref->as.named.segments, type_ref->as.named.segment_count);
            break;
        case FENG_TYPE_REF_POINTER:
            fputc('*', stream);
            dump_type_ref(stream, type_ref->as.inner);
            break;
        case FENG_TYPE_REF_ARRAY:
            dump_type_ref(stream, type_ref->as.inner);
            fputs("[]", stream);
            break;
    }
}

static void dump_expr(FILE *stream, const FengExpr *expr, int indent);
static void dump_stmt(FILE *stream, const FengStmt *stmt, int indent);
static void dump_block(FILE *stream, const FengBlock *block, int indent);

static void dump_annotations(FILE *stream, const FengAnnotation *annotations, size_t count, int indent) {
    size_t index;
    size_t arg_index;

    for (index = 0U; index < count; ++index) {
        dump_indent(stream, indent);
        fputc('@', stream);
        dump_slice(stream, annotations[index].name);
        if (annotations[index].arg_count > 0U) {
            fputc('(', stream);
            for (arg_index = 0U; arg_index < annotations[index].arg_count; ++arg_index) {
                if (arg_index != 0U) {
                    fputs(", ", stream);
                }
                dump_expr(stream, annotations[index].args[arg_index], 0);
            }
            fputc(')', stream);
        }
        fputc('\n', stream);
    }
}

static void dump_expr(FILE *stream, const FengExpr *expr, int indent) {
    size_t index;

    if (expr == NULL) {
        dump_indent(stream, indent);
        fputs("<null-expr>", stream);
        return;
    }

    if (indent > 0) {
        dump_indent(stream, indent);
    }

    switch (expr->kind) {
        case FENG_EXPR_IDENTIFIER:
        case FENG_EXPR_SELF:
            dump_slice(stream, expr->as.identifier);
            break;
        case FENG_EXPR_BOOL:
            fputs(expr->as.boolean ? "true" : "false", stream);
            break;
        case FENG_EXPR_INTEGER:
            fprintf(stream, "%lld", (long long)expr->as.integer);
            break;
        case FENG_EXPR_FLOAT:
            fprintf(stream, "%.17g", expr->as.floating);
            break;
        case FENG_EXPR_STRING:
            dump_slice(stream, expr->as.string);
            break;
        case FENG_EXPR_ARRAY_LITERAL:
            fputc('[', stream);
            for (index = 0U; index < expr->as.array_literal.count; ++index) {
                if (index != 0U) {
                    fputs(", ", stream);
                }
                dump_expr(stream, expr->as.array_literal.items[index], 0);
            }
            fputc(']', stream);
            break;
        case FENG_EXPR_OBJECT_LITERAL:
            dump_expr(stream, expr->as.object_literal.target, 0);
            fputs(" {", stream);
            for (index = 0U; index < expr->as.object_literal.field_count; ++index) {
                if (index != 0U) {
                    fputs(", ", stream);
                }
                dump_slice(stream, expr->as.object_literal.fields[index].name);
                fputs(": ", stream);
                dump_expr(stream, expr->as.object_literal.fields[index].value, 0);
            }
            fputc('}', stream);
            break;
        case FENG_EXPR_CALL:
            dump_expr(stream, expr->as.call.callee, 0);
            fputc('(', stream);
            for (index = 0U; index < expr->as.call.arg_count; ++index) {
                if (index != 0U) {
                    fputs(", ", stream);
                }
                dump_expr(stream, expr->as.call.args[index], 0);
            }
            fputc(')', stream);
            break;
        case FENG_EXPR_MEMBER:
            dump_expr(stream, expr->as.member.object, 0);
            fputc('.', stream);
            dump_slice(stream, expr->as.member.member);
            break;
        case FENG_EXPR_INDEX:
            dump_expr(stream, expr->as.index.object, 0);
            fputc('[', stream);
            dump_expr(stream, expr->as.index.index, 0);
            fputc(']', stream);
            break;
        case FENG_EXPR_UNARY:
            fprintf(stream, "(%s ", feng_token_kind_name(expr->as.unary.op));
            dump_expr(stream, expr->as.unary.operand, 0);
            fputc(')', stream);
            break;
        case FENG_EXPR_BINARY:
            fputc('(', stream);
            dump_expr(stream, expr->as.binary.left, 0);
            fprintf(stream, " %s ", feng_token_kind_name(expr->as.binary.op));
            dump_expr(stream, expr->as.binary.right, 0);
            fputc(')', stream);
            break;
        case FENG_EXPR_LAMBDA:
            fputc('(', stream);
            for (index = 0U; index < expr->as.lambda.param_count; ++index) {
                if (index != 0U) {
                    fputs(", ", stream);
                }
                if (expr->as.lambda.params[index].mutability != FENG_MUTABILITY_DEFAULT) {
                    fputs(mutability_name(expr->as.lambda.params[index].mutability), stream);
                    fputc(' ', stream);
                }
                dump_slice(stream, expr->as.lambda.params[index].name);
                fputs(": ", stream);
                dump_type_ref(stream, expr->as.lambda.params[index].type);
            }
            if (expr->as.lambda.is_block_body) {
                fputs(") {\n", stream);
                dump_block(stream, expr->as.lambda.body_block, indent + 1);
                dump_indent(stream, indent);
                fputc('}', stream);
            } else {
                fputs(") -> ", stream);
                dump_expr(stream, expr->as.lambda.body, 0);
            }
            break;
        case FENG_EXPR_CAST:
            fputc('(', stream);
            dump_type_ref(stream, expr->as.cast.type);
            fputc(')', stream);
            dump_expr(stream, expr->as.cast.value, 0);
            break;
        case FENG_EXPR_IF:
            fputs("if ", stream);
            dump_expr(stream, expr->as.if_expr.condition, 0);
            fputs(" { ", stream);
            dump_expr(stream, expr->as.if_expr.then_expr, 0);
            fputs(" } else { ", stream);
            dump_expr(stream, expr->as.if_expr.else_expr, 0);
            fputs(" }", stream);
            break;
        case FENG_EXPR_MATCH:
            fputs("if ", stream);
            dump_expr(stream, expr->as.match_expr.target, 0);
            fputs(" { ", stream);
            for (index = 0U; index < expr->as.match_expr.case_count; ++index) {
                if (index != 0U) {
                    fputs(", ", stream);
                }
                dump_expr(stream, expr->as.match_expr.cases[index].label, 0);
                fputs(": ", stream);
                dump_expr(stream, expr->as.match_expr.cases[index].value, 0);
            }
            if (expr->as.match_expr.case_count > 0U) {
                fputs(", ", stream);
            }
            fputs("else: ", stream);
            dump_expr(stream, expr->as.match_expr.else_expr, 0);
            fputs(" }", stream);
            break;
    }
}

static void dump_block(FILE *stream, const FengBlock *block, int indent) {
    size_t index;

    dump_indent(stream, indent);
    fputs("{\n", stream);
    for (index = 0U; index < block->statement_count; ++index) {
        dump_stmt(stream, block->statements[index], indent + 1);
    }
    dump_indent(stream, indent);
    fputs("}\n", stream);
}

static void dump_stmt(FILE *stream, const FengStmt *stmt, int indent) {
    size_t index;

    dump_indent(stream, indent);
    switch (stmt->kind) {
        case FENG_STMT_BLOCK:
            dump_block(stream, stmt->as.block, indent);
            break;
        case FENG_STMT_BINDING:
            fputs(mutability_name(stmt->as.binding.mutability), stream);
            fputc(' ', stream);
            dump_slice(stream, stmt->as.binding.name);
            if (stmt->as.binding.type != NULL) {
                fputs(": ", stream);
                dump_type_ref(stream, stmt->as.binding.type);
            }
            if (stmt->as.binding.initializer != NULL) {
                fputs(" = ", stream);
                dump_expr(stream, stmt->as.binding.initializer, 0);
            }
            fputs(";\n", stream);
            break;
        case FENG_STMT_ASSIGN:
            dump_expr(stream, stmt->as.assign.target, 0);
            fputs(" = ", stream);
            dump_expr(stream, stmt->as.assign.value, 0);
            fputs(";\n", stream);
            break;
        case FENG_STMT_EXPR:
            dump_expr(stream, stmt->as.expr, 0);
            fputs(";\n", stream);
            break;
        case FENG_STMT_IF:
            for (index = 0U; index < stmt->as.if_stmt.clause_count; ++index) {
                dump_indent(stream, indent);
                fputs(index == 0U ? "if " : "else if ", stream);
                dump_expr(stream, stmt->as.if_stmt.clauses[index].condition, 0);
                fputc('\n', stream);
                dump_block(stream, stmt->as.if_stmt.clauses[index].block, indent);
            }
            if (stmt->as.if_stmt.else_block != NULL) {
                dump_indent(stream, indent);
                fputs("else\n", stream);
                dump_block(stream, stmt->as.if_stmt.else_block, indent);
            }
            break;
        case FENG_STMT_WHILE:
            fputs("while ", stream);
            dump_expr(stream, stmt->as.while_stmt.condition, 0);
            fputc('\n', stream);
            dump_block(stream, stmt->as.while_stmt.body, indent);
            break;
        case FENG_STMT_FOR:
            fputs("for (...)\n", stream);
            if (stmt->as.for_stmt.init != NULL) {
                dump_indent(stream, indent + 1);
                fputs("init:\n", stream);
                dump_stmt(stream, stmt->as.for_stmt.init, indent + 2);
            }
            if (stmt->as.for_stmt.condition != NULL) {
                dump_indent(stream, indent + 1);
                fputs("cond: ", stream);
                dump_expr(stream, stmt->as.for_stmt.condition, 0);
                fputc('\n', stream);
            }
            if (stmt->as.for_stmt.update != NULL) {
                dump_indent(stream, indent + 1);
                fputs("update:\n", stream);
                dump_stmt(stream, stmt->as.for_stmt.update, indent + 2);
            }
            dump_block(stream, stmt->as.for_stmt.body, indent);
            break;
        case FENG_STMT_TRY:
            fputs("try\n", stream);
            dump_block(stream, stmt->as.try_stmt.try_block, indent);
            if (stmt->as.try_stmt.catch_block != NULL) {
                dump_indent(stream, indent);
                fputs("catch\n", stream);
                dump_block(stream, stmt->as.try_stmt.catch_block, indent);
            }
            if (stmt->as.try_stmt.finally_block != NULL) {
                dump_indent(stream, indent);
                fputs("finally\n", stream);
                dump_block(stream, stmt->as.try_stmt.finally_block, indent);
            }
            break;
        case FENG_STMT_RETURN:
            fputs("return", stream);
            if (stmt->as.return_value != NULL) {
                fputc(' ', stream);
                dump_expr(stream, stmt->as.return_value, 0);
            }
            fputs(";\n", stream);
            break;
        case FENG_STMT_THROW:
            fputs("throw ", stream);
            dump_expr(stream, stmt->as.throw_value, 0);
            fputs(";\n", stream);
            break;
        case FENG_STMT_BREAK:
            fputs("break;\n", stream);
            break;
        case FENG_STMT_CONTINUE:
            fputs("continue;\n", stream);
            break;
    }
}

static void dump_callable(FILE *stream, const FengCallableSignature *callable, int indent) {
    size_t index;

    dump_indent(stream, indent);
    dump_slice(stream, callable->name);
    fputc('(', stream);
    for (index = 0U; index < callable->param_count; ++index) {
        if (index != 0U) {
            fputs(", ", stream);
        }
        if (callable->params[index].mutability != FENG_MUTABILITY_DEFAULT) {
            fputs(mutability_name(callable->params[index].mutability), stream);
            fputc(' ', stream);
        }
        dump_slice(stream, callable->params[index].name);
        fputs(": ", stream);
        dump_type_ref(stream, callable->params[index].type);
    }
    fputc(')', stream);
    if (callable->return_type != NULL) {
        fputs(": ", stream);
        dump_type_ref(stream, callable->return_type);
    }
    fputc('\n', stream);
    if (callable->body != NULL) {
        dump_block(stream, callable->body, indent);
    }
}

void feng_program_dump(FILE *stream, const FengProgram *program) {
    size_t index;

    fprintf(stream, "Program(path=%s)\n", program->path != NULL ? program->path : "<memory>");
    dump_indent(stream, 1);
    fprintf(stream, "module %s ", visibility_name(program->module_visibility));
    dump_path(stream, program->module_segments, program->module_segment_count);
    fputc('\n', stream);

    for (index = 0U; index < program->use_count; ++index) {
        dump_indent(stream, 1);
        fputs("use ", stream);
        dump_path(stream, program->uses[index].segments, program->uses[index].segment_count);
        if (program->uses[index].has_alias) {
            fputs(" as ", stream);
            dump_slice(stream, program->uses[index].alias);
        }
        fputc('\n', stream);
    }

    for (index = 0U; index < program->declaration_count; ++index) {
        const FengDecl *decl = program->declarations[index];
        size_t member_index;

        dump_annotations(stream, decl->annotations, decl->annotation_count, 1);
        dump_indent(stream, 1);
        fprintf(stream, "%s %s ", visibility_name(decl->visibility), decl->is_extern ? "extern" : "plain");
        switch (decl->kind) {
            case FENG_DECL_GLOBAL_BINDING:
                fprintf(stream, "binding %s ", mutability_name(decl->as.binding.mutability));
                dump_slice(stream, decl->as.binding.name);
                if (decl->as.binding.type != NULL) {
                    fputs(": ", stream);
                    dump_type_ref(stream, decl->as.binding.type);
                }
                if (decl->as.binding.initializer != NULL) {
                    fputs(" = ", stream);
                    dump_expr(stream, decl->as.binding.initializer, 0);
                }
                fputc('\n', stream);
                break;
            case FENG_DECL_TYPE:
                fputs("type ", stream);
                dump_slice(stream, decl->as.type_decl.name);
                if (decl->as.type_decl.declared_spec_count > 0U) {
                    fputs(" : ", stream);
                    for (member_index = 0U; member_index < decl->as.type_decl.declared_spec_count; ++member_index) {
                        if (member_index != 0U) {
                            fputs(", ", stream);
                        }
                        dump_type_ref(stream, decl->as.type_decl.declared_specs[member_index]);
                    }
                }
                fputc('\n', stream);
                for (member_index = 0U; member_index < decl->as.type_decl.member_count; ++member_index) {
                    const FengTypeMember *member = decl->as.type_decl.members[member_index];

                    dump_annotations(stream, member->annotations, member->annotation_count, 2);
                    dump_indent(stream, 2);
                    fprintf(stream, "%s ", visibility_name(member->visibility));
                    if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                        fprintf(stream, "field %s ", mutability_name(member->as.field.mutability));
                        dump_slice(stream, member->as.field.name);
                        fputs(": ", stream);
                        dump_type_ref(stream, member->as.field.type);
                        if (member->as.field.initializer != NULL) {
                            fputs(" = ", stream);
                            dump_expr(stream, member->as.field.initializer, 0);
                        }
                        fputc('\n', stream);
                    } else {
                        const char *kind_label;
                        switch (member->kind) {
                            case FENG_TYPE_MEMBER_CONSTRUCTOR:
                                kind_label = "constructor\n";
                                break;
                            case FENG_TYPE_MEMBER_FINALIZER:
                                kind_label = "finalizer\n";
                                break;
                            default:
                                kind_label = "method\n";
                                break;
                        }
                        fputs(kind_label, stream);
                        dump_callable(stream, &member->as.callable, 3);
                    }
                }
                break;
            case FENG_DECL_SPEC:
                fprintf(stream,
                        "spec %s ",
                        decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT ? "object" : "callable");
                dump_slice(stream, decl->as.spec_decl.name);
                if (decl->as.spec_decl.parent_spec_count > 0U) {
                    fputs(" : ", stream);
                    for (member_index = 0U; member_index < decl->as.spec_decl.parent_spec_count; ++member_index) {
                        if (member_index != 0U) {
                            fputs(", ", stream);
                        }
                        dump_type_ref(stream, decl->as.spec_decl.parent_specs[member_index]);
                    }
                }
                fputc('\n', stream);
                if (decl->as.spec_decl.form == FENG_SPEC_FORM_OBJECT) {
                    for (member_index = 0U; member_index < decl->as.spec_decl.as.object.member_count; ++member_index) {
                        const FengTypeMember *member = decl->as.spec_decl.as.object.members[member_index];

                        dump_indent(stream, 2);
                        if (member->kind == FENG_TYPE_MEMBER_FIELD) {
                            fprintf(stream, "field %s ", mutability_name(member->as.field.mutability));
                            dump_slice(stream, member->as.field.name);
                            fputs(": ", stream);
                            dump_type_ref(stream, member->as.field.type);
                            fputc('\n', stream);
                        } else {
                            fputs("method\n", stream);
                            dump_callable(stream, &member->as.callable, 3);
                        }
                    }
                } else {
                    dump_indent(stream, 2);
                    dump_slice(stream, decl->as.spec_decl.name);
                    fputc('(', stream);
                    for (member_index = 0U; member_index < decl->as.spec_decl.as.callable.param_count; ++member_index) {
                        if (member_index != 0U) {
                            fputs(", ", stream);
                        }
                        dump_slice(stream, decl->as.spec_decl.as.callable.params[member_index].name);
                        fputs(": ", stream);
                        dump_type_ref(stream, decl->as.spec_decl.as.callable.params[member_index].type);
                    }
                    fputs("): ", stream);
                    dump_type_ref(stream, decl->as.spec_decl.as.callable.return_type);
                    fputc('\n', stream);
                }
                break;
            case FENG_DECL_FIT:
                fputs("fit ", stream);
                dump_type_ref(stream, decl->as.fit_decl.target);
                if (decl->as.fit_decl.spec_count > 0U) {
                    fputs(" : ", stream);
                    for (member_index = 0U; member_index < decl->as.fit_decl.spec_count; ++member_index) {
                        if (member_index != 0U) {
                            fputs(", ", stream);
                        }
                        dump_type_ref(stream, decl->as.fit_decl.specs[member_index]);
                    }
                }
                fputc('\n', stream);
                for (member_index = 0U; member_index < decl->as.fit_decl.member_count; ++member_index) {
                    const FengTypeMember *member = decl->as.fit_decl.members[member_index];

                    dump_indent(stream, 2);
                    fprintf(stream, "%s method\n", visibility_name(member->visibility));
                    dump_callable(stream, &member->as.callable, 3);
                }
                break;
            case FENG_DECL_FUNCTION:
                fputs("function\n", stream);
                dump_callable(stream, &decl->as.function_decl, 2);
                break;
        }
    }
}
