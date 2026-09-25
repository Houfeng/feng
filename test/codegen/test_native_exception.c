#include "../throw_constraint_helpers.h"
#include "codegen/codegen.h"

#include <sys/stat.h>
#include <unistd.h>

/* Read compiler IR as a NUL-terminated test artifact. */
static char *native_read(const char *path) {
    FILE *file = fopen(path, "rb");
    THROW_CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    THROW_CHECK(size >= 0 && fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)size + 1U);
    THROW_CHECK(text != NULL && fread(text, 1U, (size_t)size, file) == (size_t)size);
    text[size] = '\0';
    THROW_CHECK(fclose(file) == 0);
    return text;
}

/* Locate an LLVM definition instead of confusing a call with the function. */
static char *native_ir_body(const char *ir, const char *name) {
    const char *cursor = ir;
    while ((cursor = strstr(cursor, "define ")) != NULL) {
        const char *line = strchr(cursor, '\n');
        const char *symbol = strstr(cursor, name);
        THROW_CHECK(line != NULL);
        if (symbol != NULL && symbol < line) {
            const char *end = strstr(line, "\n}");
            THROW_CHECK(end != NULL);
            char *body = strndup(line, (size_t)(end - line));
            THROW_CHECK(body != NULL);
            return body;
        }
        cursor = line;
    }
    THROW_CHECK(false);
    return NULL;
}

/* Exercise actual Feng emission, not a hand-written approximation of its EH.
 * Pure arithmetic must retain no protocol condition in native IR. */
void test_native_exception_codegen(void (*compile_c)(const char *)) {
    static const char source[] =
        "open module native.eh;"
        "open spec Action(n:int):int;"
        "func fail(n:int):int{if n>0{throw \"x\";}return n;}"
        "open func clean(n:int):int{let text=\"managed\";"
        "defer{try fail(0) catch{}}return fail(n);}"
        "open func forward(n:int):int{return clean(n);}"
        "open func nested(n:int):int{return try(try clean(n) catch e:int{e;})"
        "catch e:string{n+7;};}"
        "open func indirect(f:Action,n:int):int{return try f(n) catch e:string{n;};}"
        "open func generic<T:throw>(value:T):void{throw value;}"
        "open func typed(n:int):int{try generic<int>(n) catch e:int{return e;}return 0;}"
        "open type Resource{func ~Resource(){try fail(0) catch{}}}"
        "open spec Named{func text():string;}"
        "open spec Choice:Named|int;"
        "open type NamedValue:Named{func text():string{return \"named\";}}"
        "func projected<T:Choice>(v:T):string{"
        "return match v{item:Named{item.text()}else{\"other\"}};}"
        "open func project(v:NamedValue):string{return projected<NamedValue>(v);}"
        "func condition(n:int):bool{return n<5;}"
        "open func loops(n:int):int{var i=0;while condition(i){let text=\"loop\";"
        "if i==n{return i;}if i==2{i+=1;continue;}i+=1;}"
        "for var j=0;condition(j);j+=1{let text=\"for\";if j==n{return j;}"
        "if j==2{continue;}}for let item in [1,2,3]{if item==n{return item;}}return i;}"
        "open func add(n:int):int{return n+1;}"
        "open func destructure(n:int):int{let (a,_,b)=(n,n+1,n+2);return a+b;}";
    ThrowConstraintUnit unit = throw_constraint_analyze(source, NULL, NULL);
    FengCodegenOutput output = {0};
    FengCodegenError error = {0};
    THROW_CHECK(feng_codegen_emit_program(unit.analysis, FENG_COMPILE_TARGET_LIB,
                                          NULL, &output, &error));
    compile_c(output.c_source);
    THROW_CHECK(strstr(output.c_source, "__materialize_") != NULL);
    char directory[] = "temp/native-eh-ir-XXXXXX";
    THROW_CHECK(mkdtemp(directory) != NULL);
    char input[256], result[256], command[2048];
    snprintf(input, sizeof input, "%s/generated.c", directory);
    snprintf(result, sizeof result, "%s/generated.ll", directory);
    FILE *file = fopen(input, "wb");
    THROW_CHECK(file != NULL && fputs(output.c_source, file) >= 0 && fclose(file) == 0);
    for (unsigned optimization = 0U; optimization <= 3U; ++optimization) {
        for (unsigned sanitized = 0U; sanitized < 2U; ++sanitized) {
            snprintf(command, sizeof command,
                "cc " FENG_TEST_C_EH_FLAGS
                "-Isrc -Isrc/runtime -std=gnu11 -fexceptions -O%u -Werror "
                "%s -S -emit-llvm '%s' -o '%s'", optimization,
                sanitized ? "-fsanitize=address,undefined -fno-sanitize-recover=all" : "", input, result);
            THROW_CHECK(system(command) == 0);
            char *ir = native_read(result);
            THROW_CHECK(strstr(ir, "__llvm_c_eh_") == NULL);
            THROW_CHECK(strstr(ir, "feng_register_lsda") == NULL);
            THROW_CHECK(strstr(ir, "feng_empty_function_lsda") == NULL);
            THROW_CHECK(strstr(ir, "invoke ") != NULL);
            THROW_CHECK(strstr(ir, "landingpad ") != NULL);
            THROW_CHECK(strstr(ir, "resume ") != NULL);
            THROW_CHECK(strstr(ir, "personality ptr @__feng_personality_v0") != NULL);
            if (!sanitized) {
                char *body = native_ir_body(ir, "__add__from__");
                THROW_CHECK(strstr(body, "br i1") == NULL);
                free(body);
                body = native_ir_body(ir, "__destructure__from__");
                THROW_CHECK(strstr(body, "br i1") == NULL);
                THROW_CHECK(strstr(body, "@feng_object_new(") == NULL);
                THROW_CHECK(strstr(body, "@feng_array_new(") == NULL);
                free(body);
            }
            if (optimization >= 2U && !sanitized) {
                char *body = native_ir_body(ir, "__clean__from__");
                /* The callee's empty marker is optional. Its throw must be
                 * inlined while the caller retains its resource boundary. */
                THROW_CHECK(strstr(body, "@feng_frame_push(") != NULL);
                THROW_CHECK(strstr(body, "@feng_throw(") != NULL);
                THROW_CHECK(strstr(body, "@feng__native__eh__fail__") == NULL);
                free(body);
                THROW_CHECK(strstr(ir, "optnone") == NULL && strstr(ir, "noinline") == NULL);
            }
            free(ir);
        }
    }
    THROW_CHECK(unlink(input) == 0 && unlink(result) == 0 && rmdir(directory) == 0);
    feng_codegen_output_free(&output);
    feng_codegen_error_free(&error);
    throw_constraint_dispose(&unit);
    puts("native Feng EH: O0/O1/O2/O3, sanitizer IR, real inlining and branch elimination passed");
}
