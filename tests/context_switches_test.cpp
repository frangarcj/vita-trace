#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "context_switches.hpp"

namespace {

using Event = VitaContextSwitches::Event;

VitaTraceSwitch Rec(uint64_t t, uint16_t cpu, uint8_t kind, uint8_t reason = 0) {
    VitaTraceSwitch r{};
    r.timestamp = t;
    r.cpu = cpu;
    r.kind = kind;
    r.reason = reason;
    return r;
}

void Collect(void *ctx, const Event &e) {
    static_cast<std::vector<Event> *>(ctx)->push_back(e);
}

bool Is(const Event &e, uint64_t t, uint32_t old_thread, uint32_t new_thread, uint8_t cpu) {
    return e.timestamp == t && e.old_thread == old_thread && e.new_thread == new_thread && e.cpu == cpu;
}

} // namespace

TEST_CASE("context switches from different cores are emitted in time order") {
    auto sw = std::make_unique<VitaContextSwitches>();
    std::vector<Event> out;
    /* Drained core 1 first: thread 7 migrated from core 0 to core 1. */
    sw->Add(Rec(20, 1, VITA_TRACE_SWITCH_IN), 7);
    sw->Add(Rec(5, 0, VITA_TRACE_SWITCH_IN), 7);
    sw->Add(Rec(10, 0, VITA_TRACE_SWITCH_OUT, 4), 7);
    sw->Flush(100, Collect, &out);
    REQUIRE(out.size() == 3);
    CHECK(Is(out[0], 5, 0, 7, 0));
    CHECK(Is(out[1], 10, 7, 0, 0));
    CHECK(out[1].reason == 4);
    CHECK(Is(out[2], 20, 0, 7, 1));
}

TEST_CASE("context switches newer than the watermark wait for the next flush") {
    auto sw = std::make_unique<VitaContextSwitches>();
    std::vector<Event> out;
    sw->Add(Rec(10, 0, VITA_TRACE_SWITCH_IN), 3);
    sw->Add(Rec(50, 0, VITA_TRACE_SWITCH_OUT), 3);
    sw->Flush(30, Collect, &out);
    REQUIRE(out.size() == 1);
    CHECK(Is(out[0], 10, 0, 3, 0));
    sw->Add(Rec(40, 1, VITA_TRACE_SWITCH_IN), 4); /* arrived late, still in order */
    sw->Flush(60, Collect, &out);
    REQUIRE(out.size() == 3);
    CHECK(Is(out[1], 40, 0, 4, 1));
    CHECK(Is(out[2], 50, 3, 0, 0));
}

TEST_CASE("context switches never close a thread that is not running there") {
    auto sw = std::make_unique<VitaContextSwitches>();
    std::vector<Event> out;
    sw->Add(Rec(5, 0, VITA_TRACE_SWITCH_OUT), 3);  /* went on before we started */
    sw->Add(Rec(10, 0, VITA_TRACE_SWITCH_IN), 3);
    sw->Add(Rec(20, 0, VITA_TRACE_SWITCH_IN), 4);  /* thread 3's switch-out was lost */
    sw->Add(Rec(30, 1, VITA_TRACE_SWITCH_IN), 4);  /* and so was thread 4's on core 0 */
    sw->Add(Rec(40, 1, VITA_TRACE_SWITCH_OUT), 9); /* not the thread core 1 runs */
    sw->Flush(100, Collect, &out);
    REQUIRE(out.size() == 5);
    CHECK(Is(out[0], 10, 0, 3, 0));
    CHECK(Is(out[1], 20, 3, 0, 0));
    CHECK(Is(out[2], 20, 0, 4, 0));
    CHECK(Is(out[3], 30, 4, 0, 0));
    CHECK(Is(out[4], 30, 0, 4, 1));
}

TEST_CASE("context switch buffer counts what it cannot hold") {
    auto sw = std::make_unique<VitaContextSwitches>();
    for (uint32_t i = 0; i < VitaContextSwitches::kPending; ++i)
        REQUIRE(sw->Add(Rec(i, 0, VITA_TRACE_SWITCH_IN), 1));
    CHECK_FALSE(sw->Add(Rec(1, 0, VITA_TRACE_SWITCH_IN), 1));
    CHECK_FALSE(sw->Add(Rec(1, 9, VITA_TRACE_SWITCH_IN), 1)); /* bad core */
    CHECK(sw->dropped() == 2);
    std::vector<Event> out;
    sw->Reset();
    sw->Flush(100000, Collect, &out);
    CHECK(out.empty());
}
