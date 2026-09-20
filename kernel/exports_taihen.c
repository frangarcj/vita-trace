#include <stdint.h>
#include <taihen.h>

/* Exported by taihenModuleUtils_stub; not declared in installed taihen.h. */
extern int module_get_export_func(SceUID pid, const char *module, uint32_t library,
                                  uint32_t function, uintptr_t *address);

int vita_tracy_lookup_export(const char *module, uint32_t library, uint32_t function,
                            uintptr_t *address) {
    return module_get_export_func(KERNEL_PID, module, library, function, address);
}
