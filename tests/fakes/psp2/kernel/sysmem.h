#pragma once
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW 1
#ifdef __cplusplus
extern "C" {
#endif
SceUID sceKernelAllocMemBlock(const char *, int, SceSize, const void *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#ifdef __cplusplus
}
#endif
