#include "codegen/codegen.h"
#include "parser/parser.h"
#include "runtime/feng_runtime.h"
#include "semantic/semantic.h"
#include "symbol/export.h"
#include "symbol/imported_module.h"

#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: reified metadata count check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

_Static_assert(offsetof(FengTypeDescriptor, reified_union_projections_count) <
                   offsetof(FengTypeDescriptor, reified_union_projections),
               "type union projection count must precede its table");
_Static_assert(offsetof(FengTypeDescriptor, reified_spec_view_coercions_count) <
                   offsetof(FengTypeDescriptor, reified_spec_view_coercions),
               "type spec view count must precede its table");
_Static_assert(offsetof(FengTypeDescriptor,
                        reified_constraint_projection_descriptors_count) <
                   offsetof(FengTypeDescriptor,
                            reified_constraint_projection_descriptors),
               "type constraint projection count must precede its table");
_Static_assert(offsetof(FengAggregateDescriptor,
                        reified_union_projections_count) <
                   offsetof(FengAggregateDescriptor, reified_union_projections),
               "aggregate union projection count must precede its table");
_Static_assert(offsetof(FengAggregateDescriptor,
                        reified_spec_view_coercions_count) <
                   offsetof(FengAggregateDescriptor, reified_spec_view_coercions),
               "aggregate spec view count must precede its table");
_Static_assert(offsetof(FengAggregateDescriptor,
                        reified_constraint_projection_descriptors_count) <
                   offsetof(FengAggregateDescriptor,
                            reified_constraint_projection_descriptors),
               "aggregate constraint projection count must precede its table");
_Static_assert(offsetof(FengFunctionDescriptor,
                        reified_union_projections_count) <
                   offsetof(FengFunctionDescriptor, reified_union_projections),
               "function union projection count must precede its table");
_Static_assert(offsetof(FengFunctionDescriptor,
                        reified_spec_view_coercions_count) <
                   offsetof(FengFunctionDescriptor, reified_spec_view_coercions),
               "function spec view count must precede its table");
_Static_assert(offsetof(FengFunctionDescriptor,
                        reified_constraint_projection_descriptors_count) <
                   offsetof(FengFunctionDescriptor,
                            reified_constraint_projection_descriptors),
               "function constraint projection count must precede its table");

/** One half-open span inside generated C. */
typedef struct MetadataCountSpan {
    const char *begin;
    const char *end;
} MetadataCountSpan;

