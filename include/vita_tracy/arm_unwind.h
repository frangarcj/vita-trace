#pragma once

#include <stdint.h>

#include "vita_tracy/stack_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A module's readable image as the console maps it, with the bounds of its
 * .ARM.exidx table inside it (the linker's __exidx_start/__exidx_end). The
 * table's prel31 offsets make it valid wherever ASLR put the module. */
typedef struct VitaTraceUnwindImage {
    const uint8_t *bytes; /* bytes[0] is the byte at `base` */
    uint32_t base;
    uint32_t size;
    uint32_t exidx_start;
    uint32_t exidx_end;
} VitaTraceUnwindImage;

/* Registers at the sample. `stack` is a copy of `stack_words` words starting
 * at `sp`. */
typedef struct VitaTraceUnwindRegs {
    uint32_t pc, sp, lr, r7, r11;
} VitaTraceUnwindRegs;

/* Callstack from the ARM EHABI unwind tables (-funwind-tables): the PC, then
 * one return address per frame, innermost first. A frame without a table
 * entry (JIT code, CANTUNWIND ranges, other modules) or whose unwind does
 * not land on a call site falls back to LR on the first frame, then to
 * scanning the stack for the next return address, and exact unwinding
 * resumes from there. A frame whose saved registers lie beyond the stack
 * copy ends the callstack: scanning its locals would only find stale return
 * addresses. `is_return` is the scan's call-site check; it also vets every
 * unwound address. Returns the number of frames written. */
uint32_t vita_trace_arm_unwind(const VitaTraceUnwindImage *image, const VitaTraceUnwindRegs *regs,
                               const uint32_t *stack, uint32_t stack_words,
                               VitaTraceReturnCheck is_return, void *ctx,
                               uint32_t *frames, uint32_t max_frames);

#ifdef __cplusplus
}
#endif
