#include "../generic_sibling_constraint_helpers.h"

/* Each positive closes the sibling relation through a distinct language path. */
static void sibling_positive_semantics(void) {
    const char *cases[] = {
        "func run(x:Ref<i32>){let a=first<i32,Ref<i32>>(x);let b=later<Ref<i32>,i32>(x);}",
        "func run(x:Value<string>){let a=first<string,Value<string>>(x);let b=later<Value<string>,string>(x);}",
        "func run(x:Ref<i32>){let a=inferFirst(x,(i32)1);let b=inferLater(x,(i32)2);}",
        "func run(x:Ref<string>){let a:string=first(x);let b:string=later(x);}",
        "func run<Z:Child<A>,A>(x:Z):A{return relayFirst<A,Z>(x);}",
        "func run<A,Z:Child<A>>(x:Z):A{return relayLater<Z,A>(x);}",
        "func run(x:Ref<string>){childFirst<string,Ref<string>>(x);childLater<Ref<string>,string>(x);ownerFirst<string,Ref<string>>(x);ownerLater<Ref<string>,string>(x);}",
        "func run(x:Ref<Packet<i32[]>>):Packet<i32[]>{return nested<Ref<Packet<i32[]>>,i32>(x);}",
        "func run(f:Mapper<i32>){let a=mapFirst(f,(i32)1);let b=mapLater(f,(i32)2);}",
        "func run(){let a=choiceFirst<i32,i32>(1);let b=choiceLater<i32,i32>(2);}",
        "func run(x:Ref<string>){let a=bothFirst<string,Ref<string>>(x);let b=bothLater<Ref<string>,string>(x);}",
        "func run(x:Box<i32>){let a=first<i32,Box<i32>>(x);let b=later<Box<i32>,i32>(x);}",
        "func run(x:Ref<i32>){let a=FirstOwner<i32,Ref<i32>>{item:x};let b=LaterOwner<Ref<i32>,i32>{item:x};a.get();b.get();}",
        "func run(a:FirstBound<i32,Ref<i32>>,b:LaterBound<Ref<i32>,i32>){}",
        "func run(x:Ref<i32>){let m=Methods<string>();m.first(x,(i32)1);Methods<string>.later(x,(i32)2);m.fitLater(x,(i32)3);Methods<string>.fitFirst(x,(i32)4);}",
        "spec Read<A,B>(value:A):B;func run(x:Ref<i32>){let a:Read<Ref<i32>,i32> =first<i32,Ref<i32>>;let b:Read<Ref<i32>,i32> =later<Ref<i32>,i32>;a(x);b(x);}",
        "type Payload{}func own<T:Box<Payload>,Payload>(x:T):Payload{return x.get();}func run(x:Ref<i32>):i32{return own<Ref<i32>,i32>(x);}",
        "spec Required<E:Tag>{}func before<A:Tag,B:Required<A>>(){}func after<B:Required<A>,A:Tag>(){}",
        "func selfBound<T:Box<T>>(x:T):T{return x.get();}func mutual<T:Box<U>,U:Box<T>>(x:T,y:U):U{return x.get();}"
    };
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        char *source = sibling_source(sibling_declarations(), cases[i]);
        SiblingConstraintUnit unit = sibling_analyze(source, NULL, NULL, NULL);
        sibling_dispose(&unit);
        free(source);
    }
}

/* Inference records actuals in declaration order, independently of the bound. */
static void sibling_inferred_slots(void) {
    char *source = sibling_source(sibling_declarations(),
        "func run(x:Ref<string>){let a=inferFirst(x,\"hint\");let b=inferLater(x,\"hint\");}");
    SiblingConstraintUnit unit = sibling_analyze(source, NULL, NULL, NULL);
    const FengBlock *body = unit.program->declarations[unit.program->declaration_count - 1U]->as.function_decl.body;
    for (size_t order = 0U; order < 2U; ++order) {
        const FengResolvedCallable *call = &body->statements[order]->as.binding.initializer->as.call.resolved_callable;
        SIBLING_CHECK(call->kind == FENG_RESOLVED_CALLABLE_FUNCTION);
        SIBLING_CHECK(call->callable_type_arg_count == 2U);
        const FengTypeRef *plain = call->callable_type_args[order == 0U ? 0U : 1U];
        const FengTypeRef *box = call->callable_type_args[order == 0U ? 1U : 0U];
        SIBLING_CHECK(plain->kind == FENG_TYPE_REF_NAMED && plain->as.named.type_arg_count == 0U);
        SIBLING_CHECK(plain->as.named.segments[0].length == 6U);
        SIBLING_CHECK(memcmp(plain->as.named.segments[0].data, "string", 6U) == 0);
        SIBLING_CHECK(box->kind == FENG_TYPE_REF_NAMED && box->as.named.type_arg_count == 1U);
        SIBLING_CHECK(box->as.named.segments[0].length == 3U);
        SIBLING_CHECK(memcmp(box->as.named.segments[0].data, "Ref", 3U) == 0);
    }
    sibling_dispose(&unit);
    free(source);
}

