#pragma once
#include <psp2kern/types.h>

/* Host test fields only; Vita builds compile against the real SDK layout. */
typedef struct SceKernelSegmentInfo {
    void *vaddr;
    SceSize memsz;
    uint32_t perms;
} SceKernelSegmentInfo;

typedef struct SceKernelModuleInfo {
    SceSize size;
    SceUID modid;
    char module_name[28];
    SceKernelSegmentInfo segments[4];
} SceKernelModuleInfo;
