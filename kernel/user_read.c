#include <stdint.h>

#include "internal.h"

/* The kernel runs with the user domains set to no-access in DACR;
 * ksceKernelMemcpyUserToKernel opens them (0x55555555, all client) around
 * its copy, and so does this. IRQ context only: local interrupts are masked
 * so nothing else runs with the widened DACR.
 *
 * Each page is translated with ATS1CUR (stage 1, user read) before it is
 * read, and the copy stops at the first page a user load would fault on. The
 * translation reports a fault in PAR instead of taking it, so an unmapped
 * page ends the copy rather than the process: a copy past the stack's last
 * mapped page left the whole process suspended on the console. PAR is saved
 * and restored because the kernel uses the same operation to look up
 * physical addresses. */
uint32_t vita_tracy_read_user_words(uint32_t *dst, uint32_t user_src, uint32_t words) {
    uint32_t dacr, par_saved;
    __asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(dacr));
    __asm__ volatile("mrc p15, 0, %0, c7, c4, 0" : "=r"(par_saved));
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\n\tisb" :: "r"(0x55555555u) : "memory");
    const volatile uint32_t *src = (const volatile uint32_t *)(uintptr_t)user_src;
    uint32_t i = 0;
    while (i < words) {
        const uint32_t addr = user_src + 4u * i;
        uint32_t par;
        __asm__ volatile("mcr p15, 0, %1, c7, c8, 2\n\tisb\n\tmrc p15, 0, %0, c7, c4, 0"
                         : "=r"(par) : "r"(addr & ~0xFFFu) : "memory");
        if (par & 1u) break;
        uint32_t page_words = (((addr | 0xFFFu) + 1u) - addr) / 4u;
        if (page_words > words - i) page_words = words - i;
        for (uint32_t end = i + page_words; i < end; ++i) dst[i] = src[i];
    }
    __asm__ volatile("mcr p15, 0, %0, c7, c4, 0" :: "r"(par_saved));
    __asm__ volatile("mcr p15, 0, %0, c3, c0, 0\n\tisb" :: "r"(dacr) : "memory");
    return i;
}
