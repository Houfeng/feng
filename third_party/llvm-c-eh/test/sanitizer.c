#include "runtime.h"
#include <limits.h>
#include <stdio.h>

/* Give the function sanitizer a real signature to compare at an indirect call. */
static int identity(int value) { return value; }

/* Exercise checks in the protected body and handler after protocol conversion. */
int main(int argc, char **argv) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, CEH_TEST_PERSONALITY);
    if (argc != 2) return 2;
    if (__llvm_c_eh_region(1u, 0u, &&caught, 1u, (const void *)0)) goto caught;
    __llvm_c_eh_activate(1u);
    volatile int maximum = INT_MAX;
    volatile int index = 2;
    int values[2] = {1, 2};
    /* Deliberately invalid: reach the runtime check rather than its static warning. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
    long (*volatile mismatched)(long) = (long (*)(long))identity;
#pragma clang diagnostic pop
    switch (argv[1][0]) {
    case 'o': (void)(maximum + 1); break;
    case 'b': (void)values[index]; break;
    case 'f': (void)mismatched(7); break;
    case 'l': ceh_test_raise(&ceh_test_key_a, 7);
    default: break;
    }
    __llvm_c_eh_activate(0u);
    puts("body completed");
    return 0;
caught:;
    void *record = __llvm_c_eh_exception(1u);
    ceh_test_check(argv[1][0] == 'l', "UBSan must not enter a language catch");
    ceh_test_check(ceh_test_value(record) == 7, "handler payload");
    ceh_test_delete(record);
    volatile int handler_maximum = INT_MAX;
    (void)(handler_maximum + 1);
    puts("handler completed");
    return 0;
}
