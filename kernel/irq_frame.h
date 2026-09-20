#pragma once

/* Shared with irq_entry.S: only this project constructs this frame. These are
 * not offsets into an undocumented Sony exception frame. */
#define VITA_IRQ_GPRS       0
#define VITA_IRQ_USER_SP   52
#define VITA_IRQ_USER_LR   56
#define VITA_IRQ_LINK      60
#define VITA_IRQ_SPSR      64
#define VITA_IRQ_CPSR      68
#define VITA_IRQ_CHAIN     76
#define VITA_IRQ_FRAME_SIZE 80

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>

typedef struct VitaTracyIrqFrame {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t irq_lr;
    uint32_t spsr;
    uint32_t entry_cpsr;
    uint32_t reserved;
    uint32_t next_entry;
} VitaTracyIrqFrame;

#ifdef __cplusplus
#define VITA_IRQ_ASSERT static_assert
#else
#define VITA_IRQ_ASSERT _Static_assert
#endif
VITA_IRQ_ASSERT(offsetof(VitaTracyIrqFrame, sp) == VITA_IRQ_USER_SP, "IRQ user SP offset");
VITA_IRQ_ASSERT(offsetof(VitaTracyIrqFrame, lr) == VITA_IRQ_USER_LR, "IRQ user LR offset");
VITA_IRQ_ASSERT(offsetof(VitaTracyIrqFrame, irq_lr) == VITA_IRQ_LINK, "IRQ link offset");
VITA_IRQ_ASSERT(offsetof(VitaTracyIrqFrame, spsr) == VITA_IRQ_SPSR, "IRQ SPSR offset");
VITA_IRQ_ASSERT(offsetof(VitaTracyIrqFrame, entry_cpsr) == VITA_IRQ_CPSR, "IRQ CPSR offset");
VITA_IRQ_ASSERT(offsetof(VitaTracyIrqFrame, next_entry) == VITA_IRQ_CHAIN, "IRQ chain offset");
VITA_IRQ_ASSERT(sizeof(VitaTracyIrqFrame) == VITA_IRQ_FRAME_SIZE, "IRQ frame size");
#undef VITA_IRQ_ASSERT
#endif
