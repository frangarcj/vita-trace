#pragma once
#include <stddef.h>
#include <stdint.h>
#include <psp2kern/types.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { SCE_EVENT_WAITOR = 1, SCE_EVENT_WAITCLEAR_PAT = 4 };
int ksceKernelCreateEventFlag(const char *name, int attr, int bits, void *opt);
int ksceKernelDeleteEventFlag(int event);
int ksceKernelSetEventFlag(int event, unsigned int bits);
int ksceKernelWaitEventFlag(int event, unsigned int bits, unsigned int mode,
                           unsigned int *out_bits, uint32_t *timeout);
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
SceUID ksceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int priority,
                             SceSize stack, unsigned attr, int affinity, const void *opt);
int ksceKernelStartThread(SceUID thread, SceSize args, void *argp);
int ksceKernelWaitThreadEnd(SceUID thread, int *status, SceUInt *timeout);
int ksceKernelDeleteThread(SceUID thread);
int ksceKernelDelayThread(SceUInt delay);
#ifdef __cplusplus
}
#endif
