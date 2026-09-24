#include "runtime.h"
#include <pthread.h>
#include <stdio.h>

/* The typed smoke test lives in a different translation unit. */
extern int ceh_test_smoke(int value);

/* Deliberately small and inlineable: optimized tests must exercise native EH inlining. */
static int inline_throw(int value) {
    if (value) ceh_test_raise(&ceh_test_key_b, value);
    return 4;
}

/* Same-function parents, multiple cleanup levels, and local values across unwinding. */
static int nested(int value, int *sequence) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    int local = value + 10;
    if (__llvm_c_eh_region(1, 0, &&outer, 1, (const void *)&ceh_test_key_b)) goto outer;
    if (__llvm_c_eh_region(2, 1, &&middle, 1, (const void *)&ceh_test_key_a)) goto middle;
    if (__llvm_c_eh_region(3, 2, &&inner, 0)) goto inner;
    __llvm_c_eh_activate(3);
    local += inline_throw(value);
    __llvm_c_eh_activate(0);
    return local;
inner:
    *sequence = *sequence * 10 + 1;
    __llvm_c_eh_propagate(3);
middle:
    *sequence = *sequence * 10 + 2;
    ceh_test_check(__llvm_c_eh_selector(2) != __llvm_c_eh_typeid(&ceh_test_key_a), "skip nonmatching parent");
    __llvm_c_eh_propagate(2);
outer:;
    void *record = __llvm_c_eh_exception(1);
    ceh_test_check(__llvm_c_eh_selector(1) == __llvm_c_eh_typeid(&ceh_test_key_b), "parent selector");
    *sequence = *sequence * 10 + 3;
    local += ceh_test_value(record);
    ceh_test_delete(record);
    return local;
}

/* A pure cleanup frame resumes into its caller, including after optimized inlining. */
static int cleanup(int value, int *sequence) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    __llvm_c_eh_activate(1);
    return ceh_test_produce(value);
landing:
    *sequence = *sequence * 10 + 4;
    ceh_test_check(ceh_test_smoke(-7) == 7, "nested exception during cleanup");
    __llvm_c_eh_propagate(1);
}

/* Indirect callees are protected; catch-all handles an identity absent from its list. */
static int indirect(int (*call)(int, int *), int value, int *sequence) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    __llvm_c_eh_activate(1);
    return call(value, sequence);
landing:;
    void *record = __llvm_c_eh_exception(1);
    ceh_test_check(__llvm_c_eh_selector(1) == __llvm_c_eh_typeid(0), "catch-all selector");
    int payload = ceh_test_value(record);
    ceh_test_delete(record);
    return payload;
}

/* Select a callback through volatile storage to retain a real indirect call at O3. */
static int (*volatile cleanup_pointer)(int, int *) = cleanup;

/* Restart search from an active catch, optionally replacing the owned exception. */
static int throwing_catch(int replace, void **identity) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    __llvm_c_eh_activate(1);
    ceh_test_raise(&ceh_test_key_a, 31);
landing:;
    void *record = __llvm_c_eh_exception(1);
    *identity = record;
    if (replace) {
        ceh_test_delete(record);
        ceh_test_raise(&ceh_test_key_b, 32);
    }
    ceh_test_rethrow(record);
}

/* Check rethrow identity and nested handler lifetime without any global current slot. */
static void catch_rethrow(int replace) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    void *identity = 0;
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    __llvm_c_eh_activate(1);
    throwing_catch(replace, &identity);
    ceh_test_check(0, "throwing catch returned");
    return;
landing:;
    void *record = __llvm_c_eh_exception(1);
    if (!replace) ceh_test_check(record == identity, "rethrow record identity");
    ceh_test_check(ceh_test_value(record) == 31 + replace, "rethrow payload");
    ceh_test_check(ceh_test_smoke(-9) == 9, "nested catch");
    ceh_test_check(ceh_test_value(record) == 31 + replace, "outer payload survived nested catch");
    ceh_test_delete(record);
}

