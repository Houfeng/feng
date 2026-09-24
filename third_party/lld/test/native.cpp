/* An aggregate result exercises indirect calls that use an implicit result pointer. */
struct Result { long fields[8]; };

/* Keep a real native frame even in optimized control builds. */
__attribute__((noinline)) static Result produce() { throw 66; }

/* Force a legal frame whose preserved registers require DWARF unwind information. */
__attribute__((noinline, preserve_all)) static void preserved() {
    __asm__ volatile("" : : : "x9", "x10");
    throw 66;
}

/* Cover same-frame, indirect aggregate and mandatory-DWARF native C++ unwinding. */
int main() {
    try {
#if CASE == 1
        throw 66;
#elif CASE == 2
        Result (*volatile call)() = produce;
        (void)call();
#elif CASE == 3
        preserved();
#else
#error Unsupported test case
#endif
    } catch (int value) {
        return value == 66 ? 0 : 1;
    }
    return 2;
}
