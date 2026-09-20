#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
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
    /* Control messages have multiple kernel producers. Serialize the SPSC
     * writer; disabling local IRQs prevents a same-core producer deadlock. */
    SceKernelIntrStatus intr = ksceKernelCpuSuspendIntr();
    while (__atomic_exchange_n(&st->control_writer_lock, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("yield");
    }
    if (!vita_trace_ring_try_push(ring, record)) {
        st->stats.control_dropped++;
    }
    __atomic_store_n(&st->control_writer_lock, 0u, __ATOMIC_RELEASE);
    ksceKernelCpuResumeIntr(intr);
    vita_tracy_notify(st);
}

void vita_tracy_notify(VitaTracyKernelState *st) {
    if (st->data_event > 0) ksceKernelSetEventFlag(st->data_event, 1u);
}

int vitaTracyWakeup(void) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = VITA_TRACY_ERROR_TARGET;
    if (st->target_pid != 0 && st->target_pid == ksceKernelGetProcessId()) {
        vita_tracy_notify(st);
        ret = VITA_TRACY_OK;
    }
    EXIT_SYSCALL(syscall_state);
    return ret;
}

int vitaTracyWaitForData(uint32_t timeout_us) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = VITA_TRACY_ERROR_TARGET;
    if (st->target_pid != 0 && st->target_pid == ksceKernelGetProcessId()) {
        unsigned int bits = 0;
        SceUInt timeout = timeout_us;
        ret = ksceKernelWaitEventFlag(st->data_event, 1u,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
            &bits, timeout_us ? &timeout : NULL);
    }
    EXIT_SYSCALL(syscall_state);
    return ret;
}

int vita_tracy_detach(VitaTracyKernelState *st) {
    int ret = vita_tracy_sampler_stop(st);
    if (ret < 0) return ret;
    ret = vita_tracy_pmu_sample_stop(st);
    if (ret < 0) return ret;

    if (st->map_uid > 0) {
        ret = ksceKernelUserUnmap(st->map_uid);
        if (ret < 0) return ret;
        st->map_uid = 0;
    }
    st->shared = NULL;
    st->shared_size = 0;
    st->target_pid = 0;
    vita_tracy_notify(st);

    transition(st, VITA_TRACY_EVENT_DETACH);
    return VITA_TRACY_OK;
}

static int copy_args_from_user(SceUID pid, void *dst, const void *user_src, SceSize size) {
    return ksceKernelCopyFromUserProc(pid, dst, user_src, size) < 0 ? 0 : 1;
}

/* Every syscall entry point below wraps its body in ENTER_SYSCALL/
 * EXIT_SYSCALL (psp2kern/kernel/cpu.h) -- the pattern kubridge, a kernel
 * module proven to cross the user/kernel boundary correctly on this same
 * firmware, uses for every one of its own syscalls. Found missing
 * 2026-08-21: vitaTracySetSampling, the only syscall here that creates a
 * kernel thread, was returning a return value corrupted at exactly bit 30
 * (0xFFFFFFFD internally, 0xBFFFFFFD as observed by the caller) despite the
 * kernel-side value being verified correct via debug logging first. The
 * other syscalls, which don't create threads, happened not to show it, but
 * the fix applies to all of them for the same reason it applies to this
 * one: whatever the CPU's syscall/thread-crossing state ENTER_SYSCALL sets
 * up, none of this file's functions were setting it up at all. */

static int vitaTracyRegister_impl(const VitaTracyRegisterArgs *args);
static int vitaTracyUnregister_impl(uint32_t target_pid);
static int vitaTracySetSampling_impl(const VitaTracySamplingConfig *cfg);
static int vitaTracySetPmu_impl(const VitaTracyPmuConfig *cfg);
static int vitaTracySnapshotModules_impl(uint32_t target_pid);
static int vitaTracyGetStats_impl(VitaTracyStats *stats);
static int vitaTracyPmuSampleStart_impl(void);
static int vitaTracyPmuSampleStop_impl(void);

