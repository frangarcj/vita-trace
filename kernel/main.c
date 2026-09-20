#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"

int module_start(SceSize args, void *argp);

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();
    memset(st, 0, sizeof(*st));
    vita_tracy_tick_init(&st->sample_clock);

    VitaTracyState next;
    if (!vita_tracy_state_next(st->state, VITA_TRACY_EVENT_INIT, &next)) {
        return SCE_KERNEL_START_FAILED;
    }
    st->state = next;

    st->data_event = ksceKernelCreateEventFlag("VitaTracyData", 0, 0, NULL);
    if (st->data_event < 0) return SCE_KERNEL_START_FAILED;

    if (vita_tracy_proc_events_register(st) != VITA_TRACY_OK) {
        ksceKernelDeleteEventFlag(st->data_event);
        st->data_event = 0;
        return SCE_KERNEL_START_FAILED;
    }

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();

    vita_tracy_detach(st);
    if (st->sample_clock.timer >= 0 || st->sample_clock.event >= 0) {
        return SCE_KERNEL_STOP_CANCEL;
    }
    vita_tracy_proc_events_unregister(st);
    if (st->data_event > 0) ksceKernelDeleteEventFlag(st->data_event);
    st->data_event = 0;

    return SCE_KERNEL_STOP_SUCCESS;
}
