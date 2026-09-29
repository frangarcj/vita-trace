#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

static VitaTracyKernelState g_state;

/* A syscall body that sleeps (thread joins, mappings, printf) can be resumed
 * on another core; ENTER/EXIT_SYSCALL then restore the wrong core's state
 * (2026-08-21 bit-30 corruption, 2026-09-22 freezes). Keep the caller on the
 * core it entered on for the whole syscall. */
/* Exported by SceThreadmgrForDriver (NID 0x6D0733A8 on 3.60) but absent from the installed header. */
int ksceKernelChangeThreadCpuAffinityMask(SceUID thid, int cpuAffinityMask);

static int vita_tracy_syscall_pin(void) {
    SceUID self = ksceKernelGetThreadId();
    int cpu = ksceKernelCpuId();
    if (self <= 0 || cpu < 0 || cpu > 3) return -1;
    return ksceKernelChangeThreadCpuAffinityMask(self, 0x10000 << cpu);
}

static void vita_tracy_syscall_unpin(int previous) {
    if (previous > 0) ksceKernelChangeThreadCpuAffinityMask(ksceKernelGetThreadId(), previous);
}

VitaTracyKernelState *vita_tracy_state(void) {
    return &g_state;
}

static int cleanup_exited_target(void *context) {
    VitaTracyKernelState *st = (VitaTracyKernelState *)context;
    int ret = vita_tracy_detach(st);
    __atomic_store_n(&st->stats.last_cleanup_error, ret, __ATOMIC_RELEASE);
    return ret;
}

int vita_tracy_control_begin(VitaTracyKernelState *st) {
    if (!vita_trace_control_enter(&st->control)) return VITA_TRACY_ERROR_BUSY;
    if (st->shutdown_requested || vita_trace_control_pending(&st->control)) {
        vita_tracy_control_end(st);
        return VITA_TRACY_ERROR_STATE;
    }
    return VITA_TRACY_OK;
}

void vita_tracy_control_end(VitaTracyKernelState *st) {
    vita_trace_control_leave(&st->control, cleanup_exited_target, st);
}

void vita_tracy_target_exited(VitaTracyKernelState *st, SceUID pid) {
    if (!vita_trace_control_request(&st->control, (uint32_t)pid)) return;
    vita_tracy_notify(st);
    // A callback re-entering an active syscall must not wait for its owner.
    // That owner reaps on exit; failed cleanup stays pending for a later retry.
    vita_trace_control_reap(&st->control, cleanup_exited_target, st);
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
        __atomic_fetch_add(&st->stats.samples_emitted[cpu], 1u, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&st->stats.samples_dropped[cpu], 1u, __ATOMIC_RELAXED);
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
        __atomic_fetch_add(&st->stats.control_dropped, 1u, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&st->control_writer_lock, 0u, __ATOMIC_RELEASE);
    ksceKernelCpuResumeIntr(intr);
    vita_tracy_notify(st);
}

void vita_tracy_notify(VitaTracyKernelState *st) {
    vita_tracy_notify_events(st, VITA_TRACY_WAKE_DATA);
}

void vita_tracy_notify_events(VitaTracyKernelState *st, uint32_t events) {
    if (st->data_event > 0) ksceKernelSetEventFlag(st->data_event, events & VITA_TRACY_WAKE_ALL);
}

int vitaTracyWakeup(void) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = VITA_TRACY_ERROR_TARGET;
    if (vita_trace_control_target(&st->control) == (uint32_t)ksceKernelGetProcessId()) {
        vita_tracy_notify(st);
        ret = VITA_TRACY_OK;
    }
    vita_tracy_syscall_unpin(pin_prev);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

