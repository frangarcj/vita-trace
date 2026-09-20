#include "firmware_exports.h"
#include "vita_tracy/kernel_abi.h"
#include <string.h>

typedef struct FirmwareExports {
    int (*module_list)(SceUID, int, int, SceUID *, SceSize *);
    int (*module_info)(SceUID, SceUID, SceKernelModuleInfo *);
    int (*thread_context)(SceKernelThreadContextInfo *);
    int (*register_handler)(SceExcpKind, int, void *);
} FirmwareExports;

static FirmwareExports g_exports;
static uint32_t g_ready;

static int resolve(const char *module, uint32_t lib360, uint32_t fn360,
                   uint32_t lib363, uint32_t fn363, uintptr_t *result) {
    uintptr_t address = 0;
    int ret = vita_tracy_lookup_export(module, lib360, fn360, &address);
    if (ret >= 0 && address) { *result = address; return 0; }
    address = 0;
    ret = vita_tracy_lookup_export(module, lib363, fn363, &address);
    if (ret >= 0 && address) { *result = address; return 0; }
    return VITA_TRACY_ERROR_UNSUPPORTED;
}

int vita_tracy_firmware_init(void) {
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return 0;
    FirmwareExports exports;
    memset(&exports, 0, sizeof(exports));
    uintptr_t address;
    /* Known NIDs from the installed VitaSDK 360/363 archives; the exception
     * pair is also used by kubridge. No wildcard library or offset guessing.
     * Publish all-or-nothing, before any IRQ producer can use this table. */
    if (resolve("SceModulemgr", 0xC445FA63u, 0x97CF7B4Eu,
                0x92C9FFC2u, 0xB72C75A4u, &address) < 0) return VITA_TRACY_ERROR_UNSUPPORTED;
    exports.module_list = (int (*)(SceUID, int, int, SceUID *, SceSize *))address;
    if (resolve("SceModulemgr", 0xC445FA63u, 0xD269F915u,
                0x92C9FFC2u, 0xDAA90093u, &address) < 0) return VITA_TRACY_ERROR_UNSUPPORTED;
    exports.module_info = (int (*)(SceUID, SceUID, SceKernelModuleInfo *))address;
    if (resolve("SceThreadmgr", 0xA8CA0EFDu, 0xD8B9AC8Du,
                0x7F8593BAu, 0x6C1F092Fu, &address) < 0) return VITA_TRACY_ERROR_UNSUPPORTED;
    exports.thread_context = (int (*)(SceKernelThreadContextInfo *))address;
    if (resolve("SceExcpmgr", 0x4CA0FDD5u, 0x03499636u,
                0x1496A5B5u, 0x00063675u, &address) < 0) return VITA_TRACY_ERROR_UNSUPPORTED;
    exports.register_handler = (int (*)(SceExcpKind, int, void *))address;
    g_exports = exports;
    __atomic_store_n(&g_ready, 1u, __ATOMIC_RELEASE);
    return 0;
}

int vita_tracy_fw_module_list(SceUID pid, int flags, int type, SceUID *ids, SceSize *count) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return VITA_TRACY_ERROR_UNSUPPORTED;
    return g_exports.module_list(pid, flags, type, ids, count);
}
int vita_tracy_fw_module_info(SceUID pid, SceUID module, SceKernelModuleInfo *info) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return VITA_TRACY_ERROR_UNSUPPORTED;
    return g_exports.module_info(pid, module, info);
}
int vita_tracy_fw_thread_context(SceKernelThreadContextInfo *info) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return VITA_TRACY_ERROR_UNSUPPORTED;
    return g_exports.thread_context(info);
}
int vita_tracy_fw_register_handler(SceExcpKind kind, int priority, void *node) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return VITA_TRACY_ERROR_UNSUPPORTED;
    return g_exports.register_handler(kind, priority, node);
}

#ifdef VITA_TRACY_TESTING
void vita_tracy_firmware_test_reset(void) {
    g_ready = 0;
    memset(&g_exports, 0, sizeof(g_exports));
}
#endif
