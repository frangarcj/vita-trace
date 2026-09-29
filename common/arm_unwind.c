#include "vita_tracy/arm_unwind.h"

#include <string.h>

/* ARM EHABI (IHI 0038) table-driven unwinding over a copy of the stack. Only
 * the unwind opcodes are interpreted; personality routines never run. */

#define EXIDX_CANTUNWIND 1u
#define MAX_OPCODES 32u
#define REG(n) (1u << (n))

typedef struct Frame {
    uint32_t r[16];
    uint32_t known; /* REG(n): r[n] holds this frame's value */
} Frame;

typedef struct StackCopy {
    const uint32_t *words;
    uint32_t base;
    uint32_t count;
} StackCopy;

typedef struct Opcodes {
    uint8_t b[MAX_OPCODES];
    uint32_t n;
} Opcodes;

static void push(uint32_t *frames, uint32_t *count, uint32_t max_frames, uint32_t addr) {
    if (*count >= max_frames) return;
    if (*count && frames[*count - 1] == addr) return;
    frames[(*count)++] = addr;
}

static int image_word(const VitaTraceUnwindImage *im, uint32_t addr, uint32_t *out) {
    if ((addr & 3u) || im->size < 4u || addr < im->base || addr - im->base > im->size - 4u) return 0;
    memcpy(out, im->bytes + (addr - im->base), sizeof(*out));
    return 1;
}

static uint32_t prel31(uint32_t at, uint32_t word) {
    uint32_t offset = word & 0x7FFFFFFFu;
    if (offset & 0x40000000u) offset |= 0x80000000u;
    return at + offset;
}

/* Address of the exidx entry for the function containing `addr`, 0 if the
 * table has none. Entries are sorted by function address. */
static uint32_t find_entry(const VitaTraceUnwindImage *im, uint32_t addr) {
    if (!im || !im->bytes || addr < im->base || addr - im->base >= im->size) return 0;
    if (im->exidx_end <= im->exidx_start) return 0;
    uint32_t lo = 0, hi = (im->exidx_end - im->exidx_start) / 8u, best = 0;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint32_t at = im->exidx_start + mid * 8u;
        uint32_t word;
        if (!image_word(im, at, &word)) return 0;
        if (prel31(at, word) <= addr) {
            best = at;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return best;
}

static void add_bytes(Opcodes *ops, uint32_t word, uint32_t count) {
    for (uint32_t i = 0; i < count && ops->n < MAX_OPCODES; ++i)
        ops->b[ops->n++] = (uint8_t)(word >> (8u * (count - 1u - i)));
}

static int load_opcodes(const VitaTraceUnwindImage *im, uint32_t entry, Opcodes *ops) {
    uint32_t data;
    ops->n = 0;
    if (!image_word(im, entry + 4u, &data) || data == EXIDX_CANTUNWIND) return 0;
    uint32_t at = entry + 4u, word = data;
    if (!(data & 0x80000000u)) { /* out of line, in .ARM.extab */
        at = prel31(entry + 4u, data);
        if (!image_word(im, at, &word)) return 0;
    }
    uint32_t extra;
    if (word & 0x80000000u) { /* compact model */
        const uint32_t index = (word >> 24) & 0xFu;
        if (index == 0) {
            extra = 0;
            add_bytes(ops, word, 3);
        } else if (index == 1 || index == 2) {
            extra = (word >> 16) & 0xFFu;
            add_bytes(ops, word, 2);
        } else {
            return 0;
        }
    } else { /* generic personality routine: count and opcodes follow its address */
        at += 4u;
        if (!image_word(im, at, &word)) return 0;
        extra = word >> 24;
        add_bytes(ops, word, 3);
    }
    if (extra > (MAX_OPCODES - 3u) / 4u) return 0;
    for (uint32_t i = 1; i <= extra; ++i) {
        if (!image_word(im, at + 4u * i, &word)) return 0;
        add_bytes(ops, word, 4);
    }
    return 1;
}

static int stack_word(const StackCopy *s, uint32_t addr, uint32_t *out) {
    if ((addr & 3u) || addr < s->base || (addr - s->base) / 4u >= s->count) return 0;
    *out = s->words[(addr - s->base) / 4u];
    return 1;
}

/* Results of interpreting a frame's opcodes. */
#define UNWIND_OK 1
#define UNWIND_FAILED 0
#define UNWIND_PAST_COPY (-1) /* needed a word beyond the stack copy */

static int pop_mask(Frame *f, const StackCopy *s, uint32_t *vsp, uint32_t mask) {
    uint32_t new_sp = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(mask & REG(i))) continue;
        uint32_t v;
        if (!stack_word(s, *vsp, &v)) return UNWIND_PAST_COPY;
        *vsp += 4u;
        if (i == 13) {
            new_sp = v;
        } else {
            f->r[i] = v;
            f->known |= REG(i);
        }
    }
    if (mask & REG(13)) *vsp = new_sp;
    return UNWIND_OK;
}

