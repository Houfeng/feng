#include "parser/parser.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* The identical declarations are consumed locally and through both FT profiles. */
static const char *chain_provider =
    "open module chain.provider;\n"
    "open func values<T>(item:T,n:i32):T[]{return [item];}\n"
    "open func leading<T>(n:i32,item:T):T[]{return [item];}\n"
    "open func empty<T>():T[]{return [];}\n"
    "open func identity<T>(item:T):T{return item;}\n"
    "open func pair<T>(a:T,b:T,n:i32):T[]{return [a,b];}\n"
    "open func arrays<T>(item:T,other:i32[]):T[]{return [item];}\n"
    "open func inferred<T>(item:T){return item;}\n"
    "open func inferredArray<T>(item:T){return [item];}\n"
    "open func inferredNested<T>(item:T){return [[item]];}\n"
    "open func inferredConcrete(){return [\"concrete\"];}\n"
    "open func inferredPointer(item:int*){return item;}\n"
    "open func inferredForward<T>(item:T){return inferredLater(item);}\n"
    "open func inferredLater<T>(item:T){return [item];}\n"
    "open func inferredEmpty<T>(){let item:T;return item;}\n"
    "open fit T[]{open func head():T{return self[0];} open func again():T[]{return self;}\n"
    " open func inferredHead(){return self[0];}open func inferredAgain(){return self;}\n"
    " open static func inferredMake(v:T){return [v];}}\n"
    "open fit string{open func size():int{return 7;} }\n"
    "open fit int{open func label():string{return \"int\";} }\n"
    "open type Box<T>{open let value:T;open func get():T{return self.value;}\n"
    " open func inferredGet(){return self.value;}\n"
    " open func inferredConvert<U>(v:U,n:i32){return [v];}\n"
    " open static func inferredStatic<U>(v:U,n:i32){return Box<U>{value:v};}\n"
    " open func convert<U>(v:U,n:i32):U[]{return [v];}\n"
    " open static func make<U>(v:U,n:i32):Box<U>{return Box<U>{value:v};}}\n"
    "open func boxed<T>(v:T,n:i32):Box<T>{return Box<T>{value:v};}\n"
    "open fit Box<T>{open func inferredFit(){return self.value;}}\n"
    "open spec Getter<T>():T;\n"
    "open func getter<T>(v:T,n:i32):Getter<T>{return (){return v;};}\n";

