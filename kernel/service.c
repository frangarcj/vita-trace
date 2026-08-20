#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

static VitaTracyKernelState g_state;

VitaTracyKernelState *vita_tracy_state(void) {
    return &g_state;
}

uint64_t vita_tracy_kernel_now(void) {
    return (uint64_t)ksceKernelGetSystemTimeWide();
}

static int transition(VitaTracyKernelState *st, VitaTracyStateEvent event) {
    VitaTracyState next;
    if (!vita_tracy_state_next(st->state, event, &next)) {
        return 0;
    }
    st->state = next;
    return 1;
}

void vita_tracy_emit_sample(VitaTracyKernelState *st, uint32_t cpu, const VitaTraceSample *sample) {
    if (st->shared == NULL || cpu >= VITA_TRACE_CORE_COUNT) {
        return;
    }
    void *ring = vita_trace_shared_core_ring(st->shared, cpu);
    if (ring == NULL) {
        return;
    }
    if (vita_trace_ring_try_push(ring, sample)) {
        st->stats.samples_emitted[cpu]++;
    } else {
        st->stats.samples_dropped[cpu]++;
    }
}

void vita_tracy_emit_control(VitaTracyKernelState *st, const VitaTraceControlRecord *record) {
    if (st->shared == NULL) {
        return;
    }
    void *ring = vita_trace_shared_control_ring(st->shared);
    if (ring == NULL) {
        return;
    }
    if (!vita_trace_ring_try_push(ring, record)) {
        st->stats.control_dropped++;
    }
}

void vita_tracy_detach(VitaTracyKernelState *st) {
    vita_tracy_sampler_stop(st);

    if (st->map_uid > 0) {
        ksceKernelUserUnmap(st->map_uid);
        st->map_uid = 0;
    }
    st->shared = NULL;
    st->shared_size = 0;
    st->target_pid = 0;

    transition(st, VITA_TRACY_EVENT_DETACH);
}

static int copy_args_from_user(SceUID pid, void *dst, const void *user_src, SceSize size) {
    return ksceKernelCopyFromUserProc(pid, dst, user_src, size) < 0 ? 0 : 1;
}

int vitaTracyRegister(const VitaTracyRegisterArgs *args) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();

    VitaTracyRegisterArgs local;
    if (!copy_args_from_user(caller_pid, &local, args, sizeof(local))) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (local.size != sizeof(VitaTracyRegisterArgs) || local.abi_version != VITA_TRACY_ABI_VERSION) {
        return VITA_TRACY_ERROR_ABI;
    }
    if (local.ring_user_addr == 0 || local.ring_size == 0) {
        return VITA_TRACY_ERROR_ARGS;
    }

    /* A process may only register its own ring; profiling another process
     * is the agent's job and goes through a separate path. */
    SceUID target_pid = (SceUID)local.target_pid;
    if (target_pid != caller_pid) {
        return VITA_TRACY_ERROR_TARGET;
    }

    if (st->state != VITA_TRACY_STATE_READY) {
        return VITA_TRACY_ERROR_STATE;
    }

    void *kernel_page = NULL;
    SceSize kernel_size = 0;
    SceUInt32 kernel_offset = 0;
    SceUID map_uid = ksceKernelProcUserMap(target_pid, "VitaTracyRing", 3,
                                           (void *)(uintptr_t)local.ring_user_addr, local.ring_size,
                                           &kernel_page, &kernel_size, &kernel_offset);
    if (map_uid < 0) {
        return VITA_TRACY_ERROR_MAP;
    }

    void *shared = (uint8_t *)kernel_page + kernel_offset;
    if (!vita_trace_shared_is_valid(shared)) {
        ksceKernelUserUnmap(map_uid);
        return VITA_TRACY_ERROR_ABI;
    }

    if (!transition(st, VITA_TRACY_EVENT_ATTACH)) {
        ksceKernelUserUnmap(map_uid);
        return VITA_TRACY_ERROR_STATE;
    }

    st->map_uid = map_uid;
    st->shared = shared;
    st->shared_size = local.ring_size;
    st->target_pid = target_pid;
    st->sampling_hz = VITA_TRACE_DEFAULT_SAMPLE_HZ;

    VitaTraceControlRecord record;
    memset(&record, 0, sizeof(record));
    record.type = VITA_TRACE_CLOCK_SYNC;
    record.timestamp = vita_tracy_kernel_now();
    record.payload.clock_sync.kernel_tick = record.timestamp;
    vita_tracy_emit_control(st, &record);

    vita_tracy_modules_snapshot(st, target_pid);
    return VITA_TRACY_OK;
}

