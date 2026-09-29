#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
uint64_t sceKernelGetProcessTimeWide(void);
uint64_t sceKernelGetSystemTimeWide(void);
#ifdef __cplusplus
}
#endif
