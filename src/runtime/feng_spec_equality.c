/* Compare object-form spec subjects by their managed dynamic descriptor.
 * Descriptor-specific equality handles strings and value boxes; descriptors
 * without an equality function retain managed-object identity semantics. */
#include "runtime/feng_runtime.h"

bool feng_spec_subject_equal(void *left, void *right) {
    FengManagedHeader *lh;
    FengManagedHeader *rh;

    if (left == right) return true;
    if (left == NULL || right == NULL) return false;

    lh = (FengManagedHeader *)left;
    rh = (FengManagedHeader *)right;

    if (lh->desc != rh->desc) return false;

    if (lh->desc != NULL && lh->desc->equal_fn != NULL) {
        return lh->desc->equal_fn(left, right);
    }

    return false;
}
