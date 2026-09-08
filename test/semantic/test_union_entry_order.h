#ifndef FENG_TEST_UNION_ENTRY_ORDER_H
#define FENG_TEST_UNION_ENTRY_ORDER_H

#include <stddef.h>

/* Check the sole entry site's complete path for approved legacy migrations. */
void test_assert_union_entry_path(const char *source, const char *file,
                                  const size_t *indices, size_t count);

/* Run the independent two-pass union entry and literal eligibility matrices. */
void test_union_entry_order(void);

#endif
