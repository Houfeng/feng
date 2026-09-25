#include "runtime/feng_exception_lsda.h"

#include <limits.h>
#include <string.h>

/* Cursor bounds are integer addresses so malformed offsets never form a C
 * pointer outside an object before their range has been checked. */
typedef struct LsdaReader {
    uintptr_t cursor;
    uintptr_t end;
} LsdaReader;

/* Copy a native, possibly unaligned field after validating its full extent. */
static bool lsda_read(LsdaReader *reader, void *out, size_t size) {
    if (reader->cursor > reader->end || size > reader->end - reader->cursor)
        return false;
    memcpy(out, (const void *)reader->cursor, size);
    reader->cursor += size;
    return true;
}

/* Add a nonnegative metadata offset without wrapping the address space. */
static bool lsda_add(uintptr_t base, uintptr_t offset, uintptr_t *out) {
    if (offset > UINTPTR_MAX - base) return false;
    *out = base + offset;
    return true;
}

/* Signed PC-relative and action offsets may move in either direction. */
static bool lsda_add_signed(uintptr_t base, intptr_t offset, uintptr_t *out) {
    if (offset >= 0) return lsda_add(base, (uintptr_t)offset, out);
    uintptr_t magnitude = (uintptr_t)0 - (uintptr_t)offset;
    if (magnitude > base) return false;
    *out = base - magnitude;
    return true;
}

/* Reject both unterminated and overflowing unsigned LEB128 values. */
static bool lsda_uleb(LsdaReader *reader, uintptr_t *out) {
    uintptr_t value = 0;
    for (unsigned shift = 0; shift < sizeof(value) * CHAR_BIT; shift += 7U) {
        unsigned char byte;
        if (!lsda_read(reader, &byte, 1U)) return false;
        uintptr_t payload = byte & 0x7fU;
        if (payload > (UINTPTR_MAX >> shift)) return false;
        value |= payload << shift;
        if (!(byte & 0x80U)) { *out = value; return true; }
    }
    return false;
}

/* Preserve sign bits without shifting a negative C integer. */
static bool lsda_sleb(LsdaReader *reader, intptr_t *out) {
    uintptr_t value = 0;
    const unsigned bits = sizeof(value) * CHAR_BIT;
    for (unsigned shift = 0; shift < bits; shift += 7U) {
        unsigned char byte;
        if (!lsda_read(reader, &byte, 1U)) return false;
        uintptr_t payload = byte & 0x7fU;
        unsigned remaining = bits - shift;
        if (remaining <= 7U) {
            uintptr_t high = payload >> (remaining - 1U);
            uintptr_t expected = (byte & 0x40U) ? (0x7fU >> (remaining - 1U)) : 0U;
            if (high != expected || (byte & 0x80U)) return false;
        }
        value |= payload << shift;
        if (!(byte & 0x80U)) {
            if ((byte & 0x40U) && remaining > 7U) value |= UINTPTR_MAX << (shift + 7U);
            *out = (intptr_t)value;
            return true;
        }
    }
    return false;
}

/* Type tables are indexed backwards and therefore require fixed-size fields. */
static size_t lsda_width(unsigned encoding) {
    switch (encoding & 0x0fU) {
    case 0: return sizeof(uintptr_t);
    case 2: case 10: return 2U;
    case 3: case 11: return 4U;
    case 4: case 12: return 8U;
    default: return 0U;
    }
}

/* Decode the native DW_EH_PE scalar representation, with no alignment assumption. */
static bool lsda_scalar(LsdaReader *reader, unsigned encoding, uintptr_t *out) {
    if ((encoding & 0x0fU) == 1U) return lsda_uleb(reader, out);
    if ((encoding & 0x0fU) == 9U) {
        intptr_t value;
        if (!lsda_sleb(reader, &value)) return false;
        *out = (uintptr_t)value;
        return true;
    }
    size_t width = lsda_width(encoding);
    if (width == 0U || width > sizeof(uintptr_t)) return false;
    uintptr_t value;
    if (width == 2U) {
        uint16_t field;
        if (!lsda_read(reader, &field, sizeof field)) return false;
        value = field;
    } else if (width == 4U) {
        uint32_t field;
        if (!lsda_read(reader, &field, sizeof field)) return false;
        value = field;
    } else {
        if (!lsda_read(reader, &value, sizeof value)) return false;
    }
    if ((encoding & 8U) && width < sizeof(value) &&
        (value & ((uintptr_t)1U << (width * CHAR_BIT - 1U))))
        value |= UINTPTR_MAX << (width * CHAR_BIT);
    *out = value;
    return true;
}

/* Decode absolute, PC-relative, function-relative or aligned pointers. The
 * current targets have no text/data-relative base contract; reject those. */
