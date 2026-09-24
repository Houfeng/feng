#include "runtime.h"

/* Compile each independent malformed input and require a protocol diagnostic. */
int invalid(int input, const void *key) {
    (void)key;
#if CASE == 1
    __llvm_c_eh_activate(0); /* missing configuration */
    return 0;
#elif CASE == 2
    __llvm_c_eh_configure(2, CEH_TEST_PERSONALITY);
    return 0;
#elif CASE == 3
    __llvm_c_eh_configure(input, CEH_TEST_PERSONALITY);
    return 0;
#elif CASE == 4
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    return 0;
#elif CASE == 5
    if (input) __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    return 0;
#elif CASE == 6
    __llvm_c_eh_activate(0);
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    return 0;
#elif CASE == 7
    __llvm_c_eh_configure(1, (__llvm_c_eh_personality_fn)0);
    return 0;
#else
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
#if CASE == 8
    if (__llvm_c_eh_region(0, 0, &&landing, 0)) goto landing;
#elif CASE == 9
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    if (__llvm_c_eh_region(1, 0, &&other, 0)) goto other;
#elif CASE == 10
    if (__llvm_c_eh_region(1, 99, &&landing, 0)) goto landing;
#elif CASE == 11
    if (__llvm_c_eh_region(1, 2, &&landing, 0)) goto landing;
    if (__llvm_c_eh_region(2, 1, &&other, 0)) goto other;
#elif CASE == 12
    if (__llvm_c_eh_region(1, 0, (void *)0, 0)) goto landing;
#elif CASE == 13
    if (__llvm_c_eh_region(1, 0, &&landing, 1)) goto landing;
#elif CASE == 14
    if (__llvm_c_eh_region(1, 0, &&landing, 1, key)) goto landing;
#elif CASE == 15
    if (__llvm_c_eh_region(1, 0, &&landing, 2, (const void *)0, (const void *)&ceh_test_key_a)) goto landing;
#elif CASE == 16
    if (__llvm_c_eh_region(input, 0, &&landing, 0)) goto landing;
#elif CASE == 17
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    if (__llvm_c_eh_region(2, 0, &&landing, 0)) goto landing;
#elif CASE == 18
    int observed = __llvm_c_eh_region(1, 0, &&landing, 0);
    if (observed) goto landing;
#elif CASE == 19
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto other;
#elif CASE == 20
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    if (input) goto landing;
#elif CASE == 21
    static void *escaped;
    escaped = &&landing;
    ceh_test_check(escaped != 0, "keep address");
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
#elif CASE == 31
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)1)) goto landing;
#elif CASE == 32
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)((const char *)&ceh_test_key_a + 1))) goto landing;
#else
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
#endif
#if CASE == 22
    __llvm_c_eh_activate(99);
#elif CASE == 23
    __llvm_c_eh_activate(input);
#elif CASE == 24
    if (input) __llvm_c_eh_activate(1);
    ceh_test_produce(input);
#elif CASE == 25
    __llvm_c_eh_exception(1);
#elif CASE == 26
    __llvm_c_eh_selector(1);
#elif CASE == 27
    __llvm_c_eh_propagate(1);
#elif CASE == 28
    __llvm_c_eh_typeid(key);
#elif CASE == 29
    __llvm_c_eh_exception(0);
#elif CASE == 30
    __llvm_c_eh_typeid(&&landing);
#elif CASE == 33
    __llvm_c_eh_typeid((const void *)1);
#endif
    return input;
landing:
    return 1;
other:
    return 2;
#endif
}
