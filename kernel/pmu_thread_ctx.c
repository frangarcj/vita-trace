#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/kernel/threadmgr/debugger.h>
#include <psp2kern/kernel/sysclib.h>

#include "pmu_thread_ctx.h"
#include "firmware_exports.h"
#include "internal.h"
#include "vita_tracy/kernel_abi.h"

/* Library / function NIDs (3.60) from the decrypted module export tables.
 * Confirm against the RE notes before changing. */
#define LIB_THREADMGR_FOR_KERNEL 0xA8CA0EFDu
#define LIB_THREADMGR_FOR_KERNEL_363 0x7F8593BAu /* 3.63 and later */
#define LIB_THREADMGR_FOR_DRIVER 0xE2C40624u
#define LIB_PROCESSMGR_FOR_DRIVER 0x746EC971u
/* ForDriver keeps its NIDs on 3.63+. The ForKernel functions move to a new
 * library with new NIDs there; the 3.63 values come from github.com/bythos14/libperf
 * and from matching the 3.60 code against the 3.63/3.65 dumps (2026-09-30).
 * GetCounter's 3.63 NID is a 98 % code match, not confirmed on a console. */
#define NID_SET_PROCESS_PMCR 0x1AAFA818u      /* (pid|0, pmcr): ctx+0x64 of every thread */
#define NID_SET_THREAD_COUNTER 0xD2BE5EFBu    /* (tid, ctr|0x1f, value): ctx+0x74 / +0x7c+8i */
#define NID_SET_THREAD_COUNTER_363 0x7B3368F1u
#define NID_GET_THREAD_COUNTER 0xCE99E69Cu    /* (tid, ctr, *out) */
#define NID_GET_THREAD_COUNTER_363 0x170F69D6u
#define NID_SET_THREAD_EVENT 0x6ECCDCBDu      /* (tid, ctr 0..5, type): ctx+0x78+8i */
#define NID_SET_THREAD_EVENT_363 0xFFB9CD24u
/* (tid, mask): ctx+0xF0 |= / &= mask & 0x8000003F under the thread lock, and
 * a cross-core call when the thread is running elsewhere. 0x2EC8E376, which
 * looks like the enable, is a stub returning 0x80020002. */
#define NID_SET_ENABLE_COUNTER 0x72E5DA4Eu
#define NID_SET_ENABLE_COUNTER_363 0x7F831213u
#define NID_CLEAR_ENABLE_COUNTER 0x43D13895u
#define NID_CLEAR_ENABLE_COUNTER_363 0x1D2A6815u
/* Returns 0x80029008 without effect unless DIPSW 0xE4 is set; threads inherit
 * their creator's context anyway, and the IRQ node adopts the rest. */
#define NID_SET_PROCESS_DEFAULT_PMCR 0x61B9B6FAu /* (pid, val): proc+0x290, inherited */

/* Thread object -> saved context, and fields inside it. */
#define THREAD_OBJ_CTX_OFFSET 0x34u
/* Observed on hardware (2026-09-22): obj+0x38 holds a second context block
 * that sits exactly CTX_SIZE after the first; the syscall-return and
 * first-run restore paths read the PMU fields from it. */
/* [ctx+0xF8] is a restore-only second block used on callback-return and
 * thread-first-run paths (never written from hardware). obj+0x38 is the VFP
 * save area, not a context. */
#define CTX_TWIN_OFFSET 0xF8u
#define CTX_PMCR 0x64u
#define CTX_PMUSERENR 0x70u
#define CTX_PMCCNTR 0x74u
#define CTX_PMCNTENSET 0xF0u
/* Event counter i: type at +0x78+8i, count at +0x7C+8i (3.60 threadmgr,
 * 0x6ECCDCBD and 0xD2BE5EFB, disassembled 2026-09-30). */
#define CTX_EVTYPE(i) (0x78u + 8u * (i))
#define CTX_EVCOUNT(i) (0x7Cu + 8u * (i))
#define PMU_EVENT_BITS 0x3Fu
#define PMU_CYCLE_BIT 0x80000000u
#define COUNTER_CYCLE 0x1Fu

