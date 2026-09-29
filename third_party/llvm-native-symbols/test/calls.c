#if defined(__APPLE__)
#define NATIVE(name) __asm__("_" name)
#else
#define NATIVE(name) __asm__(name)
#endif
extern double feng_sqrt(double) NATIVE("sqrt");
extern int feng_integer(int) NATIVE("foreign_mixed");
extern double feng_double(double) NATIVE("foreign_mixed");
extern double feng_unknown(double) NATIVE("foreign_unknown");

/* This eligible alias must keep _sqrt for any residual call. */
double call_sqrt(double x) { return feng_sqrt(x); }
/* Taking the address must preserve the native function identity. */
double (*address_sqrt(void))(double) { return &feng_sqrt; }
/* Different ABI declarations must not collapse to a single canonical prototype. */
int call_integer(int x) { return feng_integer(x); }
/* Exercise the second declaration regardless of its source order. */
double call_double(double x) { return feng_double(x); }
/* Unknown library semantics remain unknown after name normalization. */
double call_unknown(double x) { return feng_unknown(x); }
