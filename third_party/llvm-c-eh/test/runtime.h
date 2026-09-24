#ifndef CEH_TEST_RUNTIME_H
#define CEH_TEST_RUNTIME_H

#include "llvm_c_eh.h"
#include <stdatomic.h>

/* The same fixtures can select another consumer without rebuilding the plugin. */
#ifndef CEH_TEST_PERSONALITY
#define CEH_TEST_PERSONALITY ceh_test_personality
#endif

extern const int ceh_test_key_a;
extern const int ceh_test_key_b;
extern _Atomic int ceh_test_alive;
extern _Atomic int ceh_test_deleted;

/* Interpret native EH tables using test-owned opaque identity keys. */
extern _Unwind_Reason_Code CEH_TEST_PERSONALITY(
    int, _Unwind_Action, uint64_t, struct _Unwind_Exception *, struct _Unwind_Context *);

/* Create and throw a test-owned payload through the platform unwinder. */
extern _Noreturn void ceh_test_raise(const void *key, int value);

/* Transfer the caught record unchanged into a new native search. */
extern _Noreturn void ceh_test_rethrow(void *record);

/* Release one caught record; this operation cannot throw. */
extern void ceh_test_delete(void *record) __attribute__((nothrow));

/* Inspect a borrowed payload without affecting its lifetime. */
extern int ceh_test_value(void *record) __attribute__((nothrow));

/* Invoke a producer compiled in another translation unit. */
extern int ceh_test_produce(int value);

/* Check an invariant, terminating the test on failure without unwinding. */
extern void ceh_test_check(int condition, const char *label) __attribute__((nothrow));

#endif
