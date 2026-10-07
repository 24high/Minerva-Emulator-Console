#ifndef RA_BAREMETAL_CLEAR_CACHE_H
#define RA_BAREMETAL_CLEAR_CACHE_H

// GCC for arm-none-eabi expands __clear_cache() and __builtin___clear_cache()
// to nothing: it assumes libgcc's bare-metal version, which is empty. A
// dynarec then executes stale instruction cache lines on real hardware
// (emulators like QEMU have no caches and do not notice). Cores that generate
// code are compiled with this header forced in (-include), which turns those
// calls into calls of the real implementation in libc/circle_bridge.cpp.

#ifndef __ASSEMBLER__

#ifdef __cplusplus
extern "C"
#endif
void ra_libc_clear_cache(void *pBegin, void *pEnd);

#define __clear_cache(begin, end) ra_libc_clear_cache((void *)(begin), (void *)(end))
#define __builtin___clear_cache(begin, end) ra_libc_clear_cache((void *)(begin), (void *)(end))

#endif

#endif
