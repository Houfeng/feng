#include "codegen/codegen.h"
#include "parser/parser.h"
#include "runtime/feng_runtime.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

/* The approved ABI retains the original three-field generic parameter record. */
_Static_assert(sizeof(FengGenericParamDescriptor) ==
    offsetof(FengGenericParamDescriptor, witness) + sizeof(void *), "generic parameter ABI grew");

/* Analyze independently owned source against optional persisted dependencies. */
static FengSemanticAnalysis *projection_analyze(const char *source,
    const FengSemanticImportedModuleQuery *query, FengProgram **program) {
    FengParseError parse = {0};
    bool parsed = feng_parse_source(source, strlen(source), "projection_codegen.ff", program, &parse);
    if (!parsed) fprintf(stderr, "parse %u:%u %s\n%s", parse.token.line, parse.token.column, parse.message, source);
    CHECK(parsed);
    const FengProgram *programs[] = {*program};
    FengSemanticAnalyzeOptions options = {.target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(), .imported_modules = query};
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t count = 0U;
    bool ok = feng_semantic_analyze_with_options(programs, 1U, &options, &analysis, &errors, &count);
    if (!ok) for (size_t i = 0U; i < count; ++i) fprintf(stderr, "%s %s\n%s", errors[i].code, errors[i].message, source);
    CHECK(ok && count == 0U);
    feng_semantic_errors_free(errors, count);
    return analysis;
}

/* Ignore literal/comment braces when checking static descriptor lifetime. */
static size_t projection_depth(const char *source, const char *end) {
    size_t depth = 0U;
    for (const char *p = source; p < end; ++p) {
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (p < end && *p != quote) {
                if (*p == '\\' && p + 1 < end) ++p;
                ++p;
            }
        } else if (*p == '/' && p + 1 < end && p[1] == '/') {
            while (p < end && *p != '\n') ++p;
        } else if (*p == '/' && p + 1 < end && p[1] == '*') {
            p += 2;
            while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
            if (p + 1 < end) ++p;
        } else if (*p == '{') ++depth;
        else if (*p == '}') { CHECK(depth > 0U); --depth; }
    }
    return depth;
}

/* Check real C definitions, not only names or comments mentioning static data. */
static void projection_check_static(const char *source, bool expected) {
    CHECK(strstr(source, "&(const FengGenericParamDescriptor){") == NULL);
    CHECK(strstr(source, "(const FengGenericParamDescriptor*[]){") == NULL);
    const char *prefix = "const FengGenericParamDescriptor ";
    for (const char *p = source; (p = strstr(p, prefix)) != NULL; ++p) {
        const char *name = p + strlen(prefix);
        if (*name == '*') continue; /* parameters and static pointer tables */
        const char *end = name;
        while (isalnum((unsigned char)*end) || *end == '_') ++end;
        while (isspace((unsigned char)*end)) ++end;
        if (*end != '=' && *end != ';') continue;
        CHECK(p >= source + 7 && strncmp(p - 7, "static ", 7U) == 0);
        CHECK(projection_depth(source, p) == 0U);
    }
    size_t tables = 0U;
    const char *table_prefix = "static const FengGenericParamDescriptor *const ";
    for (const char *p = source; (p = strstr(p, table_prefix)) != NULL; ++p) {
        const char *end = strchr(p, '\n');
        const char *suffix = strstr(p, "__constraint_projections[] = {");
        if (suffix == NULL || (end != NULL && suffix > end)) continue;
        CHECK(projection_depth(source, p) == 0U);
        ++tables;
    }
    CHECK(expected ? tables > 0U : tables == 0U);
    if (!expected) CHECK(strstr(source, "->reified_constraint_projection_descriptors") == NULL);
}

