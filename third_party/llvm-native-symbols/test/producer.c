/* A separately compiled native function whose aliases must share an address. */
int native_identity(int value) { return value + 3; }

static int call_count;

/* Unknown extern semantics must preserve observable writes and call ordering. */
int native_update(int value) { return value + ++call_count; }

/* Expose the side effect without making the producer visible to the consumer. */
int native_count(void) { return call_count; }