/* Region-state merges cover loops, both branches, break, continue and early return. */
static int loop(int stop) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    int total = 0;
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    for (int i = 0; i < 6; ++i) {
        if (i & 1) __llvm_c_eh_activate(1);
        else __llvm_c_eh_activate(1);
        if (i == stop) ceh_test_produce(-5);
        total += ceh_test_produce(i);
        __llvm_c_eh_activate(0);
        if (i == 1) continue;
        if (stop == 9 && i == 2) return total;
        if (stop == 8 && i == 3) break;
    }
    return total;
landing:;
    void *record = __llvm_c_eh_exception(1);
    total += ceh_test_value(record);
    ceh_test_delete(record);
    return total;
}

/* Large aggregate arguments/returns retain their target ABI attributes after conversion. */
typedef struct Aggregate { long values[8]; } Aggregate;

/* An indirect aggregate producer prevents scalar replacement from hiding ABI mistakes. */
static Aggregate aggregate_produce(Aggregate value, int should_throw) {
    if (should_throw) ceh_test_raise(&ceh_test_key_a, 66);
    value.values[7] += value.values[0];
    return value;
}
static Aggregate (*volatile aggregate_pointer)(Aggregate, int) = aggregate_produce;

/* Exercise both sret/byval normal delivery and the corresponding exception edge. */
static long aggregate_test(int should_throw) {
    __llvm_c_eh_configure(1, CEH_TEST_PERSONALITY);
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    Aggregate value = {{3, 0, 0, 0, 0, 0, 0, 7}};
    __llvm_c_eh_activate(1);
    value = aggregate_pointer(value, should_throw);
    return value.values[7];
landing:;
    void *record = __llvm_c_eh_exception(1);
    long result = ceh_test_value(record);
    ceh_test_delete(record);
    return result;
}

/* Concurrent unwinding uses independent native exception pairs in every invocation. */
static void *worker(void *argument) {
    (void)argument;
    for (int i = 0; i < 100; ++i) {
        int order = 0;
        ceh_test_check(nested(3, &order) == 16 && order == 123, "thread nested");
        ceh_test_check(ceh_test_smoke(-i - 1) == i + 1, "thread throw");
    }
    return NULL;
}

/* Run real exception paths and check that every allocated record was destroyed. */
int main(void) {
    ceh_test_check(ceh_test_smoke(3) == 4, "normal path");
    ceh_test_check(ceh_test_smoke(-8) == 8, "typed catch");
    int order = 0;
    ceh_test_check(nested(0, &order) == 14 && order == 0, "nested normal path");
    ceh_test_check(nested(2, &order) == 14 && order == 123, "nested parent chain");
    order = 0;
    ceh_test_check(indirect(cleanup_pointer, -12, &order) == 12 && order == 4, "indirect cleanup and resume");
    ceh_test_check(indirect(cleanup_pointer, 4, &order) == 5 && order == 4, "indirect normal path");
    catch_rethrow(0);
    catch_rethrow(1);
    ceh_test_check(loop(0) == 5 && loop(2) == 8 && loop(5) == 20, "loop exceptions");
    ceh_test_check(loop(7) == 21 && loop(8) == 10 && loop(9) == 6, "loop normal exits");
    ceh_test_check(aggregate_test(0) == 10 && aggregate_test(1) == 66, "aggregate call ABI");
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) ceh_test_check(!pthread_create(&threads[i], NULL, worker, NULL), "start thread");
    for (unsigned i = 0; i < 4; ++i) ceh_test_check(!pthread_join(threads[i], NULL), "join thread");
    ceh_test_check(atomic_load(&ceh_test_alive) == 0, "all exception records released");
    ceh_test_check(atomic_load(&ceh_test_deleted) == 813, "exact exception destruction count");
    puts("llvm-c-eh: behavior, ABI, nested lifetime and concurrent unwinding passed");
    return 0;
}
