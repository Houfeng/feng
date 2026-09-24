#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test-only payload: keys are identities and have no prescribed object layout. */
typedef struct TestException {
    struct _Unwind_Exception native;
    const void *key;
    int value;
} TestException;

const int ceh_test_key_a = 11;
const int ceh_test_key_b = 22;
_Atomic int ceh_test_alive;
_Atomic int ceh_test_deleted;
static const uint64_t test_class = UINT64_C(0x4345485445535400);

/* Assertions remain enabled in release tests. */
void ceh_test_check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "llvm-c-eh test failed: %s\n", message);
        abort();
    }
}

/* Destroy the native record exactly once, including cross-function catches. */
static void destroy(_Unwind_Reason_Code reason, struct _Unwind_Exception *record) {
    (void)reason;
    atomic_fetch_sub(&ceh_test_alive, 1);
    atomic_fetch_add(&ceh_test_deleted, 1);
    free(record);
}

/* Create an independent native exception and start the platform unwinder. */
_Noreturn void ceh_test_raise(const void *key, int value) {
    TestException *record = calloc(1, sizeof(*record));
    ceh_test_check(record != NULL, "allocate exception");
    record->native.exception_class = test_class;
    record->native.exception_cleanup = destroy;
    record->key = key;
    record->value = value;
    atomic_fetch_add(&ceh_test_alive, 1);
    _Unwind_Reason_Code reason = _Unwind_RaiseException(&record->native);
    fprintf(stderr, "uncaught test exception %d (reason %d)\n", value, reason);
    _Unwind_DeleteException(&record->native);
    abort();
}

/* Restart native search while preserving the original record and payload. */
_Noreturn void ceh_test_rethrow(void *record) {
    _Unwind_Reason_Code reason = _Unwind_Resume_or_Rethrow(record);
    fprintf(stderr, "uncaught test rethrow (reason %d)\n", reason);
    _Unwind_DeleteException(record);
    abort();
}

/* Catches own deletion; the plugin never inserts an ownership operation. */
void ceh_test_delete(void *record) { _Unwind_DeleteException(record); }

/* Read a payload without modifying any process-global exception slot. */
int ceh_test_value(void *record) { return ((TestException *)record)->value; }

/* Decode unsigned LEB128 metadata emitted by the native LLVM backend. */
static uintptr_t uleb(const unsigned char **cursor) {
    uintptr_t value = 0;
    unsigned shift = 0, byte;
    do {
        byte = *(*cursor)++;
        ceh_test_check(shift < sizeof(value) * 8, "ULEB overflow");
        value |= (uintptr_t)(byte & 127U) << shift;
        shift += 7;
    } while (byte & 128U);
    return value;
}

/* Decode signed action-table offsets without signed-shift overflow. */
static intptr_t sleb(const unsigned char **cursor) {
    uintptr_t value = 0;
    unsigned shift = 0, byte;
    do {
        byte = *(*cursor)++;
        ceh_test_check(shift < sizeof(value) * 8, "SLEB overflow");
        value |= (uintptr_t)(byte & 127U) << shift;
        shift += 7;
    } while (byte & 128U);
    if ((byte & 64U) && shift < sizeof(value) * 8) value |= ~(uintptr_t)0 << shift;
    return (intptr_t)value;
}

/* Return a fixed encoding width for a reverse-indexed LSDA type table. */
static size_t width(unsigned encoding) {
    switch (encoding & 15U) {
    case 0: return sizeof(uintptr_t);
    case 2: case 10: return 2;
    case 3: case 11: return 4;
    case 4: case 12: return 8;
    default: ceh_test_check(0, "unsupported type encoding"); return 0;
    }
}

