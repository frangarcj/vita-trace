#include "vita_tracy/stack_scan.h"

#include <string.h>

static uint16_t half_at(const uint8_t *code, uint32_t offset) {
    uint16_t v;
    memcpy(&v, code + offset, sizeof(v));
    return v;
}

static uint32_t word_at(const uint8_t *code, uint32_t offset) {
    uint32_t v;
    memcpy(&v, code + offset, sizeof(v));
    return v;
}

int vita_trace_is_call_return(uint32_t addr, const uint8_t *code, uint32_t code_base, uint32_t code_size) {
    if (!code) return 0;
    if (addr & 1u) {
        const uint32_t ret = addr & ~1u;
        if (ret < code_base + 2u || ret > code_base + code_size) return 0;
        const uint32_t off = ret - code_base;
        /* BLX Rm: 0100 0111 1mmm m000 */
        if ((half_at(code, off - 2u) & 0xFF87u) == 0x4780u) return 1;
        if (off < 4u) return 0;
        /* BL / BLX imm: 11110 S imm10, then 11 J1 1 J2 (BL) or 11 J1 0 J2 (BLX) */
        const uint16_t hw1 = half_at(code, off - 4u), hw2 = half_at(code, off - 2u);
        return (hw1 & 0xF800u) == 0xF000u && (hw2 & 0xC000u) == 0xC000u;
    }
    if (addr & 3u) return 0;
    if (addr < code_base + 4u || addr > code_base + code_size) return 0;
    const uint32_t insn = word_at(code, addr - code_base - 4u);
    if ((insn & 0xFE000000u) == 0xFA000000u) return 1;                                   /* BLX imm */
    if ((insn & 0x0FFFFFF0u) == 0x012FFF30u && (insn >> 28) != 0xFu) return 1;           /* BLX Rm */
    return (insn & 0x0F000000u) == 0x0B000000u && (insn >> 28) != 0xFu;                  /* BL */
}
