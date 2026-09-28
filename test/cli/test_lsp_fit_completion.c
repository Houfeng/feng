#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cli/lsp/service.h"
#include "cli/lsp/completion_type.h"
#include "cli/project/common.h"
#include "cli/cli.h"

#define FIT_CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

/* Real service and framed transport, with one owned temporary workspace. */
typedef struct FitTestClient {
    FengLspService *service;
    FILE *output;
    FILE *errors;
    int input;
    unsigned int next_id;
    unsigned int version;
    char *directory;
    char *path;
    char *uri;
} FitTestClient;

/* One independently readable positive/negative completion expectation. */
typedef struct FitCompletionCase {
    const char *name;
    const char *declarations;
    const char *body;
    const char *present;
    const char *absent;
    const char *signature;
} FitCompletionCase;

/* Allocate a formatted message or fixture path without fixed-size limits. */
static char *fit_test_format(const char *format, ...) {
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0U, format, args);
    va_end(args);
    FIT_CHECK(length >= 0);
    char *text = malloc((size_t)length + 1U);
    FIT_CHECK(text != NULL);
    FIT_CHECK(vsnprintf(text, (size_t)length + 1U, format, copy) == length);
    va_end(copy);
    return text;
}

/* Escape source bytes for JSON while preserving ordinary UTF-8 verbatim. */
static char *fit_test_quote(const char *text) {
    size_t length = strlen(text);
    FIT_CHECK(length <= (SIZE_MAX - 3U) / 6U);
    char *quoted = malloc(length * 6U + 3U);
    FIT_CHECK(quoted != NULL);
    char *cursor = quoted;
    *cursor++ = '"';
    for (size_t index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)text[index];
        if (ch == '"' || ch == '\\') { *cursor++ = '\\'; *cursor++ = (char)ch; }
        else if (ch < 0x20U) { (void)sprintf(cursor, "\\u%04x", ch); cursor += 6U; }
        else *cursor++ = (char)ch;
    }
    *cursor++ = '"';
    *cursor = '\0';
    return quoted;
}

/* Submit a payload through the production interaction queue. */
static void fit_test_send(FitTestClient *client, const char *payload) {
    FIT_CHECK(feng_lsp_service_submit_payload(client->service, client->output, payload, strlen(payload), client->errors));
}

/* Read bounded protocol bytes; a stalled service fails instead of hanging. */
static void fit_test_read(FitTestClient *client, char *buffer, size_t length) {
    while (length != 0U) {
        struct pollfd descriptor = {.fd = client->input, .events = POLLIN};
        int ready;
        do { ready = poll(&descriptor, 1U, 20000); } while (ready < 0 && errno == EINTR);
        FIT_CHECK(ready > 0);
        ssize_t count = read(client->input, buffer, length);
        if (count < 0 && errno == EINTR) continue;
        FIT_CHECK(count > 0);
        buffer += count;
        length -= (size_t)count;
    }
}

/* Skip asynchronous notifications and return the exact requested response. */
static char *fit_test_response(FitTestClient *client, unsigned int id) {
    char identity[64];
    FIT_CHECK(snprintf(identity, sizeof(identity), "\"id\":%u,", id) > 0);
    for (;;) {
        char header[256] = {0};
        size_t used = 0U;
        do {
            FIT_CHECK(used + 1U < sizeof(header));
            fit_test_read(client, &header[used++], 1U);
        } while (used < 4U || memcmp(header + used - 4U, "\r\n\r\n", 4U) != 0);
        size_t length = 0U;
        FIT_CHECK(sscanf(header, "Content-Length: %zu", &length) == 1);
        FIT_CHECK(length < 16U * 1024U * 1024U);
        char *response = malloc(length + 1U);
        FIT_CHECK(response != NULL);
        fit_test_read(client, response, length);
        response[length] = '\0';
        if (strstr(response, identity) != NULL) return response;
        free(response);
    }
}

/* Initialize one isolated workspace without changing the process cwd. */
static FitTestClient fit_test_start(void) {
    FitTestClient client = {0};
    char directory[] = "temp/feng_lsp_fit_XXXXXX";
    int sockets[2];
    FIT_CHECK(mkdtemp(directory) != NULL);
    client.directory = realpath(directory, NULL);
    FIT_CHECK(client.directory != NULL);
    client.path = fit_test_format("%s/main.ff", client.directory);
    client.uri = fit_test_format("file://%s", client.path);
    FIT_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    client.output = fdopen(sockets[0], "w");
    client.input = sockets[1];
    client.errors = tmpfile();
    FIT_CHECK(client.output != NULL && client.errors != NULL);
    client.service = feng_lsp_service_create();
    FIT_CHECK(client.service != NULL);
    client.next_id = 2U;
    fit_test_send(&client, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"capabilities\":{}}}");
    char *response = fit_test_response(&client, 1U);
    FIT_CHECK(strstr(response, "\"completionProvider\"") != NULL);
    free(response);
    fit_test_send(&client, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    return client;
}

/* Open or replace the document through the normal versioned edit protocol. */
static void fit_test_source(FitTestClient *client, const char *source) {
    char *quoted = fit_test_quote(source);
    char *payload;
    if (client->version == 0U) {
        FILE *file = fopen(client->path, "w");
        FIT_CHECK(file != NULL);
        FIT_CHECK(fputs(source, file) >= 0 && fclose(file) == 0);
        payload = fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%u,\"languageId\":\"feng\",\"text\":%s}}}", client->uri, ++client->version, quoted);
    } else {
        payload = fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%u},\"contentChanges\":[{\"text\":%s}]}}", client->uri, ++client->version, quoted);
    }
    fit_test_send(client, payload);
    free(payload);
    free(quoted);
}

/* Request completion at a fixture marker; the marker is an ordinary comment. */
static char *fit_test_complete(FitTestClient *client, const char *source) {
    const char *marker = strstr(source, "/*cursor*/");
    FIT_CHECK(marker != NULL);
    unsigned int line = 0U, column = 0U;
    for (const char *cursor = source; cursor != marker; ++cursor) {
        if (*cursor == '\n') { ++line; column = 0U; } else ++column;
    }
    unsigned int id = client->next_id++;
    char *payload = fit_test_format("{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}}}", id, client->uri, line, column);
    fit_test_send(client, payload);
    free(payload);
    return fit_test_response(client, id);
}

