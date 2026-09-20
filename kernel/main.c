#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>

#include "internal.h"
#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"

int module_start(SceSize args, void *argp);

int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();
    memset(st, 0, sizeof(*st));
    vita_tracy_tick_init(&st->sample_clock);
    if (vita_tracy_firmware_init() < 0) return SCE_KERNEL_START_FAILED;

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

    /* Excpmgr exposes registration but no unregister operation. Once the
     * experimental IRQ observer is installed its code must remain resident
     * until reboot; refusing unload is safer than leaving a dangling branch. */
    if (vita_tracy_sampler_irq_handler_registered()) return SCE_KERNEL_STOP_CANCEL;

    if (!vita_trace_control_enter(&st->control)) return SCE_KERNEL_STOP_CANCEL;
    st->shutdown_requested = 1;
    if (vita_tracy_detach(st) < 0) goto cancel;
    if (st->sample_clock.timer >= 0 || st->sample_clock.event >= 0) {
        goto cancel;
    }
    if (vita_tracy_proc_events_unregister(st) < 0) goto cancel;
    if (st->data_event > 0 && ksceKernelDeleteEventFlag(st->data_event) < 0) goto cancel;
    st->data_event = 0;

    return SCE_KERNEL_STOP_SUCCESS;
cancel:
    vita_tracy_control_end(st);
    return SCE_KERNEL_STOP_CANCEL;
}
