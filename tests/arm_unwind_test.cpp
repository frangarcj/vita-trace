#include <doctest/doctest.h>

#include <cstring>
#include <set>
#include <vector>

#include "vita_tracy/arm_unwind.h"

namespace {

constexpr uint32_t kBase = 0x81000000u;
constexpr uint32_t kExidx = kBase + 0x180u;
constexpr uint32_t kSp = 0x83000000u;

// Six functions of 0x40 bytes each:
//   F0 0x000 leaf, nothing saved                  inline: finish
//   F1 0x040 push {r4, r7, lr}; sub sp, #16        inline: vsp += 16; pop {r4, r7, lr}
//   F2 0x080 push {r4-r11, lr}; sub sp, #724       .ARM.extab (compact, one extra word)
//   F3 0x0C0 no unwind information                 CANTUNWIND
//   F4 0x100 frame pointer r7: push {r7, lr}       inline: vsp = r7; pop {r7, lr}
//   F5 0x140 push {r4, lr}, generic personality    .ARM.extab (personality + data word)
struct Image {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x200, 0);
    void word(uint32_t addr, uint32_t v) { std::memcpy(&bytes[addr - kBase], &v, 4); }
    static uint32_t prel31(uint32_t from, uint32_t to) { return (to - from) & 0x7FFFFFFFu; }
    void entry(uint32_t index, uint32_t fn, uint32_t data) {
        const uint32_t at = kExidx + 8u * index;
        word(at, prel31(at, fn));
        word(at + 4u, data);
    }
    void entry_extab(uint32_t index, uint32_t fn, uint32_t extab) {
        const uint32_t at = kExidx + 8u * index;
        entry(index, fn, prel31(at + 4u, extab));
    }
    Image() {
        entry(0, kBase + 0x000u, 0x80B0B0B0u);
        entry(1, kBase + 0x040u, 0x80038409u);
        entry_extab(2, kBase + 0x080u, kBase + 0x1C0u);
        word(kBase + 0x1C0u, 0x8101B234u);  // index 1, 1 extra word: vsp += 0x204 + (52 << 2)
        word(kBase + 0x1C4u, 0xAFB0B0B0u);  // pop {r4-r11, r14}
        entry(3, kBase + 0x0C0u, 1u);
        entry(4, kBase + 0x100u, 0x80978408u);
        entry_extab(5, kBase + 0x140u, kBase + 0x1D0u);
        word(kBase + 0x1D0u, prel31(kBase + 0x1D0u, kBase));  // personality routine address
        word(kBase + 0x1D4u, 0x00A8B0B0u);  // no extra words; pop {r4, r14}
    }
    VitaTraceUnwindImage view() const {
        return VitaTraceUnwindImage{bytes.data(), kBase, (uint32_t)bytes.size(), kExidx, kExidx + 6u * 8u};
    }
};

// Return addresses of calls, Thumb bit set: F1 calls F0, F2 calls F1, and so on.
constexpr uint32_t kRetF1 = kBase + 0x051u, kRetF2 = kBase + 0x091u, kRetF3 = kBase + 0x0D1u;
constexpr uint32_t kRetF4 = kBase + 0x111u, kRetF5 = kBase + 0x151u, kRetF1b = kBase + 0x061u;

int known_return(void *ctx, uint32_t addr) {
    return static_cast<const std::set<uint32_t> *>(ctx)->count(addr) != 0;
}

struct Stack {
    std::vector<uint32_t> words = std::vector<uint32_t>(260, 0);
    uint32_t &at(uint32_t index) { return words[index]; }
    static uint32_t addr(uint32_t index) { return kSp + 4u * index; }
};

// The stack as F0 <- F1 <- F2 <- F5 <- F4 <- F3 leave it, SP at word 0.
Stack chain() {
    Stack s;
    s.at(4) = 0xAAAA0004u;          // F1 saved r4
    s.at(5) = 0xAAAA0007u;          // F1 saved r7
    s.at(6) = kRetF2;               // F1 saved lr
    // F2: 724 bytes of locals from word 7, then r4-r11 at 188..195 and lr.
    s.at(191) = Stack::addr(201);   // F2 saved r7: F4's frame pointer
    s.at(196) = kRetF5;
    s.at(198) = kRetF4;             // F5: r4 at 197, lr at 198
    s.at(200) = kRetF1;             // dead slot the frame pointer skips
    s.at(202) = kRetF3;             // F4: r7 at 201, lr at 202
    s.at(203) = 0x12345678u;        // F3 has no table: scanned past
    s.at(204) = kRetF1b;            // F3 was called from F1
    return s;
}