static int execute(const Opcodes *ops, Frame *f, const StackCopy *s) {
    uint32_t vsp = f->r[13];
    uint32_t i = 0;
    while (i < ops->n) {
        const uint8_t op = ops->b[i++];
        if ((op & 0xC0u) == 0x00u) {
            vsp += ((op & 0x3Fu) << 2) + 4u;
        } else if ((op & 0xC0u) == 0x40u) {
            vsp -= ((op & 0x3Fu) << 2) + 4u;
        } else if ((op & 0xF0u) == 0x80u) { /* pop r4-r15 under mask */
            if (i >= ops->n) return 0;
            const uint32_t mask = (((op & 0x0Fu) << 8) | ops->b[i++]) << 4;
            if (!mask) return 0; /* 0x8000: refuse to unwind */
            const int popped = pop_mask(f, s, &vsp, mask);
            if (popped != UNWIND_OK) return popped;
        } else if ((op & 0xF0u) == 0x90u) { /* vsp = r[n] */
            const uint32_t n = op & 0x0Fu;
            if (n == 13 || n == 15 || !(f->known & REG(n))) return 0;
            vsp = f->r[n];
        } else if ((op & 0xF0u) == 0xA0u) { /* pop r4-r[4+n], optionally r14 */
            uint32_t mask = ((2u << (op & 7u)) - 1u) << 4;
            if (op & 0x08u) mask |= REG(14);
            const int popped = pop_mask(f, s, &vsp, mask);
            if (popped != UNWIND_OK) return popped;
        } else if (op == 0xB0u) { /* finish */
            break;
        } else if (op == 0xB1u) { /* pop r0-r3 under mask */
            if (i >= ops->n) return 0;
            const uint8_t mask = ops->b[i++];
            if (!mask || (mask & 0xF0u)) return 0;
            const int popped = pop_mask(f, s, &vsp, mask);
            if (popped != UNWIND_OK) return popped;
        } else if (op == 0xB2u) { /* vsp += 0x204 + (uleb128 << 2) */
            uint32_t v = 0, shift = 0;
            uint8_t b;
            do {
                if (i >= ops->n || shift > 28u) return 0;
                b = ops->b[i++];
                v |= (uint32_t)(b & 0x7Fu) << shift;
                shift += 7u;
            } while (b & 0x80u);
            vsp += 0x204u + (v << 2);
        } else if (op == 0xB3u || op == 0xC6u || op == 0xC8u || op == 0xC9u) {
            /* VFP D registers (FSTMFDX adds a pad word) or iWMMXt wR registers */
            if (i >= ops->n) return 0;
            vsp += ((ops->b[i++] & 0x0Fu) + 1u) * 8u + (op == 0xB3u ? 4u : 0u);
        } else if ((op & 0xF8u) == 0xB8u) { /* d8-d[8+n], FSTMFDX */
            vsp += ((op & 7u) + 1u) * 8u + 4u;
        } else if ((op & 0xF8u) == 0xD0u || (op & 0xF8u) == 0xC0u) { /* d8-d[8+n] VPUSH; wR10-wR[10+n] */
            if (op == 0xC7u) {                                    /* wCGR under mask */
                if (i >= ops->n) return 0;
                const uint8_t mask = ops->b[i++];
                if (!mask || (mask & 0xF0u)) return 0;
                vsp += 4u * (uint32_t)__builtin_popcount(mask);
            } else {
                vsp += ((op & 7u) + 1u) * 8u;
            }
        } else {
            return 0; /* spare */
        }
    }
    f->r[13] = vsp;
    return UNWIND_OK;
}