/* Extract a complete JSON object while respecting escaped string contents. */
static char *fit_test_item(const char *response, const char *label) {
    char *needle = fit_test_format("{\"label\":\"%s\"", label);
    const char *start = strstr(response, needle);
    free(needle);
    if (start == NULL) return NULL;
    size_t depth = 0U;
    bool quoted = false, escaped = false;
    for (const char *cursor = start; *cursor != '\0'; ++cursor) {
        if (escaped) { escaped = false; continue; }
        if (quoted && *cursor == '\\') { escaped = true; continue; }
        if (*cursor == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (*cursor == '{') ++depth;
        if (*cursor == '}' && --depth == 0U) return strndup(start, (size_t)(cursor - start) + 1U);
    }
    FIT_CHECK(false);
    return NULL;
}

/* Ask the production lazy resolver to consume the actual returned data. */
static char *fit_test_resolve(FitTestClient *client, const char *item) {
    unsigned int id = client->next_id++;
    char *payload = fit_test_format("{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"completionItem/resolve\",\"params\":%s}", id, item);
    fit_test_send(client, payload);
    free(payload);
    return fit_test_response(client, id);
}

/* Wait for an explicitly required published candidate, not a fixed sleep. */
static char *fit_test_wait_complete(FitTestClient *client, const char *source, const char *label) {
    struct timespec start, now;
    FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        char *response = fit_test_complete(client, source);
        char *item = fit_test_item(response, label);
        if (item != NULL) { free(item); return response; }
        FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        if (now.tv_sec - start.tv_sec >= 5) {
            fprintf(stderr, "fit completion publication timeout:\n%s\n%s\n", source, response);
            FIT_CHECK(false);
        }
        free(response);
        struct timespec pause = {.tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
}

/* Wait for a successful semantic snapshot, including imported synthetic ASTs. */
static char *fit_test_wait_semantic_item(FitTestClient *client, const char *source, const char *label) {
    struct timespec start, now;
    FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        char *response = fit_test_complete(client, source);
        char *item = fit_test_item(response, label);
        if (item != NULL && strstr(item, "\"analysis\":") != NULL) { free(response); return item; }
        FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        if (now.tv_sec - start.tv_sec >= 5) {
            fprintf(stderr, "fit semantic publication timeout:\n%s\n%s\n", source, response);
            FIT_CHECK(false);
        }
        free(item);
        free(response);
        struct timespec pause = {.tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
}

/* Write isolated fixture files before asking the real project loader to read. */
static void fit_test_write(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    FIT_CHECK(file != NULL && fputs(text, file) >= 0 && fclose(file) == 0);
}

/* Shutdown joins both service workers before closing their borrowed streams. */
static void fit_test_stop(FitTestClient *client) {
    unsigned int id = client->next_id++;
    char *payload = fit_test_format("{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"shutdown\",\"params\":null}", id);
    fit_test_send(client, payload);
    free(payload);
    free(fit_test_response(client, id));
    feng_lsp_service_free(client->service);
    FIT_CHECK(fclose(client->output) == 0 && fclose(client->errors) == 0 && close(client->input) == 0);
    char *error = NULL;
    FIT_CHECK(feng_cli_project_remove_tree(client->directory, &error));
    free(error);
    free(client->uri);
    free(client->path);
    free(client->directory);
}

/* Check exact candidate labels and instantiated signatures, not mere counts. */
static void fit_test_case(const FitCompletionCase *test) {
    FitTestClient client = fit_test_start();
    char *source = fit_test_format("module fit_completion;\n%s\n%s\n", test->declarations, test->body);
    fit_test_source(&client, source);
    char *response = fit_test_complete(&client, source);
    char *present = test->present != NULL ? fit_test_format("{\"label\":\"%s\"", test->present) : NULL;
    char *absent = test->absent != NULL ? fit_test_format("{\"label\":\"%s\"", test->absent) : NULL;
    if ((present != NULL ? strstr(response, present) == NULL : strstr(response, "\"label\":") != NULL) ||
        (absent != NULL && strstr(response, absent) != NULL) ||
        (test->signature != NULL && strstr(response, test->signature) == NULL)) {
        fprintf(stderr, "fit completion case %s:\n%s\n%s\n", test->name, source, response);
        FIT_CHECK(false);
    }
    FIT_CHECK(present == NULL || strstr(strstr(response, present) + strlen(present), present) == NULL);
    free(present);
    free(absent);
    free(response);
    free(source);
    fit_test_stop(&client);
}

/* All builtin targets use the same receiver/matcher, including aliases. */
static void fit_test_builtin_targets(void) {
    static const char *const types[] = {
        "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "string",
        "int", "uint", "byte"
    };
    for (size_t index = 0U; index < sizeof(types) / sizeof(types[0]); ++index) {
        char *declarations = fit_test_format("fit %s { func marker(): int { return 1; } }", types[index]);
        char *body = fit_test_format("func probe(value: %s) { value./*cursor*/ }", types[index]);
        FitCompletionCase test = {types[index], declarations, body, "marker", NULL, "func marker(): int"};
        fit_test_case(&test);
        free(body);
        free(declarations);
    }
}

/* Source-only coverage proves the fix does not depend on an old good snapshot. */
static void fit_test_source_targets(void) {
    static const char arrays[] =
        "fit T[] { func element(): T { return self[0]; } func count(): int { return 1; } }\n"
        "fit T[!] { func writable(): int { return 1; } }\n"
        "fit int[] { func integers(): int { return 1; } }\n"
        "fit string[] { func strings(): int { return 1; } }\n";
    static const FitCompletionCase cases[] = {
        {"explicit local", arrays, "func probe() { let value: int[] = []; value./*cursor*/ }", "element", "writable", "func element(): int"},
        {"parameter", arrays, "func probe(value: int[]) { value./*cursor*/ }", "integers", "strings", NULL},
        {"writable outer", arrays, "func probe(value: int[!]) { value./*cursor*/ }", "writable", "element", NULL},
        {"nested element", arrays, "func probe(value: string[!][]) { value./*cursor*/ }", "element", "strings", "func element(): string[!]"},
        {"inferred literal", arrays, "func probe() { let value = [1, 2]; value./*cursor*/ }", "count", "strings", NULL},
        {"partial prefix", arrays, "func probe(value: int[]) { value.e/*cursor*/ }", "element", "strings", NULL},
        {"indexed receiver", arrays, "func probe(value: int[][]) { value[0]./*cursor*/ }", "integers", "strings", NULL},
        {"method result", arrays, "func probe(value: int[][]) { value.element()./*cursor*/ }", "integers", "strings", NULL},
        {"call result", arrays, "func values(): int[] { return []; } func probe() { values()./*cursor*/ }", "element", "strings", "func element(): int"},
        {"literal receiver", arrays, "func probe() { [1, 2]./*cursor*/ }", "integers", "strings", NULL},
        {"if result", arrays, "func probe(value: int[], other: int[]) { (if true { value; } else { other; })./*cursor*/ }", "integers", "strings", NULL},
        {"global", arrays, "let value: int[] = []; func probe() { value./*cursor*/ }", "integers", "strings", NULL},
        {"field", "type Holder { let values: int[]; }\nfit T[] { func element(): T { return self[0]; } }", "func probe(holder: Holder) { holder.values./*cursor*/ }", "element", NULL, "func element(): int"},
        {"static field value", "type Holder { static let values: int[] = []; } fit T[] { func marker(): T { return self[0]; } static func factory(): int { return 1; } }", "func probe() { Holder.values./*cursor*/ }", "marker", "factory", "func marker(): int"},
        {"callable parameter", "spec Getter<T>(): T[]; fit T[] { func marker(): T { return self[0]; } }", "func probe(get: Getter<int>) { get()./*cursor*/ }", "marker", NULL, "func marker(): int"},
        {"callable field", "spec Getter<T>(): T[]; type Holder { let get: Getter<string>; } fit T[] { func marker(): T { return self[0]; } }", "func probe(value: Holder) { value.get()./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"nominal", "type Box {} fit Box { func marker(): int { return 1; } }", "func probe(value: Box) { value./*cursor*/ }", "marker", NULL, NULL},
        {"value type", "@value type Box {} fit Box { func marker(): int { return 1; } }", "func probe(value: Box) { value./*cursor*/ }", "marker", NULL, NULL},
        {"generic nominal", "type Box<T> { let item: T; } fit Box<T> { func marker(value: T): T { return value; } }", "func probe(value: Box<int>) { value./*cursor*/ }", "marker", NULL, "func marker(value: int): int"},
        {"own and fit", "type Box { func own(): int { return 1; } } fit Box { func marker(): int { return 1; } }", "func probe(value: Box) { value./*cursor*/ }", "marker", NULL, "func own(): int"},
        {"generic field chain", "type Box<T> { let item: T; } fit T[] { func marker(): T { return self[0]; } }", "func probe(value: Box<int[]>) { value.item./*cursor*/ }", "marker", NULL, "func marker(): int"},
        {"static array", "fit T[] { static func marker(): int { return 1; } func instance(): int { return 1; } }", "func probe() { int[]./*cursor*/ }", "marker", "instance", NULL},
        {"instance filter", "fit T[] { static func marker(): int { return 1; } func instance(): int { return 1; } }", "func probe(value: int[]) { value./*cursor*/ }", "instance", "marker", NULL},
        {"fixed nominal array", "type Box {} type Other {} fit Box[] { func marker(): int { return 1; } } fit Other[] { func wrong(): int { return 1; } }", "func probe(value: Box[]) { value./*cursor*/ }", "marker", "wrong", NULL},
        {"visible T is nominal", "type T {} fit T[] { func marker(): int { return 1; } } fit int[] { func integers(): int { return 1; } }", "func probe(value: int[]) { value./*cursor*/ }", "integers", "marker", NULL},
        {"overloads", "fit T[] { func marker(): int { return 1; } func marker(value: T): T { return value; } }", "func probe(value: int[]) { value./*cursor*/ }", "marker", NULL, "(+1 overloads)"},
        {"nested bindings", "type Pair<A, B> {} fit Pair<A, B> { func marker(values: A[]): B[] { return []; } }", "func probe(value: Pair<int, string>) { value./*cursor*/ }", "marker", NULL, "func marker(values: int[]): string[]"},
        {"tuple", "type Pair(int, string); fit Pair { func marker(): int { return 1; } }", "func probe(value: Pair) { value./*cursor*/ }", "marker", NULL, NULL},
        {"enum", "enum State { Ready, Done } fit State { func marker(): int { return 1; } }", "func probe(value: State) { value./*cursor*/ }", "marker", NULL, NULL},
        {"generic static", "type Box<T> {} fit Box<T> { static func marker(value: T): T { return value; } }", "func probe() { Box<int>./*cursor*/ }", "marker", NULL, "func marker(value: int): int"},
        {"nested generic static", "type Box<T> {} fit Box<T> { static func marker(value: T): T { return value; } }", "func probe() { Box<Box<int>>./*cursor*/ }", "marker", NULL, "func marker(value: Box<int>): Box<int>"},
        {"method parameter identity", "type Box<T> {} fit Box<T> { func marker<U>(value: T, other: U): U { return other; } }", "func probe(value: Box<int>) { value./*cursor*/ }", "marker", NULL, "func marker<U>(value: int, other: U): U"},
        {"method constraint substitution", "spec View<T> { func get(): T; } type Box<T> {} fit Box<T> { func constrained<U: View<T>>(value: U): U { return value; } }", "func probe(value: Box<int>) { value./*cursor*/ }", "constrained", NULL, "func constrained<U: View<int>>(value: U): U"},
        {"explicit method argument", "type Box<T> {} fit Box<T> { func convert<U>(value: U): U { return value; } } fit T[] { func marker(): T { return self[0]; } }", "func probe(value: Box<int>, items: string[]) { value.convert<string[]>(items)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"inferred method argument", "type Box<T> {} fit Box<T> { func convert<U>(value: U): U { return value; } } fit T[] { func marker(): T { return self[0]; } }", "func probe(value: Box<int>, items: string[]) { value.convert(items)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"generic function argument", "func make<T>(items: T[]): T[] { return items; } fit T[] { func marker(): T { return self[0]; } }", "func probe(items: string[]) { make(items)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"function overload result", "func values(n: int): int[] { return []; } func values(n: string): string[] { return []; } fit int[] { func integers(): int { return 1; } } fit string[] { func strings(): int { return 1; } }", "func probe() { values(\"x\")./*cursor*/ }", "strings", "integers", NULL},
        {"qualified function result", "func values(): int[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { fit_completion.values()./*cursor*/ }", "marker", NULL, "func marker(): int"},
        {"unresolved overload result", "func values(n: int): int[] { return []; } func values(n: string): string[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(missing)./*cursor*/ }", NULL, "marker", NULL},
        {"contextual numeric argument", "func values(n: i32): string[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(1)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"contextual empty array", "func values(items: i32[]): string[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values([])./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"contextual method argument", "type Box {} fit Box { func values(n: i32): string[] { return []; } } fit T[] { func marker(): T { return self[0]; } }", "func probe(box: Box) { box.values(1)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"common declared result", "func values(n: i32): string[] { return []; } func values(n: string): string[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(1)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"different declared results", "func values(n: i32): int[] { return []; } func values(n: u32): string[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(1)./*cursor*/ }", NULL, "marker", NULL},
        {"unbound contextual result", "func values<T>(n: i32): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(1)./*cursor*/ }", NULL, "marker", NULL},
        {"explicit contextual result", "func values<T>(n: i32): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values<string>(1)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"fixed argument preserves binding", "func values<T>(item: T, n: i32): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values<string>(\"x\", 1)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"leading fixed argument", "func values<T>(n: i32, item: T): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values<string>(1, \"x\")./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"inferred contextual result", "func values<T>(item: T, n: i32): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(\"x\", 1)./*cursor*/ }", "marker", NULL, "func marker(): string"},
        {"conflicting generic bindings", "func values<T>(item: T, other: T, n: i32): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(1, \"x\", 1)./*cursor*/ }", NULL, "marker", NULL},
        {"overload call selection", "type Box {} fit Box { func convert(value: int): int[] { return []; } func convert(value: string): string[] { return []; } } fit T[] { func marker(): T { return self[0]; } }", "func probe(box: Box) { box.convert(1)./*cursor*/ }", "marker", NULL, "func marker(): int"},
        {"constructor result", "type Box { func Box() {} } fit Box { func marker(): int { return 1; } }", "func probe() { Box()./*cursor*/ }", "marker", NULL, NULL},
        {"self receiver", "type Box {} fit Box { func marker(): int { return 1; } func probe() { self./*cursor*/ } }", "", "marker", NULL, NULL},
        {"cast receiver", arrays, "func probe(items: int[]) { ((int[])items)./*cursor*/ }", "element", "strings", "func element(): int"},
        {"match receiver", arrays, "func probe(items: int[], other: int[], n: int) { (match n { 0 { items; } else { other; } })./*cursor*/ }", "element", "strings", "func element(): int"},
        {"try binding", arrays, "func values(): int[] { return []; } func probe(other: int[]) { let value = try values() catch { other; }; value./*cursor*/ }", "element", "strings", "func element(): int"},
        {"nested scope shadows", arrays, "func probe(value: string[]) { if true { let value: int[] = []; value./*cursor*/ } }", "integers", "strings", NULL},
        {"comment separation", arrays, "func probe(value: int[]) { value /* .fake */ ./*cursor*/ }", "element", "strings", NULL},
        {"generic self", "type Box<T> { func probe() { self./*cursor*/ } } fit Box<T> { func marker(value: T): T { return value; } }", "", "marker", NULL, "func marker(value: T): T"},
        {"captured binding", arrays, "func probe(value: int[]) { let action = () { value./*cursor*/ }; }", "integers", "strings", NULL},
        {"return position", arrays, "func probe(value: int[]): int { return value./*cursor*/ }", "element", "strings", NULL},
        {"argument position", arrays, "func consume(value: int) {} func probe(value: int[]) { consume(value./*cursor*/); }", "element", "strings", NULL},
        {"assignment position", arrays, "func probe(value: int[]) { var n: int = 0; n = value./*cursor*/; }", "element", "strings", NULL},
        {"condition position", arrays, "func probe(value: int[]) { if value./*cursor*/ {} }", "element", "strings", NULL},
        {"constraint view", "spec Shape { func safe(): int; } type Box {} fit Box { func concrete(): int { return 1; } }", "func probe<T: Shape>(value: T) { value./*cursor*/ }", "safe", "concrete", NULL},
        {"spec view", "spec Shape { func safe(): int; } type Box {} fit Box { func concrete(): int { return 1; } }", "func probe(value: Shape) { value./*cursor*/ }", "safe", "concrete", NULL},
        {"same name arity", "type Box {} type Box<T> {} fit Box { func plain(): int { return 1; } } fit Box<T> { func generic(): int { return 1; } }", "func probe(value: Box<int>) { value./*cursor*/ }", "generic", "plain", NULL},
        {"fixed generic array", "type Box<T> {} fit Box<int>[] { func integers(): int { return 1; } } fit Box<string>[] { func strings(): int { return 1; } }", "func probe(value: Box<int>[]) { value./*cursor*/ }", "integers", "strings", NULL},
        {"inferred leading context", "func values<T>(n:i32,item:T):T[]{return [item];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(1,\"x\")./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred array context", "func values<T>(item:T,n:i32[]):T[]{return [item];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(\"x\",[])./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred float context", "func values<T>(item:T,n:f32):T[]{return [item];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(\"x\",1.0)./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"explicit numeric generic context", "func values<T>(item:T):T[]{return [item];} fit T[]{func marker():T{return self[0];}}", "func probe(){values<i32>(1)./*cursor*/}", "marker", NULL, "func marker(): i32"},
        {"typed generic numeric context", "type Box<T>{func values(item:T):T[]{return [item];}} fit T[]{func marker():T{return self[0];}}", "func probe(b:Box<i32>){b.values(1)./*cursor*/}", "marker", NULL, "func marker(): i32"},
        {"inferred method context", "type Box<T>{func values<U>(item:U,n:i32):U[]{return [item];}} fit T[]{func marker():T{return self[0];}}", "func probe(b:Box<int>){b.values(\"x\",1)./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred static context", "type Box<T>{static func values<U>(item:U,n:i32):U[]{return [item];}} fit T[]{func marker():T{return self[0];}}", "func probe(){Box<int>.values(\"x\",1)./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred qualified context", "func values<T>(item:T,n:i32):T[]{return [item];} fit T[]{func marker():T{return self[0];}}", "func probe(){fit_completion.values(\"x\",1)./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred field chain", "type Box<T>{let value:T;} func boxed<T>(v:T,n:i32):Box<T>{return Box<T>{value:v};} fit T[]{func marker():T{return self[0];}}", "func probe(v:string[]){boxed(v,1).value./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred index chain", "func values<T>(v:T,n:i32):T[]{return [v];} fit T[]{func marker():T{return self[0];}}", "func probe(v:string[]){values(v,1)[0]./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred repeated chain", "func values<T>(v:T,n:i32):T[]{return [v];} fit T[]{func marker():T{return self[0];} func again():T[]{return self;}}", "func probe(){values(\"x\",1).again().again()./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"inferred immediate call", "spec Getter<T>():T; func getter<T>(v:T,n:i32):Getter<T>{return (){return v;};} fit T[]{func marker():T{return self[0];}}", "func probe(v:string[]){getter(v,1)()./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"return chain target", "func values<T>(v:T,n:i32):T[]{return [v];} fit T[]{func marker():T{return self[0];}}", "func probe():int{return values(\"x\",1)./*cursor*/}", "marker", NULL, "func marker(): string"},
        {"binding chain target", "func values<T>(v:T,n:i32):T[]{return [v];} fit T[]{func marker():T{return self[0];}}", "func probe(){let n:int=values(\"x\",1)./*cursor*/;}", "marker", NULL, "func marker(): string"},
        {"fixed context before conflict", "func values<T>(n:i32,a:T,b:T):T[]{return [a,b];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(1,\"x\",2)./*cursor*/}", NULL, "marker", NULL},
        {"unbound independent parameter", "func values<T,U>(a:T,n:i32):U[]{return [];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(\"x\",1)./*cursor*/}", NULL, "marker", NULL},
        {"generic contextual overload ambiguity", "func values<T>(a:T,n:i32):T[]{return [a];} func values<T>(a:T,n:u32):int[]{return [];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(\"x\",1)./*cursor*/}", NULL, "marker", NULL},
        {"generic exact overload wins", "func values<T>(a:T,n:int):T[]{return [a];} func values<T>(a:T,n:i32):int[]{return [];} fit T[]{func marker():T{return self[0];}}", "func probe(){values(\"x\",1)./*cursor*/}", "marker", NULL, "func marker(): string"},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) fit_test_case(&cases[index]);
}

/* Source dependencies and source-free FB dependencies must expose equivalent
 * fits, substituted signatures, access rules, overloads and resolve docs. */
static void fit_test_package(bool binary) {
    static const char declarations[] =
        "open module completion.package;\n"
        "open type Box<T> { open let item: T; open func own(): int { return 1; } }\n"
        "open type Pair(int, string);\n"
        "@value open type Value {}\n"
        "open spec Getter<T>(): T[];\n"
        "open spec View<T> { func get(): T; }\n"
        "open type Holder { open static let values: int[] = []; open let get: Getter<string>; }\n"
        "open enum State { Ready, Done }\n"
        "open fit T[] {\n"
        " /** Array element documentation. */ open func element(): T { return self[0]; }\n"
        " /** Array overloaded documentation. */ open func element(index: int): T { return self[index]; }\n"
        " seal func hidden(): int { return 0; }\n"
        " open static func factory(): int { return 1; }\n"
        "}\n"
        "open fit T[!] { open func writable(): int { return 1; } }\n"
        "open fit int[] { open func integers(): int { return 1; } }\n"
        "open fit string[] { open func strings(): int { return 1; } }\n"
        "open fit Box<T> { /** Box documentation. */ open func element(value: T): T { return value; }\n"
        " open func constrained<U: View<T>>(value: U): U { return value; }\n"
        " open func contextual(n: i32): string[] { return []; }\n"
        " open func contextualGeneric<U>(n: i32): U[] { return []; } }\n"
        "open fit Pair { open func pair(): int { return 1; } }\n"
        "open fit Value { open func valueMarker(): int { return 1; } }\n"
        "open fit State { open func state(): int { return 1; } }\n"
        "open func values(): int[] { return []; }\n"
        "open func values(n: int): int[] { return []; }\n"
        "open func values(n: string): string[] { return []; }\n"
        "open func echo<T>(items: T[]): T[] { return items; }\n"
        "open func contextual(n: i32): string[] { return []; }\n"
        "open func contextualArray(items: i32[]): string[] { return []; }\n"
        "open func commonResult(n: i32): string[] { return []; }\n"
        "open func commonResult(n: string): string[] { return []; }\n"
        "open func differentResults(n: i32): int[] { return []; }\n"
        "open func differentResults(n: u32): string[] { return []; }\n"
        "open func contextualGeneric<U>(n: i32): U[] { return []; }\n"
        "open func contextualPartial<U>(item: U, n: i32): U[] { return []; }\n"
        "open func contextualLeading<U>(n: i32, item: U): U[] { return []; }\n"
        "open func conflicting<U>(item: U, other: U, n: i32): U[] { return []; }\n"
        "open func inferredItems<T>(items: T[]) { return items; }\n"
        "open func inferredArray<T>(item:T) { return [item]; }\n"
        "open func inferredNested<T>(item:T) { return [[item]]; }\n"
        "open fit T[] { open func inferredElement() { return self[0]; } }\n"
        "open fit Box<T> { open func inferredItem() { return self.item; }\n"
        " open func inferredConvert<U>(value: U) { return value; }\n"
        " open static func inferredStatic<U>(value: U) { return value; } }\n"
        "func verifyContextualCalls(box: Box<int>): int {\n"
        " return contextual(1).strings() + contextualArray([]).strings() + commonResult(1).strings()\n"
        "   + contextualGeneric<string>(1).strings() + box.contextual(1).strings()\n"
        "   + box.contextualGeneric<string>(1).strings() + contextualPartial<string>(\"x\", 1).strings()\n"
        "   + contextualLeading<string>(1, \"x\").strings(); }\n";
    static const FitCompletionCase cases[] = {
        {"package array", "", "func probe(value: int[]) { value./*cursor*/ }", "element", "hidden", "(+1 overloads)"},
        {"package fixed array", "", "func probe(value: int[]) { value./*cursor*/ }", "integers", "strings", NULL},
        {"package writable", "", "func probe(value: string[!]) { value./*cursor*/ }", "writable", "element", NULL},
        {"package nested array", "", "func probe(value: string[!][]) { value./*cursor*/ }", "element", "writable", "func element(): string[!]"},
        {"package generic", "", "func probe(value: Box<string[]>) { value./*cursor*/ }", "element", "hidden", "func element(value: string[]): string[]"},
        {"package own", "", "func probe(value: Box<int>) { value./*cursor*/ }", "own", "hidden", NULL},
        {"package constraint substitution", "", "func probe(value: Box<string>) { value./*cursor*/ }", "constrained", "hidden", "View<string>>(value: U): U"},
        {"package tuple", "", "func probe(value: Pair) { value./*cursor*/ }", "pair", "state", NULL},
        {"package value", "", "func probe(value: Value) { value./*cursor*/ }", "valueMarker", "state", NULL},
        {"package enum", "", "func probe(value: State) { value./*cursor*/ }", "state", "pair", NULL},
        {"package enum static", "", "func probe() { State./*cursor*/ }", "Ready", "state", NULL},
        {"package call chain", "", "func probe(value: int[][]) { value.element()./*cursor*/ }", "integers", "strings", NULL},
        {"package function", "", "func probe() { values()./*cursor*/ }", "integers", "strings", NULL},
        {"package static field", "", "func probe() { Holder.values./*cursor*/ }", "integers", "factory", NULL},
        {"package callable", "", "func probe(get: Getter<int>) { get()./*cursor*/ }", "integers", "strings", NULL},
        {"package callable field", "", "func probe(value: Holder) { value.get()./*cursor*/ }", "strings", "integers", NULL},
        {"package function overload", "", "func probe() { values(\"x\")./*cursor*/ }", "strings", "integers", NULL},
        {"package qualified function", "", "func probe() { completion.package.values()./*cursor*/ }", "integers", "strings", NULL},
        {"package alias function", "", "func probe() { api.values(\"x\")./*cursor*/ }", "strings", "integers", NULL},
        {"package generic function", "", "func probe(items: string[]) { api.echo<string>(items)./*cursor*/ }", "strings", "integers", NULL},
        {"package contextual function", "", "func probe() { api.contextual(1)./*cursor*/ }", "strings", "integers", NULL},
        {"package contextual array", "", "func probe() { contextualArray([])./*cursor*/ }", "strings", "integers", NULL},
        {"package contextual method", "", "func probe(box: Box<int>) { box.contextual(1)./*cursor*/ }", "strings", "integers", NULL},
        {"package common result", "", "func probe() { commonResult(1)./*cursor*/ }", "strings", "integers", NULL},
        {"package different results", "", "func probe() { differentResults(1)./*cursor*/ }", NULL, "element", NULL},
        {"package unbound contextual result", "", "func probe() { contextualGeneric(1)./*cursor*/ }", NULL, "element", NULL},
        {"package explicit contextual result", "", "func probe() { contextualGeneric<string>(1)./*cursor*/ }", "strings", "integers", NULL},
        {"package method explicit contextual result", "", "func probe(box: Box<int>) { box.contextualGeneric<string>(1)./*cursor*/ }", "strings", "integers", NULL},
        {"package method unbound contextual result", "", "func probe(box: Box<int>) { box.contextualGeneric(1)./*cursor*/ }", NULL, "element", NULL},
        {"package contextual partial binding", "", "func probe() { contextualPartial<string>(\"x\", 1)./*cursor*/ }", "strings", "integers", NULL},
        {"package contextual leading argument", "", "func probe() { contextualLeading<string>(1, \"x\")./*cursor*/ }", "strings", "integers", NULL},
        {"package inferred contextual result", "", "func probe() { contextualPartial(\"x\", 1)./*cursor*/ }", "strings", "integers", NULL},
        {"package conflicting bindings", "", "func probe() { conflicting(1, \"x\", 1)./*cursor*/ }", NULL, "element", NULL},
        {"package generic field", "", "func probe(value: Box<int[]>) { value.item./*cursor*/ }", "integers", "strings", NULL},
        {"package static", "", "func probe() { int[]./*cursor*/ }", "factory", "element", NULL},
        {"package alias identity", "", "func probe(value: other.Box<int>) { value./*cursor*/ }", "otherMarker", "element", NULL},
        {"package nominal identity", "", "func probe(value: Box<int>) { value./*cursor*/ }", "element", "otherMarker", NULL},
        {"package inferred leading", "", "func probe(){contextualLeading(1,\"x\")./*cursor*/}", "strings", "integers", NULL},
        {"package inferred qualified", "", "func probe(){completion.package.contextualPartial(\"x\",1)./*cursor*/}", "strings", "integers", NULL},
        {"package inferred alias", "", "func probe(){api.contextualPartial(\"x\",1)./*cursor*/}", "strings", "integers", NULL},
        {"package inferred index", "", "func probe(v:string[]){contextualPartial(v,1)[0]./*cursor*/}", "strings", "integers", NULL},
        {"package inferred member", "", "func probe(v:string[]){contextualPartial(v,1).element()./*cursor*/}", "strings", "integers", NULL},
        {"package inferred return position", "", "func probe():int{return contextualPartial(\"x\",1)./*cursor*/}", "strings", "integers", NULL},
        {"package explicit generic fitting", "", "func probe(){contextualPartial<i32>(1,1)./*cursor*/}", "element", "strings", "func element(): i32"},
        {"package receiver target fitting", "", "func probe(b:Box<i32>){b.element(1)./*cursor*/}", "builtinMarker", "strings", NULL},
        {"package inferred receiver field", "", "func probe(v:Box<string[]>){contextualPartial(v,1)[0].item./*cursor*/}", "strings", "integers", NULL},
        {"package inferred return type", "", "func probe(v:string[]){inferredItems(v)./*cursor*/}", "strings", "integers", NULL},
        {"package inferred fit return type", "", "func probe(v:string[][]){v.inferredElement()./*cursor*/}", "strings", "integers", NULL},
        {"package inferred owner return type", "", "func probe(v:Box<string[]>){v.inferredItem()./*cursor*/}", "strings", "integers", NULL},
        {"package inferred method return type", "", "func probe(b:Box<int>,v:string[]){b.inferredConvert(v)./*cursor*/}", "strings", "integers", NULL},
        {"package inferred static return type", "", "func probe(v:string[]){Box<int>.inferredStatic(v)./*cursor*/}", "strings", "integers", NULL},
        {"package inferred composite return", "", "func probe(){inferredArray(\"item\")./*cursor*/}", "strings", "integers", NULL},
        {"package inferred nested composite", "", "func probe(){inferredNested(\"item\")[0]./*cursor*/}", "strings", "integers", NULL},
    };
    FitTestClient client = fit_test_start();
    char *dependency = fit_test_format("%s/dependency", client.directory);
    char *dep_src = fit_test_format("%s/src", dependency);
    char *dep_path = fit_test_format("%s/api.ff", dep_src);
    char *dep_manifest = fit_test_format("%s/feng.fm", dependency);
    char *other_path = fit_test_format("%s/other.ff", dep_src);
    char *consumer = fit_test_format("%s/consumer", client.directory);
    char *src = fit_test_format("%s/src", consumer);
    char *manifest = fit_test_format("%s/feng.fm", consumer);
    FIT_CHECK(mkdir(dependency, 0700) == 0 && mkdir(dep_src, 0700) == 0 && mkdir(consumer, 0700) == 0 && mkdir(src, 0700) == 0);
    fit_test_write(dep_manifest, "[package]\nname: \"fit_dep\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n");
    fit_test_write(dep_path, declarations);
    {
        static const char *const builtins[] = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "string"};
        FILE *file = fopen(dep_path, "a");
        FIT_CHECK(file != NULL);
        for (size_t index = 0U; index < sizeof(builtins) / sizeof(builtins[0]); ++index)
            FIT_CHECK(fprintf(file, "open fit %s { open func builtinMarker(): int { return 1; } }\n", builtins[index]) > 0);
        FIT_CHECK(fclose(file) == 0);
    }
    fit_test_write(other_path, "open module completion.other; open type Box<T> {} open fit Box<T> { open func otherMarker(): int { return 1; } }\n");
    /* Packing also subjects the declaration matrix to the real compiler. */
    char *argv[] = {dependency};
    FIT_CHECK(feng_cli_project_pack_main("feng", 1, argv) == 0);
    if (binary) {
        char *error = NULL;
        FIT_CHECK(feng_cli_project_remove_tree(dep_src, &error));
        free(error);
    }
    char *manifest_text = fit_test_format("[package]\nname: \"fit_consumer\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n[dependencies]\nfit_dep: \"../dependency%s\"\n",
        binary ? "/build/pkg/fit_dep-0.1.0.fb" : "");
    fit_test_write(manifest, manifest_text);
    free(manifest_text);
    free(client.path);
    free(client.uri);
    client.path = fit_test_format("%s/main.ff", src);
    client.uri = fit_test_format("file://%s", client.path);
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const FitCompletionCase *test = &cases[index];
        char *source = fit_test_format("module completion.consumer;\nimport completion.package;\nimport completion.package as api;\nimport completion.other as other;\n%s\n", test->body);
        fit_test_source(&client, source);
        char *response = test->present != NULL ? fit_test_wait_complete(&client, source, test->present)
                                               : fit_test_complete(&client, source);
        char *absent = fit_test_item(response, test->absent);
        if (absent != NULL || (test->present == NULL && strstr(response, "\"label\":") != NULL) ||
            (test->signature != NULL && strstr(response, test->signature) == NULL)) {
            fprintf(stderr, "fit package case %s (%s): %s\n", test->name, binary ? "FT" : "source", response);
            FIT_CHECK(false);
        }
        free(absent);
        FIT_CHECK(strstr(response, "{\"label\":\"T\"") == NULL);
        if (index == 0U) {
            char *item = fit_test_item(response, "element");
            char *resolved = fit_test_resolve(&client, item);
            FIT_CHECK(strstr(resolved, "Array element documentation.") != NULL);
            FIT_CHECK(strstr(resolved, "Array overloaded documentation.") != NULL);
            FIT_CHECK(strstr(resolved, "Box documentation.") == NULL);
            free(resolved);
            free(item);
        }
        free(response);
        free(source);
    }
    {
        static const char *const bodies[] = {
            "func probe<completion>() { completion.package.Box<int>./*cursor*/ }",
            "func probe<other>() { other.Box<int>./*cursor*/ }",
            "func probe(completion: int) { completion.package.Box<int>./*cursor*/ }",
            "func probe(other: int) { other.Box<int>./*cursor*/ }"
        };
        for (size_t index = 0U; index < sizeof(bodies) / sizeof(bodies[0]); ++index) {
            char *source = fit_test_format("module completion.consumer; import completion.package; import completion.other as other; %s", bodies[index]);
            fit_test_source(&client, source);
            char *response = fit_test_complete(&client, source);
            FIT_CHECK(strstr(response, "\"label\":") == NULL);
            free(response); free(source);
        }
    }
    {
        static const char *const builtins[] = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "string", "int", "uint", "byte"};
        for (size_t index = 0U; index < sizeof(builtins) / sizeof(builtins[0]); ++index) {
            char *source = fit_test_format("module completion.consumer; import completion.package; func probe(value: %s) { value./*cursor*/ }", builtins[index]);
            fit_test_source(&client, source);
            char *response = fit_test_wait_complete(&client, source, "builtinMarker");
            FIT_CHECK(strstr(response, "\"label\":\"element\"") == NULL);
            free(response); free(source);
        }
    }
    /* A complete call also exercises Semantic's imported AST representation;
     * it must retain exact overload identities even when FT coordinates are 0. */
    const char *complete = "module completion.consumer; import completion.package; func probe(value: int[]): int { return value./*cursor*/element(); }";
    fit_test_source(&client, complete);
    char *semantic_item = fit_test_wait_semantic_item(&client, complete, "element");
    char *semantic_doc = fit_test_resolve(&client, semantic_item);
    FIT_CHECK(strstr(semantic_doc, "Array element documentation.") != NULL);
    FIT_CHECK(strstr(semantic_doc, "Array overloaded documentation.") != NULL);
    FIT_CHECK(strstr(semantic_doc, "Box documentation.") == NULL);
    free(semantic_doc);
    /* Published package metadata must not bypass the current import set. */
    const char *without_import = "module completion.consumer; func probe(items: int[]) { items./*cursor*/ }";
    fit_test_source(&client, without_import);
    char *without_response = fit_test_complete(&client, without_import);
    FIT_CHECK(strstr(without_response, "\"label\":") == NULL);
    free(without_response);
    semantic_doc = fit_test_resolve(&client, semantic_item);
    FIT_CHECK(strstr(semantic_doc, "documentation") == NULL);
    free(semantic_doc);
    free(semantic_item);
    fit_test_stop(&client);
    free(dependency); free(dep_src); free(dep_path); free(dep_manifest);
    free(other_path);
    free(consumer); free(src); free(manifest);
}

/* Versioned edits invalidate old items and never retain the previous receiver. */
static void fit_test_edit_and_resolve(void) {
    static const char *const sources[] = {
        "module completion.edit; fit T[] { /** First docs. */ func marker(): T { return self[0]; } } fit string[] { func strings(): int { return 1; } } func probe(value: int[]) { value./*cursor*/ }",
        "module completion.edit; fit T[] { /** Second docs. */ func marker(): T { return self[0]; } } fit string[] { func strings(): int { return 1; } } func probe(value: string[]) { value./*cursor*/ }"
    };
    FitTestClient client = fit_test_start();
    fit_test_source(&client, sources[0]);
    char *response = fit_test_complete(&client, sources[0]);
    char *item = fit_test_item(response, "marker");
    FIT_CHECK(item != NULL);
    char *resolved = fit_test_resolve(&client, item);
    FIT_CHECK(strstr(resolved, "First docs.") != NULL);
    FIT_CHECK(strstr(resolved, "func marker(): int") != NULL);
    free(resolved); free(response);
    fit_test_source(&client, sources[1]);
    resolved = fit_test_resolve(&client, item);
    FIT_CHECK(strstr(resolved, "documentation") == NULL);
    free(resolved);
    response = fit_test_complete(&client, sources[1]);
    FIT_CHECK(strstr(response, "func marker(): string") != NULL);
    FIT_CHECK(strstr(response, "\"label\":\"strings\"") != NULL);
    free(response);
    char *closed = fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}", client.uri);
    fit_test_send(&client, closed);
    free(closed);
    client.version = 0U;
    fit_test_source(&client, sources[0]);
    /* The reopened document has version 1 again, but a different epoch. */
    resolved = fit_test_resolve(&client, item);
    FIT_CHECK(strstr(resolved, "documentation") == NULL);
    free(resolved); free(item);
    response = fit_test_complete(&client, sources[0]);
    FIT_CHECK(strstr(response, "func marker(): int") != NULL);
    FIT_CHECK(strstr(response, "\"label\":\"strings\"") == NULL);
    free(response);
    fit_test_stop(&client);
}

/* Request-owned matching storage releases partial results even on overflow. */
static void fit_test_structural_boundaries(void) {
    FengLspCompletionTypes query = {0};
    FengLspCompletionType integer = {.ref.kind = FENG_TYPE_REF_NAMED, .builtin = {"i64", 3U}};
    FengLspCompletionType parameter = {.parameter = &integer};
    FengLspCompletionType array = {.ref.kind = FENG_TYPE_REF_ARRAY, .ref.as.inner = &integer.ref};
    FengLspCompletionBinding *bindings = NULL;
    FIT_CHECK(feng_lsp_completion_type_match(&query, &parameter, &array, &bindings));
    FIT_CHECK(!feng_lsp_completion_type_match(&query, &parameter, &integer, &bindings));
    FIT_CHECK(feng_lsp_completion_type_substitute(&query, &parameter, bindings) == &array);
    FIT_CHECK(feng_lsp_completion_allocate(&query, SIZE_MAX, 2U) == NULL && query.failed);
    feng_lsp_completion_types_dispose(&query);
    FIT_CHECK(query.allocations == NULL);
    char type[3U + 96U * 2U + 1U] = "int";
    for (size_t index = 0U; index < 96U; ++index) strcat(type, "[]");
    char *body = fit_test_format("func probe(value: %s) { value./*cursor*/ }", type);
    FitCompletionCase test = {"deep array", "fit T[] { func marker(): int { return 1; } }", body, "marker", NULL, NULL};
    fit_test_case(&test);
    free(body);
}

/* Complete source makes authoritative effective return facts available. The
 * query waits for the fact itself, not for a timer or the first nonempty list. */
static void fit_test_effective_returns(void) {
    static const struct { const char *source; const char *signature; } cases[] = {
        {"module completion.facts; fit T[] { func element() { return self[0]; } } func probe(value: int[]) { value./*cursor*/element(); }", "func element(): i64"},
        {"module completion.facts; type Box {} fit T[] { func element() { return Box {}; } } func probe(value: int[]) { value./*cursor*/element(); }", "func element(): Box"},
        {"module completion.facts; fit T[] { func element(): T { return self[0]; } } func values() { let result: int[] = []; return result; } func probe() { values()./*cursor*/element(); }", "func element(): i64"},
        {"module completion.facts; func values<T>(v:T,n:i32):T[]{return [v];} fit T[]{func element(){return self[0];}} func probe():string{return values(\"x\",1)./*cursor*/element();}", "func element(): string"},
        {"module completion.facts; type Box<T>{let value:T;func get(){return self.value;}} fit T[]{func element(){return self[0];}} func probe(b:Box<string[]>):string{return b.get()./*cursor*/element();}", "func element(): string"},
        {"module completion.facts; func values<T>(v:T){return [v];} fit T[]{func element(){return self[0];}} func probe():string{return values(\"x\")./*cursor*/element();}", "func element(): string"},
        {"module completion.facts; func before(){return after();} func after(){let v:string[]=[\"x\"];return v;} fit T[]{func element(){return self[0];}} func probe():string{return before()./*cursor*/element();}", "func element(): string"},
        {"module completion.facts; func empty<T>():T[]{return [];} func take(v:string[]):string[]{return v;} fit T[]{func element(){return self[0];}} func probe():string{return take(empty())./*cursor*/element();}", "func element(): string"},
        {"module completion.facts; func empty<T>(){let v:T;return v;} func identity<T>(v:T):T{return v;} func take(v:string[]):string[]{return v;} fit T[]{func element(){return self[0];}} func probe():string{return take(identity(empty()))./*cursor*/element();}", "func element(): string"},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        FitTestClient client = fit_test_start();
        fit_test_source(&client, cases[index].source);
        struct timespec start, now;
        FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        for (;;) {
            char *response = fit_test_complete(&client, cases[index].source);
            bool ready = strstr(response, cases[index].signature) != NULL;
            FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
            if (!ready && now.tv_sec - start.tv_sec >= 5) {
                fprintf(stderr, "fit effective return case %zu: %s\n", index, response);
                FIT_CHECK(false);
            }
            free(response);
            if (ready) break;
            struct timespec pause = {.tv_nsec = 10000000L};
            nanosleep(&pause, NULL);
        }
        fit_test_stop(&client);
    }
}

/* Many preceding declarations must not change lexical scope or candidates;
 * quoted/comment braces remain tokens rather than structural delimiters. */
static void fit_test_many_declarations(void) {
    char *declarations = NULL;
    size_t length = 0U;
    FILE *stream = open_memstream(&declarations, &length);
    FIT_CHECK(stream != NULL);
    for (size_t index = 0U; index < 1000U; ++index)
        FIT_CHECK(fprintf(stream, "type Item%zu {} fit Item%zu { func unrelated(): string { return \"}{\"; } }\n", index, index) > 0);
    FIT_CHECK(fputs("fit T[] { /* } { */ func marker(): T { return self[0]; } }\n", stream) >= 0);
    FIT_CHECK(fclose(stream) == 0 && length != 0U);
    FitCompletionCase test = {"many declarations", declarations,
        "func probe(items: int[]) { items./*cursor*/ }", "marker", "unrelated", "func marker(): int"};
    fit_test_case(&test);
    free(declarations);
}

/* Expected overloads and parameter position for one real Signature Help request. */
typedef struct FitSignatureCase {
    const char *name;
    const char *body;
    const char *labels[3];
    size_t parameter;
} FitSignatureCase;

/* Send a marked signature request, including optional client selection context. */
static char *fit_test_signature(FitTestClient *client, const char *source, const char *context) {
    const char *cursor = strstr(source, "/*cursor*/");
    FIT_CHECK(cursor != NULL);
    unsigned int line = 0U, column = 0U;
    for (const char *p = source; p < cursor; ++p) {
        if (*p == '\n') { ++line; column = 0U; } else ++column;
    }
    unsigned int id = client->next_id++;
    char *request = fit_test_format("{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"textDocument/signatureHelp\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}%s}}",
        id, client->uri, line, column, context != NULL ? context : "");
    fit_test_send(client, request);
    free(request);
    return fit_test_response(client, id);
}

/* Check exact signatures in declaration order, without duplicate overloads. */
static void fit_test_signature_response(const FitSignatureCase *test, const char *response) {
    size_t count = 0U, expected = 0U;
    const char *cursor = response;
    while ((cursor = strstr(cursor, "\"parameters\":[")) != NULL) { ++count; ++cursor; }
    cursor = response;
    while (expected < 3U && test->labels[expected] != NULL) {
        char *quoted = fit_test_quote(test->labels[expected]);
        char *needle = fit_test_format("{\"label\":%s,\"parameters\":[", quoted);
        const char *found = strstr(cursor, needle);
        if (found == NULL) fprintf(stderr, "signature case %s: expected %s\n%s\n", test->name, needle, response);
        FIT_CHECK(found != NULL);
        cursor = found + strlen(needle);
        free(needle); free(quoted);
        ++expected;
    }
    if (count != expected) fprintf(stderr, "signature case %s: expected %zu overloads, got %zu\n%s\n", test->name, expected, count, response);
    FIT_CHECK(count == expected);
    if (expected == 0U) FIT_CHECK(strstr(response, "\"result\":null") != NULL);
    else {
        char *selection = fit_test_format("],\"activeSignature\":0,\"activeParameter\":%zu}", test->parameter);
        if (strstr(response, selection) == NULL) fprintf(stderr, "signature parameter case %s: %s\n", test->name, response);
        FIT_CHECK(strstr(response, selection) != NULL);
        free(selection);
    }
}

/* One legal provider is compiled unchanged for source and source-free FT cases. */
static const char fit_signature_declarations[] =
    "open module signature.api;\n"
    "open spec Getter<T>(): T[];\n"
    "open spec View<T> { func accept(value: T): T; }\n"
    "open type Box<T> { open let item: T; open func own(value: T): T { return value; } }\n"
    "open type Pair(i32, string); @value open type Value {} open enum State { Ready, Done }\n"
    "open type Holder { open static let values: i32[] = []; open let get: Getter<string>; }\n"
    "open fit T[] {\n"
    " open func copy(): T[] { return self; }\n"
    " open func copy(start: i32, end: i32): T[] { return self; }\n"
    " open func copy(start: i32): T[] { return self; }\n"
    " open func first(): T { return self[0]; }\n"
    " open func convert<U>(value: U): U { return value; }\n"
    " open func spread(head: T, rest: T...): T { return head; }\n"
    " open static func factory(value: T): T { return value; }\n"
    " seal func hidden(): i32 { return 0; }\n"
    "}\n"
    "open fit T[!] { open func copy(): T[!] { return self; } }\n"
    "open fit Box<T> { open func take(value: T): T { return value; }\n"
    " open func convert<U>(value: T, other: U): U { return other; }\n"
    " open func constrained<U: signature.api.View<T>>(value: U): U { return value; }\n"
    " open static func factory(value: T): T { return value; } }\n"
    "open fit Pair { open func take(value: i32): i32 { return value; } }\n"
    "open fit Value { open func take(value: i32): i32 { return value; } }\n"
    "open fit State { open func take(value: i32): i32 { return value; } }\n"
    "open func values<T>(value: T, n: i32): T[] { return [value]; }\n"
    "open func echo<T>(value: T): T { return value; }\n"
    "open func choose(n: i64): i32[] { return []; }\n"
    "open func choose(n: string): string[] { return []; }\n"
    "open func chooseUnknown(n: i32): i32[] { return []; }\n"
    "open func chooseUnknown(n: u32): string[] { return []; }\n"
    "open func function(value: i32): i32 { return value; }\n"
    "open func function(value: string): string { return value; }\n";

/* Cover structural and generic receivers, overloads and incomplete call syntax. */
static const FitSignatureCase fit_signature_cases[] = {
    {"array overloads", "func probe(value: i32[]) { value.copy(/*cursor*/); }", {"func copy(): i32[]", "func copy(start: i32, end: i32): i32[]", "func copy(start: i32): i32[]"}, 0},
    {"local array", "func probe() { let value: i32[] = []; value.first(/*cursor*/); }", {"func first(): i32"}, 0},
    {"inferred array", "func probe() { let value = [\"x\"]; value.first(/*cursor*/); }", {"func first(): string"}, 0},
    {"writable array", "func probe(value: i32[!]) { value.copy(/*cursor*/); }", {"func copy(): i32[!]"}, 0},
    {"nested array", "func probe(value: string[!][]) { value.first(/*cursor*/); }", {"func first(): string[!]"}, 0},
    {"writability excludes fit", "func probe(value: i32[!]) { value.first(/*cursor*/); }", {NULL}, 0},
    {"generic owner fit", "func probe(value: Box<string[]>) { value.take(/*cursor*/); }", {"func take(value: string[]): string[]"}, 0},
    {"generic owner method", "func probe(value: Box<i32>) { value.own(/*cursor*/); }", {"func own(value: i32): i32"}, 0},
    {"method scope", "func probe(value: Box<i32>) { value.convert(1,/*cursor*/); }", {"func convert<U>(value: i32, other: U): U"}, 1},
    {"method constraints", "func probe(value: Box<i32>) { value.constrained(/*cursor*/); }", {"func constrained<U: signature.api.View<i32>>(value: U): U"}, 0},
    {"explicit method args", "func probe(value: i32[]) { value.convert<string>(/*cursor*/); }", {"func convert<U>(value: U): U"}, 0},
    {"explicit nested method args", "func probe(value: i32[]) { value.convert<Box<string>>(/*cursor*/); }", {"func convert<U>(value: U): U"}, 0},
    {"static array", "func probe() { i32[].factory(/*cursor*/); }", {"func factory(value: i32): i32"}, 0},
    {"static owner", "func probe() { Box<string>.factory(/*cursor*/); }", {"func factory(value: string): string"}, 0},
    {"static instance exclusion", "func probe() { Box<i32>.take(/*cursor*/); }", {NULL}, 0},
    {"instance static exclusion", "func probe(value: Box<i32>) { value.factory(/*cursor*/); }", {NULL}, 0},
    {"tuple", "func probe(value: Pair) { value.take(/*cursor*/); }", {"func take(value: i32): i32"}, 0},
    {"value type", "func probe(value: Value) { value.take(/*cursor*/); }", {"func take(value: i32): i32"}, 0},
    {"enum", "func probe(value: State) { value.take(/*cursor*/); }", {"func take(value: i32): i32"}, 0},
    {"generic field", "func probe(value: Box<string[]>) { value.item.first(/*cursor*/); }", {"func first(): string"}, 0},
    {"static field", "func probe() { Holder.values.first(/*cursor*/); }", {"func first(): i32"}, 0},
    {"index", "func probe(value: string[][]) { value[0].first(/*cursor*/); }", {"func first(): string"}, 0},
    {"method result", "func probe(value: string[][]) { value.first().first(/*cursor*/); }", {"func first(): string"}, 0},
    {"generic function", "func probe() { values(\"x\", 1).first(/*cursor*/); }", {"func first(): string"}, 0},
    {"explicit generic function", "func probe() { values<string>(\"x\", 1).first(/*cursor*/); }", {"func first(): string"}, 0},
    {"inferred method result", "func probe(value: i32[], other: string[]) { value.convert(other).first(/*cursor*/); }", {"func first(): string"}, 0},
    {"explicit method result", "func probe(value: i32[], other: string[]) { value.convert<string[]>(other).first(/*cursor*/); }", {"func first(): string"}, 0},
    {"callable parameter result", "func probe(get: Getter<string>) { get().first(/*cursor*/); }", {"func first(): string"}, 0},
    {"callable field result", "func probe(value: Holder) { value.get().first(/*cursor*/); }", {"func first(): string"}, 0},
    {"overload result", "func probe() { choose(\"x\").first(/*cursor*/); }", {"func first(): string"}, 0},
    {"ambiguous result", "func probe() { chooseUnknown(1).first(/*cursor*/); }", {NULL}, 0},
    {"literal", "func probe() { [\"a\", \"b\"].first(/*cursor*/); }", {"func first(): string"}, 0},
    {"if receiver", "func probe(value: i32[]) { (if true { value; } else { value; }).first(/*cursor*/); }", {"func first(): i32"}, 0},
    {"match receiver", "func probe(value: i32[], n: i32) { (match n { 0 { value; } else { value; } }).first(/*cursor*/); }", {"func first(): i32"}, 0},
    {"cast receiver", "func probe(value: i32[]) { ((i32[])value).first(/*cursor*/); }", {"func first(): i32"}, 0},
    {"constraint", "func probe<T: View<i32>>(value: T) { value.accept(/*cursor*/); }", {"func accept(value: i32): i32"}, 0},
    {"spec view", "func probe(value: View<string>) { value.accept(/*cursor*/); }", {"func accept(value: string): string"}, 0},
    {"open array", "func probe<T>(value: T[]) { value.first(/*cursor*/); }", {"func first(): T"}, 0},
    {"variadic", "func probe(value: string[]) { value.spread(\"x\", \"y\", /*cursor*/); }", {"func spread(head: string, rest: string...): string"}, 1},
    {"nested calls", "func probe(value: Box<i32>) { value.convert(echo(1), /*cursor*/); }", {"func convert<U>(value: i32, other: U): U"}, 1},
    {"nested literals", "func probe(value: i32[]) { value.spread(1, [1, 2][0], /* a,b */ /*cursor*/); }", {"func spread(head: i32, rest: i32...): i32"}, 1},
    {"unclosed call", "func probe(value: i32[]) { value.first(/*cursor*/ }", {"func first(): i32"}, 0},
    {"unclosed nested call", "func probe(value: i32[]) { echo(value.first(/*cursor*/ }", {"func first(): i32"}, 0},
    {"unclosed comma", "func probe(value: Box<i32>) { value.convert(1, /*cursor*/ }", {"func convert<U>(value: i32, other: U): U"}, 1},
    {"comment before call", "func probe(value: i32[]) { value /* . */ . first /* ( */ (/*cursor*/); }", {"func first(): i32"}, 0},
    {"generic argument commas", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"if branch argument", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){let n=if true{value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);}else{0;};}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"if else argument", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){let n=if false{0;}else{value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);};}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"match branch argument", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){let n=match true{true{value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);}else{0;}};}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"match else argument", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){let n=match false{true{0;}else{value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);}};}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"catch argument", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){let n=try echo(1) catch {value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);};}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"lambda argument", "func pair<A,B>(a:A,b:B):A{return a;} func probe(value:Box<i32>){let fn=(){return value.convert(pair<i32,string>(1,\"x\")/*cursor*/, 2);};}", {"func convert<U>(value: i32, other: U): U"}, 0},
    {"comparison comma", "func probe(value:Box<i32>){value.convert(1 < 2, /*cursor*/);}", {"func convert<U>(value: i32, other: U): U"}, 1},
    {"string comma", "func probe(value:Box<i32>){value.convert(1, \"a,b\"/*cursor*/);}", {"func convert<U>(value: i32, other: U): U"}, 1},
    {"self owner", "fit Box<T>{func probe(){self.take(/*cursor*/);}}", {"func take(value: T): T"}, 0},
    {"self array fit", "fit T[]{func probe(){self.first(/*cursor*/);}}", {"func first(): T"}, 0},
    {"global binding", "let global:i32[]=[];func probe(){global.first(/*cursor*/);}", {"func first(): i32"}, 0},
    {"own and fit overload", "fit Box<T>{func own():string{return \"x\";}}func probe(value:Box<i32>){value.own(/*cursor*/);}", {"func own(value: i32): i32", "func own(): string"}, 0},
    {"nested nominal static", "func probe(){Box<Box<i32>>.factory(/*cursor*/);}", {"func factory(value: Box<i32>): Box<i32>"}, 0},
    {"unknown receiver", "func probe() { missing.first(/*cursor*/); }", {NULL}, 0},
};

