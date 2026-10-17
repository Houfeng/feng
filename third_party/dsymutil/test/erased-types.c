/* Exercise independently emitted type graphs with no visible typed variables. */
#if defined(PROVIDER)
/* Tag and payload types referenced only through the archived record layout. */
enum RemoteTag { RemoteSmall = 1, RemoteLarge = 2 };
union RemoteValue { short small; long large; };
/* A tagged value whose second element requires the complete record stride. */
struct RemoteRecord { enum RemoteTag tag; union RemoteValue value; };

/* Return erased storage from an archive without a typed debug variable root. */
void *remote_records(void) {
    static struct RemoteRecord records[] __attribute__((nodebug)) = {
        {RemoteSmall, {.small = 31}}, {RemoteLarge, {.large = 9000000042L}}
    };
    return records;
}
#else
/* A nested field type retained through its containing record's dependencies. */
struct LocalPayload { short number; };
/* A record with internal padding before its double field. */
struct LocalRecord { unsigned char tag; struct LocalPayload payload; double weight; };
/* Obtain the opaque address produced by the archive translation unit. */
void *remote_records(void);

/* Keep both erased pointers visible at a stable native debugger breakpoint. */
__attribute__((noinline)) int inspect_records(void *local, void *remote) {
    return local != 0 && remote != 0 ? 0 : 1;
}

/* Materialize two elements with alignment padding and distinct field values. */
int main(void) {
    static struct LocalRecord records[] __attribute__((nodebug)) = {
        {1, {7}, 1.5}, {2, {19}, 4.25}
    };
    return inspect_records(records, remote_records());
}
#endif
