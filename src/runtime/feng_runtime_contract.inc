/* Runtime contract fragment shared by feng_runtime.h declarations and the
 * codegen-side `@runtime` allow-list. Include sites must define
 * FENG_RUNTIME_CONTRACT(ret_type, name, args) before including this file. */

#ifndef FENG_RUNTIME_CONTRACT
#error "define FENG_RUNTIME_CONTRACT(ret_type, name, args) before including feng_runtime_contract.inc"
#endif

FENG_RUNTIME_CONTRACT(int64_t, feng_string_utf8_length, (FengString *value))