static const char *const kMetadataCountProvider =
    "open module metadata.counts;\n"
    "open spec Inner:i32|string;open spec Outer:Inner|bool;\n"
    "open spec Field<A>{var item:A;}open spec Tag{func tag():i32;}\n"
    "open spec Left{func left():i32;}open spec Right{func right():i32;}\n"
    "open spec Both:Left&Right;\n"
    "@value open type ViewValue<A>:Field<A>,Tag{open var item:A;"
    "func tag():i32{return 7;}}\n"
    "open type Item:Left,Right{func left():i32{return 11;}"
    "func right():i32{return 13;}}\n"
    "open func use_right<T:Right>(value:T):i32{return value.right();}\n"
    "open func zero<T>():T{let value:T;return value;}\n"
    "open func empty<T>(value:T):T{return value;}\n"
    "open func single<A,T:Outer,V:Both>(first:T,source:ViewValue<A>,value:V):i32{"
    "let result=match first{item:Inner->i32{item}else{-1}};"
    "let tag:Tag=source;return result+tag.tag()+use_right<V>(value);}\n"
    "open type EmptyOwner<T>{open let value:T;}\n"
    "@value open type EmptyAggregate<T>{open let value:T;}\n"
    "open type SingleOwner<A,T:Outer,V:Both>{"
    "open let result:i32=match zero<T>(){item:Inner->i32{item}else{-1}};"
    "open let tag:Tag=ViewValue<A>{};"
    "open let right_result=use_right<V>(zero<V>());}\n"
    "@value open type SingleAggregate<A,T:Outer,V:Both>{"
    "open let result:i32=match zero<T>(){item:Inner->i32{item}else{-1}};"
    "open let tag:Tag=ViewValue<A>{};"
    "open let right_result=use_right<V>(zero<V>());}\n"
    "open type RefOwner<A,T:Outer,U:Outer,V:Both,W:Both>{"
    "open let first:i32=match zero<T>(){item:Inner->i32{item}else{-1}};"
    "open let second:i32=match zero<U>(){item:Inner->string{1}else{2}};"
    "open let field:Field<A> =ViewValue<A>{};open let tag:Tag=ViewValue<A>{};"
    "open let left_result=use_right<V>(zero<V>());"
    "open let right_result=use_right<W>(zero<W>());"
    "open func RefOwner(){"
    "let repeated:i32=match zero<T>(){item:Inner->i32{item}else{-1}};"
    "let repeated_field:Field<A> =ViewValue<A>{};use_right<V>(zero<V>());}"
    "func ~RefOwner(){use_right<W>(zero<W>());}"
    "open func method<X:Outer,Y:Both>(first_value:X,source_value:ViewValue<A>,"
    "constrained_value:Y):i32{"
    "let result=match first_value{item:Inner->i32{item}else{-1}};"
    "let tag_value:Tag=source_value;"
    "return result+tag_value.tag()+use_right<Y>(constrained_value);}}\n"
    "open fit RefOwner<A,T,U,V,W>{"
    "open func fitted<X:Outer,Y:Both>(first_value:X,source_value:ViewValue<A>,"
    "constrained_value:Y):i32{"
    "let result=match first_value{item:Inner->i32{item}else{-1}};"
    "let tag_value:Tag=source_value;"
    "return result+tag_value.tag()+use_right<Y>(constrained_value);}}\n"
    "@value open type AggregateOwner<A,T:Outer,U:Outer,V:Both,W:Both>{"
    "open let first:i32=match zero<T>(){item:Inner->i32{item}else{-1}};"
    "open let second:i32=match zero<U>(){item:Inner->string{1}else{2}};"
    "open let field:Field<A> =ViewValue<A>{};open let tag:Tag=ViewValue<A>{};"
    "open let left_result=use_right<V>(zero<V>());"
    "open let right_result=use_right<W>(zero<W>());}\n"
    "open func combined<A,T:Outer,U:Outer,V:Both,W:Both>(first:T,second:U,"
    "source:ViewValue<A>,left_value:V,right_value:W):i32{"
    "let first_result=match first{item:Inner->i32{item}else{-1}};"
    "let second_result:i32=match second{item:Inner->string{1}else{2}};"
    "let field:Field<A> =source;let tag:Tag=source;"
    "return first_result+second_result+tag.tag()+use_right<V>(left_value)"
    "+use_right<W>(right_value);}\n"
    "open func relay<A,T:Outer,U:Outer,V:Both,W:Both>(first:T,second:U,"
    "source:ViewValue<A>,left_value:V,right_value:W):i32{"
    "return combined<A,T,U,V,W>(first,second,source,left_value,right_value);}\n"
    "open func run(source:ViewValue<string>,value:Item):i32{"
    "let ref_owner=RefOwner<string,i32,i32,Item,Item>{};"
    "let aggregate_owner=AggregateOwner<string,i32,i32,Item,Item>{};"
    "let empty_owner=EmptyOwner<string>{};"
    "let empty_aggregate=EmptyAggregate<i32>{};"
    "let single_owner=SingleOwner<string,i32,Item>{};"
    "let single_aggregate=SingleAggregate<string,i32,Item>{};"
    "return relay<string,i32,i32,Item,Item>(3,5,source,value,value)"
    "+ref_owner.first+aggregate_owner.first"
    "+single<string,i32,Item>(3,source,value)"
    "+ref_owner.method<i32,Item>(3,source,value)"
    "+ref_owner.fitted<i32,Item>(3,source,value)"
    "+empty_aggregate.value+single_owner.result+single_aggregate.result"
    "+empty<i32>(0);}\n";

