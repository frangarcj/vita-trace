#pragma once
#include <psp2kern/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SceKernelThreadContextInfo {
    SceUID process_id;
    SceUID thread_id;
} SceKernelThreadContextInfo;

int ksceKernelGetThreadContextInfo(SceKernelThreadContextInfo *info);

#ifdef __cplusplus
}
#endif
