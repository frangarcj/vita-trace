#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "vita_tracy/kernel_events.h"
#include "vita_tracy/shared_ring.h"

namespace {

std::vector<uint8_t> make_ring(uint32_t capacity, uint32_t element_size) {
    std::vector<uint8_t> mem(vita_trace_ring_layout_size(capacity, element_size));
    REQUIRE(vita_trace_ring_init(mem.data(), mem.size(), capacity, element_size) == 1);
    return mem;
}

} // namespace

TEST_CASE("is_pow2 accepts only powers of two") {
    CHECK(vita_trace_is_pow2(1) == 1);
    CHECK(vita_trace_is_pow2(2) == 1);
    CHECK(vita_trace_is_pow2(64) == 1);
    CHECK(vita_trace_is_pow2(0) == 0);
    CHECK(vita_trace_is_pow2(3) == 0);
    CHECK(vita_trace_is_pow2(6) == 0);
}

TEST_CASE("layout_size accounts for header and slot bytes") {
    size_t expected = sizeof(VitaTraceRingHeader) + (size_t)16 * sizeof(uint32_t);
    CHECK(vita_trace_ring_layout_size(16, sizeof(uint32_t)) == expected);
}

TEST_CASE("init rejects invalid arguments") {
    std::vector<uint8_t> mem(vita_trace_ring_layout_size(8, sizeof(uint32_t)));

    CHECK(vita_trace_ring_init(nullptr, mem.size(), 8, sizeof(uint32_t)) == 0);
    CHECK(vita_trace_ring_init(mem.data(), mem.size(), 0, sizeof(uint32_t)) == 0);
    CHECK(vita_trace_ring_init(mem.data(), mem.size(), 3, sizeof(uint32_t)) == 0); // not power of two
    CHECK(vita_trace_ring_init(mem.data(), mem.size(), 8, 0) == 0);
    CHECK(vita_trace_ring_init(mem.data(), mem.size() - 1, 8, sizeof(uint32_t)) == 0); // too small
}

TEST_CASE("init succeeds and ring starts empty") {
    auto mem = make_ring(8, sizeof(uint32_t));
    CHECK(vita_trace_ring_pending(mem.data()) == 0);
    CHECK(vita_trace_ring_dropped(mem.data()) == 0);
}

TEST_CASE("single element roundtrips") {
    auto mem = make_ring(4, sizeof(uint32_t));
    uint32_t in = 0xDEADBEEFu;
    uint32_t out = 0;

    CHECK(vita_trace_ring_try_push(mem.data(), &in) == 1);
    CHECK(vita_trace_ring_pending(mem.data()) == 1);
    CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 1);
    CHECK(out == in);
    CHECK(vita_trace_ring_pending(mem.data()) == 0);
}

TEST_CASE("pop on empty ring fails") {
    auto mem = make_ring(4, sizeof(uint32_t));
    uint32_t out = 0;
    CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 0);
}

TEST_CASE("elements come out in FIFO order") {
    auto mem = make_ring(4, sizeof(uint32_t));
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(vita_trace_ring_try_push(mem.data(), &i) == 1);
    }
    for (uint32_t i = 0; i < 4; ++i) {
        uint32_t out = 0xFFFFFFFFu;
        CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 1);
        CHECK(out == i);
    }
}

TEST_CASE("push beyond capacity drops and does not overwrite") {
    auto mem = make_ring(2, sizeof(uint32_t));
    uint32_t a = 1, b = 2, c = 3;

    CHECK(vita_trace_ring_try_push(mem.data(), &a) == 1);
    CHECK(vita_trace_ring_try_push(mem.data(), &b) == 1);
    CHECK(vita_trace_ring_try_push(mem.data(), &c) == 0); // ring full
    CHECK(vita_trace_ring_dropped(mem.data()) == 1);

    uint32_t out = 0;
    CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 1);
    CHECK(out == a);
    CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 1);
    CHECK(out == b);
    CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 0);
}

TEST_CASE("index wraps correctly across many cycles") {
    auto mem = make_ring(4, sizeof(uint32_t));

    for (uint32_t cycle = 0; cycle < 1000; ++cycle) {
        uint32_t value = cycle * 7u;
        CHECK(vita_trace_ring_try_push(mem.data(), &value) == 1);
        uint32_t out = 0;
        CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 1);
        CHECK(out == value);
    }
    CHECK(vita_trace_ring_dropped(mem.data()) == 0);
}

TEST_CASE("VitaTraceSample records roundtrip through the ring") {
    auto mem = make_ring(2, sizeof(VitaTraceSample));

    VitaTraceSample sample{};
    sample.timestamp = 123456789ull;
    sample.pid = 1;
    sample.tid = 2;
    sample.pc = 0x81000000u;
    sample.sp = 0x9F000000u;
    sample.lr = 0x81000004u;
    sample.cpu = 1;
    sample.flags = 0;

    CHECK(vita_trace_ring_try_push(mem.data(), &sample) == 1);

    VitaTraceSample out{};
    CHECK(vita_trace_ring_try_pop(mem.data(), &out) == 1);
    CHECK(std::memcmp(&out, &sample, sizeof(sample)) == 0);
}
