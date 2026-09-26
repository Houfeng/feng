/* Dynamic UTF-8 input must own a normal ARC reference, unlike source literals. */
#include "runtime/feng_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

/* Check exact byte copies, trailing NUL and independent, reclaimable ownership. */
void test_string_owned_utf8(void) {
    static const char utf8[] = "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x8c\x8d";
    static const char embedded_nul[] = {'a', '\0', 'b'};
    const char *inputs[] = {NULL, "", "plain", utf8, embedded_nul};
    const size_t lengths[] = {0U, 0U, 5U, sizeof utf8 - 1U, sizeof embedded_nul};

    for (size_t i = 0U; i < sizeof lengths / sizeof lengths[0]; ++i) {
        FengString *first = feng_string_from_utf8(inputs[i], lengths[i]);
        FengString *second = feng_string_from_utf8(inputs[i], lengths[i]);
        FengManagedHeader *header = (FengManagedHeader *)first;
        CHECK(first != second);
        CHECK(header->desc == &feng_string_descriptor);
        CHECK(header->tag == FENG_TYPE_TAG_STRING && header->refcount == 1U);
        CHECK(feng_string_length(first) == lengths[i]);
        CHECK(feng_string_data(first)[lengths[i]] == '\0');
        CHECK(lengths[i] == 0U ||
              memcmp(feng_string_data(first), inputs[i], lengths[i]) == 0);
        CHECK(feng_spec_subject_equal(first, second));
        feng_retain(first);
        CHECK(header->refcount == 2U);
        feng_release(first);
        CHECK(header->refcount == 1U);
        feng_release(second);
        feng_release(first);
    }

    char buffer[] = "copied";
    FengString *copy = feng_string_from_utf8(buffer, sizeof buffer - 1U);
    buffer[0] = 'X';
    CHECK(strcmp(feng_string_data(copy), "copied") == 0);

    /* Moving +1 into an array and retaining a borrowed element models argv. */
    FengArray *array = feng_array_new_kinded(FENG_VALUE_MANAGED_POINTER, NULL,
                                            &feng_string_descriptor,
                                            sizeof(FengString *), 1U);
    ((FengString **)feng_array_data(array))[0] = copy;
    feng_retain(copy);
    CHECK(((FengManagedHeader *)copy)->refcount == 2U);
    feng_release(array);
    CHECK(((FengManagedHeader *)copy)->refcount == 1U);
    CHECK(strcmp(feng_string_data(copy), "copied") == 0);
    feng_release(copy);
}
