#include "runtime/feng_runtime.h"

#include <stddef.h>
#include <stdlib.h>

typedef struct FengLSDARegistration {
    const FengLSDA *regions;
    int region_count;
} FengLSDARegistration;

static _Thread_local FengCleanupNode *g_cleanup_top = NULL;
static _Thread_local FengUnwindException *g_current_unwind = NULL;
static _Thread_local size_t g_finalizer_depth = 0U;

static FengLSDARegistration *g_lsda_registrations = NULL;
static size_t g_lsda_registration_count = 0U;
static size_t g_lsda_registration_capacity = 0U;

#if !defined(_WIN32)
const FengLSDA feng_empty_function_lsda[1] = {{0}};
#endif

static void feng_cleanup_release_node(FengCleanupNode *node) {
    void **slot;

    if (node == NULL) {
        return;
    }
    slot = node->slot;
    if (slot != NULL && *slot != NULL) {
        feng_release(*slot);
        *slot = NULL;
    }
}

static void feng_cleanup_release_to_frame_marker(void) {
    while (g_cleanup_top != NULL) {
        FengCleanupNode *node = g_cleanup_top;
        g_cleanup_top = node->prev;
        if (node->slot == NULL) {
            FengFrameMarker *marker = (FengFrameMarker *)((char *)node - offsetof(FengFrameMarker, node));
            if (marker->is_function_boundary) {
                return;
            }
            continue;
        }
        feng_cleanup_release_node(node);
    }
}

static void feng_cleanup_release_all(void) {
    while (g_cleanup_top != NULL) {
        FengCleanupNode *node = g_cleanup_top;
        g_cleanup_top = node->prev;
        feng_cleanup_release_node(node);
    }
}

#if !defined(_WIN32)
static void feng_unwind_exception_cleanup(_Unwind_Reason_Code reason,
                                          struct _Unwind_Exception *unwind) {
    FengUnwindException *exception;

    (void)reason;
    if (unwind == NULL) {
        return;
    }
    exception = (FengUnwindException *)((char *)unwind - offsetof(FengUnwindException, unwind));
    if (g_current_unwind == exception) {
        g_current_unwind = NULL;
    }
    if (exception->value != NULL) {
        feng_release(exception->value);
        exception->value = NULL;
    }
    free(exception);
}
#endif

static void feng_unwind_exception_init(FengUnwindException *exception,
                                       void *value,
                                       const FengTypeDescriptor *desc) {
#if defined(_WIN32)
    exception->exception_class = FENG_EXCEPTION_CLASS;
#else
    exception->unwind.exception_class = FENG_EXCEPTION_CLASS;
    exception->unwind.exception_cleanup = feng_unwind_exception_cleanup;
#endif
    exception->value = value;
    exception->desc = desc;
    exception->matched_clause = -1;
}

static void feng_release_current_unwind_exception(bool release_value) {
    FengUnwindException *exception = g_current_unwind;

    if (exception == NULL) {
        return;
    }
    g_current_unwind = NULL;
    if (release_value && exception->value != NULL) {
        feng_release(exception->value);
        exception->value = NULL;
    }
    free(exception);
}

void feng_register_lsda(const FengLSDA *regions, int region_count) {
    FengLSDARegistration *resized;

    if (regions == NULL || region_count <= 0) {
        return;
    }
    if (g_lsda_registration_count == g_lsda_registration_capacity) {
        size_t new_capacity = g_lsda_registration_capacity == 0U
                                  ? 8U
                                  : g_lsda_registration_capacity * 2U;
        resized = (FengLSDARegistration *)realloc(
            g_lsda_registrations, new_capacity * sizeof(*g_lsda_registrations));
        if (resized == NULL) {
            feng_panic("feng_register_lsda: out of memory");
        }
        g_lsda_registrations = resized;
        g_lsda_registration_capacity = new_capacity;
    }
    g_lsda_registrations[g_lsda_registration_count].regions = regions;
    g_lsda_registrations[g_lsda_registration_count].region_count = region_count;
    g_lsda_registration_count++;
}

void feng_cleanup_push(FengCleanupNode *node, void **slot) {
    if (node == NULL) {
        feng_panic("feng_cleanup_push: NULL node");
    }
    if (slot == NULL) {
        feng_panic("feng_cleanup_push: NULL slot");
    }
    node->slot = slot;
    node->prev = g_cleanup_top;
    g_cleanup_top = node;
}

void feng_cleanup_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_cleanup_pop: chain underflow");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

void feng_frame_push(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_frame_push: NULL marker");
    }
    marker->node.slot = NULL;
    marker->node.prev = g_cleanup_top;
    marker->is_function_boundary = true;
    g_cleanup_top = &marker->node;
}

void feng_try_frame_push(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_try_frame_push: NULL marker");
    }
    marker->node.slot = NULL;
    marker->node.prev = g_cleanup_top;
    marker->is_function_boundary = false;
    g_cleanup_top = &marker->node;
}