typedef struct PmuCtxExports {
    int (*set_process_pmcr)(SceUID pid, uint32_t pmcr);
    int (*set_thread_counter)(SceUID tid, uint32_t counter, uint32_t value);
    int (*get_thread_counter)(SceUID tid, uint32_t counter, uint32_t *out);
    int (*set_process_default_pmcr)(SceUID pid, uint32_t pmcr);
    int (*set_thread_event)(SceUID tid, uint32_t counter, uint32_t type); /* optional */
    int (*set_enable)(SceUID tid, uint32_t mask);   /* optional; else direct +0xF0 write */
    int (*clear_enable)(SceUID tid, uint32_t mask); /* optional; else direct +0xF0 write */
} PmuCtxExports;

static PmuCtxExports g_x;
static uint32_t g_ready;

/* `fn363` is the function's NID in the 3.63+ ForKernel library; 0 when the
 * function keeps its NID (ForDriver). */
static int resolve_any2(uint32_t fn, uint32_t fn363, uintptr_t *out) {
    const uint32_t libs[] = {LIB_THREADMGR_FOR_KERNEL, LIB_THREADMGR_FOR_DRIVER, LIB_THREADMGR_FOR_KERNEL_363};
    const uint32_t fns[] = {fn, fn, fn363 ? fn363 : fn};
    for (unsigned i = 0; i < 3; ++i) {
        uintptr_t a = 0;
        if (vita_tracy_lookup_export("SceKernelThreadMgr", libs[i], fns[i], &a) >= 0 && a) { *out = a; return 0; }
    }
    return VITA_TRACY_ERROR_UNSUPPORTED;
}

static int resolve_any(uint32_t fn, uintptr_t *out) { return resolve_any2(fn, 0, out); }