/* Reuse one protocol matrix across local source and both dependency layouts. */
static void fit_test_signature_matrix(FitTestClient *client, const char *prefix) {
    for (size_t i = 0U; i < sizeof(fit_signature_cases) / sizeof(fit_signature_cases[0]); ++i) {
        const FitSignatureCase *test = &fit_signature_cases[i];
        char *source = fit_test_format("%s\n%s\n", prefix, test->body);
        fit_test_source(client, source);
        char *response = fit_test_signature(client, source, NULL);
        fit_test_signature_response(test, response);
        free(response); free(source);
    }
}

/* Every builtin target and alias must supply its declared callable signature. */
static void fit_test_signature_builtins(void) {
    static const char *const types[] = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "string", "int", "uint", "byte"};
    FitTestClient client = fit_test_start();
    for (size_t i = 0U; i < sizeof(types) / sizeof(types[0]); ++i) {
        char *source = fit_test_format("module signature_builtin; fit %s { func take(value: string): string { return value; } } func probe(value: %s) { value.take(/*cursor*/); }", types[i], types[i]);
        fit_test_source(&client, source);
        char *response = fit_test_signature(&client, source, NULL);
        FitSignatureCase test = {.name = types[i], .labels = {"func take(value: string): string"}};
        fit_test_signature_response(&test, response);
        free(response); free(source);
    }
    fit_test_stop(&client);
}

