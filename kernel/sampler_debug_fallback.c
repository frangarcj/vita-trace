#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"

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

static int sampler_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();

    while (st->sampler_should_run) {
        uint32_t hz = st->sampling_hz;
        if (hz == 0) {
            hz = VITA_TRACE_DEFAULT_SAMPLE_HZ;
        }

        SceUID thids[VITA_TRACY_MAX_THREADS];
        int copied = 0;

        if (ksceKernelGetThreadIdList(st->target_pid, thids, VITA_TRACY_MAX_THREADS, &copied) >= 0) {
            uint64_t now = vita_tracy_kernel_now();

            for (int i = 0; i < copied && st->sampler_should_run; ++i) {
                SceThreadCpuRegisters regs;
                memset(&regs, 0, sizeof(regs));

                if (ksceKernelDebugSuspendThread(thids[i], VITA_TRACY_SUSPEND_STATUS) < 0) {
                    continue;
                }

                if (ksceKernelGetThreadCpuRegisters(thids[i], &regs) >= 0) {
                    VitaTraceSample sample;
                    memset(&sample, 0, sizeof(sample));
                    sample.timestamp = now;
                    sample.pid = (uint32_t)st->target_pid;
                    sample.tid = (uint32_t)thids[i];
                    sample.pc = regs.entry[0].pc;
                    sample.sp = regs.entry[0].sp;
                    sample.lr = regs.entry[0].lr;
                    sample.cpu = (uint16_t)ksceKernelCpuId();
                    vita_tracy_emit_sample(st, sample.cpu, &sample);
                }

                ksceKernelDebugResumeThread(thids[i], VITA_TRACY_RESUME_STATUS);
            }
        }

        ksceKernelDelayThread(1000000u / hz);
    }

    return 0;
}

int vita_tracy_sampler_start(VitaTracyKernelState *st) {
    if (st->sampler_thread > 0) {
        return VITA_TRACY_OK;
    }

    /* Prefer a source that does not stop the target; the fallback below is
     * used only while that source does not exist. */
    int ret = vita_tracy_sampler_pamgr_start(st);
    if (ret == VITA_TRACY_OK) {
        return VITA_TRACY_OK;
    }

    SceUID thid = ksceKernelCreateThread("VitaTracySampler", sampler_thread, 0x40, 0x2000, 0, 0, NULL);
    if (thid < 0) {
        return VITA_TRACY_ERROR_STATE;
    }

    st->sampler_thread = thid;
    st->sampler_should_run = 1;

    if (ksceKernelStartThread(thid, 0, NULL) < 0) {
        st->sampler_should_run = 0;
        ksceKernelDeleteThread(thid);
        st->sampler_thread = 0;
        return VITA_TRACY_ERROR_STATE;
    }

    return VITA_TRACY_OK;
}

void vita_tracy_sampler_stop(VitaTracyKernelState *st) {
    vita_tracy_sampler_pamgr_stop(st);

    if (st->sampler_thread <= 0) {
        return;
    }

    st->sampler_should_run = 0;
    ksceKernelWaitThreadEnd(st->sampler_thread, NULL, NULL);
    ksceKernelDeleteThread(st->sampler_thread);
    st->sampler_thread = 0;
}
