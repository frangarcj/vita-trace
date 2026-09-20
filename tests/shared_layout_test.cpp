#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "vita_tracy/abi.h"
#include "vita_tracy/kernel_events.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

namespace {

constexpr uint32_t kSampleCapacity = 64;
constexpr uint32_t kControlCapacity = 16;
constexpr uint32_t kPid = 0x10005;
constexpr uint32_t kHz = 40961000u;

std::vector<uint8_t> make_shared_block() {
    size_t size = vita_trace_shared_layout_size(kSampleCapacity, kControlCapacity);
    REQUIRE(size > 0);
    std::vector<uint8_t> mem(size, 0xCD);
    REQUIRE(vita_trace_shared_init(mem.data(), mem.size(), kPid, kHz, kSampleCapacity,
                                   kControlCapacity) == 1);
    return mem;
}

} // namespace

TEST_CASE("layout size rejects capacities that are not powers of two") {
    CHECK(vita_trace_shared_layout_size(0, kControlCapacity) == 0);
    CHECK(vita_trace_shared_layout_size(kSampleCapacity, 0) == 0);
    CHECK(vita_trace_shared_layout_size(3, kControlCapacity) == 0);
    CHECK(vita_trace_shared_layout_size(kSampleCapacity, 6) == 0);
}

TEST_CASE("layout size covers every ring") {
    size_t size = vita_trace_shared_layout_size(kSampleCapacity, kControlCapacity);
    size_t samples = (size_t)kSampleCapacity * sizeof(VitaTraceSample) * VITA_TRACE_CORE_COUNT;
    size_t control = (size_t)kControlCapacity * sizeof(VitaTraceControlRecord);
    CHECK(size >= sizeof(VitaTraceSharedHeader) + samples + control);
}

TEST_CASE("shared layout rejects sizes that overflow the 32-bit Vita ABI") {
    CHECK(vita_trace_ring_layout_size(0x80000000u, 32) == 0);
    CHECK(vita_trace_shared_layout_size(0x80000000u, 16) == 0);
    CHECK(vita_trace_shared_layout_size(0x02000000u, 16) == 0);
    CHECK(vita_trace_shared_layout_size(64, 0x80000000u) == 0);
    uint8_t tiny[sizeof(VitaTraceSharedHeader)]{};
    CHECK_FALSE(vita_trace_shared_init(tiny, sizeof(tiny), kPid, kHz, 0x80000000u, 16));
}

TEST_CASE("full layout validation rejects corrupt offsets and ring metadata") {
    auto mem = make_shared_block();
    CHECK(vita_trace_shared_validate_layout(mem.data(), mem.size()));
    CHECK_FALSE(vita_trace_shared_validate_layout(mem.data(), mem.size() - 1));
    auto *hdr = reinterpret_cast<VitaTraceSharedHeader *>(mem.data());
    const uint32_t offset = hdr->core_ring_offset[0];
    hdr->core_ring_offset[0] = 0xFFFFFFF0u;
    CHECK_FALSE(vita_trace_shared_validate_layout(mem.data(), mem.size()));
    hdr->core_ring_offset[0] = offset;
    auto *ring = reinterpret_cast<VitaTraceRingHeader *>(mem.data() + offset);
    ring->element_size = 0x10000;
    CHECK_FALSE(vita_trace_shared_validate_layout(mem.data(), mem.size()));
}

TEST_CASE("init rejects an undersized block") {
    size_t size = vita_trace_shared_layout_size(kSampleCapacity, kControlCapacity);
    std::vector<uint8_t> mem(size);
    CHECK(vita_trace_shared_init(mem.data(), size - 1, kPid, kHz, kSampleCapacity,
                                 kControlCapacity) == 0);
    CHECK(vita_trace_shared_init(nullptr, size, kPid, kHz, kSampleCapacity, kControlCapacity) == 0);
}

