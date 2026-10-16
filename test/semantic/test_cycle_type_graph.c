/* Strong-reference graph coverage independent of ownership optimizations. */
#include "../throw_constraint_helpers.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"

#include <unistd.h>

/* Find a nominal declaration without relying on its graph table position. */
static const FengDecl *cycle_graph_decl(const FengSemanticAnalysis *analysis, const char *name) {
    for (size_t m = 0; m < analysis->module_count; ++m)
        for (size_t p = 0; p < analysis->modules[m].program_count; ++p) {
            const FengProgram *program = analysis->modules[m].programs[p];
            for (size_t d = 0; d < program->declaration_count; ++d) {
                const FengDecl *decl = program->declarations[d];
                if (decl->kind != FENG_DECL_TYPE) continue;
                FengSlice found = decl->as.type_decl.name;
                if (found.length == strlen(name) && memcmp(found.data, name, found.length) == 0)
                    return decl;
            }
        }
    THROW_CHECK(false);
    return NULL;
}

/* One source graph exercises used parameters, unknown subjects and inline
 * representation edges; all exported cases must survive source-hidden FT. */
static const char *cycle_graph_source =
    "open module cycle_graph;"
    "open type Leaf{open let n:int;}"
    "open type Box<T>{open var item:T;}"
    "open type Chain<T>{open let payload:T;let leaf:Leaf;}"
    "open type Phantom<T>{open let n:int;}"
    "open spec Maybe<T>:bool|T;"
    "open spec Nested<T>:i32|Maybe<T>;"
    "@value open type Inline<T>{open var item:Maybe<T>;}"
    "open type Pair<T>(i32,Maybe<T>);"
    "open type Node{open var next:Maybe<Node>;}"
    "open type NestedNode{open var next:Nested<NestedNode>;}"
    "open type ValueNode{open var next:Inline<ValueNode>;}"
    "open type TupleNode{open var next:Pair<TupleNode>;}"
    "open type ArrayNode{open var next:Maybe<ArrayNode[!][]>;}"
    "open type Owner{open var next:Maybe<Box<Owner>>;}"
    "open type ClosedLeaf{open var next:Box<Leaf>;}"
    "open type ClosedInt{open var next:Box<int>;}"
    "open type NestedBox{open var next:Box<Box<Leaf>>;}"
    "open type Unused{open var next:Phantom<Unused>;}"
    "open type Raw{open var next:Maybe<Raw*>;}"
    "open type Static{open static var refs:Static[]=[];open let n:int;}"
    "open type Inferred{open let ref=Leaf{n:1};}"
    "open type InferredScalar{open let value=1;}"
    "open type InferredString{open let value=\"leaf\";}"
    "open func empty<T>():T[]{return [];}"
    "open type InferredCycle{open var items=empty<InferredCycle>();}"
    "open type ChainLeaf{open let value:Chain<Leaf>;}"
    "open type ChainInt{open let value:Chain<int>;}"
    "open type ChainOwner{open let value:Chain<ChainOwner>;}"
    "open spec View{open var n:int;}"
    "open spec Named{open let name:string;}"
    "open spec Both:View&Named;"
    "open spec Callback():void;"
    "open type Dynamic{open var subject:View;}"
    "open type Intersection{open var subject:Both;}"
    "open type Closure{open var callback:Callback;}"
    "open type Upstream{open var item:Box<Dynamic>;}"
    "open type Outside{open var next:Maybe<Node>;}"
    "open type Growing<T>{open var next:Growing<T[]>[];}"
    "open type Shrinking{open var next:Box<Box<int>>;}"
    "open type PrivateOwner{seal var next:PrivateNode;}"
    "type PrivateNode{var next:Maybe<PrivateOwner>;}";

/* Open unknowns are conservative, while concrete acyclic fields and unused
 * parameters do not inherit unrelated cycles or unknown generic arguments. */
