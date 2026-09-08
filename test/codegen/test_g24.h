#ifndef FENG_TEST_CODEGEN_G24_H
#define FENG_TEST_CODEGEN_G24_H

#include <stdbool.h>

/* Verify that one named empty descriptor has static storage and is passed
 * descriptor-first to the expected callable without changing its arguments. */
bool g24_has_static_descriptor_call(const char *source, const char *callee,
                                    const char *display, const char *arguments);

/* Exercise static descriptor storage, identity and dependency closure. */
void test_g24_static_descriptors(void (*compile_c)(const char *));

#endif