int vita_tracy_pmu_ctx_init(void) {
    if (__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return 0;
    PmuCtxExports x;
    uintptr_t a;
    memset(&x, 0, sizeof(x));
    if (resolve_any(NID_SET_PROCESS_PMCR, &a) < 0) { VITA_TRACY_TRACE("pmuctx: set_process_pmcr unresolved\n"); return VITA_TRACY_ERROR_UNSUPPORTED; }
    x.set_process_pmcr = (int (*)(SceUID, uint32_t))a;
    if (resolve_any2(NID_SET_THREAD_COUNTER, NID_SET_THREAD_COUNTER_363, &a) < 0) { VITA_TRACY_TRACE("pmuctx: set_thread_counter unresolved\n"); return VITA_TRACY_ERROR_UNSUPPORTED; }
    x.set_thread_counter = (int (*)(SceUID, uint32_t, uint32_t))a;
    /* Diagnostics only. */
    x.get_thread_counter = resolve_any2(NID_GET_THREAD_COUNTER, NID_GET_THREAD_COUNTER_363, &a) < 0 ? NULL :
        (int (*)(SceUID, uint32_t, uint32_t *))a;
    x.set_thread_event = resolve_any2(NID_SET_THREAD_EVENT, NID_SET_THREAD_EVENT_363, &a) < 0 ? NULL :
        (int (*)(SceUID, uint32_t, uint32_t))a;
    x.set_enable = resolve_any2(NID_SET_ENABLE_COUNTER, NID_SET_ENABLE_COUNTER_363, &a) < 0 ? NULL :
        (int (*)(SceUID, uint32_t))a;
    x.clear_enable = resolve_any2(NID_CLEAR_ENABLE_COUNTER, NID_CLEAR_ENABLE_COUNTER_363, &a) < 0 ? NULL :
        (int (*)(SceUID, uint32_t))a;
    VITA_TRACY_TRACE("pmuctx: optional exports event=%d enable=%d clear=%d get=%d\n", !!x.set_thread_event,
        !!x.set_enable, !!x.clear_enable, !!x.get_thread_counter);
    a = 0;
    if (vita_tracy_lookup_export("SceProcessmgr", LIB_PROCESSMGR_FOR_DRIVER, NID_SET_PROCESS_DEFAULT_PMCR, &a) < 0 || !a) {
        VITA_TRACY_TRACE("pmuctx: set_process_default_pmcr unresolved (optional)\n");
        x.set_process_default_pmcr = NULL;
    } else {
        x.set_process_default_pmcr = (int (*)(SceUID, uint32_t))a;
    }
    g_x = x;
    __atomic_store_n(&g_ready, 1u, __ATOMIC_RELEASE);
    return 0;
}

/* A candidate second context block must sit in the same kernel heap
 * neighbourhood as the primary one, 8-byte aligned. */
static int plausible_ctx(const uint32_t *candidate, const uint32_t *primary) {
    uintptr_t c = (uintptr_t)candidate, p = (uintptr_t)primary;
    if (!c || (c & 7u) || c == p) return 0;
    return (c > p ? c - p : p - c) < 0x01000000u;
}

/* Verify the object->context mapping on this thread before trusting it:
 * write a marker through the firmware export and read it back raw. */
static uint32_t *thread_ctx(SceUID tid, uint32_t preload, int *verified) {
    SceObjectBase *obj = NULL;
    *verified = 0;
    if (ksceGUIDReferObject(tid, &obj) < 0 || !obj) return NULL;
    uint32_t *ctx = *(uint32_t **)((uint8_t *)obj + THREAD_OBJ_CTX_OFFSET);
    if (!ctx) { ksceGUIDReleaseObject(tid); return NULL; }
    if (g_x.set_thread_counter(tid, COUNTER_CYCLE, preload) >= 0 && ctx[CTX_PMCCNTR / 4] == preload)
        *verified = 1;
    return ctx; /* caller releases */
}

/* Event types and zero counts for one thread, checked through the context.
 * Returns the enable bits to set: none unless every counter took. */
static uint32_t program_events(SceUID tid, const uint32_t *ctx, const uint32_t *events, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (g_x.set_thread_event(tid, i, events[i]) < 0 || g_x.set_thread_counter(tid, i, 0u) < 0 ||
            ctx[CTX_EVTYPE(i) / 4] != events[i] || ctx[CTX_EVCOUNT(i) / 4] != 0u) {
            VITA_TRACY_TRACE("pmuctx: tid=0x%08X event %u not programmed\n", (unsigned)tid, (unsigned)i);
            return 0;
        }
    }
    return (1u << count) - 1u;
}