/* Unwinds the frame of the function at `pc`; returns its return address.
 * `*past_copy` is set when the frame's saved registers lie beyond the copy. */
static uint32_t unwind_frame(const VitaTraceUnwindImage *im, uint32_t entry, Frame *f, const StackCopy *s,
                             int first, VitaTraceReturnCheck is_return, void *ctx, int *past_copy) {
    Opcodes ops;
    *past_copy = 0;
    if (!entry || !load_opcodes(im, entry, &ops)) return 0;
    Frame t = *f;
    t.known &= ~REG(15);
    const int result = execute(&ops, &t, s);
    if (result == UNWIND_PAST_COPY) *past_copy = 1;
    if (result != UNWIND_OK) return 0;
    const uint32_t ra = (t.known & REG(15)) ? t.r[15] : (t.known & REG(14)) ? t.r[14] : 0u;
    /* A frame that made a call saved LR, so its CFA lies above SP; only the
     * sampled frame can be a leaf. */
    if (!ra || t.r[13] < f->r[13] || (!first && t.r[13] == f->r[13]) || !is_return(ctx, ra)) return 0;
    *f = t;
    f->known &= ~(REG(14) | REG(15)); /* dead in the caller after its call */
    return ra;
}

uint32_t vita_trace_arm_unwind(const VitaTraceUnwindImage *image, const VitaTraceUnwindRegs *regs,
                               const uint32_t *stack, uint32_t stack_words,
                               VitaTraceReturnCheck is_return, void *ctx,
                               uint32_t *frames, uint32_t max_frames) {
    uint32_t count = 0;
    if (!frames || !max_frames || !regs) return 0;
    push(frames, &count, max_frames, regs->pc);
    if (!is_return) return count;

    const StackCopy s = {stack, regs->sp, stack ? stack_words : 0u};
    Frame f;
    memset(&f, 0, sizeof(f));
    f.r[7] = regs->r7;
    f.r[11] = regs->r11;
    f.r[13] = regs->sp;
    f.r[14] = regs->lr;
    f.known = REG(7) | REG(11) | REG(13) | REG(14);

    uint32_t pc = regs->pc;
    for (int first = 1; count < max_frames; first = 0) {
        /* A return address can sit just past a call at the very end of its
         * function; look up the call instruction instead. */
        const uint32_t entry = find_entry(image, (pc & ~1u) - (first ? 0u : 2u));
        int past_copy;
        uint32_t next = unwind_frame(image, entry, &f, &s, first, is_return, ctx, &past_copy);
        /* The table says where the return address is and the copy does not
         * reach it. Scanning from here would only find stale addresses in
         * this frame's locals, so the callstack ends. */
        if (!next && past_copy && !first) break;
        if (!next && first && is_return(ctx, regs->lr) &&
            (!entry || find_entry(image, (regs->lr & ~1u) - 2u) != entry)) {
            /* A leaf without a table entry, or a PC in the prologue before
             * LR was saved: LR is the caller. */
            next = regs->lr;
            f.known &= ~(REG(14) | REG(15));
        }
        if (!next && !past_copy) {
            /* No usable table: the next return address on the stack names
             * the caller, and the word above it is where its frame begins. */
            for (uint32_t addr = f.r[13];; addr += 4u) {
                uint32_t v;
                if (!stack_word(&s, addr, &v)) break;
                if (is_return(ctx, v)) {
                    next = v;
                    f.r[13] = addr + 4u;
                    break;
                }
            }
            f.known = REG(13);
        }
        if (!next) break;
        push(frames, &count, max_frames, next);
        pc = next;
    }
    return count;
}
