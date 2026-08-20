#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VitaTracyState {
    VITA_TRACY_STATE_UNINITIALIZED = 0,
    VITA_TRACY_STATE_READY,
    VITA_TRACY_STATE_ATTACHED,
    VITA_TRACY_STATE_PROFILING,
    VITA_TRACY_STATE_STOPPED
} VitaTracyState;

typedef enum VitaTracyStateEvent {
    VITA_TRACY_EVENT_INIT = 0,
    VITA_TRACY_EVENT_ATTACH,
    VITA_TRACY_EVENT_START,
    VITA_TRACY_EVENT_STOP,
    VITA_TRACY_EVENT_DETACH
} VitaTracyStateEvent;

/* Computes the next state. Returns 1 and writes *next when the transition
 * is allowed, 0 otherwise.
 *
 * DETACH is accepted from every attached state, including PROFILING: the
 * target process can die or be killed by AppMgr mid-capture, and the
 * mapping has to be released on that path too. */
int vita_tracy_state_next(VitaTracyState current, VitaTracyStateEvent event, VitaTracyState *next);

const char *vita_tracy_state_name(VitaTracyState state);

#ifdef __cplusplus
}
#endif
