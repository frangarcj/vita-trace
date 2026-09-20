#include <doctest/doctest.h>
#include <array>
#include <vector>
#include <utility>
#include "vita_tracy/pmu_core.h"

namespace {
struct Registers {
    uint32_t pmcr = 6u << 11, enabled = 0, interrupts = 0, overflow = 0;
    uint32_t selector = 4, cycles = 123;
    std::array<uint32_t, 6> types{{7, 8, 9, 10, 11, 12}};
    std::array<uint32_t, 6> values{{100, 200, 300, 400, 500, 600}};
    std::vector<std::pair<VitaPmuRegister, uint32_t>> writes;
    static uint32_t read(void *p, VitaPmuRegister r) {
        auto &s = *static_cast<Registers *>(p);
        switch (r) {
        case VITA_PMU_PMCR: return s.pmcr;
        case VITA_PMU_CNTEN: return s.enabled;
        case VITA_PMU_INTEN: return s.interrupts;
        case VITA_PMU_OVSR: return s.overflow;
        case VITA_PMU_SELR: return s.selector;
        case VITA_PMU_CYCLES: return s.cycles;
        case VITA_PMU_TYPE: return s.types.at(s.selector);
        case VITA_PMU_VALUE: return s.values.at(s.selector);
        default: FAIL("invalid read"); return 0;
        }
    }
    static void write(void *p, VitaPmuRegister r, uint32_t v) {
        auto &s = *static_cast<Registers *>(p);
        s.writes.emplace_back(r, v);
        switch (r) {
        case VITA_PMU_PMCR: CHECK((v & 6u) == 0); s.pmcr = v; break;
        case VITA_PMU_CNTEN: s.enabled |= v; break;
        case VITA_PMU_CNTCLR: s.enabled &= ~v; break;
        case VITA_PMU_OVSR: s.overflow &= ~v; break;
        case VITA_PMU_SELR: s.selector = v; break;
        case VITA_PMU_CYCLES: s.cycles = v; break;
        case VITA_PMU_TYPE: s.types.at(s.selector) = v; break;
        case VITA_PMU_VALUE: s.values.at(s.selector) = v; break;
        default: FAIL("invalid write");
        }
    }
    VitaPmuIo io() { return {this, read, write}; }
};
VitaPmuPlan plan() {
    VitaPmuPlan p{};
    p.count = 2; p.counters[0] = 0; p.counters[1] = 3;
    p.events[0] = 0x68; p.events[1] = 0x03;
    return p;
}
}