/* Closing two open slots to one actual reuses the record, never the slot. */
static void projection_check_equal_closed_slots(const char *source) {
    size_t equal = 0U, different = 0U;
    for (const char *p = source; (p = strstr(p, "{.name = \"two\"")) != NULL; ++p) {
        const char *end = strstr(p, "};");
        const char *field = strstr(p, ".reified_constraint_projection_descriptors = ");
        CHECK(end != NULL && field != NULL && field < end);
        field += strlen(".reified_constraint_projection_descriptors = ");
        size_t length = 0U;
        while (isalnum((unsigned char)field[length]) || field[length] == '_') ++length;
        CHECK(length > 0U && length < 256U);
        char declaration[280], first[256], second[256];
        snprintf(declaration, sizeof declaration, "%.*s[] = {\n", (int)length, field);
        const char *body = strstr(source, declaration);
        CHECK(body != NULL);
        body += strlen(declaration);
        int consumed = 0;
        CHECK(sscanf(body, " %255[^,], %255[^,], %n", first, second, &consumed) == 2);
        CHECK(consumed > 0 && body[consumed] == '}');
        CHECK(first[0] == '&' && second[0] == '&');
        if (strcmp(first, second) == 0) ++equal;
        else ++different;
    }
    CHECK(equal == 1U && different == 1U);
}

/* Compile every generated translation unit with the existing strict C callback. */
static void projection_emit(FengSemanticAnalysis *analysis, bool tables,
    bool shared_uses, bool owner_uses, void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);
    if (!ok) fprintf(stderr, "%s %s\n", error.code, error.message);
    CHECK(ok && output.c_source != NULL);
    projection_check_static(output.c_source, tables);
    if (shared_uses) CHECK(strstr(output.c_source, "->reified_constraint_projection_descriptors[") != NULL);
    if (owner_uses) CHECK(strstr(output.c_source, "_td->reified_constraint_projection_descriptors[") != NULL);
    if (owner_uses) projection_check_equal_closed_slots(output.c_source);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
}

/* Create both profiles while retaining no producer analysis in the caller. */
static void projection_export(const char *source, const FengSemanticImportedModuleQuery *query,
    FengSymbolImportedModuleCache *cache, const char *public_root, const char *workspace_root, bool owner_uses,
    void (*compile_c)(const char *)) {
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis = projection_analyze(source, query, &program);
    if (cache != NULL) CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
    FengSymbolExportOptions options = {.public_root = public_root, .workspace_root = workspace_root};
    FengSymbolError error = {0};
    /* Providers without a concrete caller contain shared reads but no tables. */
    if (owner_uses) projection_emit(analysis, true, true, true, compile_c);
    else {
        FengCodegenOutput output = {0};
        FengCodegenError codegen_error = {0};
        CHECK(feng_codegen_emit_program(analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &codegen_error));
        CHECK(strstr(output.c_source, "->reified_constraint_projection_descriptors[") != NULL);
        compile_c(output.c_source);
        feng_codegen_output_free(&output);
        feng_codegen_error_free(&codegen_error);
    }
    /* Match the CLI frontend: imported identities are populated explicitly
     * before emitting code or exporting a middle package's dependencies. */
    bool exported = feng_symbol_export_analysis(analysis, &options, &error);
    if (!exported) fprintf(stderr, "export %u:%u: %s\n", error.token.line, error.token.column, error.message);
    CHECK(exported);
    feng_symbol_error_free(&error);
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);
}

