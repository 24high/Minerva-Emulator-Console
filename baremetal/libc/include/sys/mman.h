#ifndef RA_BAREMETAL_SYS_MMAN_H
#define RA_BAREMETAL_SYS_MMAN_H

// newlib for arm-none-eabi has no <sys/mman.h>. There is no virtual memory to
// map: anonymous mappings come from the heap (page aligned, zeroed) and address
// hints are ignored. PROT_EXEC makes the memory executable (dynarec caches),
// other protection changes are no-ops. See newlib_glue.cpp.

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE	0x0
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define PROT_EXEC	0x4

#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02
#define MAP_FIXED	0x10
#define MAP_ANONYMOUS	0x20
#define MAP_ANON	MAP_ANONYMOUS

#define MAP_FAILED	((void *)-1)

#define MADV_NORMAL	0
#define MADV_RANDOM	1
#define MADV_SEQUENTIAL	2
#define MADV_WILLNEED	3
#define MADV_DONTNEED	4

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int mprotect(void *addr, size_t length, int prot);
int madvise(void *addr, size_t length, int advice);

#ifdef __cplusplus
}
#endif

#endif
