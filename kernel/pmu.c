#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/types.h>

#include "internal.h"
#include "vita_tracy/kernel_abi.h"

/* ScePerf's ARM PMON counters are a userland library, so a kernel module
 * cannot import them; the client drives them per thread and feeds the
 * values straight to Tracy as plots when it can.
 *
 * That path is dead on retail: ScePerf cannot be loaded at all (see
 * docs/reverse_engineering.md, "Measured on hardware"), and even if it
 * could, PMUSERENR reads 0. Reaching the PMU means driving CP15 directly
 * from kernel context, which this now does. Reverse-engineering
 * os0/kd/threadmgr.elf (3.36 and 3.60, identical) answered the questions
 * this comment used to list as open:
 *
 *  - The scheduler never touches CP15 c9 on a context switch. PMU state is
 *    not per-thread; it free-runs per core until something reprograms it.
 *  - PMUSERENR reads 0 on retail because nothing on retail ever writes it,
 *    not because of a hardware fuse. os0/kd/pamgr.elf used to reach the
 *    function that does; retail firmware (3.50+) never runs pamgr's boot
 *    code.
 *  - That function is a real, still-present SceThreadmgrForDriver export
 *    (NID 0x5053B005, `mcr p15,0,r6,c9,c14,0` at 3.60 0x8101335a) -- not
 *    ScePamgr-specific, and not removed in 3.50. Its sibling (NID
 *    0x1AAFA818, `mcr p15,0,r6,c9,c12,0`) writes PMCR the same way.
 *
 * Both are called through the plain `mcr` instruction below rather than by
 * name: their calling convention (a target-thread argument with a
 * permission check when nonzero, gated behind a caller-context check) is
 * more machinery than a kernel module -- already running at PL1, on
 * whichever core it explicitly migrated itself to -- needs. pamgr's own
 * boot code makes the same choice for its PMCR write, only calling the
 * threadmgr export for other, still-unidentified callers.
 *
 * Confirmed on hardware 2026-08-21, and still an open question: the write
 * genuinely lands -- a CP15 read-back immediately afterward, in the same
 * per-core thread that wrote it (see vita_tracy_pmu_configure), sees
 * PMUSERENR=1 on all 4 cores every time. But neither Sce's own
 * sceKernelGetPMUSERENR() nor a direct userland PMCCNTR read (which
 * outright faults, confirmed via crash dump) ever sees it from a
 * DIFFERENT thread afterward, even though this file's own earlier
 * research established that context switches never touch CP15 c9 --
 * predicting exactly the opposite.
 *
 * sceKernelGetPMUSERENR() is not the explanation: confirmed on hardware
 * (same day) that it is a bare `mrc p15,0,r0,c9,c14,0; bx lr` with no
 * kernel round-trip and no cached/stale value -- a direct read from
 * userland, right next to it, always agrees exactly. No standalone
 * SceLibKernel module was found in a decrypted 3.60 dump either (not
 * under os0/kd, os0/kd/bootimage, or vs0/sys/external -- likely built
 * into the kernel image itself rather than a separate loadable file), so
 * this was settled empirically rather than by disassembly. The
 * discrepancy is therefore a genuine hardware/architectural fact, not a
 * caching artifact: whether the enable is somehow scoped to the writing
 * thread's lifetime (and lost when that thread exits) or something else
 * entirely is unresolved; phase 4 is not usable from userland yet despite
 * the kernel-side write provably working. */

/* kuKernelCreateThread's own cpu_affinity_mask parameter (kubridge,
 * src/main.c's per-core SWP-enable loop) uses this shifted encoding, not
 * the raw 1<<core bits ksceKernelChangeThreadCpuAffinityMask takes -- and
 * psp2/kernel/cpu.h names it: SCE_KERNEL_CPU_MASK_USER_0/1/2 plus a
 * SYSTEM mask for the reserved fourth core. Not in psp2kern/kernel/cpu.h,
 * so redeclared locally; the values are firmware ABI, not SDK metadata. */
#define VITA_TRACY_CPU_MASK_USER_0 0x00010000u
#define VITA_TRACY_CPU_MASK_USER_1 0x00020000u
#define VITA_TRACY_CPU_MASK_USER_2 0x00040000u
#define VITA_TRACY_CPU_MASK_SYSTEM 0x00080000u

static const uint32_t kPmuCoreMasks[4] = {
    VITA_TRACY_CPU_MASK_USER_0,
    VITA_TRACY_CPU_MASK_USER_1,
    VITA_TRACY_CPU_MASK_USER_2,
    VITA_TRACY_CPU_MASK_SYSTEM,
};