void feng_frame_pop(void) {
    if (g_cleanup_top == NULL) {
        feng_panic("feng_frame_pop: chain underflow");
    }
    if (g_cleanup_top->slot != NULL) {
        feng_panic("feng_frame_pop: top cleanup node is not a frame marker");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

void feng_frame_release_to(FengFrameMarker *marker) {
    if (marker == NULL) {
        feng_panic("feng_frame_release_to: NULL marker");
    }
    while (g_cleanup_top != NULL && g_cleanup_top != &marker->node) {
        FengCleanupNode *node = g_cleanup_top;
        g_cleanup_top = node->prev;
        feng_cleanup_release_node(node);
    }
    if (g_cleanup_top != &marker->node) {
        feng_panic("feng_frame_release_to: marker is not on cleanup chain");
    }
    g_cleanup_top = g_cleanup_top->prev;
}

void feng_exception_enter_finalizer(void) {
    g_finalizer_depth++;
}

void feng_exception_leave_finalizer(void) {
    if (g_finalizer_depth == 0U) {
        feng_panic("feng_exception_leave_finalizer: depth underflow");
    }
    g_finalizer_depth--;
}

#if !defined(_WIN32)
static bool feng_ip_in_region(uintptr_t ip, const FengLSDA *region) {
    uintptr_t begin;
    uintptr_t end;

    if (region == NULL || region->pc_begin == NULL || region->pc_end == NULL) {
        return false;
    }
    begin = (uintptr_t)region->pc_begin;
    end = (uintptr_t)region->pc_end;
    return ip >= begin && ip <= end;
}

static bool feng_region_matches_exception(const FengLSDA *region,
                                          const FengUnwindException *exception,
                                          int *out_clause) {
    int clause_index;

    if (region == NULL || exception == NULL || region->clauses == NULL) {
        return false;
    }
    for (clause_index = 0; clause_index < region->clause_count; ++clause_index) {
        const FengCatchClause *clause = &region->clauses[clause_index];
        if (clause->type == NULL || clause->type == exception->desc) {
            if (out_clause != NULL) {
                *out_clause = clause_index;
            }
            return true;
        }
    }
    return false;
}

static const FengLSDA *feng_find_matching_region(uintptr_t ip,
                                                 const FengUnwindException *exception,
                                                 int *out_clause) {
    size_t registration_index;

    for (registration_index = g_lsda_registration_count; registration_index > 0U; --registration_index) {
        const FengLSDARegistration *registration =
            &g_lsda_registrations[registration_index - 1U];
        int region_index;

        for (region_index = registration->region_count; region_index > 0; --region_index) {
            const FengLSDA *region = &registration->regions[region_index - 1];
            if (feng_ip_in_region(ip, region) &&
                feng_region_matches_exception(region, exception, out_clause)) {
                return region;
            }
        }
    }
    return NULL;
}

_Unwind_Reason_Code __feng_personality_v0(int version,
                                          _Unwind_Action actions,
                                          uint64_t exception_class,
                                          struct _Unwind_Exception *unwind,
                                          struct _Unwind_Context *context) {
    FengUnwindException *exception;
    uintptr_t ip;
    int matched_clause = -1;
    const FengLSDA *region;

    if (version != 1 || unwind == NULL || context == NULL ||
        exception_class != FENG_EXCEPTION_CLASS) {
        return _URC_CONTINUE_UNWIND;
    }

    exception = (FengUnwindException *)((char *)unwind - offsetof(FengUnwindException, unwind));
    ip = (uintptr_t)_Unwind_GetIP(context);
    region = feng_find_matching_region(ip, exception, &matched_clause);

    if ((actions & _UA_SEARCH_PHASE) != 0) {
        if (region != NULL) {
            exception->matched_clause = matched_clause;
            return _URC_HANDLER_FOUND;
        }
        return _URC_CONTINUE_UNWIND;
    }

    if ((actions & _UA_CLEANUP_PHASE) != 0 && (actions & _UA_HANDLER_FRAME) == 0) {
        feng_cleanup_release_to_frame_marker();
        return _URC_CONTINUE_UNWIND;
    }

    if ((actions & _UA_HANDLER_FRAME) != 0 && region != NULL) {
        exception->matched_clause = matched_clause;
        g_current_unwind = exception;
        _Unwind_SetIP(context, (uintptr_t)region->landing_pad);
        return _URC_INSTALL_CONTEXT;
    }

    return _URC_CONTINUE_UNWIND;
}
#endif

void feng_throw(void *value, const FengTypeDescriptor *desc) {
#if defined(_WIN32)
    (void)desc;
    feng_release(value);
    feng_panic("feng_throw: native Windows exception backend is not implemented");
#else
    FengUnwindException *exception;
    _Unwind_Reason_Code reason;

    if (g_finalizer_depth > 0U) {
        feng_release(value);
        feng_panic("exception escaped finalizer");
    }

    exception = (FengUnwindException *)calloc(1U, sizeof(*exception));
    if (exception == NULL) {
        feng_release(value);
        feng_panic("feng_throw: out of memory");
    }

    feng_release_current_unwind_exception(true);
    feng_unwind_exception_init(exception, value, desc);
    g_current_unwind = exception;

    reason = _Unwind_RaiseException(&exception->unwind);
    g_current_unwind = NULL;
    feng_cleanup_release_all();
    if (exception->value != NULL) {
        feng_release(exception->value);
        exception->value = NULL;
    }
    free(exception);
    feng_panic("uncaught exception (unwind reason=%d)", (int)reason);
#endif
}

void *feng_caught_value(void) {
    return g_current_unwind != NULL ? g_current_unwind->value : NULL;
}

int feng_caught_clause(void) {
    return g_current_unwind != NULL ? g_current_unwind->matched_clause : -1;
}

void feng_rethrow(void) {
#if defined(_WIN32)
    feng_panic("feng_rethrow: native Windows exception backend is not implemented");
#else
    FengUnwindException *exception = g_current_unwind;

    if (exception == NULL) {
        feng_panic("feng_rethrow: no current exception");
    }
    g_current_unwind = NULL;
    _Unwind_Resume(&exception->unwind);
    feng_panic("feng_rethrow: _Unwind_Resume returned unexpectedly");
#endif
}

void feng_release_unwind_exception(void) {
    feng_release_current_unwind_exception(true);
}