static const char *const kMetadataCountConsumer =
    "module metadata.consumer;import metadata.counts;\n"
    "func run(source:ViewValue<string>,value:Item):i32{"
    "let ref_owner=RefOwner<string,i32,i32,Item,Item>{};"
    "let aggregate_owner=AggregateOwner<string,i32,i32,Item,Item>{};"
    "let empty_owner=EmptyOwner<string>{};"
    "let empty_aggregate=EmptyAggregate<i32>{};"
    "let single_owner=SingleOwner<string,i32,Item>{};"
    "let single_aggregate=SingleAggregate<string,i32,Item>{};"
    "return relay<string,i32,i32,Item,Item>(3,5,source,value,value)"
    "+ref_owner.first+aggregate_owner.first"
    "+single<string,i32,Item>(3,source,value)"
    "+ref_owner.method<i32,Item>(3,source,value)"
    "+ref_owner.fitted<i32,Item>(3,source,value)"
    "+empty_aggregate.value+single_owner.result+single_aggregate.result"
    "+empty<i32>(0);}\n";

/** Find the closing brace paired with one generated-C opening brace. */
static const char *metadata_count_matching_brace(const char *opening) {
    size_t depth = 0U;

    CHECK(opening != NULL && *opening == '{');
    for (const char *cursor = opening; *cursor != '\0'; ++cursor) {
        if (*cursor == '"' || *cursor == '\'') {
            const char quote = *cursor++;

            while (*cursor != '\0' && *cursor != quote) {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    ++cursor;
                }
                ++cursor;
            }
            CHECK(*cursor == quote);
            continue;
        }
        if (cursor[0] == '/' && cursor[1] == '/') {
            cursor += 2;
            while (*cursor != '\0' && *cursor != '\n') {
                ++cursor;
            }
            if (*cursor == '\0') {
                return NULL;
            }
            continue;
        }
        if (cursor[0] == '/' && cursor[1] == '*') {
            cursor += 2;
            while (cursor[0] != '\0' &&
                   !(cursor[0] == '*' && cursor[1] == '/')) {
                ++cursor;
            }
            CHECK(cursor[0] == '*' && cursor[1] == '/');
            ++cursor;
            continue;
        }
        if (*cursor == '{') {
            ++depth;
        } else if (*cursor == '}') {
            CHECK(depth > 0U);
            --depth;
            if (depth == 0U) {
                return cursor;
            }
        }
    }
    return NULL;
}

/** Find text only when the complete match lies inside one span. */
static const char *metadata_count_span_find(MetadataCountSpan span,
                                            const char *needle) {
    const char *match;
    size_t length;

    CHECK(span.begin != NULL && span.end != NULL && span.begin <= span.end);
    CHECK(needle != NULL);
    length = strlen(needle);
    match = strstr(span.begin, needle);
    return match != NULL && match + length <= span.end ? match : NULL;
}

/** Locate one descriptor initializer by C descriptor kind and exact name. */
static MetadataCountSpan metadata_count_descriptor_span(
    const char *source,
    const char *descriptor_kind,
    const char *descriptor_name) {
    char prefix[96];
    char marker[512];
    const char *cursor;
    MetadataCountSpan result = {0};

    CHECK(snprintf(prefix, sizeof(prefix), "const %s ", descriptor_kind) > 0);
    CHECK(snprintf(marker, sizeof(marker), ".name = \"%s\"",
                   descriptor_name) > 0);
    cursor = source;
    while ((cursor = strstr(cursor, prefix)) != NULL) {
        const char *equals = strchr(cursor, '=');
        const char *semicolon = strchr(cursor, ';');
        const char *opening;
        const char *closing;
        MetadataCountSpan candidate;

        if (semicolon == NULL || equals == NULL || semicolon < equals) {
            cursor += strlen(prefix);
            continue;
        }
        opening = strchr(equals, '{');
        CHECK(opening != NULL && opening < semicolon);
        closing = metadata_count_matching_brace(opening);
        CHECK(closing != NULL);
        candidate.begin = cursor;
        candidate.end = closing + 1;
        if (metadata_count_span_find(candidate, marker) != NULL) {
            return candidate;
        }
        cursor = closing + 1;
    }
    CHECK(result.begin != NULL);
    return result;
}