/* PMCR bits pamgr's own boot code writes: E (enable all counters) and X
 * (export events to the trace bus -- harmless with nothing listening). */
#define PMU_PMCR_E (1u << 0)
#define PMU_PMCR_X (1u << 4)

/* PMCNTENSET/PMCNTENCLR bit 31 is the dedicated cycle counter; bits 0-5
 * address the six programmable counters pamgr's PMCNTENSET write
 * (0x8000003F, from ScePerf's own scePerfArmPmonStart) implies exist. */
#define PMU_CYCLE_COUNTER_BIT (1u << 31)

static inline void pmu_write_pmuserenr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c14, 0" ::"r"(v) : "memory");
}

static inline uint32_t pmu_read_pmuserenr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c9, c14, 0" : "=r"(v));
    return v;
}

static inline void pmu_write_pmcr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" ::"r"(v) : "memory");
}

static inline void pmu_write_pmcntenclr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 2" ::"r"(v) : "memory");
}

static inline void pmu_write_pmcntenset(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" ::"r"(v) : "memory");
}

static inline void pmu_write_pmselr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 5" ::"r"(v) : "memory");
}

static inline void pmu_write_pmxevtyper(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c13, 1" ::"r"(v) : "memory");
}

static inline uint32_t pmu_read_pmccntr(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v));
    return v;
}

/* Programs the local core: opens userland PMU access, enables the unit,
 * clears any prior counter configuration, then arms the cycle counter plus
 * whichever programmable counters the caller asked for. Runs entirely on
 * whichever core is current when called -- the caller is responsible for
 * having pinned it there. */
static int pmu_configure_local_core(const VitaTracyPmuConfig *cfg) {
    pmu_write_pmuserenr(1u);
    pmu_write_pmcr(PMU_PMCR_E | PMU_PMCR_X);
    pmu_write_pmcntenclr(0xFFFFFFFFu);

    uint32_t enable_mask = PMU_CYCLE_COUNTER_BIT;
    for (uint32_t i = 0; i < cfg->counter_count; ++i) {
        uint32_t counter = cfg->counters[i].counter;
        if (counter >= 31u) {
            continue; /* leaves bit 31 exclusively the cycle counter */
        }
        pmu_write_pmselr(counter);
        pmu_write_pmxevtyper(cfg->counters[i].event_code);
        enable_mask |= (1u << counter);
    }

    pmu_write_pmcntenset(enable_mask);

    /* Reads back on the same core, in the same thread, right after the
     * write -- isolates "did the mcr itself take" from "was the userland
     * thread that checks PMUSERENR afterward even on this core." */
    return pmu_read_pmuserenr() != 0;
}

static const VitaTracyPmuConfig *g_configure_cfg;
static int g_verified_count;

static int pmu_configure_core_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    if (pmu_configure_local_core(g_configure_cfg)) {
        g_verified_count++;
    }
    return 0;
}

/* PMU state is per-core hardware, not per-thread (see the file comment
 * above), so cfg->target_tid does not select what gets programmed here --
 * every core gets the same configuration. It stays in the ABI in case a
 * future per-core policy needs it.
 *
 * One brand-new thread per core, its target core baked into
 * ksceKernelCreateThread's own cpu_affinity_mask argument, rather than one
 * thread whose affinity is changed after creation and migrated in a loop
 * (kubridge's own per-core init loop, src/main.c, uses exactly this
 * one-thread-per-core shape). Confirmed necessary on hardware 2026-08-21:
 * the migrate-in-a-loop version reliably returned success and, run on a
 * dedicated helper thread, stopped returning the syscall-boundary
 * corruption (see git log) -- but PMUSERENR still read 0 back from
 * userland afterward. ksceKernelChangeThreadCpuAffinityMask only updates
 * which cores a thread is *allowed* on; nothing forces an immediate
 * migration, so a tight loop of mask-change-then-mcr with no yield in
 * between plausibly never actually left the core the thread happened to
 * already be running on, no matter how many times the mask changed.
 * Creating a fresh thread already pinned to one core sidesteps that: the
 * scheduler places a brand new thread according to its mask the first
 * time it runs, not lazily. */
