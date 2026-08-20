#pragma once

#include <stdint.h>

#include "vita_tracy/config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VitaTraceEventType {
    VITA_TRACE_SAMPLE = 1,
    VITA_TRACE_THREAD_START,
    VITA_TRACE_THREAD_EXIT,
    VITA_TRACE_PROCESS_EXIT,
    VITA_TRACE_MODULE_SNAPSHOT,
    VITA_TRACE_PMU,
    VITA_TRACE_CLOCK_SYNC,
    VITA_TRACE_DROPPED
} VitaTraceEventType;

/* Fixed-size record for the per-core sample rings. */
typedef struct VitaTraceSample {
    uint64_t timestamp;
    uint32_t pid;
    uint32_t tid;
    uint32_t pc;
    uint32_t sp;
    uint32_t lr;
    uint16_t cpu;
    uint16_t flags;
} VitaTraceSample;

typedef struct VitaTraceThreadEvent {
    uint32_t pid;
    uint32_t tid;
} VitaTraceThreadEvent;

typedef struct VitaTraceProcessExitEvent {
    uint32_t pid;
} VitaTraceProcessExitEvent;

typedef struct VitaTraceModuleSegment {
    uint32_t vaddr;
    uint32_t memsz;
    uint32_t perm;
} VitaTraceModuleSegment;

typedef struct VitaTraceModuleSnapshotEvent {
    uint32_t pid;
    uint32_t module_nid;
    char module_name[VITA_TRACE_MODULE_NAME_MAX];
    uint32_t segment_count;
    VitaTraceModuleSegment segments[VITA_TRACE_MODULE_MAX_SEGMENTS];
} VitaTraceModuleSnapshotEvent;

typedef struct VitaTracePmuEvent {
    uint32_t tid;
    uint32_t counter;
    uint32_t event_code;
    uint32_t value;
} VitaTracePmuEvent;

typedef struct VitaTraceClockSyncEvent {
    uint64_t kernel_tick;
    uint64_t tracy_tick;
} VitaTraceClockSyncEvent;

/* Fixed-size record for the control ring: lifecycle, module and PMU events
 * that are too infrequent to justify their own per-type ring. */
typedef struct VitaTraceControlRecord {
    uint32_t type; /* VitaTraceEventType */
    uint32_t reserved;
    uint64_t timestamp;
    union {
        VitaTraceThreadEvent thread;
        VitaTraceProcessExitEvent process_exit;
        VitaTraceModuleSnapshotEvent module_snapshot;
        VitaTracePmuEvent pmu;
        VitaTraceClockSyncEvent clock_sync;
    } payload;
} VitaTraceControlRecord;

#ifdef __cplusplus
}
#endif
