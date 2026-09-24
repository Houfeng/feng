#include "runtime.h"

/* Keep the ordinary call and its native EH counterpart otherwise identical. */
int ceh_benchmark_call(int value) {
#if PROTECTED
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    __llvm_c_eh_activate(1);
#endif
    return ceh_test_produce(value) + 1;
#if PROTECTED
landing:;
    void *record = __llvm_c_eh_exception(1);
    int result = ceh_test_value(record);
    ceh_test_delete(record);
    return result + 1;
#endif
}
