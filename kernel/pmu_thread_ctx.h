#pragma once
#include <stdint.h>
#include <psp2kern/types.h>

/* The 3.60 firmware saves and restores the PMU register set per thread
 * (intrmgr's IRQ handler and two SceIntrmgrForKernel exports). A cycle
 * counter therefore only runs for threads whose saved context enables it.
 * These helpers program that saved context for every thread of one process
 * through firmware exports (PMCR, PMCCNTR) and one direct write
 * (PMCNTENSET, whose enable export is stubbed on retail). Thread context
 * only; nothing here is IRQ-safe. Offsets are from the 2026-09-22 RE of
 * threadmgr/intrmgr and are verified at runtime before any direct write. */

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves the extra exports on first use. Negative when any is missing. */
int vita_tracy_pmu_ctx_init(void);

/* Program the target process for cycle-counter overflow sampling: PMCR.E for
 * existing and future threads, PMCCNTR preload and PMCNTENSET.C in every
 * thread's saved context. `preload` is the counter start value (0 - period).
 * Returns the number of threads programmed, or a negative error. */
int vita_tracy_pmu_ctx_program(SceUID pid, uint32_t preload);

/* Undo: clear the enable bit in every thread's saved context and PMCR.E. */
int vita_tracy_pmu_ctx_release(SceUID pid);
void vita_tracy_pmu_ctx_dump(SceUID pid);
void vita_tracy_pmu_ctx_status(SceUID pid, const char *tag);
SceUID vita_tracy_pmu_ctx_find_thread(SceUID pid, const char *name);
int vita_tracy_pmu_ctx_peek(SceUID tid, uint32_t *primary, uint32_t *twin, uint32_t *primary_cnten, uint32_t *twin_cnten);

#ifdef __cplusplus
}
#endif
