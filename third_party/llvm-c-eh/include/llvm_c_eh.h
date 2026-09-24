#ifndef __LLVM_C_EH_H
#define __LLVM_C_EH_H

#include <stdint.h>
#include <unwind.h>

/* C emission protocol, independent of LLVM's plugin API and unwind ABI versions. */
#define __LLVM_C_EH_PROTOCOL_VERSION 1u

/* Native personality signature; the consuming language provides its implementation. */
typedef _Unwind_Reason_Code (*__llvm_c_eh_personality_fn)(
    int version, _Unwind_Action actions, uint64_t exception_class,
    struct _Unwind_Exception *exception, struct _Unwind_Context *context);

/* Configure one function. All declarations below are erased by the LLVM pass. */
extern void __llvm_c_eh_configure(uint32_t version,
                                __llvm_c_eh_personality_fn personality);

/* Declare a region and its parent, landing label and constant catch type keys. */
extern _Bool __llvm_c_eh_region(uint32_t id, uint32_t parent, void *landing,
                              uint32_t catch_count, ...);

/* Change the compile-time active region; zero means no local unwind handler. */
extern void __llvm_c_eh_activate(uint32_t id);

/* Borrow the exception record delivered to the identified landing region. */
extern void *__llvm_c_eh_exception(uint32_t id);

/* Read the native selector paired with that same exception record. */
extern int32_t __llvm_c_eh_selector(uint32_t id);

/* Resolve a constant type key to its native, inlining-aware selector. */
extern int32_t __llvm_c_eh_typeid(const void *type_key);

/* Continue the same unhandled exception through the parent or out of the function. */
extern _Noreturn void __llvm_c_eh_propagate(uint32_t id);

#endif