int vitaTracyUnregister(uint32_t target_pid) {
    VitaTracyKernelState *st = vita_tracy_state();

    if (st->target_pid == 0 || (SceUID)target_pid != st->target_pid) {
        return VITA_TRACY_ERROR_TARGET;
    }

    vita_tracy_detach(st);
    return VITA_TRACY_OK;
}

int vitaTracySetSampling(const VitaTracySamplingConfig *cfg) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();

    VitaTracySamplingConfig local;
    if (!copy_args_from_user(caller_pid, &local, cfg, sizeof(local))) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (local.size != sizeof(VitaTracySamplingConfig) ||
        local.abi_version != VITA_TRACY_ABI_VERSION) {
        return VITA_TRACY_ERROR_ABI;
    }

    if (st->state != VITA_TRACY_STATE_ATTACHED && st->state != VITA_TRACY_STATE_PROFILING &&
        st->state != VITA_TRACY_STATE_STOPPED) {
        return VITA_TRACY_ERROR_STATE;
    }

    if (local.frequency_hz == 0) {
        if (st->state == VITA_TRACY_STATE_PROFILING) {
            vita_tracy_sampler_stop(st);
            transition(st, VITA_TRACY_EVENT_STOP);
        }
        return VITA_TRACY_OK;
    }

    st->sampling_hz = local.frequency_hz;

    if (st->state == VITA_TRACY_STATE_PROFILING) {
        return VITA_TRACY_OK;
    }

    if (!transition(st, VITA_TRACY_EVENT_START)) {
        return VITA_TRACY_ERROR_STATE;
    }

    int ret = vita_tracy_sampler_start(st);
    if (ret != VITA_TRACY_OK) {
        transition(st, VITA_TRACY_EVENT_STOP);
    }
    return ret;
}

int vitaTracySetPmu(const VitaTracyPmuConfig *cfg) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();

    VitaTracyPmuConfig local;
    if (!copy_args_from_user(caller_pid, &local, cfg, sizeof(local))) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (local.size != sizeof(VitaTracyPmuConfig) || local.abi_version != VITA_TRACY_ABI_VERSION) {
        return VITA_TRACY_ERROR_ABI;
    }
    if (local.counter_count > VITA_TRACY_PMU_MAX_COUNTERS) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (st->state == VITA_TRACY_STATE_UNINITIALIZED || st->state == VITA_TRACY_STATE_READY) {
        return VITA_TRACY_ERROR_STATE;
    }

    return vita_tracy_pmu_configure(st, &local);
}

int vitaTracySnapshotModules(uint32_t target_pid) {
    VitaTracyKernelState *st = vita_tracy_state();

    if (st->target_pid == 0 || (SceUID)target_pid != st->target_pid) {
        return VITA_TRACY_ERROR_TARGET;
    }
    return vita_tracy_modules_snapshot(st, st->target_pid);
}

int vitaTracyGetStats(VitaTracyStats *stats) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();

    VitaTracyStats local;
    if (!copy_args_from_user(caller_pid, &local, stats, sizeof(local))) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (local.size != sizeof(VitaTracyStats) || local.abi_version != VITA_TRACY_ABI_VERSION) {
        return VITA_TRACY_ERROR_ABI;
    }

    st->stats.size = sizeof(VitaTracyStats);
    st->stats.abi_version = VITA_TRACY_ABI_VERSION;
    st->stats.uptime_ms = (uint32_t)(vita_tracy_kernel_now() / 1000ull);

    if (ksceKernelCopyToUserProc(caller_pid, stats, &st->stats, sizeof(st->stats)) < 0) {
        return VITA_TRACY_ERROR_ARGS;
    }
    return VITA_TRACY_OK;
}