/** Count top-level entries in one generated static array initializer. */
static size_t metadata_count_array_entries(MetadataCountSpan table) {
    const char *opening = strchr(table.begin, '{');
    const char *closing;
    size_t depth = 1U;
    size_t entries = 0U;
    bool has_entry = false;

    CHECK(opening != NULL && opening < table.end);
    closing = metadata_count_matching_brace(opening);
    CHECK(closing != NULL && closing < table.end);
    for (const char *cursor = opening + 1; cursor < closing; ++cursor) {
        if (*cursor == '"' || *cursor == '\'') {
            const char quote = *cursor++;

            has_entry = true;
            while (cursor < closing && *cursor != quote) {
                if (*cursor == '\\' && cursor + 1 < closing) {
                    ++cursor;
                }
                ++cursor;
            }
            CHECK(cursor < closing && *cursor == quote);
            continue;
        }
        if (cursor[0] == '/' && cursor + 1 < closing && cursor[1] == '/') {
            cursor += 2;
            while (cursor < closing && *cursor != '\n') {
                ++cursor;
            }
            continue;
        }
        if (cursor[0] == '/' && cursor + 1 < closing && cursor[1] == '*') {
            cursor += 2;
            while (cursor + 1 < closing &&
                   !(cursor[0] == '*' && cursor[1] == '/')) {
                ++cursor;
            }
            CHECK(cursor + 1 < closing);
            ++cursor;
            continue;
        }
        if (*cursor == '{') {
            if (depth == 1U) {
                has_entry = true;
            }
            ++depth;
        } else if (*cursor == '}') {
            CHECK(depth > 1U);
            --depth;
        } else if (*cursor == ',' && depth == 1U) {
            CHECK(has_entry);
            ++entries;
            has_entry = false;
        } else if (depth == 1U && !isspace((unsigned char)*cursor)) {
            has_entry = true;
        }
    }
    if (has_entry) {
        ++entries;
    }
    return entries;
}

/** Validate one count, its pointer, and the referenced array length. */
static MetadataCountSpan metadata_count_check_table(
    const char *source,
    MetadataCountSpan descriptor,
    const char *count_field,
    const char *pointer_field,
    size_t expected) {
    char count_pattern[160];
    char pointer_pattern[160];
    char table_pattern[640];
    const char *count_value;
    const char *pointer_value;
    const char *identifier_end;
    const char *table_name;
    const char *opening;
    const char *closing;
    char *number_end = NULL;
    unsigned long long actual;
    MetadataCountSpan table = {0};

    CHECK(snprintf(count_pattern, sizeof(count_pattern), ".%s = ",
                   count_field) > 0);
    CHECK(snprintf(pointer_pattern, sizeof(pointer_pattern), ".%s = ",
                   pointer_field) > 0);
    count_value = metadata_count_span_find(descriptor, count_pattern);
    pointer_value = metadata_count_span_find(descriptor, pointer_pattern);
    if (expected == 0U) {
        CHECK(count_value == NULL);
        CHECK(pointer_value == NULL);
        return table;
    }
    CHECK(count_value != NULL && pointer_value != NULL);
    count_value += strlen(count_pattern);
    actual = strtoull(count_value, &number_end, 10);
    CHECK(number_end != count_value && actual == expected);
    pointer_value += strlen(pointer_pattern);
    while (isspace((unsigned char)*pointer_value)) {
        ++pointer_value;
    }
    table_name = pointer_value;
    identifier_end = table_name;
    while (isalnum((unsigned char)*identifier_end) || *identifier_end == '_') {
        ++identifier_end;
    }
    CHECK(identifier_end > table_name);
    CHECK(snprintf(table_pattern, sizeof(table_pattern), "%.*s[] = {",
                   (int)(identifier_end - table_name), table_name) > 0);
    table.begin = strstr(source, table_pattern);
    CHECK(table.begin != NULL);
    opening = strchr(table.begin, '{');
    CHECK(opening != NULL);
    closing = metadata_count_matching_brace(opening);
    CHECK(closing != NULL);
    table.end = closing + 1;
    CHECK(metadata_count_array_entries(table) == expected);
    return table;
}