/* Keep source and source-free packages on the identical member query matrix. */
static void fit_test_signature_package(bool binary) {
    FitTestClient client = fit_test_start();
    char *dependency = fit_test_format("%s/dependency", client.directory);
    char *dep_src = fit_test_format("%s/src", dependency);
    char *dep_path = fit_test_format("%s/api.ff", dep_src);
    char *dep_manifest = fit_test_format("%s/feng.fm", dependency);
    char *consumer = fit_test_format("%s/consumer", client.directory);
    char *src = fit_test_format("%s/src", consumer);
    char *manifest = fit_test_format("%s/feng.fm", consumer);
    FIT_CHECK(mkdir(dependency, 0700) == 0 && mkdir(dep_src, 0700) == 0 && mkdir(consumer, 0700) == 0 && mkdir(src, 0700) == 0);
    fit_test_write(dep_manifest, "[package]\nname: \"signature_dep\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n");
    fit_test_write(dep_path, fit_signature_declarations);
    char *argv[] = {dependency};
    FIT_CHECK(feng_cli_project_pack_main("feng", 1, argv) == 0);
    if (binary) {
        char *error = NULL;
        FIT_CHECK(feng_cli_project_remove_tree(dep_src, &error));
        free(error);
    }
    char *manifest_text = fit_test_format("[package]\nname: \"signature_consumer\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n[dependencies]\nsignature_dep: \"../dependency%s\"\n",
        binary ? "/build/pkg/signature_dep-0.1.0.fb" : "");
    fit_test_write(manifest, manifest_text);
    free(manifest_text);
    free(client.path); free(client.uri);
    client.path = fit_test_format("%s/main.ff", src);
    client.uri = fit_test_format("file://%s", client.path);
    const char *prefix = "module signature.consumer; import signature.api; import signature.api as api;";
    fit_test_signature_matrix(&client, prefix);
    static const FitSignatureCase cases[] = {
        {"private method", "func probe(value: i32[]) { value.hidden(/*cursor*/); }", {NULL}, 0},
        {"alias type", "func probe(value: api.Box<i32>) { value.take(/*cursor*/); }", {"func take(value: i32): i32"}, 0},
        {"alias function chain", "func probe() { api.values(\"x\", 1).first(/*cursor*/); }", {"func first(): string"}, 0},
        {"qualified function chain", "func probe() { signature.api.values(\"x\", 1).first(/*cursor*/); }", {"func first(): string"}, 0},
        {"qualified generic type", "func probe() { signature.api.Box<i32>.factory(/*cursor*/); }", {"func factory(value: i32): i32"}, 0},
        {"local alias shadow", "func probe(api: i32) { api.values(/*cursor*/); }", {NULL}, 0},
        {"generic alias shadow", "func probe<api>() { api.values(/*cursor*/); }", {NULL}, 0},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        char *source = fit_test_format("%s %s", prefix, cases[i].body);
        fit_test_source(&client, source);
        char *response = fit_test_signature(&client, source, NULL);
        fit_test_signature_response(&cases[i], response);
        free(response); free(source);
    }
    char *self_source = fit_test_format("%s fit Box<T>{func probe(value:T){self./*cursor*/take(value);}}", prefix);
    fit_test_source(&client, self_source);
    char *completion = fit_test_complete(&client, self_source);
    FIT_CHECK(strstr(completion, "\"label\":\"take\"") != NULL);
    FIT_CHECK(strstr(completion, "func take(value: T): T") != NULL);
    free(completion); free(self_source);
    fit_test_stop(&client);
    free(dependency); free(dep_src); free(dep_path); free(dep_manifest); free(consumer); free(src); free(manifest);
}

