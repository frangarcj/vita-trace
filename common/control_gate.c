#include "vita_tracy/control_gate.h"
#include "vita_tracy/kernel_abi.h"

int vita_trace_control_enter(VitaTraceControlGate *gate) {
    uint32_t idle = 0;
    return __atomic_compare_exchange_n(&gate->busy, &idle, 1u, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

int vita_trace_control_pending(const VitaTraceControlGate *gate) {
    return __atomic_load_n(&gate->cleanup_pending, __ATOMIC_ACQUIRE) != 0;
}

void vita_trace_control_request(VitaTraceControlGate *gate) {
    __atomic_store_n(&gate->cleanup_pending, 1u, __ATOMIC_RELEASE);
}

int vita_trace_control_reap(VitaTraceControlGate *gate, VitaTraceCleanup cleanup, void *context) {
    if (!vita_trace_control_pending(gate)) return 0;
    if (!vita_trace_control_enter(gate)) return VITA_TRACY_ERROR_BUSY;
    int ret = 0;
    if (vita_trace_control_pending(gate)) {
        ret = cleanup(context);
        if (ret == 0) __atomic_store_n(&gate->cleanup_pending, 0u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&gate->busy, 0u, __ATOMIC_RELEASE);
    return ret;
}

int vita_trace_control_leave(VitaTraceControlGate *gate, VitaTraceCleanup cleanup, void *context) {
    __atomic_store_n(&gate->busy, 0u, __ATOMIC_RELEASE);
    // A request arriving before unlock is observed here; one arriving after
    // this check can acquire the gate in the requesting thread's reap call.
    return vita_trace_control_reap(gate, cleanup, context);
}
