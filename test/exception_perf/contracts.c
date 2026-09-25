#include "feng_runtime.h"
#include "llvm_c_eh.h"

/* Observable test cleanup prevents later optimizers from eliding a bare resume. */
volatile unsigned native_contract_cleanup_count;

/* Registration stores callbacks without executing them; every call must remain
 * an ordinary call, even before LLVM's regular optimization pipeline runs. */
void registration(FengFrameMarker *frame, FengCleanupNode *node, void **slot,
                  const FengAggregateDescriptor *desc, void (*defer)(void *)) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, __feng_personality_v0);
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    __llvm_c_eh_activate(1);
    feng_frame_push(frame);
    feng_try_frame_push(frame);
    feng_cleanup_push(node, slot);
    feng_cleanup_push_aggregate(node, slot, desc);
    feng_defer_push(node, defer, NULL);
    feng_cleanup_pop();
    feng_caught_value();
    feng_caught_clause();
    feng_frame_pop();
    return;
landing:
    ++native_contract_cleanup_count;
    __llvm_c_eh_propagate(1);
}

/* These operations may execute user cleanup and must retain real unwind edges. */
void callbacks(FengFrameMarker *frame, FengCatchContext *context, void *value) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, __feng_personality_v0);
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    __llvm_c_eh_activate(1);
    feng_frame_release_to(frame);
    feng_exception_catch_begin(context);
    feng_exception_catch_end();
    feng_release(value);
    return;
landing:
    ++native_contract_cleanup_count;
    __llvm_c_eh_propagate(1);
}

/* An unresolved indirect target is never promoted to a no-unwind contract. */
void dynamic_call(void (*target)(void)) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, __feng_personality_v0);
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    __llvm_c_eh_activate(1);
    target();
    return;
landing:
    ++native_contract_cleanup_count;
    __llvm_c_eh_propagate(1);
}

/* A noreturn declaration is compatible with unwinding; it is not nounwind. */
void throwing(void *value, const FengTypeDescriptor *desc) {
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, __feng_personality_v0);
    if (__llvm_c_eh_region(1, 0, &&landing, 0)) goto landing;
    __llvm_c_eh_activate(1);
    feng_throw(value, desc);
landing:
    ++native_contract_cleanup_count;
    __llvm_c_eh_propagate(1);
}
