#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum { SCE_EVENT_WAITOR = 1, SCE_EVENT_WAITCLEAR_PAT = 4 };
int ksceKernelCreateEventFlag(const char *name, int attr, int bits, void *opt);
int ksceKernelDeleteEventFlag(int event);
int ksceKernelSetEventFlag(int event, unsigned int bits);
int ksceKernelWaitEventFlag(int event, unsigned int bits, unsigned int mode,
                           unsigned int *out_bits, uint32_t *timeout);
#ifdef __cplusplus
}
#endif
