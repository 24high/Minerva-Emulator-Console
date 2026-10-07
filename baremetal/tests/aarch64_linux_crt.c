// Program start and newlib's system call layer for the smoke test on
// qemu-aarch64. aarch64-none-elf has no libgloss for Linux (arm-none-eabi
// has linux.specs), so this does what it would: _start, then the calls newlib
// makes for files, memory and time, on Linux system calls. Linked with
// -nostartfiles plus crtbegin.o/crtend.o (they register .eh_frame for C++
// exceptions, as in the kernel); with --fatfs, baremetal/libc/newlib_glue.cpp
// is linked first and its versions win (--allow-multiple-definition).
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>

#include "linux_syscall.h"

extern char **environ;
int main(int argc, char **argv);

void ra_smoke_start(long argc, char **argv, char **envp) __attribute__((noreturn, used));

void ra_smoke_start(long argc, char **argv, char **envp)
{
	environ = envp;
	exit(main((int)argc, argv));
}

// Linux starts a program with argc at [sp], argv and the environment after it.
__asm__(
	".text\n"
	".global _start\n"
	"_start:\n"
	"	mov x29, #0\n"
	"	mov x30, #0\n"
	"	ldr x0, [sp]\n"
	"	add x1, sp, #8\n"
	"	add x2, x1, x0, lsl #3\n"
	"	add x2, x2, #8\n"
	"	b ra_smoke_start\n");

void _exit(int code)
{
	LinuxExit(code);
	while (1)
	{
	}
}

// newlib numbers the open flags differently from Linux.
static long LinuxOpenFlags(int flags)
{
	long result = flags & 3;	// O_RDONLY, O_WRONLY, O_RDWR
	if (flags & 0x0008) result |= 0x400;	// O_APPEND
	if (flags & 0x0200) result |= 0x40;	// O_CREAT
	if (flags & 0x0400) result |= 0x200;	// O_TRUNC
	if (flags & 0x0800) result |= 0x80;	// O_EXCL
	return result;
}

int _open(const char *path, int flags, int mode)
{
	return (int)LinuxResult(LinuxOpen(path, LinuxOpenFlags(flags), mode));
}

int _close(int fd)
{
	return (int)LinuxResult(LinuxClose(fd));
}

int _read(int fd, void *buf, size_t n)
{
	return (int)LinuxResult(LinuxRead(fd, buf, (long)n));
}

int _write(int fd, const void *buf, size_t n)
{
	return (int)LinuxResult(LinuxWrite(fd, buf, (long)n));
}

off_t _lseek(int fd, off_t offset, int whence)
{
	return (off_t)LinuxResult(Syscall6(62, fd, (long)offset, whence, 0, 0, 0));
}

// The kernel's struct stat on AArch64 (asm-generic) into newlib's.
static int ConvertStat(long result, const uint8_t *linuxStat, struct stat *st)
{
	if (LinuxResult(result) < 0)
	{
		return -1;
	}
	memset(st, 0, sizeof *st);
	st->st_mode = *(const uint32_t *)(linuxStat + 16);
	st->st_nlink = *(const uint32_t *)(linuxStat + 20);
	st->st_size = *(const int64_t *)(linuxStat + 48);
	st->st_blksize = *(const int32_t *)(linuxStat + 56);
	st->st_blocks = *(const int64_t *)(linuxStat + 64);
	st->st_mtime = *(const int64_t *)(linuxStat + 88);
	return 0;
}

int _fstat(int fd, struct stat *st)
{
	uint64_t linuxStat[16];
	return ConvertStat(Syscall6(80, fd, (long)linuxStat, 0, 0, 0, 0), (const uint8_t *)linuxStat, st);
}

int _stat(const char *path, struct stat *st)
{
	uint64_t linuxStat[16];
	return ConvertStat(Syscall6(79, LINUX_AT_FDCWD, (long)path, (long)linuxStat, 0, 0, 0),
		(const uint8_t *)linuxStat, st);	// newfstatat
}

int _unlink(const char *path)
{
	return (int)LinuxResult(Syscall6(35, LINUX_AT_FDCWD, (long)path, 0, 0, 0, 0));	// unlinkat
}

int _link(const char *oldPath, const char *newPath)
{
	return (int)LinuxResult(Syscall6(37, LINUX_AT_FDCWD, (long)oldPath, LINUX_AT_FDCWD, (long)newPath, 0, 0));
}

int _rename(const char *oldPath, const char *newPath)
{
	return (int)LinuxResult(Syscall6(38, LINUX_AT_FDCWD, (long)oldPath, LINUX_AT_FDCWD, (long)newPath, 0, 0));
}

void *_sbrk(ptrdiff_t increment)
{
	static uintptr_t s_Break;
	if (!s_Break)
	{
		s_Break = (uintptr_t)Syscall6(214, 0, 0, 0, 0, 0, 0);	// brk
	}
	const uintptr_t previous = s_Break;
	const uintptr_t wanted = previous + increment;
	if ((uintptr_t)Syscall6(214, (long)wanted, 0, 0, 0, 0, 0) != wanted)
	{
		errno = ENOMEM;
		return (void *)-1;
	}
	s_Break = wanted;
	return (void *)previous;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	long linuxTime[2];
	if (LinuxGettimeofday(linuxTime) < 0)
	{
		return -1;
	}
	tv->tv_sec = linuxTime[0];
	tv->tv_usec = linuxTime[1];
	return 0;
}

clock_t _times(struct tms *buf)
{
	memset(buf, 0, sizeof *buf);
	return 0;
}

int _getpid(void)
{
	return 1;
}

int _kill(int pid, int sig)
{
	(void)pid;
	(void)sig;
	errno = EINVAL;
	return -1;
}

int mkdir(const char *path, mode_t mode)
{
	return (int)LinuxResult(Syscall6(34, LINUX_AT_FDCWD, (long)path, mode, 0, 0, 0));	// mkdirat
}

int chdir(const char *path)
{
	return (int)LinuxResult(Syscall6(49, (long)path, 0, 0, 0, 0, 0));
}

int ftruncate(int fd, off_t length)
{
	return (int)LinuxResult(Syscall6(46, fd, (long)length, 0, 0, 0, 0));
}

long sysconf(int name)
{
	(void)name;	// asked for the page size only
	return 4096;
}

// crti.o / crtn.o (left out with -nostartfiles)
void _init(void)
{
}

void _fini(void)
{
}

// The N64 renderer's worker pool (platform/circle/circle_parallel.cpp on the
// device, which runs the task on all CPU cores): here on one thread.
void parallel_run(void task(uint32_t))
{
	task(0);
}

uint32_t parallel_num_workers(void)
{
	return 1;
}

void parallel_alinit(uint32_t num)
{
	(void)num;
}

void parallel_close(void)
{
}

// libc/circle_bridge.cpp on the device: the N64 dynarec makes its code buffer
// (in .bss) executable. With --fatfs, arm_fatfs_env.c's version is used.
int ra_libc_make_executable(void *pStart, unsigned long nLength)
{
	const uintptr_t page = (uintptr_t)pStart & ~(uintptr_t)4095;
	return LinuxMprotect((void *)page, (uintptr_t)pStart + nLength - page, 7) == 0 ? 0 : -1;
}
