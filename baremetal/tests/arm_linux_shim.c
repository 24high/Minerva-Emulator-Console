// What baremetal/libc provides on the device, implemented with ARM Linux
// system calls for the qemu-arm smoke test (newlib + libgloss-linux lack them).
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "dirent.h"

static long Syscall6(long number, long a, long b, long c, long d, long e, long f)
{
	register long r0 __asm__("r0") = a;
	register long r1 __asm__("r1") = b;
	register long r2 __asm__("r2") = c;
	register long r3 __asm__("r3") = d;
	register long r4 __asm__("r4") = e;
	register long r5 __asm__("r5") = f;
	register long r7 __asm__("r7") = number;
	__asm__ volatile ("svc #0"
		: "+r" (r0)
		: "r" (r1), "r" (r2), "r" (r3), "r" (r4), "r" (r5), "r" (r7)
		: "memory");
	return r0;
}

static long Result(long value)
{
	if (value < 0 && value > -4096)
	{
		errno = (int)-value;
		return -1;
	}
	return value;
}

// Address hints are ignored like on the device; a mapping right behind the
// program would also stop newlib's brk-based malloc from growing.
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	(void)addr;
	const long value = Syscall6(192, 0, (long)length, prot, flags, fd, (long)(offset >> 12));
	return value < 0 && value > -4096 ? (errno = (int)-value, (void *)-1) : (void *)value;
}

int munmap(void *addr, size_t length)
{
	return (int)Result(Syscall6(91, (long)addr, (long)length, 0, 0, 0, 0));
}

int mprotect(void *addr, size_t length, int prot)
{
	return (int)Result(Syscall6(125, (long)addr, (long)length, prot, 0, 0, 0));
}

int madvise(void *addr, size_t length, int advice)
{
	(void)addr;
	(void)length;
	(void)advice;
	return 0;
}

// ARM Linux cacheflush(start, end, 0). Counted for smoke_main: under
// qemu-arm a dynarec runs fine without it, on the device it does not.
unsigned long ra_smoke_cache_syncs;

void ra_libc_clear_cache(void *begin, void *end)
{
	ra_smoke_cache_syncs++;
	Syscall6(0x0f0002, (long)begin, (long)end, 0, 0, 0, 0);
}

// Writes a file for smoke_main's CircleFs stub. newlib for arm-none-eabi uses
// other O_* values than Linux (its O_CREAT is Linux' O_NOCTTY), so fopen()
// cannot create files here; use the Linux values directly.
int ra_smoke_write_file(const char *path, const void *data, unsigned long size)
{
	const long fd = Syscall6(5, (long)path, 0x241, 0644, 0, 0, 0);	// open(O_WRONLY|O_CREAT|O_TRUNC)
	if (fd < 0)
	{
		return 0;
	}
	const long written = Syscall6(4, fd, (long)data, (long)size, 0, 0, 0);
	Syscall6(6, fd, 0, 0, 0, 0, 0);	// close
	return written == (long)size;
}

char *getcwd(char *buf, size_t size)
{
	static char s_Buffer[512];
	if (!buf)
	{
		buf = s_Buffer;
		size = sizeof s_Buffer;
	}
	return Syscall6(183, (long)buf, (long)size, 0, 0, 0, 0) < 0 ? 0 : buf;	// getcwd
}

// Directory listings are not needed for the test (see opendir below).
int scandir(const char *dirp, struct dirent ***namelist,
	    int (*filter)(const struct dirent *),
	    int (*compar)(const struct dirent **, const struct dirent **))
{
	(void)dirp;
	(void)namelist;
	(void)filter;
	(void)compar;
	errno = ENOENT;
	return -1;
}

// The test runs ROMs by absolute path; that is already the real path.
char *realpath(const char *path, char *resolved)
{
	if (!resolved)
	{
		resolved = (char *)malloc(strlen(path) + 1);
	}
	return resolved ? strcpy(resolved, path) : 0;
}

// Whole-cache sync after loading a game; nothing to do under qemu-arm.
void ra_libc_sync_code_caches(void)
{
}

void __clear_cache(void *begin, void *end)
{
	ra_libc_clear_cache(begin, end);
}

// No directory listing is needed for the test.
DIR *opendir(const char *name)
{
	(void)name;
	errno = ENOENT;
	return 0;
}

struct dirent *readdir(DIR *dirp)
{
	(void)dirp;
	return 0;
}

void rewinddir(DIR *dirp)
{
	(void)dirp;
}

int closedir(DIR *dirp)
{
	(void)dirp;
	return 0;
}

int _isatty(int fd)
{
	return fd >= 0 && fd <= 2;
}