/* Encode a client selection without assuming server overload order is stable. */
static char *fit_test_signature_context(const char *labels[], size_t count, size_t selected, bool retrigger) {
    char *text = NULL;
    size_t length = 0U;
    FILE *stream = open_memstream(&text, &length);
    FIT_CHECK(stream != NULL);
    FIT_CHECK(fprintf(stream, ",\"context\":{\"triggerKind\":3,\"isRetrigger\":%s,\"activeSignatureHelp\":{\"signatures\":[", retrigger ? "true" : "false") > 0);
    for (size_t i = 0U; i < count; ++i) {
        char *quoted = fit_test_quote(labels[i]);
        FIT_CHECK(fprintf(stream, "%s{\"label\":%s}", i == 0U ? "" : ",", quoted) > 0);
        free(quoted);
    }
    FIT_CHECK(fprintf(stream, "],\"activeSignature\":%zu}}", selected) > 0);
    FIT_CHECK(fclose(stream) == 0 && length != 0U);
    return text;
}

/* User choices survive argument edits and reordering, never stale signatures. */
static void fit_test_signature_selection(void) {
    FitTestClient client = fit_test_start();
    const char *labels[] = {"func copy(): i32[]", "func copy(start: i32, end: i32): i32[]", "func copy(start: i32): i32[]"};
    char *source = fit_test_format("%s func probe(value: i32[]) { value.copy(1, /*cursor*/); }", fit_signature_declarations);
    fit_test_source(&client, source);
    for (size_t selected = 0U; selected < 5U; ++selected) {
        char *context = fit_test_signature_context(labels, 3U, selected, true);
        char *response = fit_test_signature(&client, source, context);
        char *needle = fit_test_format("],\"activeSignature\":%zu,\"activeParameter\":%zu}", selected < 3U ? selected : 0U, selected == 1U ? 1U : 0U);
        FIT_CHECK(strstr(response, needle) != NULL);
        FIT_CHECK(strstr(response, "\"parameters\":[],\"activeParameter\":0}") != NULL);
        FIT_CHECK(strstr(response, "\"label\":\"end\"}],\"activeParameter\":1}") != NULL);
        free(needle); free(context); free(response);
    }
    const char *reordered[] = {labels[2], labels[0], labels[1]};
    for (size_t selected = 0U; selected < 3U; ++selected) {
        char *context = fit_test_signature_context(reordered, 3U, selected, true);
        char *response = fit_test_signature(&client, source, context);
        size_t actual = selected == 0U ? 2U : selected - 1U;
        char *needle = fit_test_format("],\"activeSignature\":%zu,\"activeParameter\":%zu}", actual, actual == 1U ? 1U : 0U);
        FIT_CHECK(strstr(response, needle) != NULL);
        free(needle); free(context); free(response);
    }
    const char *stale[] = {"func copy(removed: bool): i32[]"};
    for (size_t i = 0U; i < 3U; ++i) {
        char *context = fit_test_signature_context(i == 0U ? stale : labels, i == 0U ? 1U : 3U, i == 0U ? 0U : 2U, i != 1U);
        if (i == 2U) {
            free(source);
            source = fit_test_format("%s func probe(value: string[]) { value.copy(1, /*cursor*/); }", fit_signature_declarations);
            fit_test_source(&client, source);
        }
        char *response = fit_test_signature(&client, source, context);
        FIT_CHECK(strstr(response, "],\"activeSignature\":0,\"activeParameter\":0}") != NULL);
        free(response); free(context);
    }
    free(source);
    const char *functions[] = {"func function(value: i32): i32", "func function(value: string): string"};
    source = fit_test_format("%s func probe() { function(/*cursor*/); }", fit_signature_declarations);
    fit_test_source(&client, source);
    char *context = fit_test_signature_context(functions, 2U, 1U, true);
    char *response = fit_test_signature(&client, source, context);
    FIT_CHECK(strstr(response, "],\"activeSignature\":1,\"activeParameter\":0}") != NULL);
    free(context); free(response); free(source);
    source = fit_test_format("%s func probe(value:i32[]){value./*cursor*/copy();}", fit_signature_declarations);
    fit_test_source(&client, source);
    free(fit_test_wait_semantic_item(&client, source, "copy"));
    free(source);
    source = fit_test_format("module signature.api; fit T[]{func copy():T[]{return self;}} func probe(value:i32[]){value.copy(/*cursor*/);}");
    fit_test_source(&client, source);
    context = fit_test_signature_context(labels, 3U, 2U, true);
    response = fit_test_signature(&client, source, context);
    FitSignatureCase edited = {.name = "removed overload after publication", .labels = {"func copy(): i32[]"}};
    fit_test_signature_response(&edited, response);
    free(context); free(response);
    static const char *const invalid[] = {
        ",\"context\":{\"isRetrigger\":true,\"activeSignatureHelp\":{\"signatures\":[],\"activeSignature\":0}}",
        ",\"context\":{\"isRetrigger\":true,\"activeSignatureHelp\":{\"signatures\":[{\"label\":\"unused\"}],\"activeSignature\":-1}}",
        ",\"context\":{\"isRetrigger\":true,\"activeSignatureHelp\":{\"signatures\":[{}],\"activeSignature\":0}}",
        ",\"context\":{\"isRetrigger\":\"true\",\"activeSignatureHelp\":{\"signatures\":[]}}"
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        response = fit_test_signature(&client, source, invalid[i]);
        fit_test_signature_response(&edited, response);
        free(response);
    }
    free(source);
    source = fit_test_format("module signature.api; func probe(value:i32[]){value.copy(/*cursor*/);}");
    fit_test_source(&client, source);
    response = fit_test_signature(&client, source, NULL);
    FIT_CHECK(strstr(response, "\"result\":null") != NULL);
    free(response); free(source);
    fit_test_stop(&client);
}

