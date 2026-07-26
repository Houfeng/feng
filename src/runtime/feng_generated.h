/*
 * Platform-independent C declarations required by Feng-generated sources.
 *
 * Normal builds consume the target SDK/sysroot headers. SDK-free object
 * compilation uses the declaration-only subset below: it does not provide
 * implementations and therefore remains independent of any target libc.
 */
#ifndef FENG_GENERATED_H
#define FENG_GENERATED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(FENG_COMPILE_SDK_FREE)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FengCDivResult {
    int quot;
    int rem;
} div_t;

typedef struct FengCLDivResult {
    long quot;
    long rem;
} ldiv_t;

typedef struct FengCLLDivResult {
    long long quot;
    long long rem;
} lldiv_t;

/* <stdlib.h> declarations used by supported C interop surfaces. */
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void exit(int status);
void _Exit(int status);
void abort(void);
int atexit(void (*function)(void));
int at_quick_exit(void (*function)(void));
void quick_exit(int status);
int atoi(const char *text);
long atol(const char *text);
long long atoll(const char *text);
double atof(const char *text);
long strtol(const char *restrict text, char **restrict end, int base);
long long strtoll(const char *restrict text, char **restrict end, int base);
unsigned long strtoul(const char *restrict text, char **restrict end, int base);
unsigned long long strtoull(const char *restrict text, char **restrict end, int base);
float strtof(const char *restrict text, char **restrict end);
double strtod(const char *restrict text, char **restrict end);
long double strtold(const char *restrict text, char **restrict end);
int abs(int value);
long labs(long value);
long long llabs(long long value);
div_t div(int numerator, int denominator);
ldiv_t ldiv(long numerator, long denominator);
lldiv_t lldiv(long long numerator, long long denominator);
int rand(void);
void srand(unsigned int seed);
void qsort(void *base,
           size_t count,
           size_t size,
           int (*compare)(const void *, const void *));
void *bsearch(const void *key,
              const void *base,
              size_t count,
              size_t size,
              int (*compare)(const void *, const void *));
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int system(const char *command);

/* <string.h> declarations used by generated code and C interop. */
void *memcpy(void *restrict destination,
             const void *restrict source,
             size_t count);
void *memmove(void *destination, const void *source, size_t count);
void *memset(void *destination, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
void *memchr(const void *memory, int value, size_t count);
size_t strlen(const char *text);
char *strcpy(char *restrict destination, const char *restrict source);
char *strncpy(char *restrict destination,
              const char *restrict source,
              size_t count);
char *strcat(char *restrict destination, const char *restrict source);
char *strncat(char *restrict destination,
              const char *restrict source,
              size_t count);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strchr(const char *text, int value);
char *strrchr(const char *text, int value);
char *strstr(const char *text, const char *substring);
char *strtok(char *restrict text, const char *restrict delimiters);
char *strerror(int error_number);

/* <math.h> double-precision declarations plus generated f32 remainder. */
double fabs(double value);
double fmin(double left, double right);
double fmax(double left, double right);
double copysign(double magnitude, double sign);
double sin(double value);
double cos(double value);
double tan(double value);
double asin(double value);
double acos(double value);
double atan(double value);
double atan2(double y, double x);
double sinh(double value);
double cosh(double value);
double tanh(double value);
double exp(double value);
double exp2(double value);
double expm1(double value);
double log(double value);
double log2(double value);
double log10(double value);
double log1p(double value);
double pow(double base, double exponent);
double sqrt(double value);
double cbrt(double value);
double hypot(double x, double y);
double ceil(double value);
double floor(double value);
double round(double value);
double trunc(double value);
double fmod(double numerator, double denominator);
float fmodf(float numerator, float denominator);
double remainder(double numerator, double denominator);
double nan(const char *tag);
double fdim(double left, double right);
double frexp(double value, int *exponent);
double ldexp(double value, int exponent);
double modf(double value, double *integer_part);
double scalbn(double value, int exponent);
double scalbln(double value, long exponent);
double nearbyint(double value);
double rint(double value);
long lrint(double value);
long long llrint(double value);
long lround(double value);
long long llround(double value);

#define isinf(value) __builtin_isinf(value)
#define isnan(value) __builtin_isnan(value)

#ifdef __cplusplus
}
#endif

#else

#include <math.h>
#include <stdlib.h>
#include <string.h>

#endif

#endif /* FENG_GENERATED_H */
