#include "runtime/feng_exception_lsda.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LSDA_CHECK(value) do { if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); abort(); \
} } while (0)

/* Unaligned tables and nearby relocation slots, independent of linker output. */
typedef struct LsdaFixture {
    unsigned char bytes[128];
    uintptr_t slot;
    size_t size;
} LsdaFixture;

/* Construct a one-handler table with a caller-selected native pointer encoding. */
static void lsda_fixture(LsdaFixture *fixture, unsigned encoding, uintptr_t key) {
    memset(fixture, 0, sizeof(*fixture));
    size_t width = (encoding & 15U) == 11U ? 4U : sizeof(uintptr_t);
    const unsigned char prefix[] = {0xff, 0, 0, 1, 4, 0, 10, 20, 1, 1, 0};
    memcpy(fixture->bytes, prefix, sizeof prefix);
    fixture->size = sizeof prefix + width;
    fixture->bytes[1] = (unsigned char)encoding;
    fixture->bytes[2] = (unsigned char)(fixture->size - 3U);
    uintptr_t value = key;
    if (encoding & 0x80U) {
        fixture->slot = key;
        value = (uintptr_t)&fixture->slot;
    }
    if (value != 0U && (encoding & 0x70U) == 0x10U)
        value -= (uintptr_t)(fixture->bytes + sizeof prefix);
    if (value != 0U && (encoding & 0x70U) == 0x40U)
        value -= (uintptr_t)fixture->bytes;
    if (width == 4U) {
        int32_t field = (int32_t)value;
        memcpy(fixture->bytes + sizeof prefix, &field, sizeof field);
    } else {
        memcpy(fixture->bytes + sizeof prefix, &value, width);
    }
}

/* Resolve one table using an explicit outer bound, including malformed inputs. */
static bool lsda_find(const LsdaFixture *fixture, uintptr_t offset,
                      uintptr_t key, bool allow, FengExceptionLanding *landing) {
    uintptr_t base = (uintptr_t)fixture->bytes;
    return feng_exception_lsda_find(fixture->bytes, fixture->bytes + fixture->size,
                                    base, base + offset, (const void *)key, allow, landing);
}

/* Required native encodings, exact identity, catch-all, search suppression and
 * half-open call-site ranges all have independent expected results. */
static void lsda_valid_tables(void) {
    static const unsigned encodings[] = {0, 0x1b, 0x1c, 0x9b, 0x9c, 0x4c};
    for (size_t i = 0U; i < sizeof(encodings) / sizeof(encodings[0]); ++i) {
        LsdaFixture fixture;
        uintptr_t key = (uintptr_t)&fixture.bytes[1];
        lsda_fixture(&fixture, encodings[i], key);
        FengExceptionLanding landing;
        LSDA_CHECK(lsda_find(&fixture, 0U, key, true, &landing));
        LSDA_CHECK(landing.address == (uintptr_t)fixture.bytes + 20U);
        LSDA_CHECK(landing.selector == 1 && !landing.cleanup);
        LSDA_CHECK(lsda_find(&fixture, 9U, key, true, &landing) && landing.selector == 1);
        LSDA_CHECK(lsda_find(&fixture, 10U, key, true, &landing) && landing.address == 0U);
        LSDA_CHECK(lsda_find(&fixture, 0U, key + 1U, true, &landing) && landing.selector == 0);
        LSDA_CHECK(lsda_find(&fixture, 0U, key, false, &landing) && landing.selector == 0);
        /* Every truncated header, action or type entry must fail closed. */
        for (size_t length = 0U; length < fixture.size; ++length) {
            LSDA_CHECK(!feng_exception_lsda_find(fixture.bytes, fixture.bytes + length,
                (uintptr_t)fixture.bytes, (uintptr_t)fixture.bytes, (void *)key, true, &landing));
        }
        lsda_fixture(&fixture, encodings[i], 0U);
        LSDA_CHECK(lsda_find(&fixture, 0U, 123U, true, &landing) && landing.selector == 1);
    }
    const unsigned char cleanup[] = {0xff, 0xff, 1, 4, 0, 10, 20, 0};
    FengExceptionLanding landing;
    LSDA_CHECK(feng_exception_lsda_find(cleanup, cleanup + sizeof cleanup, 100U, 109U,
                                       NULL, true, &landing));
    LSDA_CHECK(landing.cleanup && landing.selector == 0 && landing.address == 120U);
    LSDA_CHECK(feng_exception_lsda_find(NULL, NULL, 0U, 0U, NULL, true, &landing));
    LSDA_CHECK(landing.address == 0U && landing.selector == 0 && !landing.cleanup);
}

/* Native selector indices are not source clause positions. Linked actions
 * preserve match priority, including a backwards edge to a shared tail. */
