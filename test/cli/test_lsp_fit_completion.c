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
        {"partial inference cannot escape", "func values<T>(item: T, n: i32): T[] { return []; } fit T[] { func marker(): T { return self[0]; } }", "func probe() { values(\"x\", 1)./*cursor*/ }", NULL, "marker", NULL},
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
        {"package incomplete inference", "", "func probe() { contextualPartial(\"x\", 1)./*cursor*/ }", NULL, "element", NULL},
        {"package conflicting bindings", "", "func probe() { conflicting(1, \"x\", 1)./*cursor*/ }", NULL, "element", NULL},
        {"package generic field", "", "func probe(value: Box<int[]>) { value.item./*cursor*/ }", "integers", "strings", NULL},
        {"package static", "", "func probe() { int[]./*cursor*/ }", "factory", "element", NULL},
        {"package alias identity", "", "func probe(value: other.Box<int>) { value./*cursor*/ }", "otherMarker", "element", NULL},
        {"package nominal identity", "", "func probe(value: Box<int>) { value./*cursor*/ }", "element", "otherMarker", NULL},
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
    fprintf(stdout, "lsp fit completion: source/FT target, receiver, edit and resolve matrices passed\n");
}
