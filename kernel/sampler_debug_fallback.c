#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"
#include "vita_tracy/shared_layout.h"

/* Bring-up sampler only.
 *
 * Suspending a thread to read its registers perturbs exactly what a
 * profiler is supposed to observe: latencies, scheduling, lock hold times
 * and wait states. It exists to prove the rings, ABI, module maps and
 * viewer path work end to end while a non-intrusive source is still being
 * reverse engineered, and must not be the sampler a release ships with.
 * See docs/reverse_engineering.md. */

#define VITA_TRACY_MAX_THREADS 128

/* The suspend status mask is documented as 0xF7F03; these two values are
 * the ones the debugger path uses. Both need confirming on a CEX. */
#define VITA_TRACY_SUSPEND_STATUS 0x1002
#define VITA_TRACY_RESUME_STATUS 0x0002

/* Names cannot identify ownership: VitaSDK calls both application workers
 * and Tracy workers "pthread". The client publishes only its own PUIDs. */

static int sampler_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();

    while (__atomic_load_n(&st->sampler_should_run, __ATOMIC_ACQUIRE)) {
        if (vita_tracy_tick_wait(&st->sample_clock) < 0) break;
        if (!__atomic_load_n(&st->sampler_should_run, __ATOMIC_ACQUIRE)) break;
        if (vita_trace_control_pending(&st->control)) break;
        __atomic_fetch_add(&st->stats.diagnostic_batches, 1u, __ATOMIC_RELAXED);

        SceUID thids[VITA_TRACY_MAX_THREADS];
        int copied = 0;

        VitaTraceSharedHeader *shared = (VitaTraceSharedHeader *)st->shared;
        if (!vita_trace_thread_registry_complete(&shared->profiler_threads)) {
            __atomic_fetch_add(&st->stats.registry_incomplete_ticks, 1u, __ATOMIC_RELAXED);
        } else if (ksceKernelGetThreadIdList(st->target_pid, thids, VITA_TRACY_MAX_THREADS, &copied) >= 0) {

            for (int i = 0; i < copied && i < VITA_TRACY_MAX_THREADS &&
                 __atomic_load_n(&st->sampler_should_run, __ATOMIC_ACQUIRE); ++i) {
                /* Never suspend the thread driving the profiler's own
                 * control calls (vitaTracySetSampling, vitaTracyGetStats,
                 * ...): confirmed on hardware 2026-08-21 that catching it
                 * mid-syscall -- most easily done by simply existing in
                 * the thread list the very first tick after it just
                 * called vitaTracySetSampling -- hangs the process with
                 * no crash dump, because the suspend lands before it has
                 * fully unwound back to ordinary userland code. */
                if (thids[i] == st->control_thread) {
                    continue;
                }
                /* The list contains global kernel UIDs; Tracy zones carry
                 * process-local IDs returned by sceKernelGetThreadId. */
                SceUID puid = ksceKernelGetUserThreadId(thids[i]);
                if (puid < 0 || vita_trace_thread_contains(&shared->profiler_threads, (uint32_t)puid)) {
                    continue;
                }

                SceKernelThreadInfo info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                if (ksceKernelGetThreadInfo(thids[i], &info) < 0 ||
                    (info.status != SCE_THREAD_RUNNING && info.status != SCE_THREAD_READY)) {
                    continue; /* Do not present sleeping/debug-stopped threads as CPU work. */
                }

                SceThreadCpuRegisters regs;
                memset(&regs, 0, sizeof(regs));

                if (ksceKernelDebugSuspendThread(thids[i], VITA_TRACY_SUSPEND_STATUS) < 0) {
                    continue;
                }

                if (ksceKernelGetThreadCpuRegisters(thids[i], &regs) >= 0) {
                    VitaTraceSample sample;
                    memset(&sample, 0, sizeof(sample));
                    sample.timestamp = vita_tracy_kernel_now();
                    sample.pid = (uint32_t)st->target_pid;
                    sample.tid = (uint32_t)puid;
                    sample.pc = regs.entry[0].pc;
                    sample.sp = regs.entry[0].sp;
                    sample.lr = regs.entry[0].lr;
                    sample.cpu = VITA_TRACE_CPU_UNKNOWN;
                    sample.flags = VITA_TRACE_SAMPLE_DEBUG_SUSPEND;
                    if (regs.entry[0].cpsr & (1u << 5)) sample.flags |= VITA_TRACE_SAMPLE_THUMB;
                    /* Ring 0 is this single producer's transport, not a
                     * claim about which core executed the sampled thread. */
                    vita_tracy_emit_sample(st, 0, &sample);
                } else {
                    __atomic_fetch_add(&st->stats.sample_read_failures, 1u, __ATOMIC_RELAXED);
                }

                if (ksceKernelDebugResumeThread(thids[i], VITA_TRACY_RESUME_STATUS) < 0) {
                    __atomic_fetch_add(&st->stats.sample_resume_failures, 1u, __ATOMIC_RELAXED);
                    __atomic_store_n(&st->sampler_should_run, 0, __ATOMIC_RELEASE);
                    break;
                }
            }
        }

        vita_tracy_notify(st); /* One notification per batch, not per sample. */
    }

    return 0;
}

