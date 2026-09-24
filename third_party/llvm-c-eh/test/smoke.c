#include "runtime.h"

/* Exercise a typed handler and preserve the ordinary result path. */
int ceh_test_smoke(int value) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, CEH_TEST_PERSONALITY);
    if (__llvm_c_eh_region(1u, 0u, &&caught, 1u,
                          (const void *)&ceh_test_key_a)) goto caught;
    __llvm_c_eh_activate(1u);
    int result = ceh_test_produce(value);
    __llvm_c_eh_activate(0u);
    return result;
caught:;
    void *record = __llvm_c_eh_exception(1u);
    if (__llvm_c_eh_selector(1u) == __llvm_c_eh_typeid(&ceh_test_key_a)) {
        int payload = ceh_test_value(record);
        ceh_test_delete(record);
        return payload;
    }
    __llvm_c_eh_propagate(1u);
}
