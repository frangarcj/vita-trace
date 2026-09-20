#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>

#include "internal.h"
#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"

#define VITA_TRACY_MAX_MODULES 96

/* The Vita only reports addresses and mappings; names and lines are
 * resolved on the PC from the ELF, so this emits the module base/size table
 * needed to turn a sampled PC into a module-relative offset. */
int vita_tracy_modules_snapshot(VitaTracyKernelState *st, SceUID pid) {
    SceUID modids[VITA_TRACY_MAX_MODULES];
    SceSize count = VITA_TRACY_MAX_MODULES;

    int ret = vita_tracy_fw_module_list(pid, 0x7FFFFFFF, 1, modids, &count);
    if (ret < 0 || count > VITA_TRACY_MAX_MODULES) {
        return VITA_TRACY_ERROR_ARGS;
    }

    uint64_t now = vita_tracy_kernel_now();

    for (SceSize i = 0; i < count; ++i) {
        SceKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);

        if (vita_tracy_fw_module_info(pid, modids[i], &info) < 0) {
            continue;
        }

        VitaTraceControlRecord record;
        memset(&record, 0, sizeof(record));
        record.type = VITA_TRACE_MODULE_SNAPSHOT;
        record.timestamp = now;
        record.payload.module_snapshot.pid = (uint32_t)pid;
        /* GetModuleInfo has no module NID field. Leave it explicitly unknown
         * rather than confusing the process-local module UID with a NID. */
        strncpy(record.payload.module_snapshot.module_name, info.module_name,
                          VITA_TRACE_MODULE_NAME_MAX - 1);

        uint32_t segment_count = 0;
        for (uint32_t seg = 0; seg < VITA_TRACE_MODULE_MAX_SEGMENTS; ++seg) {
            if (info.segments[seg].memsz == 0) {
                continue;
            }
            /* Preserve the original segment ordinal for ELF PT_LOAD lookup;
             * omitting empty slots must not renumber the remaining segments. */
            record.payload.module_snapshot.segments[seg].vaddr =
                (uint32_t)(uintptr_t)info.segments[seg].vaddr;
            record.payload.module_snapshot.segments[seg].memsz = info.segments[seg].memsz;
            record.payload.module_snapshot.segments[seg].perm = info.segments[seg].perms;
            segment_count = seg + 1;
        }
        record.payload.module_snapshot.segment_count = segment_count;

        vita_tracy_emit_control(st, &record);
    }

    return VITA_TRACY_OK;
}