/** Validate all three metadata tables on one descriptor initializer. */
static void metadata_count_check_descriptor(
    const char *source,
    const char *descriptor_kind,
    const char *descriptor_name,
    size_t expected,
    MetadataCountSpan *union_table,
    MetadataCountSpan *constraint_table) {
    MetadataCountSpan descriptor = metadata_count_descriptor_span(
        source, descriptor_kind, descriptor_name);
    MetadataCountSpan found_union = metadata_count_check_table(
        source, descriptor, "reified_union_projections_count",
        "reified_union_projections", expected);
    (void)metadata_count_check_table(
        source, descriptor, "reified_spec_view_coercions_count",
        "reified_spec_view_coercions", expected);
    MetadataCountSpan found_constraint = metadata_count_check_table(
        source, descriptor,
        "reified_constraint_projection_descriptors_count",
        "reified_constraint_projection_descriptors", expected);

    if (union_table != NULL) {
        *union_table = found_union;
    }
    if (constraint_table != NULL) {
        *constraint_table = found_constraint;
    }
}

/** Verify two preserved slots reuse one closed constraint descriptor record. */
static void metadata_count_check_duplicate_constraint_slots(
    MetadataCountSpan table) {
    const char *opening = strchr(table.begin, '{');
    const char *first;
    const char *first_end;
    const char *second;
    const char *second_end;

    CHECK(opening != NULL);
    first = opening + 1;
    while (isspace((unsigned char)*first)) {
        ++first;
    }
    first_end = strchr(first, ',');
    CHECK(first_end != NULL && first_end < table.end);
    second = first_end + 1;
    while (isspace((unsigned char)*second)) {
        ++second;
    }
    second_end = strchr(second, ',');
    CHECK(second_end != NULL && second_end < table.end);
    while (first_end > first && isspace((unsigned char)first_end[-1])) {
        --first_end;
    }
    while (second_end > second && isspace((unsigned char)second_end[-1])) {
        --second_end;
    }
    CHECK((size_t)(first_end - first) == (size_t)(second_end - second));
    CHECK(memcmp(first, second, (size_t)(first_end - first)) == 0);
}