const std::set<uint32_t> kReturns{kRetF1, kRetF2, kRetF3, kRetF4, kRetF5, kRetF1b};

} // namespace

TEST_CASE("EHABI unwind follows each table kind and resumes after a scan") {
    Image image;
    const VitaTraceUnwindImage view = image.view();
    Stack s = chain();
    const VitaTraceUnwindRegs regs{kBase + 0x011u, kSp, kRetF1, 0u, 0u};
    uint32_t frames[16];
    const uint32_t n = vita_trace_arm_unwind(&view, &regs, s.words.data(), (uint32_t)s.words.size(),
                                             known_return, (void *)&kReturns, frames, 16);
    REQUIRE(n == 7);
    CHECK(frames[0] == kBase + 0x011u);
    CHECK(frames[1] == kRetF1);   // F0 is a leaf: LR
    CHECK(frames[2] == kRetF2);   // F1: sub sp + pop under mask
    CHECK(frames[3] == kRetF5);   // F2: 760-byte frame from .ARM.extab
    CHECK(frames[4] == kRetF4);   // F5: generic personality data
    CHECK(frames[5] == kRetF3);   // F4: vsp from r7, restored by F2's pop
    CHECK(frames[6] == kRetF1b);  // F3: CANTUNWIND, found by scanning
}

TEST_CASE("EHABI unwind stops at the end of the stack copy") {
    Image image;
    const VitaTraceUnwindImage view = image.view();
    Stack s = chain();
    s.at(100) = kRetF4;             // stale return address in F2's locals
    const VitaTraceUnwindRegs regs{kBase + 0x011u, kSp, kRetF1, 0u, 0u};
    uint32_t frames[16];
    // F2's saved lr (word 196) is beyond a 190-word copy.
    const uint32_t n = vita_trace_arm_unwind(&view, &regs, s.words.data(), 190, known_return,
                                             (void *)&kReturns, frames, 16);
    CHECK(n == 3); // not the stale kRetF4 a scan would find
    CHECK(frames[2] == kRetF2);
    CHECK(vita_trace_arm_unwind(&view, &regs, s.words.data(), 260, known_return, (void *)&kReturns, frames, 2) == 2);
}

TEST_CASE("EHABI unwind falls back to LR in a prologue and outside any table") {
    Image image;
    const VitaTraceUnwindImage view = image.view();
    Stack s = chain();
    uint32_t frames[16];

    // PC in F1 before it saved LR: the table's slot holds no call site.
    s.at(6) = 0xDEADBEEFu;
    s.at(10) = kRetF4;
    VitaTraceUnwindRegs regs{kBase + 0x045u, kSp, kRetF2, 0u, 0u};
    uint32_t n = vita_trace_arm_unwind(&view, &regs, s.words.data(), 20, known_return, (void *)&kReturns, frames, 16);
    REQUIRE(n >= 2);
    CHECK(frames[1] == kRetF2);

    // PC in generated code (no module): LR, then F2's table.
    s = chain();
    regs = VitaTraceUnwindRegs{0x90000000u, Stack::addr(7), kRetF2, 0u, 0u};
    n = vita_trace_arm_unwind(&view, &regs, s.words.data() + 7, 200, known_return, (void *)&kReturns, frames, 16);
    REQUIRE(n >= 3);
    CHECK(frames[1] == kRetF2);
    CHECK(frames[2] == kRetF5);
}

TEST_CASE("EHABI unwind without a table degrades to stack scanning") {
    Stack s = chain();
    const VitaTraceUnwindRegs regs{0x90000000u, kSp, 0x1234u, 0u, 0u};
    uint32_t frames[16];
    const uint32_t n = vita_trace_arm_unwind(nullptr, &regs, s.words.data(), (uint32_t)s.words.size(),
                                             known_return, (void *)&kReturns, frames, 16);
    REQUIRE(n == 7); // every return address on the stack, dead slots included
    CHECK(frames[1] == kRetF2);
    CHECK(frames[2] == kRetF5);
    CHECK(frames[4] == kRetF1);
    CHECK(frames[6] == kRetF1b);
    CHECK(vita_trace_arm_unwind(nullptr, &regs, nullptr, 0, nullptr, nullptr, frames, 16) == 1);
}
