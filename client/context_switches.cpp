#include "context_switches.hpp"

#include <algorithm>

bool VitaContextSwitches::Add(const VitaTraceSwitch &record, uint32_t tid) {
    if (count_ >= kPending || record.cpu >= VITA_TRACE_CORE_COUNT || !tid) {
        ++dropped_;
        return false;
    }
    pending_[count_].record = record;
    pending_[count_].tid = tid;
    pending_[count_].order = arrivals_++;
    ++count_;
    return true;
}

void VitaContextSwitches::Leave(uint32_t cpu, uint64_t timestamp, uint8_t reason, Sink sink, void *ctx) {
    if (!running_[cpu]) return;
    sink(ctx, Event{timestamp, running_[cpu], 0u, (uint8_t)cpu, reason});
    running_[cpu] = 0;
}

void VitaContextSwitches::Flush(uint64_t watermark, Sink sink, void *ctx) {
    /* In place (no heap: the drain runs inside the application's budget);
     * arrival order breaks ties so a core's own records keep their order. */
    std::sort(pending_, pending_ + count_, [](const Pending &a, const Pending &b) {
        if (a.record.timestamp != b.record.timestamp) return a.record.timestamp < b.record.timestamp;
        return (int32_t)(a.order - b.order) < 0;
    });
    uint32_t done = 0;
    for (; done < count_ && pending_[done].record.timestamp <= watermark; ++done) {
        const Pending &p = pending_[done];
        const uint32_t cpu = p.record.cpu;
        const uint64_t t = p.record.timestamp;
        if (p.record.kind == VITA_TRACE_SWITCH_IN) {
            /* A lost switch-out leaves a thread marked running: close it
             * here, and close this thread wherever it was last seen. */
            if (running_[cpu] && running_[cpu] != p.tid) Leave(cpu, t, 0, sink, ctx);
            for (uint32_t other = 0; other < VITA_TRACE_CORE_COUNT; ++other)
                if (other != cpu && running_[other] == p.tid) Leave(other, t, 0, sink, ctx);
            if (running_[cpu] == p.tid) continue;
            running_[cpu] = p.tid;
            sink(ctx, Event{t, 0u, p.tid, (uint8_t)cpu, 0});
        } else if (p.record.kind == VITA_TRACE_SWITCH_OUT && running_[cpu] == p.tid) {
            Leave(cpu, t, p.record.reason, sink, ctx);
        }
        /* An unmatched switch-out (the thread went on before we started) has
         * nothing to close. */
    }
    std::copy(pending_ + done, pending_ + count_, pending_);
    count_ -= done;
}

void VitaContextSwitches::Reset() {
    count_ = 0;
    for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) running_[cpu] = 0;
}
