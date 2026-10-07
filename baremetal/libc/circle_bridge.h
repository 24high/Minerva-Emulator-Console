#ifndef RA_BAREMETAL_CIRCLE_BRIDGE_H
#define RA_BAREMETAL_CIRCLE_BRIDGE_H

// The few Circle services newlib_glue.cpp needs, without Circle's headers.

#ifdef __cplusplus
extern "C" {
#endif

void ra_libc_log(const char *pLine);
void ra_libc_panic(const char *pMessage) __attribute__((noreturn));
unsigned long long ra_libc_clock_usec(void);
unsigned ra_libc_unix_time(void);

// Circle's heap (malloc() itself stays Circle's own) and a critical section
// that also excludes interrupt handlers.
void *ra_libc_heap_alloc(unsigned long nSize);
void *ra_libc_heap_realloc(void *pBlock, unsigned long nSize);
void ra_libc_heap_free(void *pBlock);
unsigned ra_libc_heap_alignment(void);
void ra_libc_lock(void);
void ra_libc_unlock(void);

// Clears the execute-never bit for the memory range (used by mmap/mprotect
// with PROT_EXEC for dynarec code buffers). Returns 0 on success.
int ra_libc_make_executable(void *pStart, unsigned long nLength);

// Makes freshly written code in [pBegin, pEnd) visible to instruction fetch
// (see include/ra_clear_cache.h for why this is not called __clear_cache).
void ra_libc_clear_cache(void *pBegin, void *pEnd);

// Same for all memory: cleans the whole data cache, invalidates the whole
// instruction cache.
void ra_libc_sync_code_caches(void);

#ifdef __cplusplus
}
#endif

#endif
