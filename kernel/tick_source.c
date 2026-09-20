#include <psp2kern/kernel/systimer.h>
#include <psp2kern/kernel/threadmgr.h>
#include "tick_source.h"
#include "vita_tracy/kernel_abi.h"

#define TICK_OPEN 0x80000000u
#define TICK_ACTIVE 1u

static void on_tick(SceSysTimerId timer, void *arg) {
    (void)timer;
    VitaTracyTickSource *source = (VitaTracyTickSource *)arg;
    uint32_t expected = TICK_OPEN;
    /* Admission and closing use one atomic word. A callback cannot slip in
     * between a separate enabled check and increment of the in-flight count. */
    if (__atomic_compare_exchange_n(&source->enabled, &expected, TICK_OPEN | TICK_ACTIVE,
                                    0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&source->ticks, 1u, __ATOMIC_RELAXED);
        if (source->callback) source->callback(source->context);
        else ksceKernelSetEventFlag(source->event, 1u);
        __atomic_fetch_and(&source->enabled, ~TICK_ACTIVE, __ATOMIC_RELEASE);
    }
}

void vita_tracy_tick_init(VitaTracyTickSource *source) {
    source->timer = -1;
    source->event = -1;
    source->enabled = 0;
    source->ticks = 0;
    source->prepared = 0;
    source->callback = NULL;
    source->context = NULL;
}

void vita_tracy_tick_wake(VitaTracyTickSource *source) {
    if (source->event >= 0) ksceKernelSetEventFlag(source->event, 1u);
}

int vita_tracy_tick_stop(VitaTracyTickSource *source) {
    source->prepared = 0;
    uint32_t previous = __atomic_fetch_and(&source->enabled, ~TICK_OPEN, __ATOMIC_ACQ_REL);
    if (previous & TICK_ACTIVE) return VITA_TRACY_ERROR_BUSY;
    if (source->timer >= 0) {
        /* Free releases the timer and its handler. Keep the event alive until
         * that succeeds; a failed release must never leave a dangling callback. */
        ksceKernelSysTimerStopCount(source->timer);
        int ret = ksceKernelSysTimerFree(source->timer);
        if (ret < 0) return ret;
        source->timer = -1;
    }
    if (source->event >= 0) {
        int ret = ksceKernelDeleteEventFlag(source->event);
        if (ret < 0) return ret;
        source->event = -1;
    }
    return 0;
}

int vita_tracy_tick_start(VitaTracyTickSource *source, uint32_t frequency_hz) {
    int ret = vita_tracy_tick_prepare(source, frequency_hz, 1u, NULL, NULL);
    return ret < 0 ? ret : vita_tracy_tick_arm(source);
}

int vita_tracy_tick_prepare(VitaTracyTickSource *source, uint32_t frequency_hz,
                           uint32_t cpu_mask, VitaTracyTickCallback callback, void *context) {
    if (!frequency_hz || frequency_hz > VITA_TRACY_MAX_SAMPLE_HZ) return VITA_TRACY_ERROR_ARGS;
    if (!cpu_mask || (cpu_mask & (cpu_mask - 1u)) || (cpu_mask & ~15u)) return VITA_TRACY_ERROR_ARGS;
    if (source->timer >= 0 || source->event >= 0) return VITA_TRACY_ERROR_STATE;
    source->callback = callback;
    source->context = context;
    source->event = ksceKernelCreateEventFlag("VitaTracyTick", 0, 0, NULL);
    if (source->event < 0) return source->event;
    source->timer = ksceKernelSysTimerAlloc(SCE_SYSTIMER_TYPE_WORD);
    int ret = source->timer;
    if (ret < 0) goto fail;
    /* Fixed 48 MHz / 48 = 1 MHz, independent of the configurable SYS clock.
     * The interrupt CPU mask is raw (unlike CreateThread's shifted mask). */
    ret = ksceKernelSysTimerSetClockSource(source->timer, SCE_SYSTIMER_CLOCK_SOURCE_48MHZ, 47);
    if (ret < 0) goto fail;
    ret = ksceKernelSysTimerSetInterval(source->timer, 1000000u / frequency_hz);
    if (ret < 0) goto fail;
    ret = ksceKernelSysTimerSetHandler(source->timer, on_tick, cpu_mask, source);
    if (ret < 0) goto fail;
    ret = ksceKernelSysTimerResetCount(source->timer);
    if (ret < 0) goto fail;
    __atomic_store_n(&source->ticks, 0u, __ATOMIC_RELAXED);
    source->prepared = 1;
    return 0;
fail:
    vita_tracy_tick_stop(source);
    return ret;
}

int vita_tracy_tick_arm(VitaTracyTickSource *source) {
    if (!source->prepared || source->timer < 0 || source->event < 0) return VITA_TRACY_ERROR_STATE;
    uint32_t closed = 0;
    if (!__atomic_compare_exchange_n(&source->enabled, &closed, TICK_OPEN, 0,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return VITA_TRACY_ERROR_STATE;
    source->prepared = 0;
    int ret = ksceKernelSysTimerStartCount(source->timer);
    if (ret < 0) vita_tracy_tick_stop(source);
    return ret;
}

int vita_tracy_tick_wait(VitaTracyTickSource *source) {
    unsigned int bits = 0;
    /* A retained binary event coalesces overruns instead of queuing a burst
     * of stale samples. The next tick is tied to the timer, not scan duration. */
    return ksceKernelWaitEventFlag(source->event, 1u,
        SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, NULL);
}
