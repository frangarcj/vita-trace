#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"

void _start() __attribute__((weak, alias("module_start")));

int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();
    memset(st, 0, sizeof(*st));

    VitaTracyState next;
    if (!vita_tracy_state_next(st->state, VITA_TRACY_EVENT_INIT, &next)) {
        return SCE_KERNEL_START_FAILED;
    }
    st->state = next;

    if (vita_tracy_proc_events_register(st) != VITA_TRACY_OK) {
        return SCE_KERNEL_START_FAILED;
    }

    return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    VitaTracyKernelState *st = vita_tracy_state();

    vita_tracy_detach(st);
    vita_tracy_proc_events_unregister(st);

    return SCE_KERNEL_STOP_SUCCESS;
}
