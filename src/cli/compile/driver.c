#include "cli/compile/driver.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

#include "parser/parser.h"

/* --- small helpers ------------------------------------------------------- */

static bool path_exists(const char *path) {
    if (path == NULL) return false;
    struct stat st;
    return stat(path, &st) == 0;
}

static char *str_dup_n(const char *s, size_t n) {
    char *out = malloc(n + 1U);
    if (out == NULL) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *str_dup_cstr(const char *s) {
    return str_dup_n(s, strlen(s));
}

static char *path_join2(const char *a, const char *b) {
    size_t la = strlen(a);
    size_t lb = strlen(b);
    bool need_sep = (la > 0U && a[la - 1U] != '/');
    char *out = malloc(la + (need_sep ? 1U : 0U) + lb + 1U);
    if (out == NULL) return NULL;
    memcpy(out, a, la);
    size_t cursor = la;
    if (need_sep) out[cursor++] = '/';
    memcpy(out + cursor, b, lb);
    out[cursor + lb] = '\0';
    return out;
}

/* Strip the trailing path component, returning a malloc'd directory copy.
 * If the path has no separator, returns ".". */
static char *path_dirname_dup(const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return str_dup_cstr(".");
    }
    size_t len = (size_t)(slash - path);
    if (len == 0U) {
        return str_dup_cstr("/");
    }
    return str_dup_n(path, len);
}

static void cleanup_empty_ir_dirs(const char *c_path) {
    char *ir_c_dir = path_dirname_dup(c_path);
    if (ir_c_dir == NULL) return;
    char *ir_dir = path_dirname_dup(ir_c_dir);
    if (ir_dir == NULL) {
        free(ir_c_dir);
        return;
    }

    if (rmdir(ir_c_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr,
                "warning: could not remove empty IR directory %s: %s\n",
                ir_c_dir, strerror(errno));
    }
    if (rmdir(ir_dir) != 0 && errno != ENOENT && errno != ENOTEMPTY) {
        fprintf(stderr,
                "warning: could not remove empty IR directory %s: %s\n",
                ir_dir, strerror(errno));
    }

    free(ir_dir);
    free(ir_c_dir);
}

/* --- runtime artefact discovery ----------------------------------------- */

/* Resolve the running executable's absolute path. Returns a malloc'd
 * string on success, or NULL on failure. */
static char *resolve_executable_path(const char *argv0) {
#if defined(__APPLE__)
    uint32_t size = 0U;
    _NSGetExecutablePath(NULL, &size);
    if (size == 0U) return NULL;
    char *raw = malloc(size);
    if (raw == NULL) return NULL;
    if (_NSGetExecutablePath(raw, &size) != 0) {
        free(raw);
        return NULL;
    }
    char *real = realpath(raw, NULL);
    free(raw);
    if (real != NULL) return real;
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1U);
    if (n > 0) {
        buf[n] = '\0';
        char *real = realpath(buf, NULL);
        if (real != NULL) return real;
    }
#endif
    /* Fallback: realpath argv[0] (works when invoked with a path). */
    if (argv0 != NULL && argv0[0] != '\0') {
        char *real = realpath(argv0, NULL);
        if (real != NULL) return real;
    }
    return NULL;
}

/* Walk up from `start_dir`, looking for `<dir>/<rel>`. Returns the first
 * matching `<dir>` (malloc'd) or NULL. Caller frees. */
static char *find_ancestor_with(const char *start_dir, const char *rel) {
    char *cur = realpath(start_dir, NULL);
    if (cur == NULL) {
        cur = str_dup_cstr(start_dir);
        if (cur == NULL) return NULL;
    }
    while (cur != NULL && cur[0] != '\0') {
        char *probe = path_join2(cur, rel);
        if (probe == NULL) {
            free(cur);
            return NULL;
        }
        if (path_exists(probe)) {
            free(probe);
            return cur;
        }
        free(probe);
        if (strcmp(cur, "/") == 0) {
            free(cur);
            return NULL;
        }
        char *parent = path_dirname_dup(cur);
        free(cur);
        cur = parent;
    }
    free(cur);
    return NULL;
}

