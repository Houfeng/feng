/* Shared observable state verifies cleanup across both translation units. */
extern int cleanup_count;

/* Record one scope exit during propagation or rethrow. */
struct Cleanup {
    /* Run the same observable cleanup on normal exit and exception unwinding. */
    ~Cleanup() { ++cleanup_count; }
};

#if defined(PROVIDER)
/* Keep a cross-unit throwing frame with a cleanup landing pad. */
__attribute__((noinline)) void throw_value(int value) {
#if defined(__APPLE__)
    /* Force a DWARF frame with a valid CFI no-op, matching upstream EH tests. */
    asm volatile(".cfi_escape 0");
#endif
    Cleanup cleanup;
    throw value;
}
#else
int cleanup_count = 0;

/* Resolve this public entry from a native or ThinLTO provider archive. */
void throw_value(int value);

/* Rethrow the original exception after running the intermediate cleanup. */
__attribute__((noinline)) static void relay() {
#if defined(__APPLE__)
    /* Keep the rethrow frame on the same DWARF path as the provider. */
    asm volatile(".cfi_escape 0");
#endif
    Cleanup cleanup;
    try {
        throw_value(37);
    } catch (int value) {
        if (value != 37) throw 99;
        throw;
    }
}

/* Verify the caught value and both cleanups, then repeat via an indirect call. */
int main() {
    try {
        relay();
    } catch (int value) {
        if (value != 37 || cleanup_count != 2) return 1;
    }
    if (cleanup_count != 2) return 2;
    void (*volatile call)(int) = throw_value;
    try {
        call(61);
    } catch (int value) {
        return value == 61 && cleanup_count == 3 ? 0 : 3;
    }
    return 4;
}
#endif
