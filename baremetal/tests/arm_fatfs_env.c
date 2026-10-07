// For arm-smoke.mjs --fatfs: runs the real baremetal/libc/newlib_glue.cpp
// and Circle's FatFs under qemu-arm / qemu-aarch64. Provides what Circle provides on the
// device: the ra_libc_* bridge, a heap with Circle's 32-byte block alignment
// (malloc/calloc stay "Circle's" like in the kernel) and a FatFs disk driver
// that reads and writes a FAT image file (SMOKE_IMAGE) with Linux system
// calls.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <fatfs/ff.h>
#include <fatfs/diskio.h>

#include "linux_syscall.h"

static void WriteOut(const char *text)
{
	LinuxWrite(1, text, (long)strlen(text));
}

// Heap: bump allocator in one large anonymous mapping, 32-byte aligned blocks
// with a size header, like Circle's heap. free() comes from newlib_glue.cpp.
#define HEAP_SIZE (384u * 1024 * 1024)
#define HEAP_ALIGN 32u

static uint8_t *s_HeapBase;
static size_t s_HeapUsed;

void *ra_libc_heap_alloc(unsigned long nSize)
{
	if (!s_HeapBase)
	{
		long base = LinuxMmapAnonymous(HEAP_SIZE, 3);	// read/write
		if (base < 0 && base > -4096)
		{
			return 0;
		}
		s_HeapBase = (uint8_t *)base;
	}

	const size_t nTotal = (HEAP_ALIGN + nSize + HEAP_ALIGN - 1) & ~(size_t)(HEAP_ALIGN - 1);
	if (s_HeapUsed + nTotal > HEAP_SIZE)
	{
		return 0;
	}
	uint8_t *pBlock = s_HeapBase + s_HeapUsed + HEAP_ALIGN;
	((size_t *)pBlock)[-1] = nSize;
	s_HeapUsed += nTotal;
	return pBlock;
}

void ra_libc_heap_free(void *pBlock)
{
	(void)pBlock;	// never reused, good enough for a test run
}

void *ra_libc_heap_realloc(void *pBlock, unsigned long nSize)
{
	void *pNew = ra_libc_heap_alloc(nSize);
	if (pNew && pBlock)
	{
		const size_t nOld = ((size_t *)pBlock)[-1];
		memcpy(pNew, pBlock, nOld < nSize ? nOld : nSize);
	}
	return pNew;
}

unsigned ra_libc_heap_alignment(void)
{
	return HEAP_ALIGN;
}

void *malloc(size_t nSize)
{
	return ra_libc_heap_alloc(nSize);
}

void *calloc(size_t nBlocks, size_t nSize)
{
	void *pBlock = ra_libc_heap_alloc(nBlocks * nSize);
	if (pBlock)
	{
		memset(pBlock, 0, nBlocks * nSize);
	}
	return pBlock;
}

void ra_libc_lock(void)
{
}

void ra_libc_unlock(void)
{
}

int ra_libc_make_executable(void *pStart, unsigned long nLength)
{
	const uintptr_t nPage = (uintptr_t)pStart & ~(uintptr_t)4095;
	const long result = LinuxMprotect((void *)nPage, (uintptr_t)pStart + nLength - nPage, 7);
	return result == 0 ? 0 : -1;
}

// Counted for smoke_main, see arm_linux_shim.c.
unsigned long ra_smoke_cache_syncs;

void ra_libc_clear_cache(void *pBegin, void *pEnd)
{
	ra_smoke_cache_syncs++;
	LinuxCacheFlush(pBegin, pEnd);
}

// Whole-cache sync after loading a game; nothing to do under qemu-arm.
void ra_libc_sync_code_caches(void)
{
}

// On AArch64 libgcc's own __clear_cache is the real one (and what the
// builtin calls).
#ifndef __aarch64__
void __clear_cache(void *pBegin, void *pEnd)
{
	ra_libc_clear_cache(pBegin, pEnd);
}
#endif

void ra_libc_log(const char *pLine)
{
	WriteOut("  [libc] ");
	WriteOut(pLine);
	WriteOut("\n");
}

void ra_libc_panic(const char *pMessage)
{
	// The glue's _exit() ends in a panic (a kernel has nowhere to exit to);
	// smoke_main returning normally is no failure here.
	const int exitCode = strcmp(pMessage, "exit(0) called") == 0 ? 0 : 3;
	if (exitCode)
	{
		WriteOut("  [panic] ");
		WriteOut(pMessage);
		WriteOut("\n");
	}
	LinuxExit(exitCode);
	while (1)
	{
	}
}

unsigned long long ra_libc_clock_usec(void)
{
	long tv[2] = { 0, 0 };
	LinuxGettimeofday(tv);
	return (unsigned long long)tv[0] * 1000000u + (unsigned long)tv[1];
}

unsigned ra_libc_unix_time(void)
{
	return (unsigned)(ra_libc_clock_usec() / 1000000u);
}

// FatFs disk driver: physical drive 0 ("SD:") is the image file.
static long s_ImageFd = -1;

DSTATUS disk_status(BYTE pdrv)
{
	return pdrv == 0 && s_ImageFd >= 0 ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	extern char **environ;
	if (pdrv != 0)
	{
		return STA_NOINIT;
	}
	if (s_ImageFd < 0)
	{
		const char *pImage = 0;
		for (char **pEnv = environ; pEnv && *pEnv; pEnv++)
		{
			if (strncmp(*pEnv, "SMOKE_IMAGE=", 12) == 0)
			{
				pImage = *pEnv + 12;
			}
		}
		s_ImageFd = pImage ? LinuxOpen(pImage, LINUX_O_RDWR, 0) : -1;
	}
	return s_ImageFd >= 0 ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (pdrv != 0 || s_ImageFd < 0)
	{
		return RES_NOTRDY;
	}
	const unsigned long long nOffset = (unsigned long long)sector * 512;
	const long result = LinuxPread(s_ImageFd, buff, (long)count * 512, nOffset);
	return result == (long)count * 512 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (pdrv != 0 || s_ImageFd < 0)
	{
		return RES_NOTRDY;
	}
	const unsigned long long nOffset = (unsigned long long)sector * 512;
	const long result = LinuxPwrite(s_ImageFd, buff, (long)count * 512, nOffset);
	return result == (long)count * 512 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	(void)pdrv;
	(void)buff;
	return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}

DWORD get_fattime(void)
{
	return ((DWORD)(2026 - 1980) << 25) | (1 << 21) | (1 << 16);
}

void *ff_memalloc(UINT msize)
{
	return malloc(msize);
}

void ff_memfree(void *mblock)
{
	extern void free(void *);
	free(mblock);
}

// FatFs is configured reentrant (FF_FS_REENTRANT); the test is single-threaded.
int ff_mutex_create(int vol)
{
	(void)vol;
	return 1;
}

void ff_mutex_delete(int vol)
{
	(void)vol;
}

int ff_mutex_take(int vol)
{
	(void)vol;
	return 1;
}

void ff_mutex_give(int vol)
{
	(void)vol;
}
