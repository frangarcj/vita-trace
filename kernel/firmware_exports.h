#pragma once
#include <stdint.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/excpmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize once, before registering callbacks or publishing module startup.
 * Wrappers never resolve exports from IRQ context and refuse an absent table. */
int vita_tracy_firmware_init(void);
int vita_tracy_fw_module_list(SceUID pid, int flags, int type, SceUID *ids, SceSize *count);
int vita_tracy_fw_module_info(SceUID pid, SceUID module, SceKernelModuleInfo *info);
int vita_tracy_fw_thread_context(SceKernelThreadContextInfo *info);
int vita_tracy_fw_register_handler(SceExcpKind kind, int priority, void *node);

/* Platform adapter, also replaced by failure-injection tests. */
int vita_tracy_lookup_export(const char *module, uint32_t library, uint32_t function,
                            uintptr_t *address);
#ifdef VITA_TRACY_TESTING
void vita_tracy_firmware_test_reset(void);
#endif
#ifdef __cplusplus
}
#endif
