#pragma once
#define SCE_SYSMODULE_PERF 1
#define SCE_SYSMODULE_LOADED 0
#ifdef __cplusplus
extern "C" {
#endif
int sceSysmoduleIsLoaded(int id);
int sceSysmoduleLoadModule(int id);
int sceSysmoduleUnloadModule(int id);
#ifdef __cplusplus
}
#endif
