#pragma once

#include <stdint.h>

#include "vita_tracy/config.h"
#include "vita_tracy/kernel_events.h"

/* Turns the kernel's per-core on/off-CPU records into the stream Tracy's
 * context-switch view accepts. Tracy closes a thread's running interval on
 * the core it started on and requires each thread's intervals in time order,
 * so records drained core by core must first be merged by timestamp, and
 * every switch-out must match the thread that core is running. */
class VitaContextSwitches {
public:
    /* One switch as Tracy wants it: oldThread leaves `cpu`, newThread
     * enters it; 0 on either side stands for no thread of this process. */
    struct Event {
        uint64_t timestamp;
        uint32_t old_thread;
        uint32_t new_thread;
        uint8_t cpu;
        uint8_t reason;
    };
    typedef void (*Sink)(void *ctx, const Event &event);

    static const uint32_t kPending = 4096;

    /* Queues a drained record; `tid` is already the Tracy thread id. Returns
     * false when the pending buffer is full and the record was dropped. */
    bool Add(const VitaTraceSwitch &record, uint32_t tid);

    /* Emits, in time order, every pending record not newer than `watermark`
     * (kernel microseconds); later ones wait for the next call, since
     * another core may still publish an earlier one. */
    void Flush(uint64_t watermark, Sink sink, void *ctx);

    /* Forget who runs where, e.g. when no viewer is connected. */
    void Reset();

    uint32_t dropped() const { return dropped_; }

private:
    struct Pending {
        VitaTraceSwitch record;
        uint32_t tid;
        uint32_t order; /* arrival, to keep a core's order at equal times */
    };
    void Leave(uint32_t cpu, uint64_t timestamp, uint8_t reason, Sink sink, void *ctx);

    Pending pending_[kPending];
    uint32_t count_ = 0;
    uint32_t dropped_ = 0;
    uint32_t arrivals_ = 0;
    uint32_t running_[VITA_TRACE_CORE_COUNT] = {};
};