int vitaTracyWaitForData(uint32_t timeout_us) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = VITA_TRACY_ERROR_TARGET;
    if (vita_trace_control_target(&st->control) == (uint32_t)ksceKernelGetProcessId()) {
        unsigned int bits = 0;
        /* Never block without a bound. The raw IRQ node cannot set the event
         * flag, so its wake bits are polled every 20 ms while it is live.
         * Otherwise the wait still returns every 250 ms: a thread asleep here
         * without a timeout kept its process from dying, and the proc-event
         * callback that would wake it only runs once the threads are gone,
         * so killing the app hung SceShell (2026-09-22 and 2026-09-28). */
        /* The IRQ node copies user stacks only inside ranges published here,
         * from the target's own drain thread, a few times a second. */
        static uint64_t stacks_refreshed_us;
        if (vita_tracy_sampler_irq_active()) {
            const uint64_t now_us = (uint64_t)ksceKernelGetSystemTimeWide();
            if (now_us - stacks_refreshed_us >= 250000u) {
                vita_tracy_sampler_irq_refresh_stacks();
                stacks_refreshed_us = now_us;
            }
        }
        const uint32_t poll_us = vita_tracy_sampler_irq_active() ? 20000u : 250000u;
        SceUInt timeout = (!timeout_us || timeout_us > poll_us) ? poll_us : timeout_us;
        ret = ksceKernelWaitEventFlag(st->data_event, VITA_TRACY_WAKE_ALL,
            SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
        if (ret == (int)0x80028005 /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */) { ret = 0; bits = 0; }
        if (ret >= 0) {
            bits |= __atomic_exchange_n(&st->irq_pending_wake, 0u, __ATOMIC_ACQ_REL);
            ret = (int)(bits & VITA_TRACY_WAKE_ALL);
        }
    }
    vita_tracy_syscall_unpin(pin_prev);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

int vitaTracyResolveThread(uint32_t global_tid) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) {
        const SceUID caller_pid = ksceKernelGetProcessId();
        if (!global_tid || st->target_pid == 0 || caller_pid != st->target_pid) {
            ret = VITA_TRACY_ERROR_TARGET;
        } else {
            SceKernelThreadInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            ret = ksceKernelGetThreadInfo((SceUID)global_tid, &info);
            if (ret >= 0) {
                if (info.processId != st->target_pid) ret = VITA_TRACY_ERROR_TARGET;
                else ret = ksceKernelGetUserThreadId((SceUID)global_tid);
            }
        }
        vita_tracy_control_end(st);
    }
    vita_tracy_syscall_unpin(pin_prev);
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
    vita_trace_control_set_target(&st->control, 0);
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
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracyRegister_impl(args); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyRegister_impl(const VitaTracyRegisterArgs *args) {
    VitaTracyKernelState *st = vita_tracy_state();
    SceUID caller_pid = ksceKernelGetProcessId();
    VITA_TRACY_TRACE("register: pid=0x%08X state=%d\n", (unsigned)caller_pid, (int)st->state);

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

    /* Deferred from module_start (see kernel/main.c). Idempotent and
     * all-or-nothing, so a failed attempt is retried by the next register. */
    if (vita_tracy_firmware_init() < 0) {
        VITA_TRACY_TRACE("register: firmware exports unresolved\n");
        return VITA_TRACY_ERROR_UNSUPPORTED;
    }
    VITA_TRACY_TRACE("register: exports resolved, mapping ring\n");

    /* No old producer remains after detach. Do not deliver the previous
     * target's retained failures to a newly attached process. */
    int event_ret = ksceKernelClearEventFlag(st->data_event, 0);
    if (event_ret < 0) return event_ret;

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
    st->sampling_event_count = 0;
    memset(&st->stats, 0, sizeof(st->stats));
    vita_trace_control_set_target(&st->control, (uint32_t)target_pid);

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
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracyUnregister_impl(target_pid); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
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
    const int pin_prev = vita_tracy_syscall_pin();
    VITA_TRACY_TRACE("set_sampling: pin -> 0x%08X (now on cpu %d)\n", (unsigned)pin_prev, ksceKernelCpuId());
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracySetSampling_impl(cfg); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
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
    const uint32_t allowed_sampling = VITA_TRACY_SAMPLING_ALLOW_SUSPEND |
                                      VITA_TRACY_SAMPLING_PMU_IRQ |
                                      VITA_TRACY_SAMPLING_IRQ_COUNT_ONLY |
                                      VITA_TRACY_SAMPLING_IRQ_REGISTER_ONLY |
                                      VITA_TRACY_SAMPLING_IRQ_SKIP_PROGRAM |
                                      VITA_TRACY_SAMPLING_IRQ_SKIP_ARM |
                                      VITA_TRACY_SAMPLING_IRQ_SPI244 |
                                      VITA_TRACY_SAMPLING_IRQ_INTEN_ONLY |
                                      VITA_TRACY_SAMPLING_IRQ_SVC_NODE;
    const uint32_t irq_only = VITA_TRACY_SAMPLING_IRQ_COUNT_ONLY |
                              VITA_TRACY_SAMPLING_IRQ_REGISTER_ONLY;
    const uint32_t exclusive = VITA_TRACY_SAMPLING_ALLOW_SUSPEND | VITA_TRACY_SAMPLING_PMU_IRQ;
    if (local.frequency_hz > VITA_TRACY_MAX_SAMPLE_HZ ||
        (local.flags & ~allowed_sampling) ||
        ((local.flags & exclusive) == exclusive) ||
        ((local.flags & irq_only) && !(local.flags & VITA_TRACY_SAMPLING_PMU_IRQ)))
        return VITA_TRACY_ERROR_ARGS;
    /* Events ride on the cycle-overflow samples; A9 event numbers fit a byte. */
    if (local.event_count > VITA_TRACY_MAX_SAMPLE_EVENTS ||
        (local.event_count && !(local.flags & VITA_TRACY_SAMPLING_PMU_IRQ)))
        return VITA_TRACY_ERROR_ARGS;
    for (uint32_t i = 0; i < local.event_count; ++i)
        if (local.events[i] > 0xFFu) return VITA_TRACY_ERROR_ARGS;

    if (st->state != VITA_TRACY_STATE_ATTACHED && st->state != VITA_TRACY_STATE_PROFILING &&
        st->state != VITA_TRACY_STATE_STOPPED) {
        return VITA_TRACY_ERROR_STATE;
    }

    /* A failed start can own resources despite leaving the logical state
     * STOPPED. Drain that backend before accepting stop or reconfiguration. */
    VITA_TRACY_TRACE("set_sampling: hz=%u flags=0x%X state=%d backend=%u cpu=%d tid=0x%08X\n",
        (unsigned)local.frequency_hz, (unsigned)local.flags, (int)st->state, (unsigned)st->sampler_backend,
        ksceKernelCpuId(), (unsigned)ksceKernelGetThreadId());
    ksceKernelDelayThread(200000);
    int stopped = vita_tracy_sampler_stop(st);
    if (stopped < 0) return stopped;
    if (st->state == VITA_TRACY_STATE_PROFILING)
        transition(st, VITA_TRACY_EVENT_STOP);
    if (local.frequency_hz == 0) {
        st->stats.sampling_flags = 0;
        return VITA_TRACY_OK;
    }
    st->sampling_hz = local.frequency_hz;
    st->sampling_flags = local.flags;
    st->sampling_event_count = local.event_count;
    for (uint32_t i = 0; i < VITA_TRACY_MAX_SAMPLE_EVENTS; ++i)
        st->sampling_events[i] = i < local.event_count ? local.events[i] : 0u;

    if (!transition(st, VITA_TRACY_EVENT_START)) {
        return VITA_TRACY_ERROR_STATE;
    }

    int ret = vita_tracy_sampler_start(st);
    VITA_TRACY_TRACE("set_sampling: start -> %d\n", ret);
    if (ret != VITA_TRACY_OK) {
        transition(st, VITA_TRACY_EVENT_STOP);
    }
    st->stats.sampling_flags = ret == VITA_TRACY_OK ? local.flags : 0;
    return ret;
}

