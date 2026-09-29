#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decides whether an address (Thumb bit included) is the return address of
 * a call; the unwinder (arm_unwind.h) uses it to vet frames and to scan the
 * stack where no unwind table applies. */
typedef int (*VitaTraceReturnCheck)(void *ctx, uint32_t addr);

/* The instruction(s) that end right before `addr` are a call: BL/BLX (imm)
 * or BLX (register), in the state the return address selects (bit 0 set:
 * Thumb). `code` holds the bytes [code_base, code_base + code_size). */
int vita_trace_is_call_return(uint32_t addr, const uint8_t *code, uint32_t code_base, uint32_t code_size);

#ifdef __cplusplus
}
#endif