TEST_CASE("init records the session metadata") {
    auto mem = make_shared_block();
    auto *hdr = (VitaTraceSharedHeader *)mem.data();

    CHECK(hdr->magic == VITA_TRACE_SHARED_MAGIC);
    CHECK(hdr->abi_version == VITA_TRACY_ABI_VERSION);
    CHECK(hdr->target_pid == kPid);
    CHECK(hdr->timebase_hz == kHz);
    CHECK(hdr->core_count == VITA_TRACE_CORE_COUNT);
    CHECK(hdr->sample_capacity == kSampleCapacity);
    CHECK(hdr->control_capacity == kControlCapacity);
}

TEST_CASE("an initialized block validates and a zeroed one does not") {
    auto mem = make_shared_block();
    CHECK(vita_trace_shared_is_valid(mem.data()) == 1);

    std::vector<uint8_t> zeroed(mem.size(), 0);
    CHECK(vita_trace_shared_is_valid(zeroed.data()) == 0);
}

TEST_CASE("a block from a different ABI version is rejected") {
    auto mem = make_shared_block();
    auto *hdr = (VitaTraceSharedHeader *)mem.data();
    hdr->abi_version = VITA_TRACY_ABI_VERSION + 1;
    CHECK(vita_trace_shared_is_valid(mem.data()) == 0);
}

TEST_CASE("every ring lives inside the block and none overlap") {
    auto mem = make_shared_block();
    auto *base = mem.data();

    std::vector<std::pair<size_t, size_t>> spans;
    for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
        void *ring = vita_trace_shared_core_ring(base, cpu);
        REQUIRE(ring != nullptr);
        size_t start = (uint8_t *)ring - base;
        size_t len = vita_trace_ring_layout_size(kSampleCapacity, sizeof(VitaTraceSample));
        CHECK(start + len <= mem.size());
        spans.emplace_back(start, len);
    }
    void *control = vita_trace_shared_control_ring(base);
    REQUIRE(control != nullptr);
    size_t control_start = (uint8_t *)control - base;
    size_t control_len = vita_trace_ring_layout_size(kControlCapacity, sizeof(VitaTraceControlRecord));
    CHECK(control_start + control_len <= mem.size());
    spans.emplace_back(control_start, control_len);

    for (size_t i = 0; i < spans.size(); ++i) {
        CHECK(spans[i].first >= sizeof(VitaTraceSharedHeader));
        for (size_t j = i + 1; j < spans.size(); ++j) {
            bool disjoint = spans[i].first + spans[i].second <= spans[j].first ||
                            spans[j].first + spans[j].second <= spans[i].first;
            CHECK(disjoint);
        }
    }
}

TEST_CASE("an out of range cpu has no ring") {
    auto mem = make_shared_block();
    CHECK(vita_trace_shared_core_ring(mem.data(), VITA_TRACE_CORE_COUNT) == nullptr);
}

TEST_CASE("rings are aligned so cores do not share a cache line") {
    auto mem = make_shared_block();
    for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
        size_t offset = (uint8_t *)vita_trace_shared_core_ring(mem.data(), cpu) - mem.data();
        CHECK(offset % VITA_TRACE_SHARED_ALIGN == 0);
    }
    size_t control_offset = (uint8_t *)vita_trace_shared_control_ring(mem.data()) - mem.data();
    CHECK(control_offset % VITA_TRACE_SHARED_ALIGN == 0);
}

TEST_CASE("each core ring is usable after init") {
    auto mem = make_shared_block();
    for (uint32_t cpu = 0; cpu < VITA_TRACE_CORE_COUNT; ++cpu) {
        void *ring = vita_trace_shared_core_ring(mem.data(), cpu);
        VitaTraceSample sample{};
        sample.pc = 0x81000000u + cpu;
        sample.cpu = (uint16_t)cpu;
        CHECK(vita_trace_ring_try_push(ring, &sample) == 1);

        VitaTraceSample out{};
        CHECK(vita_trace_ring_try_pop(ring, &out) == 1);
        CHECK(out.pc == sample.pc);
        CHECK(out.cpu == cpu);
    }
}

