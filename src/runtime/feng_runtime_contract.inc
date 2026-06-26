// Only for IntelliSense ------------------------------------------------------
#ifdef __INTELLISENSE__
#include "feng_runtime.h"  
#define FENG_RUNTIME_CONTRACT(ret_type, name, args) ret_type name args;        
#endif
// ----------------------------------------------------------------------------


/* Runtime contract fragment shared by feng_runtime.h declarations and the
 * codegen-side `@runtime` allow-list. Include sites must define
 * FENG_RUNTIME_CONTRACT(ret_type, name, args) before including this file. */

#ifndef FENG_RUNTIME_CONTRACT
#error "define FENG_RUNTIME_CONTRACT(ret_type, name, args) before including feng_runtime_contract.inc"
#endif

FENG_RUNTIME_CONTRACT(intptr_t, feng_string_utf8_length, (FengString *value))
FENG_RUNTIME_CONTRACT(FengString *, feng_string_from_utf8_bytes, (FengArray *value, intptr_t length))
FENG_RUNTIME_CONTRACT(FengArray *, feng_string_to_utf8_bytes, (FengString *value))
FENG_RUNTIME_CONTRACT(FengString *, feng_string_slice, (FengString *value, intptr_t start, intptr_t length))
FENG_RUNTIME_CONTRACT(FengArray *, feng_string_slice_bytes, (FengString *value, intptr_t start, intptr_t length))
FENG_RUNTIME_CONTRACT(intptr_t, feng_array_get_length, (const FengGenericParamDescriptor *type, const FengArray *value))
FENG_RUNTIME_CONTRACT(FengArray *, feng_array_slice, (const FengGenericParamDescriptor *type, const FengArray *value, intptr_t start, intptr_t length))
FENG_RUNTIME_CONTRACT(bool, feng_expression_equal, (const FengGenericParamDescriptor *type, const void *left, const void *right))
FENG_RUNTIME_CONTRACT(bool, feng_string_range_equal, (FengString *a, intptr_t a_start, intptr_t a_end, FengString *b, intptr_t b_start, intptr_t b_end))

/* Raw memory allocation — thin wrappers around libc malloc/free that keep
 * the Feng-side intptr_t representation while avoiding C type conflicts with
 * system-header prototypes. */
FENG_RUNTIME_CONTRACT(intptr_t, feng_alloc, (intptr_t size))
FENG_RUNTIME_CONTRACT(void, feng_free, (intptr_t ptr))

/* Pointer helpers for opaque C-side pointers.  Feng has no null-pointer
 * literal, so user code calls feng_pointer_is_null to detect a failed
 * allocation or a NULL return from a C API.  feng_pointer_equal compares
 * two C-side pointers for address equality. */
FENG_RUNTIME_CONTRACT(bool, feng_pointer_is_null, (void *ptr))
FENG_RUNTIME_CONTRACT(bool, feng_pointer_equal, (void *left, void *right))
FENG_RUNTIME_CONTRACT(void *, feng_pointer_move, (void *ptr, intptr_t offset))
FENG_RUNTIME_CONTRACT(intptr_t, feng_pointer_diff, (void *a, void *b))
FENG_RUNTIME_CONTRACT(void, feng_pointer_get_scalar, (const FengGenericParamDescriptor *type, void *ptr, void *result))
FENG_RUNTIME_CONTRACT(void *, feng_pointer_get_pointer, (void *ptr))

/* Test-only helper for compiler/runtime regressions around bare-T return. */
FENG_RUNTIME_CONTRACT(void, __test_value_identity, (const FengGenericParamDescriptor *type, const void *value, void *out))


// Only for IntelliSense ------------------------------------------------------
#ifdef __INTELLISENSE__
#undef FENG_RUNTIME_CONTRACT
#endif
// ----------------------------------------------------------------------------
