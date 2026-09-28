/* Fixed aggregate lifecycle lowering shares the descriptor's ordered managed
 * leaves. Open layouts keep using the runtime descriptor as their authority. */

/* Dispatch through the aggregate facts provider; references stop recursion at
 * their handle, so recursive object graphs never expand recursively here. */
static bool cg_visit_static_managed_slots(CG *cg, Buf *out, const CGType *type,
    const char *lvalue, CGManagedSlotAction action) {
    if (type == NULL || type->kind == CG_TYPE_GENERIC_PARAM ||
        cg_shared_generic_param_uses_address(type) ||
        cg_type_uses_reified_storage(cg, type)) return false;
    if (cgtype_is_managed(type)) {
        if (out != NULL) {
            if (action == CG_MANAGED_SLOT_CLEAR)
                buf_append_fmt(out, "    %s = NULL;\n", lvalue);
            else
                buf_append_fmt(out, "    %s(%s);\n",
                    action == CG_MANAGED_SLOT_RETAIN ? "feng_retain" : "feng_release", lvalue);
        }
        return true;
    }
    if (!cgtype_is_aggregate(type)) return true;
    CGAggregateFacts facts = {0};
    return cg_aggregate_facts(type, &facts) && facts.emit_static_slots != NULL &&
        facts.emit_static_slots(cg, out, type, lvalue, action);
}

/* Fixed object/intersection views have one subject. A union visits only its
 * active alternative, including nested values, in descriptor order. */
static bool cg_spec_aggregate_emit_static_slots(CG *cg, Buf *out,
    const CGType *type, const char *lvalue, CGManagedSlotAction action) {
    const UserSpec *spec = type->user_spec;
    if (spec == NULL) return false;
    if (spec->form == FENG_SPEC_FORM_UNION) {
        if (spec->generic_context_type_param_count != 0U) return false;
        for (size_t i = 0U; i < spec->union_member_count; ++i)
            if (!cg_visit_static_managed_slots(cg, NULL, spec->union_member_types[i], NULL, action))
                return false;
        if (out == NULL) return true;
        buf_append_fmt(out, "    switch ((%s).tag) {\n", lvalue);
        for (size_t i = 0U; i < spec->union_member_count; ++i) {
            const CGType *member = spec->union_member_types[i];
            if (cgtype_value_kind(member) == CG_VK_TRIVIAL) continue;
            Buf payload;
            buf_init(&payload);
            buf_append_fmt(&payload, "(%s).payload.m%zu", lvalue, i);
            buf_append_fmt(out, "    case %zuU:\n", i);
            bool ok = payload.data != NULL &&
                cg_visit_static_managed_slots(cg, out, member, payload.data, action);
            buf_free(&payload);
            if (!ok) return false;
            buf_append_cstr(out, "        break;\n");
        }
        buf_append_cstr(out, "    default: break;\n    }\n");
        return true;
    }
    if (spec->form != FENG_SPEC_FORM_OBJECT &&
        spec->form != FENG_SPEC_FORM_INTERSECTION) return false;
    if (out == NULL) return true;
    if (action == CG_MANAGED_SLOT_CLEAR)
        buf_append_fmt(out, "    (%s).subject = NULL;\n", lvalue);
    else
        buf_append_fmt(out, "    %s((%s).subject);\n",
            action == CG_MANAGED_SLOT_RETAIN ? "feng_retain" : "feng_release", lvalue);
    return true;
}

/* Follow the same field order used when emitting this value's slot table. */
static bool cg_value_aggregate_emit_static_slots(CG *cg, Buf *out,
    const CGType *type, const char *lvalue, CGManagedSlotAction action) {
    const UserType *value = type->user;
    if (value == NULL || value->generic_context_type_param_count != 0U) return false;
    for (size_t i = 0U; i < value->field_count; ++i)
        if (!cg_visit_static_managed_slots(cg, NULL, value->fields[i].type, NULL, action))
            return false;
    if (out == NULL) return true;
    for (size_t i = 0U; i < value->field_count; ++i) {
        Buf field;
        buf_init(&field);
        buf_append_fmt(&field, "(%s).%s", lvalue, value->fields[i].c_name);
        bool ok = field.data != NULL && cg_visit_static_managed_slots(
            cg, out, value->fields[i].type, field.data, action);
        buf_free(&field);
        if (!ok) return false;
    }
    return true;
}

/* Preserve assign/take's self-alias, retain-before-release and moved-slot
 * rules. Addresses are evaluated once; release callbacks still see the live
 * source storage, rather than an eagerly copied snapshot. */
static bool cg_emit_static_aggregate_store(CG *cg, const CGType *type,
    const char *destination, const char *source_address, bool take_source,
    bool replace_existing, bool *out_handled) {
    *out_handled = false;
    if (!cg_visit_static_managed_slots(cg, NULL, type, NULL, CG_MANAGED_SLOT_RETAIN))
        return true;
    char *ctype = cg_ctype_dup(type);
    if (ctype == NULL) return false;
    *out_handled = true;
    Buf *out = cg->cur_body;
    buf_append_fmt(out,
        "    {\n"
        "    FENG_CODEGEN_NODEBUG %s *const _aggregate_dst = &(%s);\n"
        "    FENG_CODEGEN_NODEBUG %s *const _aggregate_src = (%s *)%s;\n"
        "    if (_aggregate_dst != _aggregate_src) {\n",
        ctype, destination, ctype, ctype, source_address);
    free(ctype);
    bool ok = take_source || cg_visit_static_managed_slots(
        cg, out, type, "(*_aggregate_src)", CG_MANAGED_SLOT_RETAIN);
    if (ok && replace_existing)
        ok = cg_visit_static_managed_slots(cg, out, type, "(*_aggregate_dst)", CG_MANAGED_SLOT_RELEASE);
    if (ok) {
        buf_append_cstr(out, "    memcpy(_aggregate_dst, _aggregate_src, sizeof *_aggregate_dst);\n");
        if (take_source)
            ok = cg_visit_static_managed_slots(cg, out, type, "(*_aggregate_src)", CG_MANAGED_SLOT_CLEAR);
    }
    buf_append_cstr(out, "    }\n    }\n");
    return ok;
}

/* An implicit, source-visible construction with no declaration/mixin
 * initializer cannot publish its new receiver before the literal stores.
 * A zero-byte language default has no old managed references to release. */
static bool cg_object_field_has_empty_default(CG *cg, const UserType *type,
    const UserMethod *constructor, const UserField *field) {
    if (constructor != NULL || type == NULL || type->decl == NULL ||
        type->decl->kind != FENG_DECL_TYPE || field == NULL ||
        cg_program_origin(cg, type->owner_program) == FENG_SEMANTIC_MODULE_ORIGIN_IMPORTED_PACKAGE ||
        !cg_type_default_zero_is_zero_bytes(cg, field->type, 0U)) return false;
    for (size_t i = 0U; i < type->field_count; ++i) {
        const FengTypeMember *member = type->fields[i].member;
        if (member == NULL || member->kind != FENG_TYPE_MEMBER_FIELD ||
            member->as.field.initializer != NULL) return false;
    }
    for (size_t i = 0U; i < type->decl->as.type_decl.mixin_count; ++i)
        if (type->decl->as.type_decl.mixins[i].source_constructor != NULL) return false;
    return true;
}