TEST_CASE("writing one core ring does not disturb another") {
    auto mem = make_shared_block();
    void *ring0 = vita_trace_shared_core_ring(mem.data(), 0);
    void *ring1 = vita_trace_shared_core_ring(mem.data(), 1);

    VitaTraceSample sample{};
    sample.pc = 0xAABBCCDDu;
    for (uint32_t i = 0; i < kSampleCapacity; ++i) {
        CHECK(vita_trace_ring_try_push(ring0, &sample) == 1);
    }
    CHECK(vita_trace_ring_try_push(ring0, &sample) == 0); // full
    CHECK(vita_trace_ring_dropped(ring0) == 1);

    CHECK(vita_trace_ring_pending(ring1) == 0);
    CHECK(vita_trace_ring_dropped(ring1) == 0);
}

TEST_CASE("the control ring carries module snapshots intact") {
    auto mem = make_shared_block();
    void *control = vita_trace_shared_control_ring(mem.data());

    VitaTraceControlRecord record{};
    record.type = VITA_TRACE_MODULE_SNAPSHOT;
    record.timestamp = 987654321ull;
    record.payload.module_snapshot.pid = kPid;
    record.payload.module_snapshot.module_nid = 0x12345678u;
    record.payload.module_snapshot.segment_count = 2;
    record.payload.module_snapshot.segments[0].vaddr = 0x81000000u;
    record.payload.module_snapshot.segments[0].memsz = 0x2000u;
    std::strncpy(record.payload.module_snapshot.module_name, "SceLibKernel",
                 VITA_TRACE_MODULE_NAME_MAX - 1);

    CHECK(vita_trace_ring_try_push(control, &record) == 1);

    VitaTraceControlRecord out{};
    CHECK(vita_trace_ring_try_pop(control, &out) == 1);
    CHECK(out.type == VITA_TRACE_MODULE_SNAPSHOT);
    CHECK(out.payload.module_snapshot.module_nid == 0x12345678u);
    CHECK(std::strcmp(out.payload.module_snapshot.module_name, "SceLibKernel") == 0);
}

TEST_CASE("offsets resolve against whichever base the caller holds") {
    // The kernel maps this block at a different virtual address than the
    // process, so resolving against a relocated copy must land on the same
    // ring contents.
    auto mem = make_shared_block();
    void *ring = vita_trace_shared_core_ring(mem.data(), 2);
    VitaTraceSample sample{};
    sample.pc = 0x5A5A5A5Au;
    REQUIRE(vita_trace_ring_try_push(ring, &sample) == 1);

    std::vector<uint8_t> relocated(mem.begin(), mem.end());
    void *relocated_ring = vita_trace_shared_core_ring(relocated.data(), 2);
    REQUIRE(relocated_ring != nullptr);
    CHECK(relocated_ring != ring);

    VitaTraceSample out{};
    CHECK(vita_trace_ring_try_pop(relocated_ring, &out) == 1);
    CHECK(out.pc == 0x5A5A5A5Au);
}

TEST_CASE("a fresh block is not acknowledged") {
    auto mem = make_shared_block();
    CHECK(vita_trace_shared_is_acknowledged(mem.data()) == 0);
}

TEST_CASE("acknowledgement is visible to the other side") {
    // Stands in for the kernel writing through its own mapping while the
    // client reads through the process mapping.
    auto mem = make_shared_block();
    vita_trace_shared_acknowledge(mem.data());
    CHECK(vita_trace_shared_is_acknowledged(mem.data()) == 1);
}

TEST_CASE("acknowledging leaves the rings untouched") {
    auto mem = make_shared_block();
    vita_trace_shared_acknowledge(mem.data());

    CHECK(vita_trace_shared_is_valid(mem.data()) == 1);
    void *ring = vita_trace_shared_core_ring(mem.data(), 0);
    REQUIRE(ring != nullptr);
    CHECK(vita_trace_ring_pending(ring) == 0);

    VitaTraceSample sample{};
    sample.pc = 0x1234u;
    CHECK(vita_trace_ring_try_push(ring, &sample) == 1);
}

TEST_CASE("an unacknowledged block is still a valid block") {
    // The distinction matters: a valid block with no acknowledgement means
    // the plugin is absent, not that the memory is corrupt.
    auto mem = make_shared_block();
    CHECK(vita_trace_shared_is_valid(mem.data()) == 1);
    CHECK(vita_trace_shared_is_acknowledged(mem.data()) == 0);
}
