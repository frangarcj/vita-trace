#pragma once

#include <stdint.h>

/* Copy and clear with core registers only, for code that runs in interrupt
 * context ahead of Sony's dispatcher (raw IRQ/SVC nodes, systimer callbacks):
 * there the VFP/NEON registers belong to the interrupted thread, and the
 * kernel's memcpy/memset (SceSysclibForDriver) use d0-d3. The volatile
 * destination keeps GCC from turning these loops back into library calls. */
static inline void vita_irq_copy(void *dst, const void *src, uint32_t size) {
    if ((((uintptr_t)dst | (uintptr_t)src | size) & 3u) == 0) {
        volatile uint32_t *d = (volatile uint32_t *)dst;
        const uint32_t *s = (const uint32_t *)src;
        for (uint32_t i = 0; i < size / 4u; ++i) d[i] = s[i];
        return;
    }
    volatile uint8_t *d = (volatile uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < size; ++i) d[i] = s[i];
}

static inline void vita_irq_zero(void *dst, uint32_t size) {
    if ((((uintptr_t)dst | size) & 3u) == 0) {
        volatile uint32_t *d = (volatile uint32_t *)dst;
        for (uint32_t i = 0; i < size / 4u; ++i) d[i] = 0;
        return;
    }
    volatile uint8_t *d = (volatile uint8_t *)dst;
    for (uint32_t i = 0; i < size; ++i) d[i] = 0;
}
