#include <doctest/doctest.h>

#include <array>
#include <utility>
#include <vector>

#include "vita_tracy/pmu_overflow.h"

namespace {

struct Registers {
    uint32_t pmcr = 6u << 11;
    uint32_t enabled = 0;
    uint32_t interrupts = 0;
    uint32_t overflow = 0;
    uint32_t cycles = 1234;
    uint32_t selector = 0;
    std::array<uint32_t, 6> types{};
    std::array<uint32_t, 6> values{};
    std::vector<std::pair<VitaPmuRegister, uint32_t>> writes;

    static uint32_t read(void *ctx, VitaPmuRegister reg) {
        auto &r = *static_cast<Registers *>(ctx);
        switch (reg) {
        case VITA_PMU_PMCR: return r.pmcr;
        case VITA_PMU_CNTEN: return r.enabled;
        case VITA_PMU_INTEN: return r.interrupts;
        case VITA_PMU_OVSR: return r.overflow;
        case VITA_PMU_SELR: return r.selector;
        case VITA_PMU_CYCLES: return r.cycles;
        case VITA_PMU_TYPE: return r.types.at(r.selector);
        case VITA_PMU_VALUE: return r.values.at(r.selector);
        default: FAIL("unexpected read"); return 0;
        }
    }

    static void write(void *ctx, VitaPmuRegister reg, uint32_t value) {
        auto &r = *static_cast<Registers *>(ctx);
        r.writes.emplace_back(reg, value);
        switch (reg) {
        case VITA_PMU_PMCR:
            CHECK((value & 6u) == 0);
            r.pmcr = value;
            break;
        case VITA_PMU_CNTEN: r.enabled |= value; break;
        case VITA_PMU_CNTCLR: r.enabled &= ~value; break;
        case VITA_PMU_INTEN: r.interrupts |= value; break;
        case VITA_PMU_INTCLR: r.interrupts &= ~value; break;
        case VITA_PMU_OVSR: r.overflow &= ~value; break;
        case VITA_PMU_CYCLES: r.cycles = value; break;
        case VITA_PMU_SELR: r.selector = value; break;
        case VITA_PMU_TYPE: r.types.at(r.selector) = value; break;
        case VITA_PMU_VALUE: r.values.at(r.selector) = value; break;
        }
    }

    VitaPmuIo io() { return VitaPmuIo{this, read, write}; }
};

} // namespace

TEST_CASE("overflow sampler exclusively acquires the cycle counter without reset commands") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();

    REQUIRE(vita_pmu_overflow_acquire(&overflow, &io, 1000) == 0);
    CHECK(overflow.acquired == 1);
    CHECK(overflow.preload == 0u - 1000u);
    CHECK(r.enabled == VITA_PMU_CYCLE_BIT);
    CHECK(r.interrupts == VITA_PMU_CYCLE_BIT);
    CHECK(r.cycles == 0u - 1000u);
    CHECK((r.pmcr & 0x39u) == 1u);
}

TEST_CASE("overflow preparation leaves IRQ and counter disabled until arm") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();

    REQUIRE(vita_pmu_overflow_prepare(&overflow, &io, 500) == 0);
    CHECK(overflow.acquired == 1);
    CHECK(overflow.armed == 0);
    CHECK(r.enabled == 0);
    CHECK(r.interrupts == 0);
    CHECK(r.cycles == 0u - 500u);

    REQUIRE(vita_pmu_overflow_arm(&overflow, &io) == 0);
    CHECK(overflow.armed == 1);
    CHECK(r.enabled == VITA_PMU_CYCLE_BIT);
    CHECK(r.interrupts == VITA_PMU_CYCLE_BIT);
}

TEST_CASE("prepared overflow state can be restored without ever arming") {
    Registers r;
    r.pmcr = (6u << 11) | 0x08u;
    r.cycles = 0xCAFEBABEu;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    REQUIRE(vita_pmu_overflow_prepare(&overflow, &io, 500) == 0);

    REQUIRE(vita_pmu_overflow_release(&overflow, &io) == 0);
    CHECK(r.enabled == 0);
    CHECK(r.interrupts == 0);
    CHECK(r.cycles == 0xCAFEBABEu);
    CHECK(r.pmcr == ((6u << 11) | 0x08u));
}

