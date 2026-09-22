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
/* In .text, exactly like kubridge's lvl0 nodes: Excpmgr writes the link word
 * into the node, and the chain executes the code at node+8. Registering a
 * node placed in the RWX data segment rebooted the 3.60 console (2026-09-22). */
.section .text
.balign 16
/* One raw priority-0 node per exception kind. The SVC node exists because
 * the firmware saves and restores PMOVSR (write-one-to-clear) around the
 * blocking switch inside a syscall: an overflow raised during a user work
 * window that ends with a syscall rather than an IRQ would be cleared before
 * any IRQ entry could see it. Servicing at SVC entry closes that gap. */
.macro VITA_TRACY_RAW_NODE node, entry, handler
.global \node
.type \node, %object
\node:
    .word 0
    .word 0
.size \node, 8

.global \entry
.type \entry, %function
\entry:
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

    mov r4, sp
    bic sp, sp, #7
    mov r0, r4
    bl \handler
    mov sp, r4

    add r0, sp, #VITA_IRQ_USER_SP
    ldmia r0, {sp, lr}^
    nop
    ldr r0, [sp, #VITA_IRQ_SPSR]
    msr spsr_cxsf, r0
    adr r0, \node
    ldr r0, [r0]
    add r0, r0, #8
    str r0, [sp, #VITA_IRQ_CHAIN]
    ldr r0, [sp, #VITA_IRQ_CPSR]
    msr cpsr_fs, r0
    ldr lr, [sp, #VITA_IRQ_LINK]
    ldmia sp, {r0-r12}
    add sp, sp, #VITA_IRQ_CHAIN
    ldr pc, [sp], #4
.size \entry, .-\entry
.endm

VITA_TRACY_RAW_NODE vita_tracy_irq_handler_node, vita_tracy_irq_entry, vita_tracy_irq_handler_c
.balign 16
VITA_TRACY_RAW_NODE vita_tracy_svc_handler_node, vita_tracy_svc_entry, vita_tracy_svc_handler_c

.section .note.GNU-stack,"",%progbits
