// Linux system calls for the smoke test programs under qemu-arm (32-bit ARM,
// EABI) and qemu-aarch64. The bare-metal toolchains bring no Linux libc, so
// the tests talk to the kernel directly.
#ifndef RA_SMOKE_LINUX_SYSCALL_H
#define RA_SMOKE_LINUX_SYSCALL_H

#include <errno.h>
#include <stdint.h>

#if defined(__aarch64__)

static inline long Syscall6(long number, long a, long b, long c, long d, long e, long f)
{
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	register long x8 __asm__("x8") = number;
	__asm__ volatile ("svc #0"
		: "+r" (x0)
		: "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5), "r" (x8)
		: "memory");
	return x0;
}

#define LINUX_AT_FDCWD (-100)

static inline long LinuxOpen(const char *path, long flags, long mode)
{
	return Syscall6(56, LINUX_AT_FDCWD, (long)path, flags, mode, 0, 0);	// openat
}
static inline long LinuxRead(long fd, void *buf, long n) { return Syscall6(63, fd, (long)buf, n, 0, 0, 0); }
static inline long LinuxWrite(long fd, const void *buf, long n) { return Syscall6(64, fd, (long)buf, n, 0, 0, 0); }
static inline long LinuxClose(long fd) { return Syscall6(57, fd, 0, 0, 0, 0, 0); }
static inline long LinuxPread(long fd, void *buf, long n, unsigned long long offset)
{
	return Syscall6(67, fd, (long)buf, n, (long)offset, 0, 0);	// pread64
}
static inline long LinuxPwrite(long fd, const void *buf, long n, unsigned long long offset)
{
	return Syscall6(68, fd, (long)buf, n, (long)offset, 0, 0);	// pwrite64
}
static inline long LinuxMmapAnonymous(unsigned long length, long prot)
{
	return Syscall6(222, 0, (long)length, prot, 0x22, -1, 0);	// MAP_PRIVATE | MAP_ANONYMOUS
}
static inline long LinuxMmap(void *hint, unsigned long length, long prot, long flags, long fd, long offset)
{
	return Syscall6(222, (long)hint, (long)length, prot, flags, fd, offset);
}
static inline long LinuxMunmap(void *addr, unsigned long length) { return Syscall6(215, (long)addr, (long)length, 0, 0, 0, 0); }
static inline long LinuxMprotect(void *addr, unsigned long length, long prot)
{
	return Syscall6(226, (long)addr, (long)length, prot, 0, 0, 0);
}
static inline long LinuxGetcwd(char *buf, unsigned long size) { return Syscall6(17, (long)buf, (long)size, 0, 0, 0, 0); }
static inline long LinuxGettimeofday(long *tv) { return Syscall6(169, (long)tv, 0, 0, 0, 0, 0); }
static inline void LinuxExit(long code) { Syscall6(94, code, 0, 0, 0, 0, 0); }	// exit_group
// User space can clean and invalidate the caches itself on AArch64 Linux.
static inline void LinuxCacheFlush(void *begin, void *end) { __builtin___clear_cache((char *)begin, (char *)end); }

#else	// 32-bit ARM EABI

static inline long Syscall6(long number, long a, long b, long c, long d, long e, long f)
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

static inline long LinuxOpen(const char *path, long flags, long mode) { return Syscall6(5, (long)path, flags, mode, 0, 0, 0); }
static inline long LinuxRead(long fd, void *buf, long n) { return Syscall6(3, fd, (long)buf, n, 0, 0, 0); }
static inline long LinuxWrite(long fd, const void *buf, long n) { return Syscall6(4, fd, (long)buf, n, 0, 0, 0); }
static inline long LinuxClose(long fd) { return Syscall6(6, fd, 0, 0, 0, 0, 0); }
// pread64/pwrite64(fd, buf, count, 0, offset_low, offset_high): EABI register pairs
static inline long LinuxPread(long fd, void *buf, long n, unsigned long long offset)
{
	return Syscall6(180, fd, (long)buf, n, 0, (long)(offset & 0xFFFFFFFFu), (long)(offset >> 32));
}
static inline long LinuxPwrite(long fd, const void *buf, long n, unsigned long long offset)
{
	return Syscall6(181, fd, (long)buf, n, 0, (long)(offset & 0xFFFFFFFFu), (long)(offset >> 32));
}
static inline long LinuxMmapAnonymous(unsigned long length, long prot)
{
	return Syscall6(192, 0, (long)length, prot, 0x22, -1, 0);	// mmap2, MAP_PRIVATE | MAP_ANONYMOUS
}
static inline long LinuxMmap(void *hint, unsigned long length, long prot, long flags, long fd, long offset)
{
	return Syscall6(192, (long)hint, (long)length, prot, flags, fd, offset >> 12);	// mmap2: offset in pages
}
static inline long LinuxMunmap(void *addr, unsigned long length) { return Syscall6(91, (long)addr, (long)length, 0, 0, 0, 0); }
static inline long LinuxMprotect(void *addr, unsigned long length, long prot)
{
	return Syscall6(125, (long)addr, (long)length, prot, 0, 0, 0);
}
static inline long LinuxGetcwd(char *buf, unsigned long size) { return Syscall6(183, (long)buf, (long)size, 0, 0, 0, 0); }
static inline long LinuxGettimeofday(long *tv) { return Syscall6(78, (long)tv, 0, 0, 0, 0, 0); }
static inline void LinuxExit(long code) { Syscall6(1, code, 0, 0, 0, 0, 0); }
// ARM Linux cacheflush(start, end, 0)
static inline void LinuxCacheFlush(void *begin, void *end) { Syscall6(0x0f0002, (long)begin, (long)end, 0, 0, 0, 0); }

#endif

// O_WRONLY | O_CREAT | O_TRUNC in Linux' numbering (the same on both).
#define LINUX_O_WRONLY_CREAT_TRUNC 0x241
#define LINUX_O_RDWR 2

static inline long LinuxResult(long value)
{
	if (value < 0 && value > -4096)
	{
		errno = (int)-value;
		return -1;
	}
	return value;
}

#endif