static void lsda_action_chains(void) {
    LsdaFixture fixture = {0};
    const unsigned char prefix[] = {0xff, 0, 0, 1, 4, 0, 10, 20, 1, 2, 1, 1, 1, 0, 0};
    memcpy(fixture.bytes, prefix, sizeof prefix);
    fixture.size = sizeof prefix + 2U * sizeof(uintptr_t);
    fixture.bytes[2] = (unsigned char)(fixture.size - 3U);
    uintptr_t first = 11U, second = 22U;
    memcpy(fixture.bytes + sizeof prefix, &first, sizeof first);
    memcpy(fixture.bytes + sizeof prefix + sizeof first, &second, sizeof second);
    FengExceptionLanding landing;
    LSDA_CHECK(lsda_find(&fixture, 1U, first, true, &landing));
    LSDA_CHECK(landing.selector == 2 && landing.cleanup);
    LSDA_CHECK(lsda_find(&fixture, 1U, second, true, &landing));
    LSDA_CHECK(landing.selector == 1 && landing.cleanup);
    LSDA_CHECK(lsda_find(&fixture, 1U, 33U, true, &landing));
    LSDA_CHECK(landing.selector == 0 && landing.cleanup);
    LSDA_CHECK(lsda_find(&fixture, 1U, first, false, &landing));
    LSDA_CHECK(landing.selector == 0 && landing.cleanup);
    /* Point the first entry at the later handler, then return to cleanup. */
    fixture.bytes[8] = 5U;
    fixture.bytes[9] = 0U; fixture.bytes[10] = 0U;
    fixture.bytes[13] = 1U; fixture.bytes[14] = 0x7bU; /* -5 from byte 14 to 9 */
    LSDA_CHECK(lsda_find(&fixture, 1U, second, true, &landing));
    LSDA_CHECK(landing.selector == 1 && landing.cleanup);
    fixture.bytes[14] = 0x7fU; /* cycle back to the same action */
    LSDA_CHECK(!lsda_find(&fixture, 1U, second, true, &landing));
}

/* An explicit landing-pad base and fixed-width call sites are independent of
 * the function start; an earlier unprotected call must not hide later entries. */
static void lsda_explicit_bases(void) {
    const unsigned applications[] = {0U, 0x50U};
    for (size_t i = 0U; i < sizeof applications / sizeof applications[0]; ++i) {
        LsdaFixture fixture = {0};
        fixture.bytes[0] = (unsigned char)applications[i];
        size_t cursor = applications[i] == 0U ? 1U : sizeof(uintptr_t);
        uintptr_t base = 5000U;
        memcpy(fixture.bytes + cursor, &base, sizeof base);
        cursor += sizeof base;
        fixture.bytes[cursor++] = 0xffU;
        fixture.bytes[cursor++] = 3U;
        fixture.bytes[cursor++] = 26U;
        const uint32_t sites[][3] = {{0U, 5U, 0U}, {5U, 10U, 30U}};
        for (size_t site = 0U; site < 2U; ++site) {
            memcpy(fixture.bytes + cursor, sites[site], sizeof sites[site]);
            cursor += sizeof sites[site];
            fixture.bytes[cursor++] = 0U;
        }
        fixture.size = cursor;
        FengExceptionLanding landing;
        LSDA_CHECK(feng_exception_lsda_find(fixture.bytes, fixture.bytes + cursor,
            1000U, base + 2U, NULL, true, &landing));
        LSDA_CHECK(landing.address == 0U);
        LSDA_CHECK(feng_exception_lsda_find(fixture.bytes, fixture.bytes + cursor,
            1000U, base + 5U, NULL, true, &landing));
        LSDA_CHECK(landing.address == base + 30U && landing.cleanup && landing.selector == 0);
        LSDA_CHECK(feng_exception_lsda_find(fixture.bytes, fixture.bytes + cursor,
            1000U, base + 15U, NULL, true, &landing));
        LSDA_CHECK(landing.address == 0U);
    }
}

/* Reject unsupported bases, native filters and overflowing/truncated metadata. */
static void lsda_invalid_tables(void) {
    LsdaFixture fixture;
    FengExceptionLanding landing;
    const struct { size_t index; unsigned char value; } invalid[] = {
        {0, 0x21}, {1, 0x01}, {1, 0x20}, {1, 0x30}, {2, 0}, {2, 127},
        {3, 0xff}, {4, 127}, {8, 127}, {9, 0x7f}, {9, 127}, {10, 127}
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        lsda_fixture(&fixture, 0U, 42U);
        fixture.bytes[invalid[i].index] = invalid[i].value;
        LSDA_CHECK(!lsda_find(&fixture, 1U, 42U, true, &landing));
    }
    lsda_fixture(&fixture, 0U, 42U);
    fixture.bytes[7] = 0U; /* A call-site entry without a landing is legal. */
    LSDA_CHECK(lsda_find(&fixture, 1U, 42U, true, &landing) && landing.address == 0U);
    const unsigned char overflow[] = {0xff, 0xff, 1,
        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02};
    LSDA_CHECK(!feng_exception_lsda_find(overflow, overflow + sizeof overflow,
                                        0U, 0U, NULL, true, &landing));
    lsda_fixture(&fixture, 0U, 42U);
    fixture.bytes[4] = 3U; /* call-site length stops before its action offset */
    LSDA_CHECK(!lsda_find(&fixture, 1U, 42U, true, &landing));
    const unsigned char wrapped[] = {0xff, 0xff, 1, 4, 1, 10, 20, 0};
    LSDA_CHECK(!feng_exception_lsda_find(wrapped, wrapped + sizeof wrapped,
                                        UINTPTR_MAX, 0U, NULL, true, &landing));
    /* A signed action offset cannot overflow merely by sign extension. */
    lsda_fixture(&fixture, 0U, 42U);
    fixture.size = 40U;
    fixture.bytes[2] = (unsigned char)(fixture.size - 3U);
    memset(fixture.bytes + 10U, 0x80, 9U);
    fixture.bytes[19] = 2U;
    LSDA_CHECK(!lsda_find(&fixture, 1U, 42U, true, &landing));
    fixture.bytes[19] = 0x7eU;
    LSDA_CHECK(!lsda_find(&fixture, 1U, 42U, true, &landing));
    LSDA_CHECK(!feng_exception_lsda_find(NULL, NULL, 0U, 0U, NULL, true, NULL));
}

/* Run independently of the host's chosen native unwind-table encoding. */
void test_exception_lsda(void) {
    lsda_valid_tables();
    lsda_action_chains();
    lsda_explicit_bases();
    lsda_invalid_tables();
    puts("native LSDA encodings, actions, bounds and malformed metadata passed");
}
