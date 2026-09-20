#pragma once
#include "sysmem.h"
typedef int (*SceKernelThreadEntry)(SceSize, void *);
#ifdef __cplusplus
extern "C" {
#endif
SceUID sceKernelGetProcessId(void);
SceUID sceKernelCreateThread(const char *, SceKernelThreadEntry, int, SceSize, unsigned, int, const void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelWaitThreadEnd(SceUID, int *, unsigned *);
int sceKernelDeleteThread(SceUID);
SceUID sceKernelCreateSema(const char *, unsigned, int, int, const void *);
int sceKernelDeleteSema(SceUID);
int sceKernelSignalSema(SceUID, int);
int sceKernelWaitSema(SceUID, int, unsigned *);
#ifdef __cplusplus
}
#endif
