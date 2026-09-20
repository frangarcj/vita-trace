#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/sysclib.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"

/* Mappings and sampler threads must not outlive the target, so every path
 * that can end the process releases them. Only the events whose semantics
 * are documented are acted on; the rest are left alone until their
 * behaviour is observed on hardware. */

static void release_if_target(SceUID pid) {
    VitaTracyKernelState *st = vita_tracy_state();
    vita_tracy_target_exited(st, pid);
}

static int on_create(SceUID pid, SceProcEventInvokeParam2 *param, int a3) {
    (void)pid;
    (void)param;
    (void)a3;
    return 0;
}

static int on_exit(SceUID pid, SceProcEventInvokeParam1 *param, int a3) {
    (void)param;
    (void)a3;
    release_if_target(pid);
    return 0;
}

static int on_kill(SceUID pid, SceProcEventInvokeParam1 *param, int a3) {
    (void)param;
    (void)a3;
    release_if_target(pid);
    return 0;
}

static int on_stop(SceUID pid, int event_type, SceProcEventInvokeParam1 *param, int a4) {
    (void)pid;
    (void)event_type;
    (void)param;
    (void)a4;
    return 0;
}

static int on_start(SceUID pid, int event_type, SceProcEventInvokeParam1 *param, int a4) {
    (void)pid;
    (void)event_type;
    (void)param;
    (void)a4;
    return 0;
}

static int on_switch_process(int event_id, int event_type, SceProcEventInvokeParam2 *param, int a4) {
    (void)event_id;
    (void)event_type;
    (void)param;
    (void)a4;
    return 0;
}

static const SceProcEventHandler g_handler = {
    .size = sizeof(SceProcEventHandler),
    .create = on_create,
    .exit = on_exit,
    .kill = on_kill,
    .stop = on_stop,
    .start = on_start,
    .switch_process = on_switch_process,
};

int vita_tracy_proc_events_register(VitaTracyKernelState *st) {
    SceUID uid = ksceKernelRegisterProcEventHandler("VitaTracyProcEvent", &g_handler, 0);
    if (uid < 0) {
        return VITA_TRACY_ERROR_STATE;
    }
    st->proc_event_uid = uid;
    return VITA_TRACY_OK;
}

int vita_tracy_proc_events_unregister(VitaTracyKernelState *st) {
    if (st->proc_event_uid > 0) {
        int ret = ksceKernelUnregisterProcEventHandler(st->proc_event_uid);
        if (ret < 0) return ret;
        st->proc_event_uid = 0;
    }
    return 0;
}
