#include "llvm_c_eh.h"

extern int native_producer(int value);
extern void *__cxa_begin_catch(void *record);
extern void __cxa_end_catch(void);
extern _Unwind_Reason_Code __gxx_personality_v0(
    int, _Unwind_Action, uint64_t, struct _Unwind_Exception *, struct _Unwind_Context *);

/* Use the same C++ personality, payload and catch ownership as the native control. */
int native_call(int value) {
#if PROTECTED
    __llvm_c_eh_configure(__LLVM_C_EH_PROTOCOL_VERSION, __gxx_personality_v0);
    if (__llvm_c_eh_region(1, 0, &&landing, 1, (const void *)0)) goto landing;
    __llvm_c_eh_activate(1);
#endif
    return native_producer(value) + 1;
#if PROTECTED
landing:
    __cxa_begin_catch(__llvm_c_eh_exception(1));
    __cxa_end_catch();
    return 2;
#endif
}