/* New signature coverage shares the existing executable and both test phases. */
static void fit_test_signatures(void) {
    FitTestClient client = fit_test_start();
    fit_test_signature_matrix(&client, fit_signature_declarations);
    fit_test_stop(&client);
    fit_test_signature_builtins();
    fit_test_signature_package(false);
    fit_test_signature_package(true);
    fit_test_signature_selection();
    fprintf(stdout, "lsp signature help: receiver, source/FT, overload and edit matrices passed\n");
}

/* Each completeness case asserts the whole overload set, including negatives. */
typedef struct SignatureCompletenessCase {
    const char *name;
    const char *body;
    const char *labels[6];
    size_t parameter;
} SignatureCompletenessCase;

/* Count UTF-16 units rather than UTF-8 bytes at the protocol boundary. */
static unsigned int signature_test_send_request(FitTestClient *client, const char *source, const char *context) {
    const unsigned char *end = (const unsigned char *)strstr(source, "/*cursor*/");
    FIT_CHECK(end != NULL);
    unsigned int line = 0U, column = 0U;
    for (const unsigned char *p = (const unsigned char *)source; p < end; ++p) {
        if (*p == '\n') { ++line; column = 0U; }
        else if ((*p & 0xc0U) != 0x80U) column += *p >= 0xf0U ? 2U : 1U;
    }
    unsigned int id = client->next_id++;
    char *message = fit_test_format("{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"textDocument/signatureHelp\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},\"position\":{\"line\":%u,\"character\":%u}%s}}",
        id, client->uri, line, column, context != NULL ? context : "");
    fit_test_send(client, message);
    free(message);
    return id;
}

/* Read exactly the signature response belonging to this request. */
static char *signature_test_request(FitTestClient *client, const char *source, const char *context) {
    return fit_test_response(client, signature_test_send_request(client, source, context));
}

/* Preserve exact labels, parameter position, multiplicity and declaration order. */
static void signature_test_expect(const SignatureCompletenessCase *test, const char *response) {
    const char *cursor = response;
    size_t expected = 0U, actual = 0U;
    while ((cursor = strstr(cursor, "\"parameters\":[")) != NULL) { ++actual; ++cursor; }
    cursor = response;
    while (expected < sizeof(test->labels) / sizeof(test->labels[0]) && test->labels[expected] != NULL) {
        char *quoted = fit_test_quote(test->labels[expected]);
        char *needle = fit_test_format("{\"label\":%s,\"parameters\":[", quoted);
        const char *found = strstr(cursor, needle);
        if (found == NULL) fprintf(stderr, "signature completeness %s: missing %s\n%s\n", test->name, needle, response);
        FIT_CHECK(found != NULL);
        cursor = found + strlen(needle);
        free(quoted); free(needle); ++expected;
    }
    if (actual != expected) fprintf(stderr, "signature completeness %s: expected %zu, got %zu\n%s\n", test->name, expected, actual, response);
    FIT_CHECK(actual == expected);
    if (expected == 0U) FIT_CHECK(strstr(response, "\"result\":null") != NULL);
    else {
        char *needle = fit_test_format("],\"activeSignature\":0,\"activeParameter\":%zu}", test->parameter);
        FIT_CHECK(strstr(response, needle) != NULL);
        free(needle);
    }
}

/* The identical legal declarations are used as source and source-hidden FB. */
static const char signature_complete_declarations[] =
    "open module signature.complete;\n"
    "open spec Callback<T>(value: T): T;\n"
    "open spec Many<T>(head: T, rest: T...): T; open spec Zero(): void;\n"
    "open type Holder<T> { let callback: Callback<T>; let callbacks: Callback<T>[]; }\n"
    "open type Point { func Point(value: i32) {} func Point(text: string) {} }\n"
    "open type Default {}\n"
    "open type Empty { func Empty() {} }\n"
    "open type Construct<T> { func Construct(value: T) {} func Construct(value: T, count: i32) {} }\n"
    "open type Hidden { seal func Hidden() {} }\n"
    "open spec Parent<T> { func take(value: T): T; static func build(value: T): T; }\n"
    "open spec Child<T>: Parent<T> {}\n"
    "open spec Leaf<T>: Child<T[]> {}\n"
    "open spec Left<T>: Parent<T> {} open spec Right<T>: Parent<T> {}\n"
    "open spec Diamond<T>: Left<T>, Right<T> {}\n"
    "open spec Repeated<T>: Parent<T> { func take(renamed: T): T; }\n"
    "open spec Tag { func tag(): i32; }\n"
    "open spec Both<T>: Child<T> & Tag; open spec Nested<T>: Both<T> & Tag;\n"
    "open spec Dual: Parent<i32> & Parent<string>;\n"
    "open spec Field<T> { let callback: Callback<T>; } open spec FieldChild<T>: Field<T> {}\n"
    "open spec Secret { seal func hidden(value: i32): i32; }\n"
    "open spec SecretChild: Secret {}\n"
    "open func relay<T>(fn: Callback<T>): Callback<T> { return fn; }\n"
    "open func pair<A,B>(first:A, second:B): A { return first; }\n"
    "open let global: Callback<i32> = (x: i32) -> x;\n"
    "open let globalAlias = global;\n"
    "open fit i32 { func ready(): i32 { return self; } }\n"
    "open func pick(value:i32) {} open func pick(value:i64) {} open func pick(value:string) {}\n"
    "open func pick(value:bool) {} open func pick(value:f64) {}\n";

