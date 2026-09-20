#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t SceSysTimerId;
typedef uint64_t SceKernelSysClock;
typedef void (*SceSysTimerCallback)(SceSysTimerId, void *);
typedef enum { SCE_SYSTIMER_TYPE_LONG = 1, SCE_SYSTIMER_TYPE_WORD = 2 } SceSysTimerType;
typedef enum { SCE_SYSTIMER_CLOCK_SOURCE_SYS = 0, SCE_SYSTIMER_CLOCK_SOURCE_48MHZ = 3 } SceSysTimerClockSource;
SceSysTimerId ksceKernelSysTimerAlloc(SceSysTimerType type);
int ksceKernelSysTimerFree(SceSysTimerId timer);
int ksceKernelSysTimerStartCount(SceSysTimerId timer);
int ksceKernelSysTimerStopCount(SceSysTimerId timer);
int ksceKernelSysTimerSetClockSource(SceSysTimerId timer, SceSysTimerClockSource source, uint8_t prescale);
int ksceKernelSysTimerSetInterval(SceSysTimerId timer, SceKernelSysClock interval);
int ksceKernelSysTimerSetHandler(SceSysTimerId timer, SceSysTimerCallback callback, uint32_t mask, void *arg);
int ksceKernelSysTimerResetCount(SceSysTimerId timer);
#ifdef __cplusplus
}
#endif