/** Check every descriptor context and ensure count metadata has no reader. */
static void metadata_count_check_generated(const char *source) {
    MetadataCountSpan union_table;
    MetadataCountSpan constraint_table;
    const char *forbidden_reads[] = {
        "->reified_union_projections_count",
        "->reified_spec_view_coercions_count",
        "->reified_constraint_projection_descriptors_count"
    };

    metadata_count_check_descriptor(source, "FengFunctionDescriptor",
                                    "empty", 0U, NULL, NULL);
    metadata_count_check_descriptor(source, "FengFunctionDescriptor",
                                    "relay", 0U, NULL, NULL);
    metadata_count_check_descriptor(source, "FengFunctionDescriptor",
                                    "single", 1U, NULL, NULL);
    metadata_count_check_descriptor(source, "FengFunctionDescriptor",
                                    "method", 1U, NULL, NULL);
    metadata_count_check_descriptor(source, "FengFunctionDescriptor",
                                    "fitted", 1U, NULL, NULL);
    metadata_count_check_descriptor(source, "FengFunctionDescriptor",
                                    "combined", 2U, &union_table,
                                    &constraint_table);
    CHECK(metadata_count_span_find(union_table, ".possible = false") != NULL);
    metadata_count_check_duplicate_constraint_slots(constraint_table);

    metadata_count_check_descriptor(
        source, "FengTypeDescriptor", "metadata.counts.EmptyOwner<string>",
        0U, NULL, NULL);
    metadata_count_check_descriptor(
        source, "FengTypeDescriptor",
        "metadata.counts.SingleOwner<string, i32, Item>",
        1U, NULL, NULL);
    metadata_count_check_descriptor(
        source, "FengTypeDescriptor",
        "metadata.counts.RefOwner<string, i32, i32, Item, Item>",
        2U, &union_table, &constraint_table);
    CHECK(metadata_count_span_find(union_table, ".possible = false") != NULL);
    metadata_count_check_duplicate_constraint_slots(constraint_table);

    metadata_count_check_descriptor(
        source, "FengAggregateDescriptor",
        "metadata.counts.EmptyAggregate<i32>", 0U, NULL, NULL);
    metadata_count_check_descriptor(
        source, "FengAggregateDescriptor",
        "metadata.counts.SingleAggregate<string, i32, Item>",
        1U, NULL, NULL);
    metadata_count_check_descriptor(
        source, "FengAggregateDescriptor",
        "metadata.counts.AggregateOwner<string, i32, i32, Item, Item>",
        2U, &union_table, &constraint_table);
    CHECK(metadata_count_span_find(union_table, ".possible = false") != NULL);
    metadata_count_check_duplicate_constraint_slots(constraint_table);

    for (size_t index = 0U;
         index < sizeof(forbidden_reads) / sizeof(forbidden_reads[0]);
         ++index) {
        CHECK(strstr(source, forbidden_reads[index]) == NULL);
    }
}

/** Parse and analyze an independently owned source program. */
static FengSemanticAnalysis *metadata_count_analyze(
    const char *source,
    const FengSemanticImportedModuleQuery *query,
    FengProgram **out_program) {
    FengParseError parse_error = {0};
    FengSemanticAnalyzeOptions options = {
        .target = FENG_COMPILE_TARGET_LIB,
        .pointer_size = feng_get_host_pointer_size(),
        .imported_modules = query
    };
    FengSemanticAnalysis *analysis = NULL;
    FengSemanticError *errors = NULL;
    size_t error_count = 0U;
    bool ok;

    ok = feng_parse_source(source, strlen(source),
                           "reified_metadata_counts.ff",
                           out_program, &parse_error);
    if (!ok) {
        fprintf(stderr, "%s %s\n%s", parse_error.code,
                parse_error.message, source);
    }
    CHECK(ok);
    {
        const FengProgram *programs[] = {*out_program};

        ok = feng_semantic_analyze_with_options(
            programs, 1U, &options, &analysis, &errors, &error_count);
    }
    if (!ok) {
        for (size_t index = 0U; index < error_count; ++index) {
            fprintf(stderr, "%s %s\n%s", errors[index].code,
                    errors[index].message, source);
        }
    }
    CHECK(ok && error_count == 0U);
    feng_semantic_errors_free(errors, error_count);
    return analysis;
}

/** Emit, structurally validate, and host-compile one generated translation unit. */
static void metadata_count_emit(
    FengSemanticAnalysis *analysis,
    void (*compile_c)(const char *)) {
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    bool ok = feng_codegen_emit_program(
        analysis, FENG_COMPILE_TARGET_LIB, NULL, &output, &error);

    if (!ok) {
        fprintf(stderr, "%s %s\n", error.code, error.message);
    }
    CHECK(ok && output.c_source != NULL);
    metadata_count_check_generated(output.c_source);
    compile_c(output.c_source);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
}