int vitaTracySetPmu(const VitaTracyPmuConfig *cfg) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracySetPmu_impl(cfg); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
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
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracySnapshotModules_impl(target_pid); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
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
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracyGetStats_impl(stats); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static uint32_t g_stats_calls;
static int vitaTracyGetStats_impl_inner(VitaTracyStats *stats);
static int vitaTracyGetStats_impl(VitaTracyStats *stats) {
    int r = vitaTracyGetStats_impl_inner(stats);
    VITA_TRACY_TRACE("stats: exit %u -> %d\n", g_stats_calls, r);
    return r;
}
static int vitaTracyGetStats_impl_inner(VitaTracyStats *stats) {
    const uint32_t n = ++g_stats_calls;
    VITA_TRACY_TRACE("stats: enter %u cpu=%d\n", n, ksceKernelCpuId());
#if defined(__vita__) && !defined(VITA_TRACY_TESTING)
    vita_tracy_sampler_irq_fill_diagnostics();
    VITA_TRACY_TRACE("stats: diagnostics done %u\n", n);
#endif
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

    /* Independent monotonic counters, not a simultaneous cross-core snapshot.
     * PMU vector records contain the coherent measurement intervals. */
    memset(&local, 0, sizeof(local));
    local.size = sizeof(local);
    local.abi_version = VITA_TRACY_ABI_VERSION;
    local.uptime_ms = (uint32_t)(vita_tracy_kernel_now() / 1000ull);
    local.timer_ticks = __atomic_load_n(&st->sample_clock.ticks, __ATOMIC_RELAXED);
#define LOAD_STAT(member) local.member = __atomic_load_n(&st->stats.member, __ATOMIC_RELAXED)
    LOAD_STAT(control_dropped);
    LOAD_STAT(pmu_cycle_delta_total);
    LOAD_STAT(pmu_sample_ticks);
    LOAD_STAT(sampling_flags);
    LOAD_STAT(registry_incomplete_ticks);
    LOAD_STAT(sample_read_failures);
    LOAD_STAT(sample_resume_failures);
    LOAD_STAT(diagnostic_batches);
    LOAD_STAT(pmu_active_mask);
    LOAD_STAT(pmu_last_error);
    LOAD_STAT(last_cleanup_error);
    LOAD_STAT(sample_irq_arm_mhz);
    LOAD_STAT(sample_irq_core_mask);
    LOAD_STAT(sample_irq_last_error);
    LOAD_STAT(sample_irq_handler_registered);
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) {
        LOAD_STAT(samples_emitted[i]); LOAD_STAT(samples_dropped[i]);
        LOAD_STAT(pmu_records[i]); LOAD_STAT(pmu_dropped[i]); LOAD_STAT(pmu_gaps[i]);
        LOAD_STAT(pmu_wrong_cpu[i]); LOAD_STAT(pmu_counter_errors[i]);
        LOAD_STAT(sample_irq_calls[i]); LOAD_STAT(sample_irq_overflows[i]);
        LOAD_STAT(sample_irq_not_target[i]); LOAD_STAT(sample_irq_kernel[i]);
        LOAD_STAT(sample_irq_context_errors[i]);
    }
#undef LOAD_STAT
    if (ksceKernelCopyToUserProc(caller_pid, stats, &local, sizeof(local)) < 0) {
        return VITA_TRACY_ERROR_ARGS;
    }
    return VITA_TRACY_OK;
}

int vitaTracyPmuSampleStart(void) {
    uint32_t syscall_state;
    ENTER_SYSCALL(syscall_state);
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracyPmuSampleStart_impl(); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
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
    const int pin_prev = vita_tracy_syscall_pin();
    VitaTracyKernelState *st = vita_tracy_state();
    int ret = vita_tracy_control_begin(st);
    if (ret == 0) { ret = vitaTracyPmuSampleStop_impl(); vita_tracy_control_end(st); }
    vita_tracy_syscall_unpin(pin_prev);
    EXIT_SYSCALL(syscall_state);
    return ret;
}

static int vitaTracyPmuSampleStop_impl(void) {
    VitaTracyKernelState *st = vita_tracy_state();
    if (st->target_pid == 0 || ksceKernelGetProcessId() != st->target_pid) return VITA_TRACY_ERROR_TARGET;
    return vita_tracy_pmu_sample_stop(st);
}
