#include <doctest/doctest.h>
#include <atomic>
#include <thread>
#include <vector>
#include "vita_tracy/control_gate.h"
#include "vita_tracy/kernel_abi.h"

namespace {
struct Cleanup {
    VitaTraceControlGate gate{};
    int calls = 0, result = 0;
    Cleanup() { vita_trace_control_set_target(&gate, 123); }
    static int run(void *p) {
        auto &s = *static_cast<Cleanup *>(p);
        CHECK_FALSE(vita_trace_control_enter(&s.gate));
        ++s.calls;
        return s.result;
    }
};
}

TEST_CASE("cleanup waits for control ownership without blocking inside a process callback") {
    Cleanup c;
    REQUIRE(vita_trace_control_enter(&c.gate));
    vita_trace_control_request(&c.gate, 123);
    CHECK(vita_trace_control_reap(&c.gate, Cleanup::run, &c) == VITA_TRACY_ERROR_BUSY);
    CHECK(c.calls == 0);
    CHECK(vita_trace_control_leave(&c.gate, Cleanup::run, &c) == 0);
    CHECK(c.calls == 1); CHECK_FALSE(vita_trace_control_pending(&c.gate));
}
TEST_CASE("failed process cleanup remains pending and is retryable") {
    Cleanup c; c.result = -100;
    vita_trace_control_request(&c.gate, 123);
    CHECK(vita_trace_control_reap(&c.gate, Cleanup::run, &c) == -100);
    CHECK(vita_trace_control_pending(&c.gate));
    c.result = 0;
    CHECK(vita_trace_control_reap(&c.gate, Cleanup::run, &c) == 0);
    CHECK(c.calls == 2); CHECK_FALSE(vita_trace_control_pending(&c.gate));
}
TEST_CASE("cleanup requested after unlock runs in the requesting context") {
    Cleanup c;
    REQUIRE(vita_trace_control_enter(&c.gate));
    REQUIRE(vita_trace_control_leave(&c.gate, Cleanup::run, &c) == 0);
    vita_trace_control_request(&c.gate, 123);
    CHECK(vita_trace_control_reap(&c.gate, Cleanup::run, &c) == 0);
    CHECK(c.calls == 1);
}
TEST_CASE("concurrent control callers never own the session simultaneously") {
    VitaTraceControlGate gate{};
    std::atomic<int> owners{0}, violations{0}, completed{0};
    auto cleanup = [](void *) { return 0; };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back([&] {
        for (int i = 0; i < 4000; ++i) {
            if (!vita_trace_control_enter(&gate)) { std::this_thread::yield(); continue; }
            if (owners.fetch_add(1) != 0) ++violations;
            ++completed;
            if (owners.fetch_sub(1) != 1) ++violations;
            vita_trace_control_leave(&gate, cleanup, nullptr);
        }
    });
    for (auto &thread : threads) thread.join();
    CHECK(violations == 0); CHECK(completed > 0);
}

TEST_CASE("a stale exit callback cannot request cleanup of another process") {
    Cleanup c;
    REQUIRE(vita_trace_control_enter(&c.gate));
    vita_trace_control_set_target(&c.gate, 456);
    CHECK_FALSE(vita_trace_control_request(&c.gate, 123));
    CHECK_FALSE(vita_trace_control_request(&c.gate, 0));
    CHECK_FALSE(vita_trace_control_request(&c.gate, 0x800001c8u));
    CHECK_FALSE(vita_trace_control_pending(&c.gate));
    CHECK(vita_trace_control_target(&c.gate) == 456);
    REQUIRE(vita_trace_control_leave(&c.gate, Cleanup::run, &c) == 0);
    CHECK(c.calls == 0);
    CHECK(vita_trace_control_request(&c.gate, 456));
    CHECK(vita_trace_control_request(&c.gate, 456));
    CHECK(vita_trace_control_reap(&c.gate, Cleanup::run, &c) == 0);
    CHECK(c.calls == 1);
}

TEST_CASE("a detached session rejects delayed cleanup requests") {
    Cleanup c;
    REQUIRE(vita_trace_control_enter(&c.gate));
    REQUIRE(vita_trace_control_request(&c.gate, 123));
    vita_trace_control_set_target(&c.gate, 0);
    CHECK_FALSE(vita_trace_control_request(&c.gate, 123));
    CHECK_FALSE(vita_trace_control_pending(&c.gate));
    CHECK(vita_trace_control_leave(&c.gate, Cleanup::run, &c) == 0);
    CHECK(c.calls == 0);
}