TEST_CASE("PMU refuses active counters and interrupts without writing registers") {
    for (bool interrupt : {false, true}) {
        Registers r; if (interrupt) r.interrupts = 1; else r.enabled = VITA_PMU_CYCLE_BIT;
        auto io = r.io(); auto p = plan(); VitaPmuCore core{};
        CHECK(vita_pmu_acquire(&core, &io, &p) == VITA_PMU_ERROR_BUSY);
        CHECK(r.writes.empty()); CHECK(core.acquired == 0);
    }
}
TEST_CASE("PMU validates count slots events and hardware capacity before writes") {
    for (int kind = 0; kind != 5; ++kind) {
        Registers r; auto io = r.io(); auto p = plan(); VitaPmuCore core{};
        if (kind == 0) p.count = 7;
        if (kind == 1) p.counters[1] = 0;
        if (kind == 2) p.counters[1] = 31;
        if (kind == 3) p.events[1] = 256;
        if (kind == 4) r.pmcr = 2u << 11;
        CHECK(vita_pmu_acquire(&core, &io, &p) == VITA_PMU_ERROR_ARGS);
        CHECK(r.writes.empty());
    }
}
TEST_CASE("PMU restore preserves disabled counter values types selector and control") {
    Registers r; r.pmcr |= 0x38; r.overflow = 1;
    const Registers before = r;
    auto io = r.io(); auto p = plan(); VitaPmuCore core{};
    REQUIRE(vita_pmu_acquire(&core, &io, &p) == 0);
    CHECK(r.enabled == (VITA_PMU_CYCLE_BIT | 9u));
    CHECK(r.selector == before.selector);
    CHECK((r.pmcr & 0x39u) == 1u);
    r.cycles += 1000; r.values[0] += 20; r.values[3] += 40;
    r.overflow |= VITA_PMU_CYCLE_BIT | 8u;
    CHECK(vita_pmu_release(&core, &io) == 0);
    CHECK(r.pmcr == before.pmcr); CHECK(r.enabled == before.enabled);
    CHECK(r.cycles == before.cycles); CHECK(r.selector == before.selector);
    CHECK(r.types == before.types); CHECK(r.values == before.values);
    CHECK(r.overflow == before.overflow);
    CHECK(vita_pmu_release(&core, &io) == 0);
}
TEST_CASE("PMU deltas handle wrap and retain the actual measurement interval") {
    Registers r; auto io = r.io(); auto p = plan(); VitaPmuCore core{}; VitaPmuDelta d{};
    REQUIRE(vita_pmu_acquire(&core, &io, &p) == 0);
    r.cycles = 0xFFFFFFF0u; r.values[0] = 0xFFFFFFFEu;
    CHECK(vita_pmu_read(&core, &io, 10000, &d) == 0);
    r.cycles = 0x20; r.values[0] = 4; r.values[3] += 17;
    CHECK(vita_pmu_read(&core, &io, 22222, &d) == 1);
    CHECK(d.cycles == 48); CHECK(d.values[0] == 6); CHECK(d.values[1] == 17);
    CHECK(d.elapsed_us == 12222); CHECK(d.timestamp == 22222); CHECK(d.flags == 0);
}
TEST_CASE("PMU timing discontinuities are explicit and never yield fabricated deltas") {
    for (uint64_t now : {10000ull, 9999ull, 2000000ull}) {
        Registers r; auto io = r.io(); auto p = plan(); VitaPmuCore core{}; VitaPmuDelta d{};
        REQUIRE(vita_pmu_acquire(&core, &io, &p) == 0);
        REQUIRE(vita_pmu_read(&core, &io, 10000, &d) == 0);
        r.cycles += 100;
        REQUIRE(vita_pmu_read(&core, &io, now, &d) == 1);
        CHECK(d.flags == VITA_PMU_DELTA_GAP); CHECK(d.cycles == 0); CHECK(d.elapsed_us == 0);
        r.cycles += 20;
        REQUIRE(vita_pmu_read(&core, &io, now + 10000, &d) == 1);
        CHECK(d.flags == 0); CHECK(d.cycles == 20);
    }
}
TEST_CASE("PMU never restores over another owner's changed configuration") {
    Registers r; auto io = r.io(); auto p = plan(); VitaPmuCore core{}; VitaPmuDelta d{};
    REQUIRE(vita_pmu_acquire(&core, &io, &p) == 0);
    r.types[3] = 0x60;
    CHECK(vita_pmu_read(&core, &io, 100, &d) == VITA_PMU_ERROR_OWNERSHIP);
    CHECK(vita_pmu_release(&core, &io) == VITA_PMU_ERROR_OWNERSHIP);
    CHECK(r.types[3] == 0x60); CHECK(core.acquired == 0);
    CHECK(r.enabled != 0); // The new owner, not this session, now controls it.
}
TEST_CASE("PMU cycle-only sessions do not need programmable counters") {
    Registers r; r.pmcr = 0; auto io = r.io(); VitaPmuPlan p{}; VitaPmuCore core{};
    REQUIRE(vita_pmu_acquire(&core, &io, &p) == 0);
    CHECK(r.enabled == VITA_PMU_CYCLE_BIT);
    CHECK(vita_pmu_release(&core, &io) == 0);
}
TEST_CASE("PMU state and deltas are independent for different cores") {
    Registers a, b; auto ia = a.io(); auto ib = b.io(); auto p = plan();
    VitaPmuCore ca{}, cb{}; VitaPmuDelta da{}, db{};
    REQUIRE(vita_pmu_acquire(&ca, &ia, &p) == 0);
    REQUIRE(vita_pmu_acquire(&cb, &ib, &p) == 0);
    vita_pmu_read(&ca, &ia, 10, &da); vita_pmu_read(&cb, &ib, 10, &db);
    a.cycles += 300; b.cycles += 900;
    vita_pmu_read(&ca, &ia, 10010, &da); vita_pmu_read(&cb, &ib, 10010, &db);
    CHECK(da.cycles == 300); CHECK(db.cycles == 900);
}
