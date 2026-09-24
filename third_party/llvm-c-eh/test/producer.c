#include "runtime.h"

/* A separate translation unit prevents ordinary optimizer visibility. */
int ceh_test_produce(int value) {
    if (value < 0) ceh_test_raise(&ceh_test_key_a, -value);
    return value + 1;
}
