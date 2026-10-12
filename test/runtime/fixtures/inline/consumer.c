/* Compile twice with distinct names, placing each object in its own archive. */
#include "consumer.h"
#include "feng_runtime.h"
_Static_assert(__alignof__(max_align_t) == _Alignof(max_align_t), "payload ABI alignment");
#define JOIN_IMPL(a, b) a##b
#define JOIN(a, b) JOIN_IMPL(a, b)
#define NAME(suffix) JOIN(CONSUMER, suffix)
/* Function addresses must resolve to the runtime archive's ordinary functions. */
LengthFn NAME(_length)(void) { return feng_array_length; }
/* Taking an address must not emit a private copy into this consumer. */
DataFn NAME(_data)(void) { return feng_array_data; }
/* Bounds checks retain their ordinary address even when calls are expanded. */
CheckFn NAME(_check)(void) { return feng_array_check_index; }
/* These are the same accessor calls that generated C uses for byte indexing. */
size_t NAME(_read)(FengArray *array, size_t index) {
    feng_array_check_index(array, index);
    return ((unsigned char *)feng_array_data(array))[index] + feng_array_length(array);
}
