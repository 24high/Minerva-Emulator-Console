// Circle side of newlib_glue.cpp. Circle's and newlib's headers disagree on
// basic types (e.g. time_t), so the two never meet in one translation unit.

#include "circle_bridge.h"

#include <circle/heapallocator.h>
#include <circle/logger.h>
#include <circle/memory.h>
#include <circle/startup.h>
#include <circle/synchronize.h>
#include <circle/timer.h>
#if AARCH == 32 && RASPPI == 1
#include <circle/armv6mmu.h>
#endif

static const char FromLibc[] = "libc";

void ra_libc_log(const char *pLine)
{
	CLogger::Get()->Write(FromLibc, LogNotice, "%s", pLine);
}

void ra_libc_panic(const char *pMessage)
{
	CLogger::Get()->Write(FromLibc, LogPanic, "%s", pMessage);
	halt();
	while (1)
	{
	}
}

unsigned long long ra_libc_clock_usec(void)
{
	return CTimer::GetClockTicks64();
}

unsigned ra_libc_unix_time(void)
{
	CTimer *pTimer = CTimer::Get();
	return pTimer ? pTimer->GetUniversalTime() : 0;
}

void *ra_libc_heap_alloc(unsigned long nSize)
{
	return CMemorySystem::HeapAllocate(nSize, HEAP_DEFAULT_MALLOC);
}

void *ra_libc_heap_realloc(void *pBlock, unsigned long nSize)
{
	return CMemorySystem::HeapReAllocate(pBlock, nSize);
}

void ra_libc_heap_free(void *pBlock)
{
	CMemorySystem::HeapFree(pBlock);
}

unsigned ra_libc_heap_alignment(void)
{
	return HEAP_BLOCK_ALIGN;
}

void ra_libc_lock(void)
{
	EnterCritical(IRQ_LEVEL);
}

void ra_libc_unlock(void)
{
	LeaveCritical();
}

#if AARCH == 32 && RASPPI == 1

static const u32 SECTION_ATTRIBUTE_MASK = 0xFFFFF;
static const u32 CACHE_LINE = DATA_CACHE_LINE_LENGTH_MIN;
static const u32 ICACHE_SIZE = 16 * 1024;

// Circle maps everything behind the kernel code as execute-never 1 MB
// sections (lib/pagetable.cpp). Dynarec buffers come from the heap, so their
// sections get the plain "normal memory" attributes instead.
int ra_libc_make_executable(void *pStart, unsigned long nLength)
{
	if (nLength == 0)
	{
		return 0;
	}

	u32 nTTBR;
	asm volatile ("mrc p15, 0, %0, c2, c0, 0" : "=r" (nTTBR));
	u32 *pTable = (u32 *)(nTTBR & ~0x3FFFU);

	const u32 nFirst = (u32)pStart >> 20;
	const u32 nLast = ((u32)pStart + nLength - 1) >> 20;
	for (u32 i = nFirst; i <= nLast; i++)
	{
		const u32 nAttributes = pTable[i] & SECTION_ATTRIBUTE_MASK;
		if (nAttributes != ARMV6MMUL1SECTION_NORMAL && nAttributes != ARMV6MMUL1SECTION_NORMAL_XN)
		{
			return -1;
		}
	}

	for (u32 i = nFirst; i <= nLast; i++)
	{
		pTable[i] = (pTable[i] & ~SECTION_ATTRIBUTE_MASK) | ARMV6MMUL1SECTION_NORMAL;
	}

	CleanAndInvalidateDataCacheRange((u32)&pTable[nFirst], (nLast - nFirst + 1) * sizeof(u32));
	asm volatile ("mcr p15, 0, %0, c8, c7, 0" : : "r" (0) : "memory");	// invalidate TLBs
	FlushBranchTargetCache();
	DataSyncBarrier();
	FlushPrefetchBuffer();
	return 0;
}

// libgcc's __clear_cache is an empty function for arm-none-eabi, so code
// written by a dynarec would run stale instructions. Done like Linux does it
// for the ARM1176 (v6_coherent_user_range): clean the data cache lines of the
// range, then invalidate the whole instruction cache, with the workaround for
// erratum 411920 (the invalidation can miss lines during a linefill).
extern "C" void ra_libc_clear_cache(void *pBegin, void *pEnd)
{
	const u32 nStart = (u32)pBegin & ~(CACHE_LINE - 1);
	const u32 nEnd = (u32)pEnd;
	if (nEnd <= nStart)
	{
		return;
	}

	if (nEnd - nStart > 2 * ICACHE_SIZE)
	{
		CleanDataCache();
	}
	else
	{
		for (u32 nAddress = nStart; nAddress < nEnd; nAddress += CACHE_LINE)
		{
			asm volatile ("mcr p15, 0, %0, c7, c10, 1" : : "r" (nAddress) : "memory");
		}
	}
	DataSyncBarrier();

	u32 nFlags;
	asm volatile ("mrs %0, cpsr\n"
		      "cpsid ifa\n"
		      "mcr p15, 0, %1, c7, c5, 0\n"
		      "mcr p15, 0, %1, c7, c5, 0\n"
		      "mcr p15, 0, %1, c7, c5, 0\n"
		      "mcr p15, 0, %1, c7, c5, 0\n"
		      "msr cpsr_c, %0\n"
		      "nop\n nop\n nop\n nop\n nop\n nop\n"
		      "nop\n nop\n nop\n nop\n nop\n"
		      : "=&r" (nFlags) : "r" (0) : "memory");

	FlushBranchTargetCache();
	DataSyncBarrier();
	FlushPrefetchBuffer();
}

extern "C" void ra_libc_sync_code_caches(void)
{
	ra_libc_clear_cache(0, (void *)~(u32)0);
}

// For code that really calls __clear_cache (GCC usually drops such calls,
// see include/ra_clear_cache.h).
extern "C" void __clear_cache(void *pBegin, void *pEnd)
{
	ra_libc_clear_cache(pBegin, pEnd);
}

#else

int ra_libc_make_executable(void *, unsigned long)
{
	return -1;
}

extern "C" void ra_libc_clear_cache(void *pBegin, void *pEnd)
{
	__builtin___clear_cache((char *)pBegin, (char *)pEnd);
}

extern "C" void ra_libc_sync_code_caches(void)
{
	CleanDataCache();
	InvalidateInstructionCache();
}

#endif
