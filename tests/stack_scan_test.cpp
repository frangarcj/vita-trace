#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "vita_tracy/stack_scan.h"

namespace {

constexpr uint32_t kBase = 0x81000000u;

struct Code {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(64, 0);
    void half(uint32_t off, uint16_t v) { std::memcpy(&bytes[off], &v, 2); }
    void word(uint32_t off, uint32_t v) { std::memcpy(&bytes[off], &v, 4); }
    int check(uint32_t addr) const {
        return vita_trace_is_call_return(addr, bytes.data(), kBase, (uint32_t)bytes.size());
    }
};

} // namespace

TEST_CASE("call-site check recognises Thumb and ARM calls and nothing else") {
    Code c;
    c.half(0, 0xF000); c.half(2, 0xF800);   // Thumb BL
    c.half(4, 0xF7FF); c.half(6, 0xEFFE);   // Thumb BLX imm (J bits set, bit 12 clear)
    c.half(8, 0x4798);                      // Thumb BLX r3
    c.half(10, 0x2000);                     // movs r0, #0
    c.word(16, 0xEB000010u);                // ARM BL
    c.word(20, 0xE12FFF33u);                // ARM BLX r3
    c.word(24, 0xFA000001u);                // ARM BLX imm
    c.word(28, 0xE1A00000u);                // ARM mov r0, r0

    CHECK(c.check(kBase + 4u + 1u));
    CHECK(c.check(kBase + 8u + 1u));
    CHECK(c.check(kBase + 10u + 1u));
    CHECK_FALSE(c.check(kBase + 12u + 1u));   // after movs
    CHECK(c.check(kBase + 20u));
    CHECK(c.check(kBase + 24u));
    CHECK(c.check(kBase + 28u));
    CHECK_FALSE(c.check(kBase + 32u));        // after mov
    CHECK_FALSE(c.check(kBase + 22u));        // misaligned ARM
    CHECK_FALSE(c.check(kBase + 1u));         // before the code
    CHECK_FALSE(c.check(kBase + 0x100u + 1u)); // outside the code
}