/* Many method sets force reallocation between owner initializers/finalizers. */
static void projection_source(char *source, size_t capacity) {
    int written = snprintf(source, capacity,
        "open module projection.base;\n"
        "open spec Left{func left():i32;} open spec Right{func right():i32;}\n"
        "open spec Extra{func extra():i32;} open spec Both:Left&Right; open spec All:Both&Extra;\n"
        "open spec Reader<T>(value:T):i32; open spec Number():i32;\n"
        "open type Ref:Left,Right,Extra{func left():i32{return 11;}func right():i32{return 23;}func extra():i32{return 37;}}\n"
        "@value open type Value:Left,Right,Extra{func left():i32{return 41;}func right():i32{return 53;}func extra():i32{return 67;}}\n"
        "open func right<T:Right>(value:T):i32{return value.right();}\n"
        "open func left<T:Left>(value:T):i32{return value.left();}\n"
        "open func extra<T:Extra>(value:T):i32{return value.extra();}\n"
        "open func zero<T>():T{let value:T;return value;}\n"
        "open func two<T:All,U:All>(first:T,second:U):i32{return right<T>(first)*10+right<U>(second);}\n"
        "open func capture<T:Right>(value:T):Number{return (){return right<T>(value);};}\n"
        "open func escape<T:All>(value:T):Number{return capture<T>(value);}\n"
        "open func relay<T:All>(value:T,depth:i32):i32{if depth>0{return relay<T>(value,depth-1);}return right<T>(value)+left<T>(value);}\n"
        "open type Owner<T:All>{open let value:T;open let first=right<T>(zero<T>());\n"
        "open let reader:Reader<T> = right<T>;\n");
    CHECK(written > 0 && (size_t)written < capacity);
    size_t length = (size_t)written;
    for (size_t i = 0U; i < 40U; ++i) {
        written = snprintf(source + length, capacity - length,
            "func method%zu<U:All>(item:U):i32{return right<T>(self.value)+left<U>(item);}\n", i);
        CHECK(written > 0 && (size_t)written < capacity - length);
        length += (size_t)written;
    }
    written = snprintf(source + length, capacity - length,
        "open let last=left<T>(zero<T>());\n"
        "open func Owner(value:T){self.value=value;right<T>(value);}\n"
        "func ~Owner(){right<T>(self.value);}\n"
        "open static func read_static(value:T):i32{return right<T>(value);}}\n"
        "@value open type ValueOwner<T:All>{open let value:T;open let first=right<T>(zero<T>());"
        "open let reader:Reader<T> = right<T>;func read():i32{return left<T>(self.value);}}\n"
        "open func run():i32{let ref=Ref{};let val=Value{};let owner=Owner<Ref>(ref);"
        "let aggregate=ValueOwner<Value>{value:val};"
        "return owner.method0<Ref>(ref)+owner.method39<Value>(val)+owner.reader(ref)"
        "+Owner<Ref>.read_static(ref)+aggregate.read()+relay<Ref>(ref,2)+escape<Value>(val)()"
        "+two<Ref,Ref>(ref,ref)+two<Ref,Value>(ref,val);}\n");
    CHECK(written > 0 && (size_t)written < capacity - length);
}

