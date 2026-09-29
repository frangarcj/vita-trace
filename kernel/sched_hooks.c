#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/shared_layout.h"
#include "vita_tracy/shared_ring.h"

/* threadmgr calls one on-CPU and one off-CPU hook from the scheduler, right
 * after its sched:::on-cpu / off-cpu DTrace probes. Each hook has a single
 * global slot, set by a bare store; NULL removes it. pamgr (gone since 3.50)
 * was their only user, and no retail module sets them. Call sites on 3.60
 * (0x8100aaf8, 0x8100b064):
 *   off-cpu(pid, tid, reason)   reason 2 or 4
 *   on-cpu(pid, tid, [thread+0xB8], [thread+0xC0])
 * with pid = [[thread+0x18]+0x64] and tid = [thread+0x98], or [thread+8]
 * when that is negative. The only guard at a call site is the NULL check,
 * so the hooks run inside the scheduler: no locks, no threadmgr, no library
 * copies (the kernel's memcpy uses the interrupted context's VFP registers).
 * The 3.63+ NIDs are the same three-instruction setters writing the same
 * data offsets, matched against the 3.63/3.65 dumps (2026-09-30). */
#define LIB_THREADMGR_FOR_KERNEL 0xA8CA0EFDu
#define LIB_THREADMGR_FOR_KERNEL_363 0x7F8593BAu
#define NID_SET_ON_CPU_HOOK 0x15AAB4F9u
#define NID_SET_ON_CPU_HOOK_363 0xA3975A5Au
#define NID_SET_OFF_CPU_HOOK 0xDBE2EE32u
#define NID_SET_OFF_CPU_HOOK_363 0xF885ECCAu

typedef int (*SetHook)(void *hook);

static struct {
    SetHook set_on, set_off;
    VitaTracyKernelState *st;
    uint32_t target_pid;
    uint32_t enabled;
    uint32_t installed;
    uint32_t active; /* hook calls in flight */
    uint32_t busy[VITA_TRACE_CORE_COUNT];
} g_sched;

static int resolve(uint32_t fn, uint32_t fn363, SetHook *out) {
    uintptr_t a = 0;
    if ((vita_tracy_lookup_export("SceKernelThreadMgr", LIB_THREADMGR_FOR_KERNEL, fn, &a) >= 0 && a) ||
        (a = 0, vita_tracy_lookup_export("SceKernelThreadMgr", LIB_THREADMGR_FOR_KERNEL_363, fn363, &a) >= 0 && a)) {
        *out = (SetHook)a;
        return 0;
    }
    return VITA_TRACY_ERROR_UNSUPPORTED;
}

static void record(uint32_t pid, uint32_t tid, uint32_t kind, uint32_t reason) {
    __atomic_fetch_add(&g_sched.active, 1u, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&g_sched.enabled, __ATOMIC_SEQ_CST)) goto out;
    const uint32_t cpu = (uint32_t)ksceKernelCpuId();
    VitaTracyKernelState *st = g_sched.st;
    if (cpu >= VITA_TRACE_CORE_COUNT || !st) goto out;
    __atomic_fetch_add(&st->stats.switch_calls[cpu], 1u, __ATOMIC_RELAXED);
    if (pid != g_sched.target_pid) {
        __atomic_store_n(&st->stats.switch_last_other_pid, pid, __ATOMIC_RELAXED);
        goto out;
    }
    /* One producer per core ring. The scheduler runs with interrupts masked,
     * but a nested entry is counted and dropped rather than trusted. */
    if (__atomic_exchange_n(&g_sched.busy[cpu], 1u, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&st->stats.switch_dropped[cpu], 1u, __ATOMIC_RELAXED);
        goto out;
    }
    VitaTraceSwitch rec;
    rec.timestamp = vita_tracy_kernel_now();
    rec.tid = tid;
    rec.cpu = (uint16_t)cpu;
    rec.kind = (uint8_t)kind;
    rec.reason = (uint8_t)reason;
    void *ring = vita_trace_shared_switch_ring(st->shared, cpu);
    if (ring && vita_trace_ring_try_push(ring, &rec))
        __atomic_fetch_add(&st->stats.switch_recorded[cpu], 1u, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&st->stats.switch_dropped[cpu], 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sched.busy[cpu], 0u, __ATOMIC_RELEASE);
out:
    __atomic_fetch_sub(&g_sched.active, 1u, __ATOMIC_SEQ_CST);
}

static void on_cpu_hook(uint32_t pid, uint32_t tid, uint32_t a, uint32_t b) {
    (void)a;
    (void)b;
    record(pid, tid, VITA_TRACE_SWITCH_IN, 0u);
}

static void off_cpu_hook(uint32_t pid, uint32_t tid, uint32_t reason) {
    record(pid, tid, VITA_TRACE_SWITCH_OUT, reason);
}

int vita_tracy_sched_hooks_start(VitaTracyKernelState *st) {
    if (!st || !st->shared) return VITA_TRACY_ERROR_STATE;
    if (__atomic_load_n(&g_sched.installed, __ATOMIC_ACQUIRE)) return VITA_TRACY_ERROR_BUSY;
    if (!g_sched.set_on && (resolve(NID_SET_ON_CPU_HOOK, NID_SET_ON_CPU_HOOK_363, &g_sched.set_on) < 0 ||
                            resolve(NID_SET_OFF_CPU_HOOK, NID_SET_OFF_CPU_HOOK_363, &g_sched.set_off) < 0)) {
        g_sched.set_on = g_sched.set_off = NULL;
        return VITA_TRACY_ERROR_UNSUPPORTED;
    }
    for (uint32_t i = 0; i < VITA_TRACE_CORE_COUNT; ++i) g_sched.busy[i] = 0;
    g_sched.st = st;
    g_sched.target_pid = st->target_pid;
    __atomic_store_n(&g_sched.enabled, 1u, __ATOMIC_SEQ_CST);
    g_sched.set_off((void *)off_cpu_hook);
    g_sched.set_on((void *)on_cpu_hook);
    __atomic_store_n(&g_sched.installed, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&st->stats.switch_hooks_installed, 1u, __ATOMIC_RELEASE);
    return 0;
}

/* Before the shared block can go away: after this returns no hook touches it. */
void vita_tracy_sched_hooks_stop(void) {
    if (!__atomic_load_n(&g_sched.installed, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&g_sched.enabled, 0u, __ATOMIC_SEQ_CST);
    g_sched.set_on(NULL);
    g_sched.set_off(NULL);
    /* A hook already entered either saw `enabled` set before we cleared it
     * and is counted in `active`, or sees it clear. */
    for (int i = 0; i < 1000 && __atomic_load_n(&g_sched.active, __ATOMIC_SEQ_CST); ++i)
        ksceKernelDelayThread(100);
    if (g_sched.st) __atomic_store_n(&g_sched.st->stats.switch_hooks_installed, 0u, __ATOMIC_RELEASE);
    g_sched.st = NULL;
    __atomic_store_n(&g_sched.installed, 0u, __ATOMIC_RELEASE);
}

#ifdef VITA_TRACY_TESTING
void vita_tracy_sched_hooks_test_reset(void) {
    g_sched.set_on = g_sched.set_off = NULL;
    g_sched.st = NULL;
    g_sched.enabled = g_sched.installed = g_sched.active = 0;
}
#endif
