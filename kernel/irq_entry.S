#include "irq_frame.h"

.syntax unified
.arch armv7-a
.cpu cortex-a9
.arm

/* Register a raw priority-zero node, not a C callback. kubridge's abort
 * handlers demonstrate the two-word link prefix and explicit next+8 chain.
 * This independent IRQ entry preserves the interrupted registers itself and
 * always leaves GIC dispatch/acknowledgement and exception return to Sony.
 * See docs/kubridge-review.md for evidence and the still-unverified IRQ route. */
.section .vita_tracy_exc_handler,"awx",%progbits
.balign 8
.global vita_tracy_irq_handler_node
.type vita_tracy_irq_handler_node, %object
vita_tracy_irq_handler_node:
    .word 0
    .word 0
.size vita_tracy_irq_handler_node, 8

.global vita_tracy_irq_entry
.type vita_tracy_irq_entry, %function
vita_tracy_irq_entry:
    sub sp, sp, #VITA_IRQ_FRAME_SIZE
    stmia sp, {r0-r12}
    str lr, [sp, #VITA_IRQ_LINK]
    add r0, sp, #VITA_IRQ_USER_SP
    stmia r0, {sp, lr}^
    nop
    mrs r0, spsr
    str r0, [sp, #VITA_IRQ_SPSR]
    mrs r0, cpsr
    str r0, [sp, #VITA_IRQ_CPSR]

    /* AAPCS requires 8-byte SP alignment at C calls. Do not assume the raw
     * firmware entry provides it. r4 is callee-saved and remembers the frame. */
    mov r4, sp
    bic sp, sp, #7
    mov r0, r4
    bl vita_tracy_irq_handler_c
    mov sp, r4

    /* No exception return here. Restore the exact entry state and tail-chain
     * even when no sample was wanted. A C return to raw LR_irq is not valid. */
    add r0, sp, #VITA_IRQ_USER_SP
    ldmia r0, {sp, lr}^
    nop
    ldr r0, [sp, #VITA_IRQ_SPSR]
    msr spsr_cxsf, r0
    adr r0, vita_tracy_irq_handler_node
    ldr r0, [r0]
    add r0, r0, #8
    str r0, [sp, #VITA_IRQ_CHAIN]
    ldr r0, [sp, #VITA_IRQ_CPSR]
    msr cpsr_fs, r0
    ldr lr, [sp, #VITA_IRQ_LINK]
    ldmia sp, {r0-r12}
    add sp, sp, #VITA_IRQ_CHAIN
    ldr pc, [sp], #4
.size vita_tracy_irq_entry, .-vita_tracy_irq_entry

.section .note.GNU-stack,"",%progbits