int vita_tracy_pmu_ctx_program(SceUID pid, uint32_t preload, const uint32_t *events, uint32_t event_count) {
    int ret = vita_tracy_pmu_ctx_init();
    if (ret < 0) return ret;
    if (event_count > 6u || (event_count && (!events || !g_x.set_thread_event)))
        return VITA_TRACY_ERROR_UNSUPPORTED;
    SceUID ids[128];
    int count = 0;
    ret = ksceKernelGetThreadIdList(pid, ids, 128, &count);
    if (ret < 0) return ret;
    if (g_x.set_process_default_pmcr) {
        const int def = g_x.set_process_default_pmcr(pid, 1u);
        VITA_TRACY_TRACE("pmuctx: set_process_default_pmcr -> 0x%08X (0x80029008: DIPSW 0xE4 clear)\n", (unsigned)def);
    }
    ret = g_x.set_process_pmcr(pid, 1u);
    VITA_TRACY_TRACE("pmuctx: pid=0x%08X threads=%d set_process_pmcr -> %d\n", (unsigned)pid, count, ret);
    if (ret < 0) return ret;
    int programmed = 0;
    for (int i = 0; i < count; ++i) {
        int verified = 0;
        uint32_t *ctx = thread_ctx(ids[i], preload, &verified);
        if (!ctx) { VITA_TRACY_TRACE("pmuctx: tid=0x%08X no ctx\n", (unsigned)ids[i]); continue; }
        if (!verified) {
            VITA_TRACY_TRACE("pmuctx: tid=0x%08X ctx=%p mapping NOT verified (pmccntr=%08X pmcr=%08X) - skipped\n",
                (unsigned)ids[i], (void *)ctx, (unsigned)ctx[CTX_PMCCNTR / 4], (unsigned)ctx[CTX_PMCR / 4]);
            ksceGUIDReleaseObject(ids[i]);
            continue;
        }
        const uint32_t event_bits = event_count ? program_events(ids[i], ctx, events, event_count) : 0u;
        /* Through the firmware when available: it also reaches a thread that
         * is running on another core, whose live bank would otherwise
         * overwrite a direct context write at its next switch-out. */
        const uint32_t enable = PMU_CYCLE_BIT | event_bits;
        if (!g_x.set_enable || g_x.set_enable(ids[i], enable) < 0) ctx[CTX_PMCNTENSET / 4] |= enable;
        /* The second context block: reported at ctx+0xf8 by the RE, but read
         * as 0 on hardware; the thread object's +0xf8 is the other candidate. */
        SceObjectBase *obj = NULL;
        if (ksceGUIDReferObject(ids[i], &obj) >= 0 && obj) {
            uint32_t *twin = (uint32_t *)ctx[CTX_TWIN_OFFSET / 4];
            if (!plausible_ctx(twin, ctx)) twin = NULL;
            if (twin) {
                for (uint32_t e = 0; event_bits && e < event_count; ++e) {
                    twin[CTX_EVTYPE(e) / 4] = events[e];
                    twin[CTX_EVCOUNT(e) / 4] = 0u;
                }
                twin[CTX_PMCNTENSET / 4] |= PMU_CYCLE_BIT | event_bits;
                twin[CTX_PMCR / 4] = ctx[CTX_PMCR / 4];
                twin[CTX_PMCCNTR / 4] = preload;
            }
            ksceGUIDReleaseObject(ids[i]);
        }
        VITA_TRACY_TRACE("pmuctx: tid=0x%08X pmcr=%08X cnten=%08X\n", (unsigned)ids[i],
            (unsigned)ctx[CTX_PMCR / 4], (unsigned)ctx[CTX_PMCNTENSET / 4]);
        ksceGUIDReleaseObject(ids[i]);
        ++programmed;
    }
    return programmed;
}

/* Diagnostics: what the firmware last saved for each thread, and the live/
 * saved cycle counter through the firmware's own getter. */
void vita_tracy_pmu_ctx_dump(SceUID pid) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    SceUID ids[128];
    int count = 0;
    if (ksceKernelGetThreadIdList(pid, ids, 128, &count) < 0) return;
    for (int i = 0; i < count; ++i) {
        SceObjectBase *obj = NULL;
        if (ksceGUIDReferObject(ids[i], &obj) < 0 || !obj) continue;
        uint32_t *ctx = *(uint32_t **)((uint8_t *)obj + THREAD_OBJ_CTX_OFFSET);
        uint32_t counter = 0;
        int g = g_x.get_thread_counter ? g_x.get_thread_counter(ids[i], COUNTER_CYCLE, &counter) : -1;
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        int ti = ksceKernelGetThreadInfo(ids[i], &info);
        VITA_TRACY_TRACE("pmuctx: tid=0x%08X %s status=%u prio=%d aff=%08X cpu=%d last=%d run=%llu us\n",
            (unsigned)ids[i], ti >= 0 ? info.name : "?", (unsigned)info.status, info.currentPriority,
            (unsigned)info.currentCpuAffinityMask, info.currentCpuId, info.lastExecutedCpuId,
            (unsigned long long)(ti >= 0 ? info.runClocks : 0));
        VITA_TRACY_TRACE("pmuctx:   ctx: pmcr=%08X ovsr=%08X pmccntr=%08X cnten=%08X get(%d)=%08X\n",
            (unsigned)ctx[CTX_PMCR / 4], (unsigned)ctx[0x68 / 4],
            (unsigned)ctx[CTX_PMCCNTR / 4], (unsigned)ctx[CTX_PMCNTENSET / 4], g, (unsigned)counter);
        ksceGUIDReleaseObject(ids[i]);
    }
}

