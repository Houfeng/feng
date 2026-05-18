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

FENG_RUNTIME_CONTRACT(int64_t, feng_string_utf8_length, (FengString *value))
FENG_RUNTIME_CONTRACT(FengString *, feng_string_from_utf8_bytes, (FengArray *value, int64_t length))
FENG_RUNTIME_CONTRACT(int64_t, feng_array_length_i64, (const FengArray *value))
FENG_RUNTIME_CONTRACT(FengArray *, feng_array_slice, (const FengArray *value, int64_t start, int64_t length))


// Only for IntelliSense ------------------------------------------------------
#ifdef __INTELLISENSE__
#undef FENG_RUNTIME_CONTRACT
#endif
// ----------------------------------------------------------------------------
