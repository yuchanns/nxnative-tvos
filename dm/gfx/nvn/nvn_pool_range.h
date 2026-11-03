#ifndef NVN_POOL_RANGE_H
#define NVN_POOL_RANGE_H

#include <stdint.h>

// Round a known access extent to reusable windows without crossing the owning
// no-copy buffer. Pool bounds and the window quantum are VM-page aligned.
static inline int nvn_pool_window(uintptr_t addr, uintptr_t len,
                                  uintptr_t base, uintptr_t bytes, uintptr_t quantum,
                                  uintptr_t *out_base, uintptr_t *out_bytes) {
    if (!len || !bytes || !quantum || (quantum & (quantum - 1)) ||
        base > UINTPTR_MAX - bytes || addr < base || addr - base >= bytes ||
        len > bytes - (addr - base)) return 0;
    uintptr_t start = (addr - base) & ~(quantum - 1);
    uintptr_t end = addr - base + len;
    uintptr_t padding = (quantum - (end & (quantum - 1))) & (quantum - 1);
    end += padding < bytes - end ? padding : bytes - end;
    *out_base = base + start;
    *out_bytes = end - start;
    return 1;
}

#endif
