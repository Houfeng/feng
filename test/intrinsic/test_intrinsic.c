#include "intrinsic/feng_intrinsic.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ASSERT(expr)                                                                  \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static void test_ascii_length(void) {
    uint8_t value[] = "hello";

    ASSERT(feng_string_utf8_length(value) == 5);
}

static void test_utf8_length(void) {
    uint8_t value[] = "你好";

    ASSERT(feng_string_utf8_length(value) == 6);
}

static void test_null_length(void) {
    ASSERT(feng_string_utf8_length(NULL) == 0);
}

int main(void) {
    test_ascii_length();
    test_utf8_length();
    test_null_length();
    return 0;
}
