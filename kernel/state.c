#include "vita_tracy/state.h"

#include <stddef.h>

int vita_tracy_state_next(VitaTracyState current, VitaTracyStateEvent event, VitaTracyState *next) {
    if (next == NULL) {
        return 0;
    }

    VitaTracyState result;

    switch (event) {
    case VITA_TRACY_EVENT_INIT:
        if (current != VITA_TRACY_STATE_UNINITIALIZED) {
            return 0;
        }
        result = VITA_TRACY_STATE_READY;
        break;

    case VITA_TRACY_EVENT_ATTACH:
        if (current != VITA_TRACY_STATE_READY) {
            return 0;
        }
        result = VITA_TRACY_STATE_ATTACHED;
        break;

    case VITA_TRACY_EVENT_START:
        if (current != VITA_TRACY_STATE_ATTACHED && current != VITA_TRACY_STATE_STOPPED) {
            return 0;
        }
        result = VITA_TRACY_STATE_PROFILING;
        break;

    case VITA_TRACY_EVENT_STOP:
        if (current != VITA_TRACY_STATE_PROFILING) {
            return 0;
        }
        result = VITA_TRACY_STATE_STOPPED;
        break;

    case VITA_TRACY_EVENT_DETACH:
        if (current != VITA_TRACY_STATE_ATTACHED && current != VITA_TRACY_STATE_PROFILING &&
            current != VITA_TRACY_STATE_STOPPED) {
            return 0;
        }
        result = VITA_TRACY_STATE_READY;
        break;

    default:
        return 0;
    }

    *next = result;
    return 1;
}

const char *vita_tracy_state_name(VitaTracyState state) {
    switch (state) {
    case VITA_TRACY_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case VITA_TRACY_STATE_READY:
        return "READY";
    case VITA_TRACY_STATE_ATTACHED:
        return "ATTACHED";
    case VITA_TRACY_STATE_PROFILING:
        return "PROFILING";
    case VITA_TRACY_STATE_STOPPED:
        return "STOPPED";
    default:
        return "INVALID";
    }
}