/* All receiver forms use common type facts; all contract edges substitute args. */
static const SignatureCompletenessCase signature_complete_cases[] = {
    {"field", "func probe(value:Holder<i32>){value.callback(/*cursor*/1);}", {"func callback(value: i32): i32"}, 0U},
    {"generic field", "func probe<T>(value:Holder<T[]>){value.callback(/*cursor*/[]);}", {"func callback(value: T[]): T[]"}, 0U},
    {"array", "func probe(values:Callback<i32>[]){values[0](/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"writable array", "func probe(values:Callback<string>[!]){values[0](/*cursor*/\"x\");}", {"func Callback(value: string): string"}, 0U},
    {"nested field and index", "func probe(value:Holder<string[]>){value.callbacks[0](/*cursor*/[]);}", {"func Callback(value: string[]): string[]"}, 0U},
    {"call result", "func probe(fn:Callback<i32>){relay(fn)(/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"generic call result", "func probe(fn:Callback<i32>){relay<i32>(fn)(/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"nested call result", "func probe(fn:Callback<i32>){relay(relay(fn))(/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"alias", "func probe(fn:Callback<i32>){let alias=fn;alias(/*cursor*/1);}", {"func alias(value: i32): i32"}, 0U},
    {"alias chain", "func probe(fn:Callback<i32>){let one=fn;let two=one;two(/*cursor*/1);}", {"func two(value: i32): i32"}, 0U},
    {"field alias", "func probe(value:Holder<i32>){let alias=value.callback;alias(/*cursor*/1);}", {"func alias(value: i32): i32"}, 0U},
    {"returned alias", "func probe(fn:Callback<i32>){let alias=relay(fn);alias(/*cursor*/1);}", {"func alias(value: i32): i32"}, 0U},
    {"typed local", "func probe(fn:Callback<i32>){let alias:Callback<i32> = fn;alias(/*cursor*/1);}", {"func alias(value: i32): i32"}, 0U},
    {"global", "func probe(){global(/*cursor*/1);}", {"func global(value: i32): i32"}, 0U},
    {"global inferred alias", "func probe(){globalAlias(/*cursor*/1);}", {"func globalAlias(value: i32): i32"}, 0U},
    {"direct local", "func probe(fn:Callback<i32>){fn(/*cursor*/1);}", {"func fn(value: i32): i32"}, 0U},
    {"lambda", "func probe(){let fn=(value:i32)->value;fn(/*cursor*/1);}", {"func fn(value: i32)"}, 0U},
    {"lambda alias", "func probe(){let fn=(value:i32)->value;let alias=fn;alias(/*cursor*/1);}", {"func alias(value: i32)"}, 0U},
    {"zero", "func probe(fn:Zero){fn(/*cursor*/);}", {"func fn(): void"}, 0U},
    {"variadic", "func probe(fn:Many<i32>){fn(1,2,/*cursor*/3);}", {"func fn(head: i32, rest: i32...): i32"}, 1U},
    {"if callable", "func probe(fn:Callback<i32>){(if true {fn;}else{fn;})(/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"match callable", "func probe(fn:Callback<i32>){(match true{true{fn;}else{fn;}})(/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"try callable", "func probe(fn:Callback<i32>){(try relay(fn) catch{fn;})(/*cursor*/1);}", {"func Callback(value: i32): i32"}, 0U},
    {"constructor overloads", "func probe(){let value=Point(/*cursor*/1);}", {"ctor Point(value: i32): void", "ctor Point(text: string): void"}, 0U},
    {"explicit empty constructor", "func probe(){let value=Empty(/*cursor*/);}", {"ctor Empty(): void"}, 0U},
    {"implicit constructor", "func probe(){let value=Default(/*cursor*/);}", {"ctor Default(): void"}, 0U},
    {"generic constructor", "func probe(){let value=Construct<string>(/*cursor*/\"x\");}", {"ctor Construct(value: string): void", "ctor Construct(value: string, count: i32): void"}, 0U},
    {"nested generic constructor", "func probe(){let value=Construct<Callback<i32>>(/*cursor*/global);}", {"ctor Construct(value: Callback<i32>): void", "ctor Construct(value: Callback<i32>, count: i32): void"}, 0U},
    {"constructor literal suffix", "func probe(){let value=Construct<i32>(/*cursor*/1){};}", {"ctor Construct(value: i32): void", "ctor Construct(value: i32, count: i32): void"}, 0U},
    {"parent control", "func probe(value:Parent<i32>){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"child", "func probe(value:Child<i32>){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"two edges", "func probe(value:Leaf<i32>){value.take(/*cursor*/[]);}", {"func take(value: i32[]): i32[]"}, 0U},
    {"diamond", "func probe(value:Diamond<i32>){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"child representative", "func probe(value:Repeated<i32>){value.take(/*cursor*/1);}", {"func take(renamed: i32): i32"}, 0U},
    {"intersection", "func probe(value:Both<i32>){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"nested intersection", "func probe(value:Nested<i32>){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"distinct instances", "func probe(value:Dual){value.take(/*cursor*/1);}", {"func take(value: i32): i32", "func take(value: string): string"}, 0U},
    {"generic child constraint", "func probe<T:Child<i32>>(value:T){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"generic intersection constraint", "func probe<T:Both<i32>>(value:T){value.take(/*cursor*/1);}", {"func take(value: i32): i32"}, 0U},
    {"generic static requirement", "func probe<T:Child<i32>>(){T.build(/*cursor*/1);}", {"func build(value: i32): i32"}, 0U},
    {"inherited callable field", "func probe(value:FieldChild<i32>){value.callback(/*cursor*/1);}", {"func callback(value: i32): i32"}, 0U},
    {"hidden requirement", "func probe(value:SecretChild){value.hidden(/*cursor*/1);}", {NULL}, 0U},
    {"UTF16", "func probe(fn:Callback<i32>){let text=\"中文😀\";fn(/*cursor*/1);}", {"func fn(value: i32): i32"}, 0U},
    {"CRLF variadic", "func probe(fn:Many<i32>){\r\nfn(1,\r\n/*cursor*/2);\r\n}", {"func fn(head: i32, rest: i32...): i32"}, 1U},
    {"generic comma", "func probe(fn:Many<i32>){fn(pair<i32,string>(1,\"x\")/*cursor*/,2);}", {"func fn(head: i32, rest: i32...): i32"}, 0U},
    {"string and comment delimiters", "func probe(fn:Many<string>){fn(\"a,(\",/*,(*/ /*cursor*/\"b\");}", {"func fn(head: string, rest: string...): string"}, 1U},
    {"incomplete call", "func probe(fn:Callback<i32>){let alias=fn;alias(/*cursor*/}", {"func alias(value: i32): i32"}, 0U},
    {"incomplete constructor", "func probe(){Point(/*cursor*/}", {"ctor Point(value: i32): void", "ctor Point(text: string): void"}, 0U},
    {"not callable", "func probe(value:i32){value(/*cursor*/);}", {NULL}, 0U},
    {"not callable field", "type Plain{let number:i32;}func probe(value:Plain){value.number(/*cursor*/);}", {NULL}, 0U},
    {"unknown alias", "func probe(){let fn=unknown;fn(/*cursor*/);}", {NULL}, 0U},
    {"unknown generic argument", "func probe(fn:Callback<Missing>){fn(/*cursor*/);}", {NULL}, 0U},
    {"unknown array element", "func probe(fn:Callback<Missing[]>){fn(/*cursor*/);}", {NULL}, 0U},
    {"cyclic binding", "let cyclicA=cyclicB;let cyclicB=cyclicA;func probe(){cyclicA(/*cursor*/);}", {NULL}, 0U},
    {"invalid expanding spec cycle", "spec CycleA<T>:CycleB<T[]>{}spec CycleB<T>:CycleA<T[]>{}func probe(value:CycleA<i32>){value.missing(/*cursor*/);}", {NULL}, 0U},
    {"value shadows type", "func probe(Point:i32){Point(/*cursor*/);}", {NULL}, 0U},
    {"value shadows global", "func probe(global:i32){global(/*cursor*/);}", {NULL}, 0U},
    {"type parameter shadows contract", "func probe<Callback>(fn:Callback<i32>){fn(/*cursor*/);}", {NULL}, 0U},
    {"out of scope lambda", "func probe(){{let fn=(x:i32)->x;}fn(/*cursor*/);}", {NULL}, 0U},
    {"out of if scope", "func probe(){if true{let fn=(x:i32)->x;}fn(/*cursor*/);}", {NULL}, 0U},
    {"out of loop scope", "func probe(){while false{let fn=(x:i32)->x;}fn(/*cursor*/);}", {NULL}, 0U},
    {"inner scope control", "func probe(){{let fn=(x:i32)->x;fn(/*cursor*/1);}}", {"func fn(x: i32)"}, 0U},
    {"unclosed scope control", "func probe(){{let fn=(x:i32)->x;fn(/*cursor*/", {"func fn(x: i32)"}, 0U},
    {"open callable constraint", "func probe<T:Callback<i32>>(fn:T){fn(/*cursor*/1);}", {"func fn(value: i32): i32"}, 0U},
    {"constructor wrong arity", "func probe(){Construct<i32,string>(/*cursor*/1);}", {NULL}, 0U},
    {"constructor unknown argument", "func probe(){Construct<Missing>(/*cursor*/1);}", {NULL}, 0U},
    {"contract wrong arity", "func probe(fn:Callback<i32,string>){fn(/*cursor*/1);}", {NULL}, 0U},
    {"tuple is not constructor", "type Tuple(i32,string);func probe(){Tuple(/*cursor*/1);}", {NULL}, 0U},
    {"five overloads", "func probe(){pick(/*cursor*/1);}", {"func pick(value: i32): void", "func pick(value: i64): void", "func pick(value: string): void", "func pick(value: bool): void", "func pick(value: f64): void"}, 0U},
};

/* Run source, published source and source-hidden metadata through one matrix. */
static void signature_test_matrix(FitTestClient *client, const char *prefix) {
    for (size_t i = 0U; i < sizeof(signature_complete_cases) / sizeof(signature_complete_cases[0]); ++i) {
        const SignatureCompletenessCase *test = &signature_complete_cases[i];
        char *source = fit_test_format("%s\n%s\n", prefix, test->body);
        fit_test_source(client, source);
        char *response = signature_test_request(client, source, NULL);
        signature_test_expect(test, response);
        free(response); free(source);
    }
}

/* Build the dependency and physically hide its source for the FT path. */
static void signature_test_package(bool binary) {
    FitTestClient client = fit_test_start();
    char *dependency = fit_test_format("%s/dependency", client.directory);
    char *dep_src = fit_test_format("%s/src", dependency);
    char *dep_path = fit_test_format("%s/api.ff", dep_src);
    char *dep_manifest = fit_test_format("%s/feng.fm", dependency);
    char *consumer = fit_test_format("%s/consumer", client.directory);
    char *src = fit_test_format("%s/src", consumer);
    char *manifest = fit_test_format("%s/feng.fm", consumer);
    FIT_CHECK(mkdir(dependency, 0700) == 0 && mkdir(dep_src, 0700) == 0 &&
        mkdir(consumer, 0700) == 0 && mkdir(src, 0700) == 0);
    fit_test_write(dep_manifest, "[package]\nname: \"signature_complete\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n");
    fit_test_write(dep_path, signature_complete_declarations);
    char *argv[] = {dependency};
    FIT_CHECK(feng_cli_project_check_main("feng", 1, argv) == 0);
    FIT_CHECK(feng_cli_project_pack_main("feng", 1, argv) == 0);
    if (binary) {
        char *error = NULL;
        FIT_CHECK(feng_cli_project_remove_tree(dep_src, &error));
        free(error);
    }
    char *config = fit_test_format("[package]\nname: \"signature_consumer\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"src/\"\nout: \"build/\"\n[dependencies]\nsignature_complete: \"../dependency%s\"\n",
        binary ? "/build/pkg/signature_complete-0.1.0.fb" : "");
    fit_test_write(manifest, config);
    free(config);
    free(client.path); free(client.uri);
    client.path = fit_test_format("%s/main.ff", src);
    client.uri = fit_test_format("file://%s", client.path);
    const char *prefix = "module signature.consumer; import signature.complete; import signature.complete as api;";
    char *source = fit_test_format("%s func probe(){let value:i32=1;value./*cursor*/ready();}", prefix);
    fit_test_source(&client, source);
    free(fit_test_wait_complete(&client, source, "ready"));
    free(source);
    signature_test_matrix(&client, prefix);
    static const SignatureCompletenessCase extra[]={
        {"module alias global","func probe(){api.global(/*cursor*/1);}",{"func global(value: i32): i32"}, 0U},
        {"qualified global","func probe(){signature.complete.global(/*cursor*/1);}",{"func global(value: i32): i32"}, 0U},
        {"alias of qualified global","func probe(){let fn=api.global;fn(/*cursor*/1);}",{"func fn(value: i32): i32"}, 0U},
        {"alias of full module global","func probe(){let fn=signature.complete.global;fn(/*cursor*/1);}",{"func fn(value: i32): i32"}, 0U},
        {"shadowed global alias","func probe(api:i32){let fn=api.global;fn(/*cursor*/);}",{NULL}, 0U},
        {"alias constructor","func probe(){api.Construct<i32>(/*cursor*/1);}",{"ctor Construct(value: i32): void","ctor Construct(value: i32, count: i32): void"}, 0U},
        {"qualified constructor","func probe(){signature.complete.Point(/*cursor*/1);}",{"ctor Point(value: i32): void","ctor Point(text: string): void"}, 0U},
        {"private suppresses default","func probe(){Hidden(/*cursor*/);}",{NULL}, 0U},
        {"shadowed alias","func probe(api:i32){api.global(/*cursor*/);}",{NULL}, 0U},
    };
    for (size_t i = 0U; i < sizeof(extra) / sizeof(extra[0]); ++i) {
        source = fit_test_format("%s %s", prefix, extra[i].body);
        fit_test_source(&client, source);
        char *response = signature_test_request(&client, source, NULL);
        signature_test_expect(&extra[i], response);
        free(response); free(source);
    }
    fit_test_stop(&client);
    free(dependency); free(dep_src); free(dep_path); free(dep_manifest);
    free(consumer); free(src); free(manifest);
}

/* A selection belongs to the full signature, including instantiated types. */
static void signature_test_selection(void) {
    FitTestClient client = fit_test_start();
    char *source = fit_test_format("%s func probe(){pick(/*cursor*/1);}", signature_complete_declarations);
    fit_test_source(&client, source);
    const char *labels[] = {"func pick(value: i32): void", "func pick(value: i64): void",
        "func pick(value: string): void", "func pick(value: bool): void", "func pick(value: f64): void"};
    for (size_t i = 0U; i < 7U; ++i) {
        char *context = fit_test_signature_context(labels, 5U, i, true);
        char *response = signature_test_request(&client, source, context);
        char *needle = fit_test_format("],\"activeSignature\":%zu,\"activeParameter\":0}", i < 5U ? i : 0U);
        FIT_CHECK(strstr(response, needle) != NULL);
        free(needle); free(response); free(context);
    }
    const char *reordered[] = {labels[4], labels[2], labels[0], labels[3], labels[1]};
    const size_t positions[] = {4U, 2U, 0U, 3U, 1U};
    for (size_t i = 0U; i < 5U; ++i) {
        char *context = fit_test_signature_context(reordered, 5U, i, true);
        char *response = signature_test_request(&client, source, context);
        char *needle = fit_test_format("],\"activeSignature\":%zu,\"activeParameter\":0}", positions[i]);
        FIT_CHECK(strstr(response, needle) != NULL);
        free(needle); free(response); free(context);
    }
    free(source);
    source = fit_test_format("%s func probe(){Construct<i32>(1,/*cursor*/2);}", signature_complete_declarations);
    fit_test_source(&client, source);
    const char *constructors[] = {"ctor Construct(value: i32): void", "ctor Construct(value: i32, count: i32): void"};
    char *context = fit_test_signature_context(constructors, 2U, 1U, true);
    char *response = signature_test_request(&client, source, context);
    FIT_CHECK(strstr(response, "],\"activeSignature\":1,\"activeParameter\":1}") != NULL);
    free(response); free(source);
    source = fit_test_format("%s func probe(){Construct<string>(\"x\",/*cursor*/2);}", signature_complete_declarations);
    fit_test_source(&client, source);
    response = signature_test_request(&client, source, context);
    FIT_CHECK(strstr(response, "],\"activeSignature\":0,\"activeParameter\":0}") != NULL);
    free(response); free(context); free(source);
    source = fit_test_format("module changed;func pick(value:i32){}func probe(){pick(/*cursor*/1);}");
    fit_test_source(&client, source);
    context = fit_test_signature_context(labels, 5U, 4U, true);
    response = signature_test_request(&client, source, context);
    SignatureCompletenessCase remaining = {.name = "removed selected overload", .labels = {labels[0]}};
    signature_test_expect(&remaining, response);
    free(response); free(context); free(source);
    fit_test_stop(&client);
}

/* Update a second document in the same service without changing the request URI. */
static void signature_test_edit_document(FitTestClient *client, const char *uri,
    const char *source, unsigned int version) {
    char *quoted = fit_test_quote(source);
    char *payload = version == 1U
        ? fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%u,\"languageId\":\"feng\",\"text\":%s}}}", uri, version, quoted)
        : fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"%s\",\"version\":%u},\"contentChanges\":[{\"text\":%s}]}}", uri, version, quoted);
    fit_test_send(client, payload);
    free(payload); free(quoted);
}

/* Cross-file updates are observed through publication, never a fixed delay. */
static void signature_test_current_doc(FitTestClient *client, const char *source,
    const char *label, const char *documentation) {
    char *completion = fit_test_wait_complete(client, source, label);
    char *item = fit_test_item(completion, label);
    free(completion);
    FIT_CHECK(item != NULL);
    struct timespec start, now;
    FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        char *resolved = fit_test_resolve(client, item);
        if (strstr(resolved, documentation) != NULL) { free(resolved); break; }
        /* Publication can invalidate the item between the two requests. Only
         * a new identity permits retry; missing or wrong stable docs fail. */
        completion = fit_test_complete(client, source);
        char *current = fit_test_item(completion, label);
        free(completion);
        bool stale = current != NULL && strcmp(current, item) != 0 &&
            strstr(resolved, "\"documentation\":") == NULL;
        if (!stale) fprintf(stderr, "cross-file resolve expected %s\n%s\n%s\n", documentation, item, resolved);
        FIT_CHECK(stale);
        FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0 && now.tv_sec - start.tv_sec < 5);
        free(resolved); free(item);
        item = current;
    }
    free(item);
}

/* Both same-module and imported-module edits must update the common queries. */
static void signature_test_cross_file(bool other_module) {
    FitTestClient client = fit_test_start();
    char *path = fit_test_format("%s/api.ff", client.directory);
    char *uri = fit_test_format("file://%s", path);
    char *manifest = fit_test_format("%s/feng.fm", client.directory);
    const char *prefix = other_module ? "module signature.client;import signature.edit;" : "module signature.edit;";
    fit_test_write(manifest, "[package]\nname: \"signature_edit\"\nversion: \"0.1.0\"\ntarget: \"lib\"\nsrc: \"./\"\nout: \"build/\"\n");
    for (size_t round = 0U; round < 4U; ++round) {
        const char *type = round == 1U ? "string" : round == 2U ? "bool" : "i32";
        char *api = round == 3U
            ? fit_test_format("open module signature.edit;open fit i32{/** api revision 3 */func published3():i32{return self;}}")
            : fit_test_format("open module signature.edit;open spec Callback(value:%s):%s;"
            "open type Construct{func Construct(value:%s){}}open spec Parent{func take(value:%s):%s;}"
            "open spec Child:Parent{} open fit i32{/** api revision %zu */func published%zu():i32{return self;}}", type, type, type, type, type, round, round);
        if (round == 0U) fit_test_write(path, api);
        signature_test_edit_document(&client, uri, api, (unsigned int)round + 1U);
        char *source = fit_test_format("%sfunc probe(){let value:i32=1;value./*cursor*/published%zu();}", prefix, round);
        fit_test_source(&client, source);
        char *marker = fit_test_format("published%zu", round);
        char *documentation = fit_test_format("api revision %zu", round);
        signature_test_current_doc(&client, source, marker, documentation);
        free(documentation);
        free(marker); free(source);
        const char *bodies[] = {
            "func probe(fn:Callback){let alias=fn;alias(/*cursor*/);}",
            "func probe(){Construct(/*cursor*/);}",
            "func probe(value:Child){value.take(/*cursor*/);}"
        };
        const char *formats[] = {"func alias(value: %s): %s", "ctor Construct(value: %s): void", "func take(value: %s): %s"};
        for (size_t i = 0U; i < 3U; ++i) {
            source = fit_test_format("%s%s", prefix, bodies[i]);
            fit_test_source(&client, source);
            char *label = fit_test_format(formats[i], type, type);
            SignatureCompletenessCase test = {.name = "other-file declaration edit", .labels = {round < 3U ? label : NULL}};
            char *response = signature_test_request(&client, source, NULL);
            signature_test_expect(&test, response);
            free(response); free(label); free(source);
        }
        free(api);
    }
    char *closed = fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}", client.uri);
    fit_test_send(&client, closed);
    client.version = 0U;
    char *source = fit_test_format("%sfunc probe(fn:Callback){fn(/*cursor*/);}", prefix);
    fit_test_source(&client, source);
    char *response = signature_test_request(&client, source, NULL);
    SignatureCompletenessCase reopened = {.name = "reopened consumer after declaration removal"};
    signature_test_expect(&reopened, response);
    free(response); free(closed);
    closed = fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"%s\"}}}", uri);
    fit_test_send(&client, closed);
    free(closed);
    char *restored = fit_test_format("%sfunc probe(){let value:i32=1;value./*cursor*/published0();}", prefix);
    fit_test_source(&client, restored);
    free(fit_test_wait_complete(&client, restored, "published0"));
    fit_test_source(&client, source);
    response = signature_test_request(&client, source, NULL);
    SignatureCompletenessCase disk = {.name = "closed provider restores disk", .labels = {"func fn(value: i32): i32"}};
    signature_test_expect(&disk, response);
    free(response);
    /* Cancellation may race a completed response; neither outcome may leak a
     * stale signature into the subsequent version's request. */
    unsigned int id = signature_test_send_request(&client, source, NULL);
    char *cancel = fit_test_format("{\"jsonrpc\":\"2.0\",\"method\":\"$/cancelRequest\",\"params\":{\"id\":%u}}", id);
    fit_test_send(&client, cancel);
    response = fit_test_response(&client, id);
    FIT_CHECK(strstr(response, "\"result\":") != NULL || strstr(response, "-32800") != NULL);
    free(response); free(cancel);
    char *invalid = fit_test_format("%sfunc probe(fn:i32){fn(/*cursor*/);}", prefix);
    fit_test_source(&client, invalid);
    fit_test_source(&client, source);
    fit_test_source(&client, invalid);
    response = signature_test_request(&client, invalid, NULL);
    SignatureCompletenessCase noncallable = {.name = "latest version wins"};
    signature_test_expect(&noncallable, response);
    free(response);
    fit_test_stop(&client);
    free(path); free(uri); free(manifest);
    free(source); free(restored); free(invalid);
}

/* Completion consumes the exact same instantiated contract closure. */
static void signature_test_contract_completion(void) {
    static const char *const types[] = {"Child<i32>", "Diamond<i32>", "Repeated<i32>", "Nested<i32>", "Dual"};
    FitTestClient client = fit_test_start();
    for (size_t i = 0U; i < sizeof(types) / sizeof(types[0]); ++i) {
        char *source = fit_test_format("%s func probe(value:%s){value./*cursor*/take(1);}", signature_complete_declarations, types[i]);
        fit_test_source(&client, source);
        char *response = fit_test_complete(&client, source);
        const char *expected = i == 2U ? "func take(renamed: i32): i32" : "func take(value: i32): i32";
        FIT_CHECK(strstr(response, expected) != NULL);
        if (i == 4U) FIT_CHECK(strstr(response, "func take(value: string): string") != NULL);
        free(response); free(source);
    }
    fit_test_stop(&client);
}

/* A repeated diamond must converge on one instantiated requirement. */
static void signature_test_deep_contract(void) {
    char *source = NULL;
    size_t length = 0U;
    FILE *stream = open_memstream(&source, &length);
    FIT_CHECK(stream != NULL);
    FIT_CHECK(fputs("module deep;spec D0<T>{func take(value:T):T;}\n", stream) >= 0);
    for (size_t i = 1U; i <= 12U; ++i)
        FIT_CHECK(fprintf(stream, "spec L%zu<T>:D%zu<T>{}spec R%zu<T>:D%zu<T>{}spec D%zu<T>:L%zu<T>,R%zu<T>{}\n",
            i, i - 1U, i, i - 1U, i, i, i) > 0);
    FIT_CHECK(fputs("func probe(value:D12<i32>){value.take(/*cursor*/1);}", stream) >= 0);
    FIT_CHECK(fclose(stream) == 0 && length != 0U);
    FitTestClient client = fit_test_start();
    fit_test_source(&client, source);
    SignatureCompletenessCase test = {.name = "12 repeated diamonds", .labels = {"func take(value: i32): i32"}};
    double samples[100];
    for (size_t i = 0U; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        struct timespec start, end;
        FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        char *response = signature_test_request(&client, source, NULL);
        FIT_CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
        signature_test_expect(&test, response);
        free(response);
        double elapsed = (double)(end.tv_sec - start.tv_sec) * 1000.0 + (double)(end.tv_nsec - start.tv_nsec) / 1000000.0;
        size_t position = i;
        while (position > 0U && samples[position - 1U] > elapsed) {
            samples[position] = samples[position - 1U];
            --position;
        }
        samples[position] = elapsed;
    }
    fprintf(stdout, "lsp deep signatures: 12 diamonds, 100 requests, p50 %.3f ms, p99 %.3f ms\n", samples[49], samples[98]);
    fit_test_stop(&client);
    free(source);
}

/* New matrices are registered through the existing CLI binary and test phases. */
static void signature_test_completeness(void) {
    FitTestClient client = fit_test_start();
    signature_test_matrix(&client, signature_complete_declarations);
    fit_test_stop(&client);
    signature_test_selection();
    signature_test_cross_file(false);
    signature_test_cross_file(true);
    signature_test_contract_completion();
    signature_test_deep_contract();
    signature_test_package(false);
    signature_test_package(true);
    fprintf(stdout, "lsp call signatures: callable, constructor and spec source/FT matrices passed\n");
}

/* Public registration point called by the existing CLI test executable. */
void test_lsp_fit_completion(void) {
    fit_test_builtin_targets();
    fit_test_source_targets();
    fit_test_edit_and_resolve();
    fit_test_structural_boundaries();
    fit_test_many_declarations();
    fit_test_effective_returns();
    fit_test_package(false);
    fit_test_package(true);
    fit_test_signatures();
    signature_test_completeness();
    fprintf(stdout, "lsp fit completion: source/FT target, receiver, edit and resolve matrices passed\n");
}
