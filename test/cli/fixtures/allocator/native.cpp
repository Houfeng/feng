/* Exercise C++ allocation entry points against the same native override archive. */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#if defined(__APPLE__)
#define ALLOCATOR_WEAK __attribute__((weak_import))
#else
#define ALLOCATOR_WEAK __attribute__((weak))
#endif
extern "C" bool mi_is_in_heap_region(const void *) ALLOCATOR_WEAK;

/* Ordinary scalar, array and nothrow new/delete must all select the same heap. */
int main() {
    assert(mi_is_in_heap_region != nullptr);
    int *one = new int(17);
    assert(*one == 17 && mi_is_in_heap_region(one));
    delete one;
    int *many = new int[79]{};
    assert(many[78] == 0 && mi_is_in_heap_region(many));
    delete[] many;
    void *optional = ::operator new(123, std::nothrow);
    assert(optional != nullptr && mi_is_in_heap_region(optional));
    ::operator delete(optional, std::nothrow);
    void *aligned = ::operator new(192, std::align_val_t(64));
    assert(mi_is_in_heap_region(aligned) && reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
    ::operator delete(aligned, std::align_val_t(64));
}