/* A top-level member cannot grant an unconstrained generic a capability. */
static void sibling_shadowed_members(void) {
    const char *prefix = "module shadow;open type T{func get():i32{return 7;}}"
        "open type U{func get():i32{return 9;}}spec Box<V>{func get():V;}";
    const struct {
        const char *body;
        const char *code;
    } cases[] = {
        {"func first<T,U:Box<T>>(x:U):T{return x.get();}", NULL},
        {"func later<T:Box<U>,U>(x:T):U{return x.get();}", NULL},
        {"func bad<T,U>(x:U){x.get();}", "AE0306"},
        {"func bad<T,U>(x:T){x.get();}", "AE0306"},
        {"func concrete(x:U):i32{return x.get();}", NULL},
        {"func concrete():U{return U();}func run<U>(x:U):i32{return concrete().get();}", NULL},
        {"func run<U>(x:U):i32{return concrete().get();}func concrete():U{return U();}", NULL},
        {"func qualified<U>(x:shadow.U):i32{return x.get();}", NULL}
    };
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        char *source = sibling_source(prefix, cases[i].body);
        SiblingConstraintUnit unit = sibling_analyze(source, NULL, cases[i].code,
            cases[i].code != NULL ? "get" : NULL);
        sibling_dispose(&unit);
        free(source);
    }
}

/* Both orders reject wrong instances, missing bounds and invalid declarations. */
void test_generic_sibling_constraint_semantics(void) {
    sibling_positive_semantics();
    sibling_inferred_slots();
    sibling_shadowed_members();
    /* Bind each rejected source to one diagnostic and its precise token. */
    const struct {
        const char *body;
        const char *code;
        const char *token;
    } cases[] = {
        {"func bad(x:Ref<i32>){first<string,Ref<i32>>(x);}", "AE0512", "first"},
        {"func bad(x:Ref<i32>){later<Ref<i32>,string>(x);}", "AE0512", "later"},
        {"func bad(x:Ref<i32>){inferFirst(x,\"wrong\");}", "AE0512", "inferFirst"},
        {"func bad(x:Ref<i32>){inferLater(x,\"wrong\");}", "AE0512", "inferLater"},
        {"type Fake{func get():i32{return 1;}}func bad(x:Fake){first<i32,Fake>(x);}", "AE0512", "first"},
        {"type Fake{func get():i32{return 1;}}func bad(x:Fake){later<Fake,i32>(x);}", "AE0512", "later"},
        {"func bad<T,U>(x:U):T{return first<T,U>(x);}", "AE0512", "first"},
        {"func bad<T,U>(x:T):U{return later<T,U>(x);}", "AE0512", "later"},
        {"func bad<T:Box<i32>,U>(x:T):U{return later<T,U>(x);}", "AE0512", "later"},
        {"func bad<T,U:Box<i32>>(x:U):T{return first<T,U>(x);}", "AE0512", "first"},
        {"func bad(x:FirstOwner<string,Ref<i32>>){}", "AE0710", "Ref"},
        {"func bad(x:LaterOwner<Ref<i32>,string>){}", "AE0710", "Ref"},
        {"func bad(x:FirstBound<string,Ref<i32>>){}", "AE0710", "Ref"},
        {"func bad(x:LaterBound<Ref<i32>,string>){}", "AE0710", "Ref"},
        {"func bad<T,U:Ref<T>>(x:U){}", "AE0709", "U"},
        {"func bad<T:Ref<U>,U>(x:T){}", "AE0709", "T"},
        {"func bad<T,U:Box<Missing>>(x:U){}", "AE1013", "Missing"},
        {"func bad<T:Box<Missing>,U>(x:T){}", "AE1013", "Missing"},
        {"spec Required<E:Tag>{}func bad<A,B:Required<A>>(){}", "AE0710", "A"},
        {"spec Required<E:Tag>{}func bad<B:Required<A>,A>(){}", "AE0710", "A"},
        {"func bad<T,U:Box<T>,T>(){}", "AE1017", "T"},
        {"func bad<T:Box<U>,U,U>(){}", "AE1017", "U"},
        {"type U<V>{}func invoke(){bad<i32>();}func bad<U>():U<i32>{throw \"bad\";}", "AE1012", "U"},
        {"type T<V>{}func invoke(){bad<i32>();}func bad<T>():T<i32>{throw \"bad\";}", "AE1012", "T"},
        {"func bad(x:Ref<i32>){Methods<string>.later<Ref<i32>,string>(x,\"wrong\");}", "AE0512", "later"},
        {"func bad(x:Ref<i32>){Methods<string>().first<string,Ref<i32>>(x,\"wrong\");}", "AE0512", "first"},
        {"func bad(x:Ref<i32>){Methods<string>().fitLater<Ref<i32>,string>(x,\"wrong\");}", "AE0512", "fitLater"},
        {"func bad(x:Ref<i32>){Methods<string>.fitFirst<string,Ref<i32>>(x,\"wrong\");}", "AE0512", "fitFirst"},
        {"func bad(f:Mapper<i32>){mapFirst<string,Mapper<i32>>(f,\"wrong\");}", "AE0512", "mapFirst"},
        {"func bad(f:Mapper<i32>){mapLater<Mapper<i32>,string>(f,\"wrong\");}", "AE0512", "mapLater"},
        {"func bad(){choiceFirst<i32,bool>(true);}", "AE0512", "choiceFirst"},
        {"func bad(){choiceLater<bool,i32>(true);}", "AE0512", "choiceLater"},
        {"func bad(x:Value<i32>){bothFirst<i32,Value<i32>>(x);}", "AE0512", "bothFirst"},
        {"func bad(x:Value<i32>){bothLater<Value<i32>,i32>(x);}", "AE0512", "bothLater"}
    };
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        char *source = sibling_source(sibling_declarations(), cases[i].body);
        SiblingConstraintUnit unit = sibling_analyze(source, NULL, cases[i].code, cases[i].token);
        sibling_dispose(&unit);
        free(source);
    }
    puts("generic sibling constraint semantic cases passed");
}
