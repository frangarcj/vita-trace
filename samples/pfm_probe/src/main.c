/* Standalone kernel probe for the PA performance-monitor block (ScePfmReg,
 * 0xE50D0000) and its GIC line, SPI 244 ("SceKernelPaPmoni" in pamgr).
 *
 * Retail 3.36 pamgr only initialises this block when DIPSW 212 is set, which
 * retail consoles never have, and retail 3.60 has no pamgr at all. Whether
 * the Cortex-A9 PMU overflow reaches the GIC through this block is a
 * hypothesis; this module tests it in stages, each gated on a marker file so
 * a hang can be placed from the log alone:
 *
 *   always                        DIPSW 212/213
 *   unless ..._skip_mmio          map the block and read the registers pamgr touches
 *   ux0:data/vita_pfm_probe_write replay pamgr's init writes, read back
 *   ux0:data/vita_pfm_probe_irq   register SPI 244 on core 0, arm a PMCCNTR
 *                                 overflow in a thread spinning on core 0, and
 *                                 count deliveries against polled overflows
 *
 * Every log line is appended and the file closed before the next step. The
 * module does its work in module_start and does not stay resident unless an
 * interrupt handler could not be released.
 *
 * Result on the retail 3.60 console (2026-09-28): DIPSW 212 and 213 read 0,
 * the mapping succeeds, and the first read (+0x10BC) never completes. The
 * loading thread stays blocked, the rest of the system keeps running, and
 * only a reboot clears it. The block does not answer on retail, so the
 * write and IRQ stages have never run. */

#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/dipsw.h>
#include <psp2kern/kernel/intrmgr.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/io/fcntl.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#define LOG_PATH "ux0:data/vita_pfm_probe.txt"

#define PFM_PADDR 0xE50D0000u
#define PFM_SIZE 0x2000u
#define PFM_INTR 244
#define PFM_INTR_PRIORITY 0xD0
#define PFM_INTR_CORE0 1
#define PFM_ACK 0x00010003u

#define OVF_C (1u << 31)
#define PERIOD_CYCLES 0x01000000u /* ~20-27 overflows/s at 333-444 MHz */
#define SPIN_US 2000000u
#define HANDLER_CAP 5000u

static volatile uint32_t *g_pfm;

static struct {
    volatile uint32_t calls;
    volatile uint32_t with_overflow;
    volatile uint32_t wrong_core;
    volatile uint32_t capped;
    volatile uint32_t status_log[8];
} g_intr;

static struct {
    uint32_t polled;
    uint32_t unserviced;
    uint32_t loops;
    uint32_t pmcr, cnten, inten;
    int core;
} g_spin;

static void log_line(const char *fmt, ...) {
    char line[192];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    SceUID fd = ksceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd < 0) return;
    ksceIoWrite(fd, line, n);
    ksceIoClose(fd);
}

static int marker(const char *path) {
    SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    ksceIoClose(fd);
    return 1;
}

static inline void dsb(void) { __asm__ volatile("dsb" ::: "memory"); }

static uint32_t pfm_read(uint32_t off) {
    log_line("read  +0x%04X ...", (unsigned)off);
    uint32_t v = g_pfm[off / 4];
    log_line("read  +0x%04X = 0x%08X", (unsigned)off, (unsigned)v);
    return v;
}

static void pfm_write(uint32_t off, uint32_t v) {
    log_line("write +0x%04X <- 0x%08X ...", (unsigned)off, (unsigned)v);
    g_pfm[off / 4] = v;
    dsb();
    log_line("write +0x%04X done", (unsigned)off);
}