int vitaTracyRegister(const VitaTracyRegisterArgs *args) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracyRegister_impl(args);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyRegister_impl(const VitaTracyRegisterArgs *args) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();

    VitaTracyRegisterArgs local;
    if (!copy_args_from_user(caller_pid, &local, args, sizeof(local))) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (local.size != sizeof(VitaTracyRegisterArgs) || local.abi_version != VITA_TRACY_ABI_VERSION) {
        return VITA_TRACY_ERROR_ABI;
    }
    if (local.ring_user_addr == 0 || local.ring_size < sizeof(VitaTraceSharedHeader) || local.flags != 0) {
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
    if ((uint64_t)kernel_offset + local.ring_size > kernel_size ||
        !vita_trace_shared_validate_layout(shared, local.ring_size) ||
        ((VitaTraceSharedHeader *)shared)->target_pid != local.target_pid) {
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
    st->sampling_flags = 0;
    memset(&st->stats, 0, sizeof(st->stats));

    /* Proves to the client that the call reached the kernel rather than a
     * weak import stub standing in for an absent plugin. */
    vita_trace_shared_acknowledge(shared);

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
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracyUnregister_impl(target_pid);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyUnregister_impl(uint32_t target_pid) {
    VitaTracyKernelState *st = vita_tracy_state();

    if (st->target_pid == 0 || (SceUID)target_pid != st->target_pid ||
        ksceKernelGetProcessId() != st->target_pid) {
        return VITA_TRACY_ERROR_TARGET;
    }

    return vita_tracy_detach(st);
}

int vitaTracySetSampling(const VitaTracySamplingConfig *cfg) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracySetSampling_impl(cfg);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracySetSampling_impl(const VitaTracySamplingConfig *cfg) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();
    if (st->target_pid == 0 || caller_pid != st->target_pid) return VITA_TRACY_ERROR_TARGET;

    VitaTracySamplingConfig local;
    if (!copy_args_from_user(caller_pid, &local, cfg, sizeof(local))) {
        return VITA_TRACY_ERROR_ARGS;
    }
    if (local.size != sizeof(VitaTracySamplingConfig) ||
        local.abi_version != VITA_TRACY_ABI_VERSION) {
        return VITA_TRACY_ERROR_ABI;
    }
    if (local.frequency_hz > VITA_TRACY_MAX_SAMPLE_HZ ||
        (local.flags & ~VITA_TRACY_SAMPLING_ALLOW_SUSPEND)) return VITA_TRACY_ERROR_ARGS;

    if (st->state != VITA_TRACY_STATE_ATTACHED && st->state != VITA_TRACY_STATE_PROFILING &&
        st->state != VITA_TRACY_STATE_STOPPED) {
        return VITA_TRACY_ERROR_STATE;
    }

    if (local.frequency_hz == 0) {
        if (st->state == VITA_TRACY_STATE_PROFILING) {
            int ret = vita_tracy_sampler_stop(st);
            if (ret < 0) return ret;
            transition(st, VITA_TRACY_EVENT_STOP);
        }
        st->stats.sampling_flags = 0;
        return VITA_TRACY_OK;
    }

    if (st->state == VITA_TRACY_STATE_PROFILING) {
        int ret = vita_tracy_sampler_stop(st);
        if (ret < 0) return ret;
        transition(st, VITA_TRACY_EVENT_STOP);
    }
    st->sampling_hz = local.frequency_hz;
    st->sampling_flags = local.flags;

    if (!transition(st, VITA_TRACY_EVENT_START)) {
        return VITA_TRACY_ERROR_STATE;
    }

    int ret = vita_tracy_sampler_start(st);
    if (ret != VITA_TRACY_OK) {
        transition(st, VITA_TRACY_EVENT_STOP);
    }
    st->stats.sampling_flags = ret == VITA_TRACY_OK ? local.flags : 0;
    return ret;
}

int vitaTracySetPmu(const VitaTracyPmuConfig *cfg) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracySetPmu_impl(cfg);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracySetPmu_impl(const VitaTracyPmuConfig *cfg) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();
    if (st->target_pid == 0 || caller_pid != st->target_pid) return VITA_TRACY_ERROR_TARGET;

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
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracySnapshotModules_impl(target_pid);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracySnapshotModules_impl(uint32_t target_pid) {
    VitaTracyKernelState *st = vita_tracy_state();

    if (st->target_pid == 0 || (SceUID)target_pid != st->target_pid ||
        ksceKernelGetProcessId() != st->target_pid) {
        return VITA_TRACY_ERROR_TARGET;
    }
    return vita_tracy_modules_snapshot(st, st->target_pid);
}

int vitaTracyGetStats(VitaTracyStats *stats) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracyGetStats_impl(stats);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyGetStats_impl(VitaTracyStats *stats) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();
    if (st->target_pid == 0 || caller_pid != st->target_pid) return VITA_TRACY_ERROR_TARGET;

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
    st->stats.timer_ticks = __atomic_load_n(&st->sample_clock.ticks, __ATOMIC_RELAXED);

    if (ksceKernelCopyToUserProc(caller_pid, stats, &st->stats, sizeof(st->stats)) < 0) {
        return VITA_TRACY_ERROR_ARGS;
    }
    return VITA_TRACY_OK;
}

int vitaTracyPmuSampleStart(void) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracyPmuSampleStart_impl();
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyPmuSampleStart_impl(void) {
    VitaTracyKernelState *st = vita_tracy_state();
    if (st->target_pid == 0 || ksceKernelGetProcessId() != st->target_pid) return VITA_TRACY_ERROR_TARGET;
    if (st->state == VITA_TRACY_STATE_UNINITIALIZED || st->state == VITA_TRACY_STATE_READY) {
        return VITA_TRACY_ERROR_STATE;
    }
    return vita_tracy_pmu_sample_start(st);
}

int vitaTracyPmuSampleStop(void) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    int ret = vitaTracyPmuSampleStop_impl();
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyPmuSampleStop_impl(void) {
    VitaTracyKernelState *st = vita_tracy_state();
    if (st->target_pid == 0 || ksceKernelGetProcessId() != st->target_pid) return VITA_TRACY_ERROR_TARGET;
    return vita_tracy_pmu_sample_stop(st);
}
