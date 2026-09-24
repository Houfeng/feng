#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* Ordinary C with no protocol: loading the extension must not change this function. */
static int calculate(int value) {
    int total = 0;
    for (int i = 0; i < value; ++i) total += i * 2;
    return total;
}
static int (*volatile callback)(int) = calculate;

/* The optional undefined operation tests that the same sanitizer still diagnoses it. */
int main(int argc, char **argv) {
    if (argc > 1) {
        volatile int value = INT_MAX;
        return value + atoi(argv[1]);
    }
    int result = callback(8);
    printf("ordinary result=%d\n", result);
    return result != 56;
}