static bool lsda_pointer(LsdaReader *reader, unsigned encoding,
                          uintptr_t function, uintptr_t *out) {
    if (encoding == 0xffU) return false;
    unsigned application = encoding & 0x70U;
    if (application == 0x50U) {
        uintptr_t rounded;
        if (!lsda_add(reader->cursor, sizeof(uintptr_t) - 1U, &rounded)) return false;
        reader->cursor = rounded & ~(uintptr_t)(sizeof(uintptr_t) - 1U);
    } else if (application != 0U && application != 0x10U && application != 0x40U) {
        return false;
    }
    uintptr_t field = reader->cursor, value;
    if (!lsda_scalar(reader, encoding, &value)) return false;
    if (value == 0U) { *out = 0U; return true; }
    if (application == 0x10U || application == 0x40U) {
        uintptr_t base = application == 0x10U ? field : function;
        if (encoding & 8U) {
            if (!lsda_add_signed(base, (intptr_t)value, &value)) return false;
        } else if (!lsda_add(base, value, &value)) return false;
    }
    /* Indirection references a compiler-emitted relocation slot outside the
     * LSDA itself. Its validity belongs to the loader, like the unwind tables. */
    if (encoding & 0x80U) memcpy(&value, (const void *)value, sizeof value);
    *out = value;
    return true;
}

/* Walk a bounded action chain. Revisited entries exhaust the byte budget,
 * detecting cycles without allocating during exception propagation. */
static bool lsda_actions(uintptr_t start, uintptr_t types, uintptr_t offset,
                          unsigned type_encoding, uintptr_t function,
                          const void *thrown_type, bool allow_catch,
                          FengExceptionLanding *out) {
    size_t width = lsda_width(type_encoding);
    uintptr_t entry, action_end = types;
    if (types <= start || width == 0U || !lsda_add(start, offset - 1U, &entry)) return false;
    uintptr_t budget = types - start;
    while (budget-- != 0U) {
        if (entry < start || entry >= action_end) return false;
        LsdaReader reader = {entry, action_end};
        intptr_t filter, next;
        if (!lsda_sleb(&reader, &filter)) return false;
        uintptr_t next_base = reader.cursor;
        if (!lsda_sleb(&reader, &next)) return false;
        if (filter == 0) {
            out->cleanup = true;
        } else if (filter > 0) {
            if (filter > INT32_MAX) return false;
            if ((uintptr_t)filter > (types - start) / width) return false;
            uintptr_t type_entry = types - (uintptr_t)filter * width;
            if (type_entry < reader.cursor) return false;
            if (type_entry < action_end) action_end = type_entry;
            LsdaReader type = {type_entry, types};
            uintptr_t key;
            if (!lsda_pointer(&type, type_encoding, function, &key)) return false;
            if (allow_catch && out->selector == 0 &&
                (key == 0U || key == (uintptr_t)thrown_type)) out->selector = filter;
        } else {
            /* Feng does not emit native C++ exception-specification filters. */
            return false;
        }
        if (next == 0) return true;
        if (!lsda_add_signed(next_base, next, &entry)) return false;
    }
    return false;
}

/* Interpret only the call site containing IP; selectors remain native indices
 * and are translated to source clause indices by the generated handler. */
bool feng_exception_lsda_find(const unsigned char *data,
                              const unsigned char *limit,
                              uintptr_t region_start, uintptr_t ip,
                              const void *thrown_type, bool allow_catch,
                              FengExceptionLanding *out) {
    if (out == NULL) return false;
    *out = (FengExceptionLanding){0};
    if (data == NULL) return true;
    LsdaReader reader = {(uintptr_t)data, limit != NULL ? (uintptr_t)limit : UINTPTR_MAX};
    unsigned char lp_encoding, type_encoding, call_encoding;
    uintptr_t base = region_start, types = 0U, length, end;
    if (!lsda_read(&reader, &lp_encoding, 1U)) return false;
    if (lp_encoding != 0xffU && !lsda_pointer(&reader, lp_encoding, region_start, &base)) return false;
    if (!lsda_read(&reader, &type_encoding, 1U)) return false;
    if (type_encoding != 0xffU) {
        uintptr_t offset;
        if (lsda_width(type_encoding) == 0U || !lsda_uleb(&reader, &offset) ||
            !lsda_add(reader.cursor, offset, &types) || types > reader.end) return false;
        reader.end = types;
    }
    if (!lsda_read(&reader, &call_encoding, 1U) || (call_encoding & 0xf0U) ||
        !lsda_uleb(&reader, &length) || !lsda_add(reader.cursor, length, &end) ||
        end > reader.end) return false;
    reader.end = end;
    while (reader.cursor < end) {
        uintptr_t start, span, landing, action, first, last;
        if (!lsda_scalar(&reader, call_encoding, &start) ||
            !lsda_scalar(&reader, call_encoding, &span) ||
            !lsda_scalar(&reader, call_encoding, &landing) ||
            !lsda_uleb(&reader, &action) || !lsda_add(base, start, &first) ||
            !lsda_add(first, span, &last)) return false;
        if (ip < first || ip >= last) continue;
        if (landing == 0U) return true;
        if (!lsda_add(base, landing, &out->address)) return false;
        if (action == 0U) { out->cleanup = true; return true; }
        return lsda_actions(end, types, action, type_encoding, region_start,
                            thrown_type, allow_catch, out);
    }
    return true;
}