TEST_CASE("overflow sampler refuses existing PMU owners and pending cycle overflow") {
    for (int mode = 0; mode < 3; ++mode) {
        Registers r;
        if (mode == 0) r.enabled = 1;
        if (mode == 1) r.interrupts = VITA_PMU_CYCLE_BIT;
        if (mode == 2) r.overflow = VITA_PMU_CYCLE_BIT;
        VitaPmuOverflow overflow{};
        auto io = r.io();

        CHECK(vita_pmu_overflow_acquire(&overflow, &io, 1000) == VITA_PMU_ERROR_BUSY);
        CHECK(r.writes.empty());
        CHECK(overflow.acquired == 0);
    }
}

TEST_CASE("overflow service ignores unrelated IRQs and reloads only its own overflow") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    REQUIRE(vita_pmu_overflow_acquire(&overflow, &io, 333) == 0);
    r.writes.clear();

    CHECK(vita_pmu_overflow_service(&overflow, &io) == 0);
    CHECK(r.writes.empty());

    r.overflow = VITA_PMU_CYCLE_BIT | 1u;
    r.cycles = 7;
    REQUIRE(vita_pmu_overflow_service(&overflow, &io) == 1);
    CHECK((r.overflow & VITA_PMU_CYCLE_BIT) == 0);
    CHECK((r.overflow & 1u) != 0);
    CHECK(r.cycles == 0u - 333u);
}

TEST_CASE("overflow release restores prior cycle state") {
    Registers r;
    r.pmcr = (6u << 11) | 0x08u;
    r.cycles = 0x12345678u;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    REQUIRE(vita_pmu_overflow_acquire(&overflow, &io, 777) == 0);

    r.overflow = VITA_PMU_CYCLE_BIT;
    REQUIRE(vita_pmu_overflow_release(&overflow, &io) == 0);
    CHECK(r.enabled == 0);
    CHECK(r.interrupts == 0);
    CHECK((r.overflow & VITA_PMU_CYCLE_BIT) == 0);
    CHECK(r.cycles == 0x12345678u);
    CHECK(r.pmcr == ((6u << 11) | 0x08u));
    CHECK(overflow.acquired == 0);
}

TEST_CASE("overflow sampler never restores over another PMU owner") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    REQUIRE(vita_pmu_overflow_acquire(&overflow, &io, 1000) == 0);

    r.interrupts = 0;
    const uint32_t foreign_cycles = 0xAABBCCDDu;
    r.cycles = foreign_cycles;
    CHECK(vita_pmu_overflow_release(&overflow, &io) == VITA_PMU_ERROR_OWNERSHIP);
    CHECK(r.cycles == foreign_cycles);
    CHECK(overflow.acquired == 0);
}

TEST_CASE("overflow period must be nonzero") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    CHECK(vita_pmu_overflow_acquire(&overflow, &io, 0) == VITA_PMU_ERROR_ARGS);
    CHECK(r.writes.empty());
}

TEST_CASE("overflow service recovers a wrap whose PMOVSR was cleared under it") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    REQUIRE(vita_pmu_overflow_acquire(&overflow, &io, 1000) == 0);

    r.cycles = 400; /* wrapped 400 cycles ago, no overflow pending */
    CHECK(vita_pmu_overflow_service(&overflow, &io) == VITA_PMU_OVERFLOW_MISSED_RECENT);
    CHECK(r.cycles == 0u - 1000u);

    r.cycles = 50000; /* wrapped long ago: reload, but no sample */
    CHECK(vita_pmu_overflow_service(&overflow, &io) == VITA_PMU_OVERFLOW_MISSED_STALE);
    CHECK(r.cycles == 0u - 1000u);

    r.cycles = 0u - 10u; /* counting normally toward the wrap */
    r.writes.clear();
    CHECK(vita_pmu_overflow_service(&overflow, &io) == 0);
    CHECK(r.writes.empty());
}

TEST_CASE("overflow service leaves banks with other counters or a foreign PMCR alone") {
    Registers r;
    VitaPmuOverflow overflow{};
    auto io = r.io();
    REQUIRE(vita_pmu_overflow_acquire(&overflow, &io, 1000) == 0);
    r.cycles = 5;
    r.enabled = VITA_PMU_CYCLE_BIT | 1u;
    r.writes.clear();
    CHECK(vita_pmu_overflow_service(&overflow, &io) == 0);
    r.enabled = VITA_PMU_CYCLE_BIT;
    r.pmcr |= 0x10u; /* X: someone else's configuration */
    CHECK(vita_pmu_overflow_service(&overflow, &io) == 0);
    CHECK(r.writes.empty());
    CHECK(r.cycles == 5u);
}