/** Confirm omitted fields retain the C zero-initializer contract. */
static void metadata_count_check_zero_values(void) {
    const FengTypeDescriptor type_descriptor = {0};
    const FengAggregateDescriptor aggregate_descriptor = {0};
    const FengFunctionDescriptor function_descriptor = {0};

    CHECK(type_descriptor.reified_union_projections_count == 0U);
    CHECK(type_descriptor.reified_union_projections == NULL);
    CHECK(type_descriptor.reified_spec_view_coercions_count == 0U);
    CHECK(type_descriptor.reified_spec_view_coercions == NULL);
    CHECK(type_descriptor.reified_constraint_projection_descriptors_count == 0U);
    CHECK(type_descriptor.reified_constraint_projection_descriptors == NULL);
    CHECK(aggregate_descriptor.reified_union_projections_count == 0U);
    CHECK(aggregate_descriptor.reified_union_projections == NULL);
    CHECK(aggregate_descriptor.reified_spec_view_coercions_count == 0U);
    CHECK(aggregate_descriptor.reified_spec_view_coercions == NULL);
    CHECK(aggregate_descriptor.reified_constraint_projection_descriptors_count == 0U);
    CHECK(aggregate_descriptor.reified_constraint_projection_descriptors == NULL);
    CHECK(function_descriptor.reified_union_projections_count == 0U);
    CHECK(function_descriptor.reified_union_projections == NULL);
    CHECK(function_descriptor.reified_spec_view_coercions_count == 0U);
    CHECK(function_descriptor.reified_spec_view_coercions == NULL);
    CHECK(function_descriptor.reified_constraint_projection_descriptors_count == 0U);
    CHECK(function_descriptor.reified_constraint_projection_descriptors == NULL);
}

/** Exercise direct codegen and both persisted-symbol profiles. */
void test_reified_metadata_counts(void (*compile_c)(const char *)) {
    char directory[] = "temp/reified-metadata-counts-XXXXXX";
    char public_root[320];
    char workspace_root[320];
    FengProgram *program = NULL;
    FengSemanticAnalysis *analysis;
    FengSymbolExportOptions export_options;
    FengSymbolError symbol_error = {0};

    metadata_count_check_zero_values();
    CHECK(mkdir("temp", 0755) == 0 || errno == EEXIST);
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(public_root, sizeof(public_root), "%s/public", directory) > 0);
    CHECK(snprintf(workspace_root, sizeof(workspace_root), "%s/workspace",
                   directory) > 0);

    analysis = metadata_count_analyze(kMetadataCountProvider, NULL, &program);
    metadata_count_emit(analysis, compile_c);
    export_options = (FengSymbolExportOptions){
        .public_root = public_root,
        .workspace_root = workspace_root
    };
    CHECK(feng_symbol_export_analysis(analysis, &export_options, &symbol_error));
    feng_semantic_analysis_free(analysis);
    feng_program_free(program);

    for (size_t profile = 0U; profile < 2U; ++profile) {
        FengSymbolProvider *provider = NULL;
        FengSymbolImportedModuleCache *cache;
        FengSemanticImportedModuleQuery query;

        CHECK(feng_symbol_provider_create(&provider, &symbol_error));
        CHECK(feng_symbol_provider_add_ft_root(
            provider,
            profile == 0U ? public_root : workspace_root,
            profile == 0U ? FENG_SYMBOL_PROFILE_PACKAGE_PUBLIC
                          : FENG_SYMBOL_PROFILE_WORKSPACE_CACHE,
            &symbol_error));
        cache = feng_symbol_imported_module_cache_create(provider);
        CHECK(cache != NULL);
        query = feng_symbol_imported_module_cache_as_query(cache);
        program = NULL;
        analysis = metadata_count_analyze(
            kMetadataCountConsumer, &query, &program);
        CHECK(feng_symbol_imported_module_cache_populate_codegen_metadata(
            cache, analysis));
        metadata_count_emit(analysis, compile_c);
        feng_semantic_analysis_free(analysis);
        feng_program_free(program);
        feng_symbol_imported_module_cache_free(cache);
        feng_symbol_provider_free(provider);
    }
    feng_symbol_error_free(&symbol_error);
    puts("reified metadata count descriptor and FT matrices passed");
}