static char *locate_runtime_lib(const char *program_path) {
    const char *env = getenv("FENG_RUNTIME_LIB");
    if (env != NULL && env[0] != '\0') {
        if (!path_exists(env)) {
            fprintf(stderr,
                    "FENG_RUNTIME_LIB points to %s which does not exist\n",
                    env);
            return NULL;
        }
        return str_dup_cstr(env);
    }
    char *exe = resolve_executable_path(program_path);
    if (exe == NULL) return NULL;
    char *exe_dir = path_dirname_dup(exe);
    free(exe);
    if (exe_dir == NULL) return NULL;
    /* Common layout: <root>/build/bin/feng -> <root>/build/lib/libfeng_runtime.a */
    char *root = find_ancestor_with(exe_dir, "build/lib/libfeng_runtime.a");
    free(exe_dir);
    if (root == NULL) return NULL;
    char *path = path_join2(root, "build/lib/libfeng_runtime.a");
    free(root);
    return path;
}

static char *locate_runtime_include(const char *program_path) {
    const char *env = getenv("FENG_RUNTIME_INCLUDE");
    if (env != NULL && env[0] != '\0') {
        char *probe = path_join2(env, "runtime/feng_runtime.h");
        bool ok = path_exists(probe);
        free(probe);
        if (!ok) {
            fprintf(stderr,
                    "FENG_RUNTIME_INCLUDE=%s does not contain runtime/feng_runtime.h\n",
                    env);
            return NULL;
        }
        return str_dup_cstr(env);
    }
    char *exe = resolve_executable_path(program_path);
    if (exe == NULL) return NULL;
    char *exe_dir = path_dirname_dup(exe);
    free(exe);
    if (exe_dir == NULL) return NULL;
    char *root = find_ancestor_with(exe_dir, "src/runtime/feng_runtime.h");
    free(exe_dir);
    if (root == NULL) return NULL;
    char *path = path_join2(root, "src");
    free(root);
    return path;
}

/* --- @cdecl link library mining ----------------------------------------- */

/* Decode a single string-literal annotation argument. The lexer keeps the
 * surrounding quotes; library names never contain escape sequences in
 * practice, but we still tolerate the basic `\\` and `\"` forms so we
 * never silently corrupt unusual names. */
static char *decode_string_literal(const FengExpr *expr) {
    if (expr == NULL || expr->kind != FENG_EXPR_STRING) return NULL;
    const char *raw = expr->as.string.data;
    size_t rlen = expr->as.string.length;
    if (rlen < 2U || raw[0] != '"' || raw[rlen - 1U] != '"') return NULL;
    char *out = malloc(rlen);
    if (out == NULL) return NULL;
    size_t di = 0U;
    for (size_t i = 1U; i + 1U < rlen; ++i) {
        char ch = raw[i];
        if (ch == '\\' && i + 2U < rlen) {
            char esc = raw[++i];
            switch (esc) {
                case '\\': out[di++] = '\\'; break;
                case '"':  out[di++] = '"';  break;
                default:   out[di++] = esc;  break;
            }
        } else {
            out[di++] = ch;
        }
    }
    out[di] = '\0';
    return out;
}

/* Map a Feng @cdecl library name to a host link token. Returns:
 *   NULL — implicit on POSIX (libc / c), no flag needed.
 *   non-NULL — caller-owned malloc'd `name` (without "lib" prefix) to
 *              be appended after `-l`.
 */
static char *map_library_name(const char *raw) {
    if (raw == NULL || raw[0] == '\0') return NULL;
    const char *name = raw;
    if (strncmp(name, "lib", 3) == 0) name += 3;
    if (strcmp(name, "c") == 0) return NULL; /* libc is implicit */
    if (name[0] == '\0') return NULL;
    return str_dup_cstr(name);
}