static void cycle_graph_expect(const FengSemanticAnalysis *analysis) {
    static const struct { const char *name; bool cyclic; } expected[] = {
        {"Leaf", false}, {"Box", true}, {"Chain", true}, {"Phantom", false},
        {"Node", true}, {"NestedNode", true}, {"ValueNode", true},
        {"TupleNode", true}, {"ArrayNode", true}, {"Owner", true},
        {"ClosedLeaf", false}, {"ClosedInt", false}, {"NestedBox", false},
        {"Unused", false}, {"Raw", false}, {"Static", false}, {"Inferred", false},
        {"InferredScalar", false}, {"InferredString", false},
        {"InferredCycle", true}, {"ChainLeaf", false}, {"ChainInt", false}, {"ChainOwner", true},
        {"Dynamic", true}, {"Intersection", true}, {"Closure", true},
        {"Upstream", true}, {"Outside", false}, {"Growing", true},
        {"Shrinking", false}, {"PrivateOwner", true}, {"PrivateNode", true}
    };
    for (size_t i = 0; i < sizeof expected / sizeof *expected; ++i) {
        const FengDecl *decl = cycle_graph_decl(analysis, expected[i].name);
        bool actual = feng_semantic_type_is_potentially_cyclic(analysis, decl);
        if (actual != expected[i].cyclic) fprintf(stderr, "cycle graph: %s expected %d got %d\n",
                                                 expected[i].name, expected[i].cyclic, actual);
        THROW_CHECK(actual == expected[i].cyclic);
    }
    const char *holders[] = {"ClosedLeaf", "ClosedInt", "NestedBox", "Upstream", "Shrinking"};
    for (size_t i = 0; i < sizeof holders / sizeof *holders; ++i) {
        const FengDecl *holder = cycle_graph_decl(analysis, holders[i]);
        const FengTypeRef *ref = holder->as.type_decl.members[0]->as.field.type;
        THROW_CHECK(ref != NULL && ref->as.named.type_arg_count == 1U);
        bool actual = feng_semantic_type_instance_is_potentially_cyclic(analysis,
            cycle_graph_decl(analysis, "Box"), ref->resolution_program,
            (const FengTypeRef *const *)ref->as.named.type_args, 1U);
        /* Imported refs can be lexically qualified without resolution_program;
         * source refs retain nominal identities from semantic validation. */
        THROW_CHECK(actual == (strcmp(holders[i], "Upstream") == 0));
    }
    THROW_CHECK(feng_semantic_type_is_potentially_cyclic(analysis, NULL));
    THROW_CHECK(feng_semantic_type_is_potentially_cyclic(NULL, NULL));
}

/* Same-spelled types in different lexical modules must never share a node. */
static void cycle_graph_nominal_identity(void) {
    FengProgram *programs[] = {
        throw_constraint_parse("open module graph_left;open type Same{var next:Same[];}"),
        throw_constraint_parse("open module graph_right;open type Same{let n:int;}"
            "open type Holder{let value:Same;}"),
        throw_constraint_parse("module graph_user;import graph_left as L;import graph_right as R;"
            "type Mixed{let left:L.Same;let right:R.Same;}")
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0;
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size()};
    THROW_CHECK(feng_semantic_analyze_with_options((const FengProgram **)programs,
        3U, &options, &analysis, &errors, &count));
    THROW_CHECK(feng_semantic_type_is_potentially_cyclic(analysis, programs[0]->declarations[0]));
    THROW_CHECK(!feng_semantic_type_is_potentially_cyclic(analysis, programs[1]->declarations[0]));
    THROW_CHECK(!feng_semantic_type_is_potentially_cyclic(analysis, programs[1]->declarations[1]));
    THROW_CHECK(!feng_semantic_type_is_potentially_cyclic(analysis, programs[2]->declarations[0]));
    feng_semantic_analysis_free(analysis);
    feng_semantic_errors_free(errors, count);
    for (size_t i = 0; i < 3U; ++i) feng_program_free(programs[i]);
}

/* An inferred callable without an exported named signature remains unknown.
 * Source-hidden callable coverage uses the explicit Callback spec above. */
static void cycle_graph_inferred_callable(void) {
    ThrowConstraintUnit unit = throw_constraint_analyze(
        "module inferred_cycle_graph;func action():void{}"
        "type InferredClosure{let callback=action;}", NULL, NULL);
    THROW_CHECK(feng_semantic_type_is_potentially_cyclic(unit.analysis,
        cycle_graph_decl(unit.analysis, "InferredClosure")));
    throw_constraint_dispose(&unit);
}

/* Serialize both supported FT profiles and discard source before importing. */
void test_cycle_type_graph(void) {
    ThrowConstraintUnit source = throw_constraint_analyze(cycle_graph_source, NULL, NULL);
    cycle_graph_expect(source.analysis);
    char public_root[] = "temp/cycle-graph-public-XXXXXX";
    char workspace_root[] = "temp/cycle-graph-workspace-XXXXXX";
    THROW_CHECK(mkdtemp(public_root) != NULL && mkdtemp(workspace_root) != NULL);
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    bool exported = feng_symbol_export_analysis(source.analysis, &options, &error);
    if (!exported) fprintf(stderr, "cycle graph export at %u:%u: %s\n",
                           error.token.line, error.token.column, error.message);
    THROW_CHECK(exported);
    throw_constraint_dispose(&source);
    const char *roots[] = {public_root, workspace_root};
    for (size_t i = 0; i < 2U; ++i) {
        FengSymbolProvider *provider = NULL;
        THROW_CHECK(feng_symbol_provider_create(&provider, &error));
        THROW_CHECK(feng_symbol_provider_add_ft_root(provider, roots[i],
            i == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        THROW_CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        ThrowConstraintUnit consumer = throw_constraint_analyze(
            "module consumer;import cycle_graph;func consume(value:Owner):void{}", &query, NULL);
        cycle_graph_expect(consumer.analysis);
        throw_constraint_dispose(&consumer);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
        char path[256];
        THROW_CHECK(snprintf(path, sizeof path, "%s/cycle_graph.ft", roots[i]) > 0);
        THROW_CHECK(unlink(path) == 0 && rmdir(roots[i]) == 0);
    }
    feng_symbol_error_free(&error);
    cycle_graph_nominal_identity();
    cycle_graph_inferred_callable();
    puts("cycle type graph: concrete storage, unknowns, finite generics and both FT profiles passed");
}