SceUID vita_tracy_pmu_ctx_find_thread(SceUID pid, const char *name) {
    SceUID ids[128];
    int count = 0;
    if (ksceKernelGetThreadIdList(pid, ids, 128, &count) < 0) return 0;
    for (int i = 0; i < count; ++i) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ksceKernelGetThreadInfo(ids[i], &info) >= 0 && strncmp(info.name, name, sizeof(info.name)) == 0)
            return ids[i];
    }
    return 0;
}

/* One line per thread: name and status, to find who suspends the target. */
void vita_tracy_pmu_ctx_status(SceUID pid, const char *tag) {
    SceUID ids[128];
    int count = 0;
    if (ksceKernelGetThreadIdList(pid, ids, 128, &count) < 0) return;
    char line[200];
    int n = snprintf(line, sizeof(line), "pmuctx: status[%s]:", tag);
    for (int i = 0; i < count && n < (int)sizeof(line) - 24; ++i) {
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ksceKernelGetThreadInfo(ids[i], &info) < 0) continue;
        n += snprintf(line + n, sizeof(line) - n, " %.10s=%u", info.name, (unsigned)info.status);
    }
    VITA_TRACY_TRACE("%s\n", line);
}

/* Saved PMCCNTR of one thread in both context blocks (diagnostics). */
int vita_tracy_pmu_ctx_peek(SceUID tid, uint32_t *primary, uint32_t *twin, uint32_t *primary_cnten, uint32_t *twin_cnten) {
    SceObjectBase *obj = NULL;
    if (ksceGUIDReferObject(tid, &obj) < 0 || !obj) return -1;
    uint32_t *ctx = *(uint32_t **)((uint8_t *)obj + THREAD_OBJ_CTX_OFFSET);
    uint32_t *t = ctx ? (uint32_t *)ctx[CTX_TWIN_OFFSET / 4] : NULL;
    if (ctx) { *primary = ctx[CTX_PMCCNTR / 4]; *primary_cnten = ctx[CTX_PMCNTENSET / 4]; }
    if (plausible_ctx(t, ctx)) { *twin = t[CTX_PMCCNTR / 4]; *twin_cnten = t[CTX_PMCNTENSET / 4]; }
    ksceGUIDReleaseObject(tid);
    return 0;
}

int vita_tracy_pmu_ctx_release(SceUID pid) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return VITA_TRACY_ERROR_UNSUPPORTED;
    SceUID ids[128];
    int count = 0;
    if (ksceKernelGetThreadIdList(pid, ids, 128, &count) < 0) return VITA_TRACY_ERROR_STATE;
    for (int i = 0; i < count; ++i) {
        SceObjectBase *obj = NULL;
        if (ksceGUIDReferObject(ids[i], &obj) < 0 || !obj) continue;
        uint32_t *ctx = *(uint32_t **)((uint8_t *)obj + THREAD_OBJ_CTX_OFFSET);
        if (ctx) {
            if (!g_x.clear_enable || g_x.clear_enable(ids[i], PMU_CYCLE_BIT | PMU_EVENT_BITS) < 0)
                ctx[CTX_PMCNTENSET / 4] &= ~(PMU_CYCLE_BIT | PMU_EVENT_BITS);
            uint32_t *twin = (uint32_t *)ctx[CTX_TWIN_OFFSET / 4];
            if (plausible_ctx(twin, ctx)) twin[CTX_PMCNTENSET / 4] &= ~(PMU_CYCLE_BIT | PMU_EVENT_BITS);
        }
        ksceGUIDReleaseObject(ids[i]);
    }
    if (g_x.set_process_default_pmcr) g_x.set_process_default_pmcr(pid, 0u);
    return g_x.set_process_pmcr(pid, 0u);
}