static bool string_array_contains(char *const *arr, size_t count, const char *needle) {
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(arr[i], needle) == 0) return true;
    }
    return false;
}

/* Returns 0 on success, -1 on allocation failure. */
static int collect_link_libs(const FengProgram *const *programs,
                             size_t program_count,
                             char ***out_libs,
                             size_t *out_count) {
    *out_libs = NULL;
    *out_count = 0;
    size_t cap = 0;
    char **libs = NULL;
    for (size_t pi = 0; pi < program_count; ++pi) {
        const FengProgram *prog = programs[pi];
        if (prog == NULL) continue;
        for (size_t di = 0; di < prog->declaration_count; ++di) {
            const FengDecl *decl = prog->declarations[di];
            if (decl == NULL || !decl->is_extern) continue;
            if (decl->kind != FENG_DECL_FUNCTION) continue;
            for (size_t ai = 0; ai < decl->annotation_count; ++ai) {
                const FengAnnotation *ann = &decl->annotations[ai];
                if (ann->builtin_kind != FENG_ANNOTATION_CDECL) continue;
                if (ann->arg_count < 1U) continue;
                char *raw = decode_string_literal(ann->args[0]);
                if (raw == NULL) continue;
                char *mapped = map_library_name(raw);
                free(raw);
                if (mapped == NULL) continue;
                if (string_array_contains(libs, *out_count, mapped)) {
                    free(mapped);
                    continue;
                }
                if (*out_count == cap) {
                    size_t new_cap = cap == 0U ? 4U : cap * 2U;
                    char **new_libs = realloc(libs, new_cap * sizeof(*libs));
                    if (new_libs == NULL) {
                        free(mapped);
                        for (size_t k = 0; k < *out_count; ++k) free(libs[k]);
                        free(libs);
                        return -1;
                    }
                    libs = new_libs;
                    cap = new_cap;
                }
                libs[(*out_count)++] = mapped;
            }
        }
    }
    *out_libs = libs;
    return 0;
}

/* --- argv builder & spawn ------------------------------------------------ */

typedef struct ArgVec {
    char **items;
    size_t count;
    size_t cap;
} ArgVec;

static bool argv_push(ArgVec *v, const char *s) {
    if (v->count + 1U >= v->cap) {
        size_t new_cap = v->cap == 0U ? 16U : v->cap * 2U;
        char **next = realloc(v->items, new_cap * sizeof(*next));
        if (next == NULL) return false;
        v->items = next;
        v->cap = new_cap;
    }
    char *dup = str_dup_cstr(s);
    if (dup == NULL) return false;
    v->items[v->count++] = dup;
    v->items[v->count] = NULL;
    return true;
}

static void argv_free(ArgVec *v) {
    if (v->items == NULL) return;
    for (size_t i = 0; i < v->count; ++i) free(v->items[i]);
    free(v->items);
    v->items = NULL;
    v->count = 0;
    v->cap = 0;
}

static int spawn_and_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        fprintf(stderr, "exec %s failed: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            fprintf(stderr, "waitpid failed: %s\n", strerror(errno));
            return -1;
        }
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        fprintf(stderr, "%s terminated by signal %d\n", argv[0], WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return -1;
}

/* --- entry --------------------------------------------------------------- */

