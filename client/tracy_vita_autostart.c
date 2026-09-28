#include <stdlib.h>
#include "vita_tracy/client.h"

#ifndef VITA_TRACY_AUTO_PMU
#define VITA_TRACY_AUTO_PMU 0
#endif
#ifndef VITA_TRACY_AUTO_PC_SAMPLING
#define VITA_TRACY_AUTO_PC_SAMPLING 0
#endif
#ifndef VITA_TRACY_AUTO_PMU_HZ
#define VITA_TRACY_AUTO_PMU_HZ 100
#endif
#ifndef VITA_TRACY_AUTO_CORE_MASK
#define VITA_TRACY_AUTO_CORE_MASK 7
#endif
#ifndef VITA_TRACY_AUTO_KERNEL_MARKER
#define VITA_TRACY_AUTO_KERNEL_MARKER "ux0:data/vita-tracy/kernel.on"
#endif
#ifndef VITA_TRACY_AUTO_SAMPLE_HZ
#define VITA_TRACY_AUTO_SAMPLE_HZ 100
#endif

extern int __real_main(int argc, char **argv);
extern int __real_sceKernelExitProcess(int status);
extern void vita_tracy_auto_activate(int enabled);
extern void vita_tracy_auto_report(const char *stage, int result);
extern int vita_tracy_auto_kernel_opt_in(void);

static uint32_t lifetime; /* 0 stopped, 1 running, 2 stopping */

void vita_tracy_auto_stop(void) {
    uint32_t running = 1;
    if (!__atomic_compare_exchange_n(&lifetime, &running, 2u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    vita_tracy_auto_activate(0);
    int ret = vita_tracy_shutdown_checked();
    /* Retain failed cleanup for an explicit exit/atexit retry. Emission stays
     * disabled; the application must have stopped its own annotated workers. */
    __atomic_store_n(&lifetime, ret < 0 ? 1u : 0u, __ATOMIC_RELEASE);
}

int __wrap_main(int argc, char **argv) {
    const int started = vita_tracy_init();
    int cleanup_on_return = 0;
    if (started == 0) {
        __atomic_store_n(&lifetime, 1u, __ATOMIC_RELEASE);
        cleanup_on_return = atexit(vita_tracy_auto_stop) != 0;
        vita_tracy_auto_report("startup", 0);
        if (cleanup_on_return) vita_tracy_auto_report("atexit registration", -1);
#if VITA_TRACY_AUTO_PMU || VITA_TRACY_AUTO_PC_SAMPLING
        /* The control ABI is a weak import: unresolved, it branches to 0 and
         * cannot be probed. Attach only when the user says the plugin was
         * resident at launch. */
        int ret = -1;
        if (!vita_tracy_auto_kernel_opt_in()) {
            vita_tracy_auto_report("kernel attach skipped, no " VITA_TRACY_AUTO_KERNEL_MARKER, 0);
        } else {
            ret = vita_tracy_kernel_attach(0, 0);
            vita_tracy_auto_report("kernel attach", ret);
        }
        if (ret == 0) {
#if VITA_TRACY_AUTO_PMU
            VitaTracyPmuConfig cfg = {0};
            cfg.size = sizeof(cfg);
            cfg.abi_version = VITA_TRACY_ABI_VERSION;
            cfg.core_mask = VITA_TRACY_AUTO_CORE_MASK;
            cfg.frequency_hz = VITA_TRACY_AUTO_PMU_HZ;
            const uint32_t events[] = {0x68, 0x60, 0x61, 0x03, 0x01, 0x10};
            cfg.counter_count = sizeof(events) / sizeof(events[0]);
            for (uint32_t i = 0; i < cfg.counter_count; ++i) {
                cfg.counters[i].counter = i;
                cfg.counters[i].event_code = events[i];
            }
            ret = vita_tracy_kernel_configure_pmu(&cfg);
            vita_tracy_auto_report("PMU configuration", ret);
            if (ret == 0) {
                ret = vita_tracy_kernel_pmu_sample_start();
                vita_tracy_auto_report("PMU start", ret);
            }
#elif VITA_TRACY_AUTO_PC_SAMPLING
            ret = vita_tracy_kernel_set_sampling_ex(
                VITA_TRACY_AUTO_SAMPLE_HZ, VITA_TRACY_SAMPLING_PMU_IRQ);
            vita_tracy_auto_report("PC sampling", ret);
#endif
        }
#endif
        vita_tracy_auto_activate(1);
    }
    const int result = __real_main(argc, argv);
    // Normal exit runs callbacks registered by the HB before ours (LIFO).
    if (cleanup_on_return) vita_tracy_auto_stop();
    return result;
}

int __wrap_sceKernelExitProcess(int status) {
    vita_tracy_auto_stop();
    return __real_sceKernelExitProcess(status);
}
