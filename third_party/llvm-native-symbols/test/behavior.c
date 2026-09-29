#include <errno.h>
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

#if defined(__APPLE__)
#define NATIVE(name) __asm__("_" name)
#else
#define NATIVE(name) __asm__(name)
#endif
extern double feng_sqrt(double) NATIVE("sqrt");
extern int alias_first(int) NATIVE("native_identity");
extern int alias_second(int) NATIVE("native_identity");
extern int alias_update(int) NATIVE("native_update");
extern int native_count(void);

/* Check direct/indirect identity, unknown side effects and libm boundary results. */
int main(void) {
    int (*volatile first)(int) = alias_first;
    int (*volatile second)(int) = alias_second;
    if (first != second || first(7) != 10 || second(9) != 12) return 1;
    int a = alias_update(10);
    int b = alias_update(10);
    if (a != 11 || b != 12 || native_count() != 2) return 2;
    volatile double values[] = {0.0, -0.0, 1.0, 2.0, 4.0, -1.0,
                               INFINITY, -INFINITY, NAN, DBL_MIN, DBL_MAX,
                               DBL_TRUE_MIN, __builtin_nans("")};
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        if (fesetround(modes[mode]) != 0) return 3;
        for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            errno = 123;
            if (feclearexcept(FE_ALL_EXCEPT) != 0) return 4;
            double result = feng_sqrt(values[i]);
            int error = errno;
            int exceptions = fetestexcept(FE_ALL_EXCEPT);
            if (isnan(result)) printf("nan %d %d\n", error, exceptions);
            else printf("%a %d %d %d\n", result, !!signbit(result), error, exceptions);
        }
    }
    return 0;
}
