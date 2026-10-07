// What baremetal/libc provides on the device, implemented with Linux system
// calls for the qemu-arm / qemu-aarch64 smoke test (newlib lacks them).
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "dirent.h"

#include "linux_syscall.h"

// Address hints are ignored like on the device; a mapping right behind the
// program would also stop newlib's brk-based malloc from growing. (gpSP's
// AArch64 dynarec needs its cache within BL range of its code, which only
// the kernel's code area in libc/newlib_glue.cpp provides: here, without
// --fatfs, it runs interpreted.)
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	(void)addr;
	const long value = LinuxMmap(0, (unsigned long)length, prot, flags, fd, (long)offset);
	return value < 0 && value > -4096 ? (errno = (int)-value, (void *)-1) : (void *)value;
}

int munmap(void *addr, size_t length)
{
	return (int)LinuxResult(LinuxMunmap(addr, length));
}

int mprotect(void *addr, size_t length, int prot)
{
	return (int)LinuxResult(LinuxMprotect(addr, length, prot));
}

int madvise(void *addr, size_t length, int advice)
{
	(void)addr;
	(void)length;
	(void)advice;
	return 0;
}

// Counted for smoke_main: under qemu a dynarec runs fine without it, on the
// device it does not.
unsigned long ra_smoke_cache_syncs;

void ra_libc_clear_cache(void *begin, void *end)
{
	ra_smoke_cache_syncs++;
	LinuxCacheFlush(begin, end);
}

// Writes a file for smoke_main's CircleFs stub. newlib for arm-none-eabi uses
// other O_* values than Linux (its O_CREAT is Linux' O_NOCTTY), so fopen()
// cannot create files here; use the Linux values directly.
int ra_smoke_write_file(const char *path, const void *data, unsigned long size)
{
	const long fd = LinuxOpen(path, LINUX_O_WRONLY_CREAT_TRUNC, 0644);
	if (fd < 0)
	{
		return 0;
	}
	const long written = LinuxWrite(fd, data, (long)size);
	LinuxClose(fd);
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
	return LinuxGetcwd(buf, size) < 0 ? 0 : buf;
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

// On AArch64 libgcc's own __clear_cache is the real one (and what the
// builtin calls).
#ifndef __aarch64__
void __clear_cache(void *begin, void *end)
{
	ra_libc_clear_cache(begin, end);
}
#endif

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
