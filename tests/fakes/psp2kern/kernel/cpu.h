#pragma once
#include <psp2kern/types.h>
#ifdef __cplusplus
extern "C" {
#endif
int ksceKernelCpuId(void);
SceKernelIntrStatus ksceKernelCpuSuspendIntr(void);
SceKernelIntrStatus ksceKernelCpuResumeIntr(SceKernelIntrStatus state);
#ifdef __cplusplus
}
#endif