static inline uint32_t pmovsr(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 3" : "=r"(v)); return v; }
static inline void pmovsr_clear(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c12, 3" :: "r"(v) : "memory"); }
static inline void pmccntr_set(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c13, 0" :: "r"(v) : "memory"); }
static inline uint32_t pmcr_get(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(v)); return v; }
static inline void pmcr_set(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v) : "memory"); }
static inline uint32_t cnten_get(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(v)); return v; }
static inline void cnten_set(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(v) : "memory"); }
static inline void cnten_clr(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(v) : "memory"); }
static inline uint32_t inten_get(void) { uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c14, 1" : "=r"(v)); return v; }
static inline void inten_set(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c14, 1" :: "r"(v) : "memory"); }
static inline void inten_clr(uint32_t v) { __asm__ volatile("mcr p15, 0, %0, c9, c14, 2" :: "r"(v) : "memory"); }

/* Mirrors pamgr's handler: read the status word, acknowledge with 0x10003.
 * It also services the PMU overflow of whatever thread it interrupted, which
 * on core 0 during the spin is ours. */
static int pfm_intr_handler(int code, void *ctx) {
    (void)code;
    (void)ctx;
    uint32_t n = g_intr.calls++;
    uint32_t status = g_pfm[0x1098 / 4];
    if (n < 8) g_intr.status_log[n] = status;
    if (ksceKernelCpuId() != 0) g_intr.wrong_core++;
    if (pmovsr() & OVF_C) {
        pmovsr_clear(OVF_C);
        pmccntr_set(0u - PERIOD_CYCLES);
        g_intr.with_overflow++;
    }
    g_pfm[0x1098 / 4] = PFM_ACK;
    dsb();
    if (n + 1 >= HANDLER_CAP && !g_intr.capped) {
        g_intr.capped = 1;
        inten_clr(OVF_C);
        ksceKernelDisableIntr(PFM_INTR);
    }
    return -1;
}

/* PMU state is saved per thread, so the thread that programs the counter is
 * the only one whose overflow can fire. It spins on core 0 and also polls
 * PMOVSR: an overflow that stays pending was not delivered as an interrupt. */
static int spin_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    g_spin.core = ksceKernelCpuId();
    pmcr_set((pmcr_get() & ~0x8u) | 0x1u); /* E, no divider */
    pmovsr_clear(OVF_C);
    pmccntr_set(0u - PERIOD_CYCLES);
    cnten_set(OVF_C);
    inten_set(OVF_C);
    g_spin.pmcr = pmcr_get();
    g_spin.cnten = cnten_get();
    g_spin.inten = inten_get();

    SceInt64 end = ksceKernelGetSystemTimeWide() + SPIN_US;
    while (ksceKernelGetSystemTimeWide() < end) {
        g_spin.loops++;
        if (!(pmovsr() & OVF_C)) continue;
        g_spin.polled++;
        for (volatile int i = 0; i < 20000; ++i) {
        }
        if (pmovsr() & OVF_C) {
            g_spin.unserviced++;
            pmovsr_clear(OVF_C);
            pmccntr_set(0u - PERIOD_CYCLES);
        }
    }

    inten_clr(OVF_C);
    cnten_clr(OVF_C);
    pmovsr_clear(OVF_C);
    return 0;
}

static int irq_test(void) {
    log_line("irq: register %d prio 0x%X core mask %d ...", PFM_INTR, PFM_INTR_PRIORITY, PFM_INTR_CORE0);
    int ret = ksceKernelRegisterIntrHandler(PFM_INTR, "VitaPfmProbe", 0, pfm_intr_handler, NULL,
                                            PFM_INTR_PRIORITY, PFM_INTR_CORE0, NULL);
    log_line("irq: register -> 0x%08X", (unsigned)ret);
    if (ret < 0) return 0;

    if (marker("ux0:data/vita_pfm_probe_write")) pfm_write(0x1098, PFM_ACK);
    log_line("irq: enable ...");
    ret = ksceKernelEnableIntr(PFM_INTR);
    log_line("irq: enable -> 0x%08X, calls so far %u", (unsigned)ret, (unsigned)g_intr.calls);

    ksceKernelDelayThread(100000);
    log_line("irq: idle 100 ms, calls %u (overflow %u)", (unsigned)g_intr.calls, (unsigned)g_intr.with_overflow);

    SceUID th = ksceKernelCreateThread("VitaPfmSpin", spin_thread, 0x10000100, 0x2000, 0, 0x10000, NULL);
    log_line("irq: spin thread 0x%08X", (unsigned)th);
    if (th >= 0) {
        ret = ksceKernelStartThread(th, 0, NULL);
        log_line("irq: spin start 0x%08X (spinning %u ms)", (unsigned)ret, (unsigned)(SPIN_US / 1000));
        if (ret >= 0) ksceKernelWaitThreadEnd(th, NULL, NULL);
        ksceKernelDeleteThread(th);
    }
    log_line("irq: spin core %d loops %u, pmcr %08X cnten %08X inten %08X", g_spin.core, (unsigned)g_spin.loops,
             (unsigned)g_spin.pmcr, (unsigned)g_spin.cnten, (unsigned)g_spin.inten);
    log_line("irq: overflows polled %u, left pending (not delivered) %u", (unsigned)g_spin.polled,
             (unsigned)g_spin.unserviced);
    log_line("irq: handler calls %u, with overflow %u, wrong core %u, capped %u", (unsigned)g_intr.calls,
             (unsigned)g_intr.with_overflow, (unsigned)g_intr.wrong_core, (unsigned)g_intr.capped);
    for (int i = 0; i < 8 && i < (int)g_intr.calls; ++i)
        log_line("irq: status[%d] = 0x%08X", i, (unsigned)g_intr.status_log[i]);

    int d = ksceKernelDisableIntr(PFM_INTR);
    int r = ksceKernelReleaseIntrHandler(PFM_INTR);
    log_line("irq: disable 0x%08X release 0x%08X", (unsigned)d, (unsigned)r);
    return r < 0; /* still registered: stay resident */
}

int module_start(SceSize args, void *argp);
int _start(SceSize args, void *argp) __attribute__((weak, alias("module_start")));
int module_start(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int resident = 0;
    log_line("---- pfm probe ----");
    log_line("dipsw 212 (PA mapping) = %d", ksceKernelCheckDipsw(212));
    log_line("dipsw 213 (PA 128 MiB) = %d", ksceKernelCheckDipsw(213));

    if (marker("ux0:data/vita_pfm_probe_skip_mmio")) {
        log_line("skip_mmio marker: done");
        return SCE_KERNEL_START_NO_RESIDENT;
    }

    SceKernelAllocMemBlockKernelOpt opt = {0};
    opt.size = sizeof(opt);
    opt.attr = 2; /* HAS_PADDR, as pamgr */
    opt.paddr = PFM_PADDR;
    SceUID block = ksceKernelAllocMemBlock("VitaPfmReg", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_IO_RW, PFM_SIZE, &opt);
    log_line("map 0x%08X: block 0x%08X", (unsigned)PFM_PADDR, (unsigned)block);
    if (block < 0) return SCE_KERNEL_START_NO_RESIDENT;
    void *base = NULL;
    int ret = ksceKernelGetMemBlockBase(block, &base);
    log_line("base %p (0x%08X)", base, (unsigned)ret);
    if (ret < 0 || !base) goto out;
    g_pfm = (volatile uint32_t *)base;

    /* pamgr reads 0x10BC first, before writing anything. */
    static const uint32_t regs[] = {0x10BC, 0x1098, 0x1048, 0x1050, 0x10C0, 0x10C4};
    for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) pfm_read(regs[i]);

    if (marker("ux0:data/vita_pfm_probe_write")) {
        /* pamgr's init order. */
        pfm_write(0x1048, 0xFFFF);
        pfm_write(0x1050, 0xFFFF);
        pfm_write(0x10C4, 0);
        pfm_write(0x10C0, 0);
        pfm_write(0x10BC, 1);
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) pfm_read(regs[i]);
    }

    if (marker("ux0:data/vita_pfm_probe_irq")) resident = irq_test();

out:
    if (!resident) ksceKernelFreeMemBlock(block);
    log_line("done (%s)", resident ? "resident" : "unloading");
    return resident ? SCE_KERNEL_START_SUCCESS : SCE_KERNEL_START_NO_RESIDENT;
}

int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return SCE_KERNEL_STOP_SUCCESS;
}
