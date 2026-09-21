#include "../throw_constraint_helpers.h"
#include "symbol/export.h"
#include "symbol/ft.h"
#include "symbol/imported_module.h"
#include "symbol/provider.h"
#include <unistd.h>

/* A consumer must prove its own cleanup boundary using only imported facts. */
typedef struct DeferImportCase {
    const char *source;
    bool accepted;
} DeferImportCase;

/* Run both FT profiles after destroying all producer source and analysis. */
void test_defer_exception_effects_ft(void) {
    const char *source = "open module cleanup.api;"
        "func hidden<T:throw>(x:T){throw x;}"
        "open func raise<T:throw>(x:T){hidden(x);}"
        "open func filtered<T:throw>(x:T){try hidden(x) catch e:string{}}"
        "open spec Action():void;open func invoke(f:Action){f();}"
        "open func make<T:throw>(x:T):Action{return (){hidden(x);};}"
        "open func quiet(){}"
        "open func safe<T:throw>(x:T){defer{try hidden(x) catch{}}}"
        "open func again(){try hidden(\"x\") catch{throw;}}"
        "open func recursive<T:throw>(x:T,n:i32){if n>0{recursive(x,n-1);}else{hidden(x);}}"
        "open type Box<T:throw>{open let value:T;open func fail(){hidden(self.value);}"
        "open func both<U:throw>(x:U){hidden(self.value);hidden(x);}}"
        "open spec Worker{func work():void;}"
        "open type NoFail:Worker{open func work():void{}}"
        "open type Fail:Worker{open func work():void{hidden(true);}}"
        "open func work<T:Worker>(x:T){x.work();}"
        "open func indexed<T:Worker>(x:T[]){x[0].work();}"
        "open spec Factory{static func work():void;}"
        "open type Static:Factory{open static func work():void{hidden(true);}}"
        "open func staticWork<T:Factory>(){T.work();}"
        "open func initial():i32{throw \"init\";}"
        "open let lazy=initial();"
        "open type Item{open let n=initial();}";
    ThrowConstraintUnit producer = throw_constraint_analyze(source, NULL, NULL);
    FengSymbolGraph *graph = NULL;
    FengSymbolError error = {0};
    THROW_CHECK(feng_symbol_build_graph(producer.analysis, &graph, &error));
    char directory[] = "temp/defer-effects-ft-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char paths[2][256];
    for (size_t i = 0U; i < 2U; ++i) {
        snprintf(paths[i], sizeof(paths[i]), "%s/%zu.ft", directory, i);
        THROW_CHECK(feng_symbol_ft_write_module(feng_symbol_graph_module_at(graph, 0U),
            i == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
            paths[i], &error));
    }
    feng_symbol_graph_free(graph);
    throw_constraint_dispose(&producer);
    const DeferImportCase cases[] = {
        {"func run(){defer{raise(\"x\");}}", false},
        {"func run(){defer{try raise(\"x\") catch e:string{}}}", true},
        {"func run(){defer{try raise(true) catch e:string{}}}", false},
        {"func run<T:throw>(x:T){defer{raise(x);}}", false},
        {"func run<T:throw>(x:T){defer{try raise(x) catch{}}}", true},
        {"func run(){defer{filtered(\"x\");}}", true},
        {"func run(){defer{filtered(true);}}", false},
        {"func run(){defer{invoke(quiet);}}", true},
        {"func run(){defer{invoke((){throw \"x\";});}}", false},
        {"func run(f:Action){defer{invoke(f);}}", false},
        {"func run(f:Action){defer{try invoke(f) catch{}}}", true},
        {"func run(f:Action){defer{try invoke(f) catch e:bool{}}}", false},
        {"func run(){defer{make(true)();}}", false},
        {"func run(){defer{try make(true)() catch e:bool{}}}", true},
        {"func run(){defer{let f=make(true);}}", true},
        {"func run(){defer{safe(\"x\");}}", true},
        {"func run(){defer{again();}}", false},
        {"func run(){defer{try again() catch e:string{}}}", true},
        {"func run(){defer{recursive(true,3);}}", false},
        {"func run(){defer{try recursive(true,3) catch e:bool{}}}", true},
        {"func run(x:Box<string>){defer{x.fail();}}", false},
        {"func run(x:Box<string>){defer{try x.fail() catch e:string{}}}", true},
        {"func run(x:Box<string>){defer{try x.both(true) catch e:string{}}}", false},
        {"func run(x:Box<string>){defer{try x.both(true) catch e:string{} catch e:bool{}}}", true},
        {"func run(x:Worker){defer{x.work();}}", false},
        {"func run(x:Worker){defer{try x.work() catch{}}}", true},
        {"func run(x:NoFail){defer{work(x);}}", true},
        {"func run(x:Fail){defer{work(x);}}", false},
        {"func run(x:NoFail[]){defer{indexed(x);}}", true},
        {"func run(x:Fail[]){defer{indexed(x);}}", false},
        {"func run(x:Fail[]){defer{try indexed(x) catch e:bool{}}}", true},
        {"func run(x:Worker[]){defer{indexed(x);}}", false},
        {"func run(x:Worker[]){defer{try indexed(x) catch{}}}", true},
        {"func run(){defer{staticWork<Static>();}}", false},
        {"func run(){defer{try staticWork<Static>() catch e:bool{}}}", true},
        {"func run(){defer{let x=lazy;}}", false},
        {"func run(){defer{let y=try lazy catch{0;};}}", true},
        {"func run(){defer{let x=Item{};}}", false},
        {"func run(){let result=try (if true{defer{raise(\"x\");}0;}else{0;}) catch{0;};}", false},
    };
    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolGraph *loaded = NULL;
        THROW_CHECK(feng_symbol_ft_read_file(paths[profile], NULL, &loaded, &error));
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_graph(provider, loaded, &error));
        feng_symbol_graph_free(loaded);
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        for (size_t i = 0U; i < sizeof(cases) / sizeof(*cases); ++i) {
            char consumer[2048];
            snprintf(consumer, sizeof(consumer), "module consumer;import cleanup.api;%s", cases[i].source);
            ThrowConstraintUnit unit = throw_constraint_analyze(consumer, &query,
                cases[i].accepted ? NULL : "AE1507");
            throw_constraint_dispose(&unit);
        }
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
        THROW_CHECK(unlink(paths[profile]) == 0);
    }
    THROW_CHECK(rmdir(directory) == 0);
    printf("defer exception FT matrix: %zu cases in both profiles passed\n", sizeof(cases) / sizeof(*cases));
}