int vita_tracy_sampler_start(VitaTracyKernelState *st) {
    if (st->sampler_thread > 0) {
        return VITA_TRACY_OK;
    }

    if (st->sampling_flags & VITA_TRACY_SAMPLING_PMU_IRQ) {
        int ret = vita_tracy_sampler_irq_start(st);
        if (ret == VITA_TRACY_OK) st->sampler_backend = VITA_TRACY_SAMPLER_PMU_IRQ;
        return ret;
    }

    /* Prefer a source that does not stop the target; the fallback below is
     * entered only by explicit diagnostic opt-in. */
    int ret = vita_tracy_sampler_pamgr_start(st);
    if (ret == VITA_TRACY_OK) {
        st->sampler_backend = VITA_TRACY_SAMPLER_PAMGR;
        return VITA_TRACY_OK;
    }

    if (!(st->sampling_flags & VITA_TRACY_SAMPLING_ALLOW_SUSPEND)) {
        return VITA_TRACY_ERROR_UNSUPPORTED;
    }

    /* The calling thread -- still executing this same syscall -- is the
     * one sampler_thread must never suspend. See the comment above its
     * suspend loop. */
    st->control_thread = (SceUID)ksceKernelGetThreadId();

    ret = vita_tracy_tick_start(&st->sample_clock, st->sampling_hz);
    if (ret < 0) return ret;

    SceUID thid = ksceKernelCreateThread("VitaTracySampler", sampler_thread, 0x40, 0x2000, 0, 0, NULL);
    if (thid < 0) {
        vita_tracy_tick_stop(&st->sample_clock);
        return VITA_TRACY_ERROR_STATE;
    }

    st->sampler_thread = thid;
    __atomic_store_n(&st->sampler_should_run, 1, __ATOMIC_RELEASE);

    if (ksceKernelStartThread(thid, 0, NULL) < 0) {
        __atomic_store_n(&st->sampler_should_run, 0, __ATOMIC_RELEASE);
        ksceKernelDeleteThread(thid);
        st->sampler_thread = 0;
        vita_tracy_tick_stop(&st->sample_clock);
        return VITA_TRACY_ERROR_STATE;
    }

    st->sampler_backend = VITA_TRACY_SAMPLER_SUSPEND;
    return VITA_TRACY_OK;
}

int vita_tracy_sampler_stop(VitaTracyKernelState *st) {
    if (st->sampler_backend == VITA_TRACY_SAMPLER_PMU_IRQ) {
        int ret = vita_tracy_sampler_irq_stop(st);
        if (ret >= 0) st->sampler_backend = VITA_TRACY_SAMPLER_NONE;
        return ret;
    }
    if (st->sampler_backend == VITA_TRACY_SAMPLER_PAMGR) {
        vita_tracy_sampler_pamgr_stop(st);
        st->sampler_backend = VITA_TRACY_SAMPLER_NONE;
        return 0;
    }

    if (st->sampler_thread <= 0) {
        int ret = vita_tracy_tick_stop(&st->sample_clock);
        if (ret >= 0) st->sampler_backend = VITA_TRACY_SAMPLER_NONE;
        return ret;
    }

    __atomic_store_n(&st->sampler_should_run, 0, __ATOMIC_RELEASE);
    vita_tracy_tick_wake(&st->sample_clock);
    int ret = ksceKernelWaitThreadEnd(st->sampler_thread, NULL, NULL);
    if (ret < 0) return ret;
    ret = ksceKernelDeleteThread(st->sampler_thread);
    if (ret < 0) return ret;
    st->sampler_thread = 0;
    ret = vita_tracy_tick_stop(&st->sample_clock);
    if (ret >= 0) st->sampler_backend = VITA_TRACY_SAMPLER_NONE;
    return ret;
}