/* Diagnose at the source boundary, preserving the error code and source token. */
static FengSemanticAnalysis *chain_analyze(const char *name, const char *source,
    const FengSemanticImportedModuleQuery *query, const char *code, FengProgram **program) {
    FengParseError parse = {0};
    if (!feng_parse_source(source, strlen(source), "generic_chain.ff", program, &parse)) {
        fprintf(stderr, "%s: parse failed: %s\n%s\n", name, parse.message, source);
        CHECK(false);
    }
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (code == NULL ? !ok || count != 0U : ok || count != 1U || strcmp(errors[0].code, code) != 0) {
        fprintf(stderr, "%s: expected %s\n%s\n", name, code != NULL ? code : "success", source);
        for (size_t i = 0U; i < count; ++i)
            fprintf(stderr, "%s %u:%u %s\n", errors[i].code, errors[i].token.line,
                errors[i].token.column, errors[i].message);
        CHECK(false);
    }
    if (code != NULL) {
        CHECK(strcmp(errors[0].path, "generic_chain.ff") == 0);
        CHECK(errors[0].token.line != 0U && errors[0].token.column != 0U && errors[0].token.length != 0U);
    }
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Each target or suffix changes independently; negative cases retain rejection. */
static size_t chain_matrix(const FengSemanticImportedModuleQuery *query) {
    const struct { const char *name, *body, *code; } cases[] = {
        {"return", "func probe():int{return values(\"x\",1).head().size();}", NULL},
        {"statement", "func probe(){values(\"x\",1).head().size();}", NULL},
        {"explicit", "func probe():int{return values<string>(\"x\",1).head().size();}", NULL},
        {"typed argument", "func probe(n:i32):int{return values(\"x\",n).head().size();}", NULL},
        {"leading fixed", "func probe():int{return leading(1,\"x\").head().size();}", NULL},
        {"fixed array", "func probe():int{return arrays(\"x\",[]).head().size();}", NULL},
        {"binding", "func probe(){let result:int=values(\"x\",1).head().size();}", NULL},
        {"assignment", "func probe(){var result:int=0;result=values(\"x\",1).head().size();}", NULL},
        {"argument", "func consume(v:int){} func probe(){consume(values(\"x\",1).head().size());}", NULL},
        {"if yield", "func probe(b:bool):int{return if b{values(\"x\",1).head().size();}else{0;};}", NULL},
        {"match yield", "func probe(n:int):int{return match n{0{values(\"x\",1).head().size();}else{0;}};}", NULL},
        {"try yield", "func probe():int{return try values(\"x\",1).head().size() catch{0;};}", NULL},
        {"array element", "func probe():int[]{return [values(\"x\",1).head().size()];}", NULL},
        {"field initializer", "func probe():Box<int>{return Box<int>{value:values(\"x\",1).head().size()};}", NULL},
        {"field suffix", "func probe():int{return boxed(\"x\",1).value.size();}", NULL},
        {"method suffix", "func probe():int{return boxed(\"x\",1).get().size();}", NULL},
        {"index suffix", "func probe():int{return values(\"x\",1)[0].size();}", NULL},
        {"index operand", "func probe():string{return values(\"x\",1)[identity(0)];}", NULL},
        {"successive indices", "func probe():int{return values(values(\"x\",1),1)[0][0].size();}", NULL},
        {"call suffix", "func probe():int{return getter(\"x\",1)().size();}", NULL},
        {"instance method", "func probe(b:Box<int>):int{return b.convert(\"x\",1).head().size();}", NULL},
        {"static method", "func probe():int{return Box<int>.make(\"x\",1).get().size();}", NULL},
        {"module call", "func probe():int{return chain.provider.values(\"x\",1).head().size();}", NULL},
        {"many suffixes", "func probe():int{return values(\"x\",1).again().again().head().size();}", NULL},
        {"open forwarding", "func probe<U>(v:U):U{return values(v,1).head();}", NULL},
        {"open owner", "type Local<U>{let v:U;func read():U{return boxed(self.v,1).get();}}", NULL},
        {"shadowed parameters", "func probe<T>(b:Box<T>):string{return b.convert(\"x\",1).head();}", NULL},
        {"direct target inference", "func probe():string[]{return empty();}", NULL},
        {"binding target inference", "func probe(){let result:string[]=empty();}", NULL},
        {"assignment target inference", "func probe(){var result:string[]=[];result=empty();}", NULL},
        {"restored outer target", "func probe():string[]{values(\"x\",1).head().size();return empty();}", NULL},
        {"numeric explicit fitting", "func probe():i32[]{return values<i32>(1,1);}", NULL},
        {"no receiver target", "func probe():int{return empty().head().size();}", "AE0525"},
        {"conflicting bindings", "func probe():int{return pair(\"x\",1,1).head().size();}", "AE0512"},
        {"fixed argument mismatch", "func probe():int{return values(\"x\",\"bad\").head().size();}", "AE0512"},
        {"explicit conflict", "func probe():int{return values<int>(\"x\",1).head().size();}", "AE0512"},
        {"wrong final return", "func probe():string{return values(\"x\",1).head().size();}", "AE1003"},
        {"wrong binding target", "func probe(){let v:string=values(\"x\",1).head().size();}", "AE1003"},
        {"constraint rejection", "spec Readable{func read():int;}func constrained<T:Readable>(v:T):T[]{return [v];}func probe():int{return constrained(\"x\").head().size();}", "AE0512"},
        {"mutable array actual", "func probe(v:string[!]):string[!]{return values(v,1).head();}", NULL},
        {"nominal parameter identity", "type A{}type B{}func probe(a:A,b:B){pair(a,b,1).head();}", "AE0512"},
        {"overload selection", "func pick(n:int):string[]{return [\"int\"];}func pick(n:string):int[]{return [1];}func probe():int{return pick(1).head().size();}", NULL},
        {"inferred fit return", "func probe(v:string[]):string{return v.inferredHead();}", NULL},
        {"inferred fit chain", "func probe():int{return values(\"x\",1).inferredHead().size();}", NULL},
        {"inferred nested return", "func probe(v:string[][]):string{return v.inferredHead().inferredHead();}", NULL},
        {"inferred array result", "func probe():int{return values(\"x\",1).inferredAgain().inferredHead().size();}", NULL},
        {"inferred top level", "func probe():int{return inferred(\"x\").size();}", NULL},
        {"inferred generic array", "func probe():int{return inferredArray(\"x\").inferredHead().size();}", NULL},
        {"inferred module call", "func probe():int{return chain.provider.inferred(\"x\").size();}", NULL},
        {"inferred receiver parameter", "func probe(b:Box<string>):string{return b.inferredGet();}", NULL},
        {"inferred method parameter", "func probe(b:Box<int>):string{return b.inferredConvert(\"x\",1).inferredHead();}", NULL},
        {"inferred static method", "func probe():string{return Box<int>.inferredStatic(\"x\",1).inferredGet();}", NULL},
        {"inferred static fit", "func probe():string{return string[].inferredMake(\"x\").inferredHead();}", NULL},
        {"inferred nominal fit", "func probe(b:Box<string>):string{return b.inferredFit();}", NULL},
        {"inferred explicit numeric", "func probe():i32{return inferred<i32>(1);}", NULL},
        {"inferred target only", "func probe():string{return inferredEmpty();}", NULL},
        {"inferred open caller", "func probe<U>(v:U):U{return inferred(v);}", NULL},
        {"inferred open receiver", "func probe<U>(v:U[]):U{return v.inferredHead();}", NULL},
        {"inferred swapped parameters", "func probe<T,U>(v:Box<U>,t:T):T{return v.inferredConvert(t,1).inferredHead();}", NULL},
        {"inferred wrong target", "func probe(v:string[]):int{return v.inferredHead();}", "AE1003"},
        {"inferred unbound parameter", "func probe(){inferredEmpty();}", "AE0525"},
        {"inferred incompatible explicit argument", "func probe():int{return inferred<int>(\"x\");}", "AE0512"},
        {"cached concrete array", "func probe():string{return inferredConcrete()[0];}", NULL},
        {"cached nested array", "func probe():string{return inferredNested(\"nested\")[0][0];}", NULL},
        {"cached nominal element", "type Item{let value:int;}func probe():int{return inferredArray(Item{value:9})[0].value;}", NULL},
        {"cached generic nominal", "func probe():string{return inferredNested(Box<string>{value:\"box\"})[0][0].inferredGet();}", NULL},
        {"cached writable element", "func probe(v:string[!]):string[!]{return inferredArray(v)[0];}", NULL},
        {"cached pointer parameter", "func probe(v:int*):int*{return inferredPointer(v);}", NULL},
        {"cached pointer element", "func probe(v:int*):int*{return inferredArray(v)[0];}", NULL},
        {"cached forward result", "func probe():string{return inferredForward(\"forward\")[0];}", NULL},
        {"cached open composite", "func probe<U>(v:U):U{return inferredNested(v)[0][0];}", NULL},
        {"cached branch returns", "func branch(b:bool){if b{return [\"a\"];}return [\"b\"];}func probe():string{return branch(true)[0];}", NULL},
        {"cached lambda result", "spec Make():string[];func probe():Make{return (){return [\"lambda\"];};}", NULL},
        {"inferred target binding", "func probe(){let value:string=inferredEmpty();}", NULL},
        {"inferred target argument", "func take(value:string){}func probe(){take(inferredEmpty());}", NULL},
        {"cached incompatible paths", "func probe(b:bool){if b{return [\"a\"];}return [1];}", "AE0504"},
        {"cached wrong composite target", "func probe():int[]{return inferredArray(\"bad\");}", "AE1003"},
        {"cached unresolved suffix", "func probe(){inferredEmpty().inferredHead();}", "AE0525"},
        {"forward index", "func before(){return after();}func after(){let v:string[]=[\"x\"];return v;}func probe():string{return before()[0];}", NULL},
        {"forward member", "func before(){return after();}func after(){return Box<string>{value:\"x\"};}func probe():string{return before().value;}", NULL},
        {"forward method", "func before(){return after();}func after(){let v:string[]=[\"x\"];return v;}func probe():string{return before().inferredHead();}", NULL},
        {"forward callable", "func before(){return after();}func after(){return getter(\"x\",1);}func probe():string{return before()();}", NULL},
        {"forward index operand", "func before(){return after();}func after(){return 0;}func probe(v:string[]):string{return v[before()];}", NULL},
        {"forward invalid array", "func before(){return after();}func after(){return 0;}func probe(){before()[0];}", "AE1021"},
        {"forward invalid index", "func before(){return after();}func after(){return \"index\";}func probe(v:string[]){v[before()];}", "AE1022"},
        {"forward invalid call", "func before(){return after();}func after(){return 0;}func probe(){before()();}", "AE0507"},
        {"recursive unresolved return", "func recursive(){return recursive();}func probe(){recursive()[0];}", "AE0503"},
        {"forward comparison", "func before(){return after();}func after(){return [\"x\"];}func probe():bool{return before()[0]==\"x\";}", NULL},
        {"forward arithmetic", "func before(){return after();}func after(){return [1];}func probe():int{return before()[0]+1;}", NULL},
        {"forward unary", "func before(){return after();}func after(){return [1];}func probe():int{return -before()[0];}", NULL},
        {"forward condition", "func before(){return after();}func after(){return [true];}func probe(){if before()[0]{}}", NULL},
        {"forward invalid comparison", "func before(){return after();}func after(){return [1];}func probe(){before()[0]==\"x\";}", "AE1019"},
        {"forward invalid unary", "func before(){return after();}func after(){return [\"x\"];}func probe(){-before()[0];}", "AE1018"},
        {"forward invalid condition", "func before(){return after();}func after(){return [1];}func probe(){if before()[0]{}}", "AE1102"},
        {"nested argument target", "func take(v:string){}func probe(){take(identity(inferredEmpty()));}", NULL},
        {"argument from later binding", "func probe():string{return pair(inferredEmpty(),\"x\",1)[0];}", NULL},
        {"argument from earlier binding", "func probe():string{return pair(\"x\",inferredEmpty(),1)[0];}", NULL},
        {"argument array target", "func take(v:string[]){}func probe(){take([inferredEmpty()]);}", NULL},
        {"argument if target", "func take(v:string){}func probe(b:bool){take(if b{inferredEmpty();}else{\"x\";});}", NULL},
        {"argument match target", "func take(v:string){}func probe(n:int){take(match n{0{inferredEmpty();}else{\"x\";}});}", NULL},
        {"argument try target", "func take(v:string){}func probe(){take(try inferredEmpty() catch{\"x\";});}", NULL},
        {"argument method target", "type Sink{func take(v:string){}}func probe(s:Sink){s.take(inferredEmpty());}", NULL},
        {"argument owner target", "type Sink<T>{func take(v:T){}}func probe(s:Sink<string>){s.take(inferredEmpty());}", NULL},
        {"argument static target", "type Sink{static func take(v:string){}}func probe(){Sink.take(inferredEmpty());}", NULL},
        {"argument fit target", "fit string{func take(v:string){}}func probe(v:string){v.take(inferredEmpty());}", NULL},
        {"argument constructor target", "type Sink{func Sink(v:string){}}func probe(){Sink(inferredEmpty());}", NULL},
        {"argument callable target", "spec Sink(v:string):void;func probe(s:Sink){s(inferredEmpty());}", NULL},
        {"argument generic callable target", "spec Sink<T>(v:T):void;func probe(s:Sink<string>){s(inferredEmpty());}", NULL},
        {"argument constrained callable", "spec Sink(v:string):void;func probe<S:Sink>(s:S){s(inferredEmpty());}", NULL},
        {"argument open caller", "func take<U>(v:U):U{return v;}func probe<T>():T{return take(inferredEmpty());}", NULL},
        {"argument object target", "func take(v:Box<string>){}func probe(){take(Box<string>{value:inferredEmpty()});}", NULL},
        {"argument variadic target", "func take(v:string...){}func probe(){take(inferredEmpty(),inferredEmpty());}", NULL},
        {"argument overload ambiguity", "func take(v:string){}func take(v:int){}func probe(){take(inferredEmpty());}", "AE0511"},
        {"argument explicit mismatch", "func take(v:string){}func probe(){take(inferredEmpty<int>());}", "AE0512"},
        {"argument unconstrained inference", "func probe(){identity(inferredEmpty());}", "AE0525"},
        {"argument unrelated missing parameter", "func missing<T,U>():T{let v:T;return v;}func take(v:string){}func probe(){take(missing());}", "AE0525"},
    };
    for (size_t i = 0U; i < sizeof cases / sizeof *cases; ++i) {
        char source[8192];
        int written = snprintf(source, sizeof source, "%s\n%s\n", query != NULL
            ? "module chain.consumer;import chain.provider;" : chain_provider, cases[i].body);
        CHECK(written > 0 && (size_t)written < sizeof source);
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = chain_analyze(cases[i].name, source, query, cases[i].code, &program);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    return sizeof cases / sizeof *cases;
}

/* Exercise local declarations and real persisted public/workspace imports. */
void test_generic_chain_context(void) {
    size_t count = chain_matrix(NULL);
    CHECK(mkdir("temp", 0777) == 0 || errno == EEXIST);
    char directory[] = "temp/generic-chain-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char roots[2][256];
    snprintf(roots[0], sizeof roots[0], "%s/public", directory);
    snprintf(roots[1], sizeof roots[1], "%s/workspace", directory);
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = chain_analyze("provider", chain_provider, NULL, NULL, &program);
    FengSymbolExportOptions options = {.public_root = roots[0], .workspace_root = roots[1]};
    FengSymbolError error = {0};
    CHECK(feng_symbol_export_analysis(analysis, &options, &error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
    for (size_t i = 0U; i < 2U; ++i) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, roots[i], i == 0U
            ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        CHECK(chain_matrix(&query) == count);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&error);
    printf("generic chain context: %zu cases across source/public FT/workspace FT passed\n", count);
}