/* Independent fit providers must not share or alter their receiver's slot domain. */
static void projection_packages(void (*compile_c)(const char *)) {
    char directory[] = "temp/constraint-codegen-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char roots[3][2][256], source[14000];
    for (size_t package = 0U; package < 3U; ++package)
        for (size_t profile = 0U; profile < 2U; ++profile)
            snprintf(roots[package][profile], sizeof roots[package][profile], "%s/p%zu-%zu", directory, package, profile);
    projection_source(source, sizeof source);
    projection_export(source, NULL, NULL, roots[0][0], roots[0][1], true, compile_c);
    const char *providers[] = {
        "open module projection.first;import projection.base;\n"
        "open spec Selected:Left&Extra;open func selected<T:Selected>(value:T):i32{return left<T>(value)+extra<T>(value);}\n"
        "open fit Owner<T>{func first():i32{return selected<T>(self.value);}}\n"
        "open fit ValueOwner<T>{func first():i32{return selected<T>(self.value);}}\n",
        "open module projection.second;import projection.base;\n"
        "open spec Selected:Extra&Right;open func selected<T:Selected>(value:T):i32{return extra<T>(value)+right<T>(value);}\n"
        "open fit Owner<T>{func second():i32{return selected<T>(self.value);}}\n"
        "open fit ValueOwner<T>{func second():i32{return selected<T>(self.value);}}\n"
    };
    FengSymbolError error = {0};
    for (size_t provider_index = 0U; provider_index < 2U; ++provider_index) {
        FengSymbolProvider *provider = NULL;
        CHECK(feng_symbol_provider_create(&provider, &error));
        CHECK(feng_symbol_provider_add_ft_root(provider, roots[0][0], FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
        FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
        projection_export(providers[provider_index], &query, cache, roots[provider_index + 1U][0],
            roots[provider_index + 1U][1], false, compile_c);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    for (size_t profile = 0U; profile < 2U; ++profile) {
        for (size_t order = 0U; order < 2U; ++order) {
            FengSymbolProvider *provider = NULL;
            CHECK(feng_symbol_provider_create(&provider, &error));
            for (size_t i = 0U; i < 3U; ++i) {
                size_t package = order ? 2U - i : i;
                CHECK(feng_symbol_provider_add_ft_root(provider, roots[package][profile],
                    profile ? FENG_SYMBOL_PROFILE_WORKSPACE_CACHE : FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC, &error));
            }
            FengSymbolImportedModuleCache *cache = feng_symbol_imported_module_cache_create(provider);
            CHECK(cache != NULL);
            FengSemanticImportedModuleQuery query = feng_symbol_imported_module_cache_as_query(cache);
            char consumer[1400];
            snprintf(consumer, sizeof consumer,
                "module consumer;import projection.base;%s\n"
                "func forward<T:All>(value:T):i32{return relay<T>(value,2);}\n"
                "func run():i32{let ref=Ref{};let val=Value{};let owner=Owner<Ref>(ref);"
                "let aggregate=ValueOwner<Value>{value:val};"
                "return owner.first()+owner.second()+aggregate.second()+aggregate.first()"
                "+owner.method0<Ref>(ref)+owner.method39<Value>(val)+owner.reader(ref)"
                "+forward<Ref>(ref)+escape<Value>(val)();}\n",
                order ? "import projection.second;import projection.first;" : "import projection.first;import projection.second;");
            FengProgram *program = NULL;
            FengSemanticAnalysis *analysis = projection_analyze(consumer, &query, &program);
            CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(cache, analysis));
            /* Imported initializer bodies remain in the provider; consumers
             * emit their closed tables, not duplicate owner-body reads. */
            projection_emit(analysis, true, false, false, compile_c);
            feng_semantic_analysis_free(analysis);
            feng_program_free(program);
            feng_symbol_imported_module_cache_free(cache);
            feng_symbol_provider_free(provider);
        }
    }
    feng_symbol_error_free(&error);
}

/* No-intersection and unused-intersection bodies gain no read or stack record. */
void test_constraint_projection_codegen(void (*compile_c)(const char *)) {
    const char *controls[] = {
        "module control;func identity<T>(value:T):T{return value;}func run():i32{return identity<i32>(3);}",
        "module control;spec Right{func right():i32;}type Item:Right{func right():i32{return 3;}}"
        "func inner<T:Right>(value:T):i32{return value.right();}"
        "func outer<T:Right>(value:T):i32{return inner<T>(value);}func run():i32{return outer<Item>(Item{});}",
        "module control;spec Left{func left():i32;}spec Right{func right():i32;}spec Both:Left&Right;"
        "type Item:Left,Right{func left():i32{return 1;}func right():i32{return 2;}}"
        "func direct<T:Both>(value:T):i32{return value.right();}func run():i32{return direct<Item>(Item{});}"
    };
    for (size_t i = 0U; i < sizeof controls / sizeof *controls; ++i) {
        FengProgram *program = NULL;
        FengSemanticAnalysis *analysis = projection_analyze(controls[i], NULL, &program);
        projection_emit(analysis, false, false, false, compile_c);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
    }
    projection_packages(compile_c);
    puts("constraint projection static ABI, owner and independent package matrices passed");
}
