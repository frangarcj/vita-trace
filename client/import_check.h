#pragma once

#include <stdint.h>

/* Whether the loader ever bound an imported function to a real address.
 *
 * A Vita import is emitted as a sixteen-byte descriptor —
 * [version|flags, library NID, function NID, padding] — that the loader
 * overwrites with a branch once it resolves. When it cannot, the entry is
 * left behind and calling it takes the process down: a crash dump from
 * hardware showed such a call arriving at PC 0 with the stub address still
 * in r12.
 *
 * Two cases make this reachable by design rather than by accident: the
 * kernel plugin is optional and imported weakly, and ScePerf is not present
 * in a process until sceSysmoduleLoadModule brings it in. Both have to be
 * checked before the first call rather than after. */
static inline int vita_tracy_import_resolved(const void *fn) {
    uintptr_t addr = (uintptr_t)fn & ~(uintptr_t)1;
    const volatile uint32_t *stub = (const volatile uint32_t *)addr;

    uint32_t header = stub[0];
    uint32_t version = header >> 16;

    /* An unresolved descriptor carries the library version in its high half;
     * a patched one starts with an instruction, which never looks like this.
     * A stub bound to nothing at all reads as zero. */
    if (header == 0) {
        return 0;
    }
    return version != 1u;
}
