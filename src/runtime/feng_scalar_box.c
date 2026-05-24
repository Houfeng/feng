#include "runtime/feng_runtime.h"

const FengTypeDescriptor feng_scalar_box_descriptor = {
    .name = "feng.<internal>.scalar_box",
    .size = sizeof(FengScalarBox),
    .finalizer = NULL,
    .release_children = NULL,
    .is_potentially_cyclic = false,
    .managed_field_count = 0,
    .managed_fields = NULL,
};

#define FENG_SCALAR_EXCEPTION_DESCRIPTOR(symbol, runtime_name) \
    const FengTypeDescriptor symbol = { \
        .name = runtime_name, \
        .size = sizeof(FengScalarBox), \
        .finalizer = NULL, \
        .release_children = NULL, \
        .is_potentially_cyclic = false, \
        .managed_field_count = 0, \
        .managed_fields = NULL, \
    }

FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_bool_exception_descriptor, "bool");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_i8_exception_descriptor, "i8");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_i16_exception_descriptor, "i16");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_i32_exception_descriptor, "i32");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_i64_exception_descriptor, "i64");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_u8_exception_descriptor, "u8");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_u16_exception_descriptor, "u16");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_u32_exception_descriptor, "u32");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_u64_exception_descriptor, "u64");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_f32_exception_descriptor, "f32");
FENG_SCALAR_EXCEPTION_DESCRIPTOR(feng_scalar_f64_exception_descriptor, "f64");

#undef FENG_SCALAR_EXCEPTION_DESCRIPTOR

static FengScalarBox *feng_scalar_box_new_empty(FengBuiltinScalarKind kind) {
    FengScalarBox *box = (FengScalarBox *)feng_object_new(&feng_scalar_box_descriptor);
    box->kind = kind;
    return box;
}

FengScalarBox *feng_scalar_box_new_bool(bool value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_BOOL);
    box->payload.b = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_i8(int8_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_I8);
    box->payload.i8 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_i16(int16_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_I16);
    box->payload.i16 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_i32(int32_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_I32);
    box->payload.i32 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_i64(int64_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_I64);
    box->payload.i64 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_u8(uint8_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_U8);
    box->payload.u8 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_u16(uint16_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_U16);
    box->payload.u16 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_u32(uint32_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_U32);
    box->payload.u32 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_u64(uint64_t value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_U64);
    box->payload.u64 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_f32(float value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_F32);
    box->payload.f32 = value;
    return box;
}

FengScalarBox *feng_scalar_box_new_f64(double value) {
    FengScalarBox *box = feng_scalar_box_new_empty(FENG_BUILTIN_SCALAR_F64);
    box->payload.f64 = value;
    return box;
}