/* Decode native absolute/PC-relative/function-relative pointers, safely unaligned. */
static uintptr_t encoded(const unsigned char **cursor, unsigned encoding,
                         struct _Unwind_Context *context) {
    uintptr_t field = (uintptr_t)*cursor, value = 0;
    if (encoding == 255) return 0;
    if ((encoding & 112U) == 80U) {
        field = (field + sizeof(uintptr_t) - 1) & ~(sizeof(uintptr_t) - 1);
        *cursor = (const unsigned char *)field;
    }
    switch (encoding & 15U) {
    case 1: value = uleb(cursor); break;
    case 9: value = (uintptr_t)sleb(cursor); break;
    default: {
        size_t size = width(encoding);
        memcpy(&value, *cursor, size);
        *cursor += size;
        if ((encoding & 8U) && size < sizeof(value) &&
            (value & ((uintptr_t)1 << (size * 8 - 1)))) value |= ~(uintptr_t)0 << (size * 8);
        break;
    }
    }
    if (!value) return 0;
    switch (encoding & 112U) {
    case 0: case 80: break;
    case 16: value += field; break;
    case 64: value += _Unwind_GetRegionStart(context); break;
    default: ceh_test_check(0, "unsupported pointer application");
    }
    if (encoding & 128U) memcpy(&value, (const void *)value, sizeof(value));
    return value;
}

/* Interpret LLVM's native LSDA using only this test runtime's opaque key identity. */
_Unwind_Reason_Code CEH_TEST_PERSONALITY(int version, _Unwind_Action actions,
        uint64_t exception_class, struct _Unwind_Exception *record,
        struct _Unwind_Context *context) {
    if (version != 1) return _URC_FATAL_PHASE1_ERROR;
    const unsigned char *p = (const unsigned char *)_Unwind_GetLanguageSpecificData(context);
    if (!p) return _URC_CONTINUE_UNWIND;
    unsigned lp_encoding = *p++;
    uintptr_t base = lp_encoding == 255 ? _Unwind_GetRegionStart(context) :
                                         encoded(&p, lp_encoding, context);
    unsigned type_encoding = *p++;
    const unsigned char *types = NULL;
    if (type_encoding != 255) {
        uintptr_t offset = uleb(&p);
        types = p + offset;
    }
    unsigned call_encoding = *p++;
    uintptr_t length = uleb(&p);
    const unsigned char *end = p + length;
    int before = 0;
    uintptr_t ip = _Unwind_GetIPInfo(context, &before);
    if (!before) --ip;
    while (p < end) {
        uintptr_t start = encoded(&p, call_encoding, context);
        uintptr_t span = encoded(&p, call_encoding, context);
        uintptr_t landing = encoded(&p, call_encoding, context);
        uintptr_t action = uleb(&p);
        if (ip < base + start || ip >= base + start + span) continue;
        if (!landing) return _URC_CONTINUE_UNWIND;
        int cleanup = action == 0;
        intptr_t selector = 0;
        if (action) {
            const unsigned char *entry = end + action - 1;
            for (;;) {
                intptr_t index = sleb(&entry);
                const unsigned char *offset_base = entry;
                intptr_t next = sleb(&entry);
                if (!index) cleanup = 1;
                else if (index > 0) {
                    ceh_test_check(types != NULL, "missing type table");
                    const unsigned char *type = types - index * width(type_encoding);
                    const void *key = (const void *)encoded(&type, type_encoding, context);
                    if (!selector && !(actions & _UA_FORCE_UNWIND) &&
                        (!key || (exception_class == test_class &&
                         key == ((TestException *)record)->key))) selector = index;
                } else ceh_test_check(0, "unexpected native filter");
                if (!next) break;
                entry = offset_base + next;
            }
        }
        if (actions & _UA_SEARCH_PHASE)
            return selector ? _URC_HANDLER_FOUND : _URC_CONTINUE_UNWIND;
        if ((actions & _UA_CLEANUP_PHASE) && (cleanup || selector)) {
            if (!(actions & _UA_HANDLER_FRAME)) selector = 0;
            _Unwind_SetGR(context, __builtin_eh_return_data_regno(0), (uintptr_t)record);
            _Unwind_SetGR(context, __builtin_eh_return_data_regno(1), (uintptr_t)selector);
            _Unwind_SetIP(context, base + landing);
            return _URC_INSTALL_CONTEXT;
        }
        return _URC_CONTINUE_UNWIND;
    }
    return _URC_CONTINUE_UNWIND;
}