int vita_tracy_pmu_configure(VitaTracyKernelState *st, const VitaTracyPmuConfig *cfg) {
    (void)st;

    g_configure_cfg = cfg;
    g_verified_count = 0;

    int programmed = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        SceUID thid = ksceKernelCreateThread("VitaTracyPmuCore", pmu_configure_core_thread, 0x40, 0x1000, 0,
                                              (int)kPmuCoreMasks[i], NULL);
        if (thid < 0) {
            continue;
        }
        if (ksceKernelStartThread(thid, 0, NULL) < 0) {
            ksceKernelDeleteThread(thid);
            continue;
        }
        ksceKernelWaitThreadEnd(thid, NULL, NULL);
        ksceKernelDeleteThread(thid);
        programmed++;
    }

    if (programmed == 0) {
        return VITA_TRACY_ERROR_STATE;
    }
    /* Temporary: encodes how many of the 4 per-core threads read PMUSERENR
     * back as nonzero immediately after writing it, distinguishable from
     * VITA_TRACY_OK (0) and every VITA_TRACY_ERROR_* (small negatives). */
    return 1000 + g_verified_count;
}

/* Kernel-owned PMCCNTR sampler: sidesteps the whole PMUSERENR-visibility
 * puzzle above by never asking userland to touch CP15 at all. PL1 has
 * unconditional access to the PMU regardless of PMUSERENR -- that bit only
 * gates PL0 -- so this thread enables the counter and reads it itself,
 * the same way the existing PC sampler (sampler_debug_fallback.c) reads
 * thread registers itself and hands values across the ring/stats
 * boundary rather than letting userland touch anything privileged
 * directly. Pinned to VITA_TRACY_CPU_MASK_USER_0 only for this first
 * test -- one core is enough to prove the model before scaling to all
 * three app-reachable cores the way vita_tracy_pmu_configure already
 * does. */
#define VITA_TRACY_PMU_SAMPLE_INTERVAL_US 10000u

static int pmu_sample_thread_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    VitaTracyKernelState *st = vita_tracy_state();

    pmu_write_pmcr(PMU_PMCR_E | PMU_PMCR_X);
    pmu_write_pmcntenclr(0xFFFFFFFFu);
    pmu_write_pmcntenset(PMU_CYCLE_COUNTER_BIT);

    /* One-shot busy-loop measurement, no ksceKernelDelayThread anywhere in
     * it: if the sleep-interval loop below shows an implausibly low rate,
     * this tells us whether that's because the counter doesn't advance
     * during idle/WFI time (this measurement would still show a plausible
     * rate) or because it doesn't really advance at all on this thread
     * (this measurement would be ~0 too). */
    {
        uint32_t busy_before = pmu_read_pmccntr();
        volatile uint32_t acc = 0;
        for (uint32_t i = 0; i < 5000000u; ++i) {
            acc += i * 3u;
        }
        (void)acc;
        st->stats.pmu_busy_loop_cycles = pmu_read_pmccntr() - busy_before;
    }

    uint32_t prev = pmu_read_pmccntr();
    while (st->pmu_sample_should_run) {
        ksceKernelDelayThread(VITA_TRACY_PMU_SAMPLE_INTERVAL_US);
        uint32_t now = pmu_read_pmccntr();
        st->stats.pmu_cycle_delta_total += (now - prev); /* truncated sub handles wrap */
        st->stats.pmu_sample_ticks++;
        prev = now;
    }
    return 0;
}

int vita_tracy_pmu_sample_start(VitaTracyKernelState *st) {
    if (st->pmu_sample_thread > 0) {
        return VITA_TRACY_OK; /* already running */
    }

    st->stats.pmu_cycle_delta_total = 0;
    st->stats.pmu_sample_ticks = 0;
    st->stats.pmu_busy_loop_cycles = 0;
    st->pmu_sample_should_run = 1;

    SceUID thid = ksceKernelCreateThread("VitaTracyPmuSample", pmu_sample_thread_entry, 0x40, 0x1000, 0,
                                          (int)VITA_TRACY_CPU_MASK_USER_0, NULL);
    if (thid < 0) {
        st->pmu_sample_should_run = 0;
        return VITA_TRACY_ERROR_STATE;
    }
    if (ksceKernelStartThread(thid, 0, NULL) < 0) {
        ksceKernelDeleteThread(thid);
        st->pmu_sample_should_run = 0;
        return VITA_TRACY_ERROR_STATE;
    }

    st->pmu_sample_thread = thid;
    return VITA_TRACY_OK;
}

void vita_tracy_pmu_sample_stop(VitaTracyKernelState *st) {
    if (st->pmu_sample_thread <= 0) {
        return;
    }
    st->pmu_sample_should_run = 0;
    ksceKernelWaitThreadEnd(st->pmu_sample_thread, NULL, NULL);
    ksceKernelDeleteThread(st->pmu_sample_thread);
    st->pmu_sample_thread = 0;
}