int feng_cli_compile_driver_invoke(const FengCliDriverOptions *opts) {
    if (opts == NULL || opts->c_path == NULL || opts->out_bin_path == NULL) {
        fprintf(stderr, "internal error: driver invoked with NULL options\n");
        return 2;
    }

    char *runtime_lib = locate_runtime_lib(opts->program_path);
    if (runtime_lib == NULL) {
        fprintf(stderr,
                "error: cannot locate libfeng_runtime.a.\n"
                "  set FENG_RUNTIME_LIB=<path-to-libfeng_runtime.a> or run from a\n"
                "  build tree where build/lib/libfeng_runtime.a exists.\n");
        return 1;
    }
    char *include_dir = locate_runtime_include(opts->program_path);
    if (include_dir == NULL) {
        fprintf(stderr,
                "error: cannot locate runtime headers.\n"
                "  set FENG_RUNTIME_INCLUDE=<dir-containing-runtime/feng_runtime.h>\n"
                "  or run from a build tree containing src/runtime/feng_runtime.h.\n");
        free(runtime_lib);
        return 1;
    }

    char **libs = NULL;
    size_t lib_count = 0;
    if (collect_link_libs(opts->programs, opts->program_count, &libs, &lib_count) != 0) {
        fprintf(stderr, "error: out of memory collecting link libraries\n");
        free(runtime_lib);
        free(include_dir);
        return 1;
    }

    const char *cc = getenv("CC");
    if (cc == NULL || cc[0] == '\0') cc = "cc";

    /* Build the cc argv. We keep the flag set aligned with the existing
     * smoke harness for predictable diagnostics across the migration. */
    ArgVec av = {0};
    char *include_flag = NULL;
    bool ok = true;
    if (!argv_push(&av, cc)) { ok = false; goto build_done; }
    if (!argv_push(&av, "-std=c11")) { ok = false; goto build_done; }
    if (!argv_push(&av, "-O2")) { ok = false; goto build_done; }
    if (!argv_push(&av, "-Wall")) { ok = false; goto build_done; }
    if (!argv_push(&av, "-Wextra")) { ok = false; goto build_done; }
    if (!argv_push(&av, "-pedantic")) { ok = false; goto build_done; }
    {
        size_t need = strlen(include_dir) + 3U;
        include_flag = malloc(need);
        if (include_flag == NULL) { ok = false; goto build_done; }
        snprintf(include_flag, need, "-I%s", include_dir);
        if (!argv_push(&av, include_flag)) { ok = false; goto build_done; }
    }
    if (!argv_push(&av, opts->c_path)) { ok = false; goto build_done; }
    if (!argv_push(&av, runtime_lib)) { ok = false; goto build_done; }
    if (!argv_push(&av, "-lpthread")) { ok = false; goto build_done; }
    for (size_t i = 0; i < lib_count; ++i) {
        size_t need = strlen(libs[i]) + 3U;
        char *flag = malloc(need);
        if (flag == NULL) { ok = false; goto build_done; }
        snprintf(flag, need, "-l%s", libs[i]);
        bool pushed = argv_push(&av, flag);
        free(flag);
        if (!pushed) { ok = false; goto build_done; }
    }
    if (!argv_push(&av, "-o")) { ok = false; goto build_done; }
    if (!argv_push(&av, opts->out_bin_path)) { ok = false; goto build_done; }

build_done:
    if (!ok) {
        fprintf(stderr, "error: out of memory building cc argv\n");
        argv_free(&av);
        free(include_flag);
        for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
        free(libs);
        free(runtime_lib);
        free(include_dir);
        return 1;
    }

    int rc = spawn_and_wait(av.items);
    argv_free(&av);
    free(include_flag);
    for (size_t i = 0; i < lib_count; ++i) free(libs[i]);
    free(libs);
    free(runtime_lib);
    free(include_dir);

    if (rc != 0) {
        fprintf(stderr,
                "error: host C compiler failed (exit=%d).\n"
                "  generated C kept at: %s\n",
                rc, opts->c_path);
        return rc;
    }

    /* Success: optionally clean the IR file and collapse the now-empty
     * <out>/ir/c and <out>/ir directories. Non-empty directories are left
     * alone, which keeps future multi-artefact layouts safe. */
    if (!opts->keep_intermediate) {
        bool can_cleanup_dirs = true;
        if (unlink(opts->c_path) != 0 && errno != ENOENT) {
            fprintf(stderr,
                    "warning: could not remove intermediate %s: %s\n",
                    opts->c_path, strerror(errno));
            can_cleanup_dirs = false;
        }
        if (can_cleanup_dirs) {
            cleanup_empty_ir_dirs(opts->c_path);
        }
    }
    return 0;
}
