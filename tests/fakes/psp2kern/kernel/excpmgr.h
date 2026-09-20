#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum SceExcpKind {
    SCE_EXCP_RESET = 0,
    SCE_EXCP_UNDEF_INSTRUCTION = 1,
    SCE_EXCP_SVC = 2,
    SCE_EXCP_PABT = 3,
    SCE_EXCP_DABT = 4,
    SCE_EXCP_UNUSED = 5,
    SCE_EXCP_IRQ = 6,
    SCE_EXCP_FIQ = 7
} SceExcpKind;

typedef enum SceExcpHandlingCode {
    SCE_EXCPMGR_EXCEPTION_HANDLED = 0,
    SCE_EXCPMGR_EXCEPTION_NOT_HANDLED = 1,
    SCE_EXCPMGR_EXCEPTION_HANDLING_CODE_2 = 2,
    SCE_EXCPMGR_EXCEPTION_NOT_HANDLED_FATAL = 3,
    SCE_EXCPMGR_EXCEPTION_HANDLING_CODE_4 = 4
} SceExcpHandlingCode;

typedef struct SceExcpmgrExceptionContext {
    uint32_t r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12;
    uint32_t sp, lr, address_of_faulting_instruction;
    SceExcpKind ExceptionKind;
    uint32_t SPSR;
} SceExcpmgrExceptionContext;

int ksceExcpmgrRegisterHandler(SceExcpKind kind, int priority, void *handler);

#ifdef __cplusplus
}
#endif
