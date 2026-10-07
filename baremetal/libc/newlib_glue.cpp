// System layer for newlib, the C library of the arm-none-eabi toolchain, so
// that libretro cores written for a hosted environment run on Circle:
//  - memory comes from Circle's heap (newlib must not manage its own),
//  - files and directories map to FatFs (volume "SD:", mounted by CircleFs),
//  - stdout/stderr go line by line to the Circle logger.

// FatFs names its directory object DIR, like <dirent.h> does.
#define DIR FF_DIR
#include <fatfs/ff.h>
#undef DIR

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <reent.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

#include "circle_bridge.h"

static const char VOLUME[] = "SD:";
static const int FIRST_FILE_FD = 3;
static const int MAX_OPEN_FILES = 16;

struct TOpenFile
{
	bool used;
	FIL file;
};

static TOpenFile s_Files[MAX_OPEN_FILES];

// Circle's memalign() only supports the heap block alignment (one cache line,
// 32 bytes on the Pi Zero) and asserts beyond it, but mmap() wants pages and
// libretro-common's file buffers 64 bytes. Larger alignments over-allocate and
// remember the original block here, so free() and realloc() can find it.
struct TAlignedBlock
{
	void *pAligned;
	void *pRaw;
	size_t nSize;
};

static const unsigned MAX_ALIGNED_BLOCKS = 64;
static TAlignedBlock s_AlignedBlocks[MAX_ALIGNED_BLOCKS];
static volatile unsigned s_nAlignedBlocks = 0;

// Caller holds ra_libc_lock().
static int FindAlignedBlock(const void *pBlock)
{
	for (unsigned i = 0; i < s_nAlignedBlocks; i++)
	{
		if (s_AlignedBlocks[i].pAligned == pBlock)
		{
			return (int)i;
		}
	}
	return -1;
}

static void *AlignedAllocate(size_t nAlign, size_t nSize)
{
	if (nAlign == 0 || (nAlign & (nAlign - 1)) != 0)
	{
		errno = EINVAL;
		return 0;
	}
	if (nAlign <= ra_libc_heap_alignment())
	{
		return ra_libc_heap_alloc(nSize);
	}

	void *pRaw = ra_libc_heap_alloc(nSize + nAlign);
	if (!pRaw)
	{
		errno = ENOMEM;
		return 0;
	}
	void *pAligned = (void *)(((uintptr_t)pRaw + nAlign - 1) & ~(uintptr_t)(nAlign - 1));

	ra_libc_lock();
	const bool bStored = s_nAlignedBlocks < MAX_ALIGNED_BLOCKS;
	if (bStored)
	{
		s_AlignedBlocks[s_nAlignedBlocks] = { pAligned, pRaw, nSize };
		s_nAlignedBlocks = s_nAlignedBlocks + 1;
	}
	ra_libc_unlock();

	if (!bStored)
	{
		ra_libc_heap_free(pRaw);
		errno = ENOMEM;
		return 0;
	}
	return pAligned;
}

// Removes an aligned block from the table; returns false for heap blocks.
static bool ReleaseAlignedBlock(void *pBlock, void **ppRaw, size_t *pSize)
{
	if (s_nAlignedBlocks == 0)
	{
		return false;
	}

	ra_libc_lock();
	const int nIndex = FindAlignedBlock(pBlock);
	if (nIndex >= 0)
	{
		*ppRaw = s_AlignedBlocks[nIndex].pRaw;
		*pSize = s_AlignedBlocks[nIndex].nSize;
		s_AlignedBlocks[nIndex] = s_AlignedBlocks[s_nAlignedBlocks - 1];
		s_nAlignedBlocks = s_nAlignedBlocks - 1;
	}
	ra_libc_unlock();
	return nIndex >= 0;
}

struct ra_dirstream
{
	FF_DIR dir;
	struct dirent entry;
};

static int SetErrno(int error)
{
	errno = error;
	return -1;
}

static int ErrnoFromResult(FRESULT result)
{
	switch (result)
	{
	case FR_NO_FILE:
	case FR_NO_PATH:		return ENOENT;
	case FR_EXIST:			return EEXIST;
	case FR_DENIED:
	case FR_WRITE_PROTECTED:	return EACCES;
	case FR_INVALID_NAME:		return EINVAL;
	case FR_TOO_MANY_OPEN_FILES:	return EMFILE;
	case FR_NOT_ENOUGH_CORE:	return ENOMEM;
	default:			return EIO;
	}
}

// "games/a.nes", "/games/a.nes" and "./games/a.nes" all map to "SD:/games/a.nes".
// Current directory (chdir/getcwd), from the root of the volume, without
// leading or trailing slash; "" is the root.
static char s_Cwd[256];

// pBase + "/" + pPath with "." and ".." resolved, without leading slash.
static void ResolvePath(char *pOut, size_t nSize, const char *pBase, const char *pPath)
{
	size_t nLength = 0;
	pOut[0] = 0;
	const char *ppParts[2] = { pBase, pPath };
	for (unsigned nPart = 0; nPart < 2; nPart++)
	{
		const char *p = ppParts[nPart];
		while (p && *p)
		{
			while (*p == '/')
			{
				p++;
			}
			const char *pEnd = p;
			while (*pEnd && *pEnd != '/')
			{
				pEnd++;
			}
			const size_t nName = (size_t)(pEnd - p);
			if (nName == 2 && p[0] == '.' && p[1] == '.')
			{
				while (nLength > 0 && pOut[nLength - 1] != '/')
				{
					nLength--;
				}
				if (nLength > 0)
				{
					nLength--;
				}
				pOut[nLength] = 0;
			}
			else if (nName > 0 && !(nName == 1 && p[0] == '.') && nLength + nName + 2 < nSize)
			{
				if (nLength > 0)
				{
					pOut[nLength++] = '/';
				}
				memcpy(pOut + nLength, p, nName);
				nLength += nName;
				pOut[nLength] = 0;
			}
			p = pEnd;
		}
	}
}

static void VolumePath(char *pOut, size_t nSize, const char *pPath)
{
	if (strncmp(pPath, VOLUME, sizeof VOLUME - 1) == 0)
	{
		strncpy(pOut, pPath, nSize - 1);
		pOut[nSize - 1] = 0;
		return;
	}

	size_t nLength = sizeof VOLUME - 1;
	memcpy(pOut, VOLUME, nLength);
	pOut[nLength++] = '/';
	ResolvePath(pOut + nLength, nSize - nLength, pPath[0] == '/' ? "" : s_Cwd, pPath);
}

static FIL *FileFromFd(int fd)
{
	const int nIndex = fd - FIRST_FILE_FD;
	if (nIndex < 0 || nIndex >= MAX_OPEN_FILES || !s_Files[nIndex].used)
	{
		return 0;
	}
	return &s_Files[nIndex].file;
}

static bool IsRoot(const char *pVolumePath)
{
	return strcmp(pVolumePath + sizeof VOLUME - 1, "/") == 0;
}

// Collects console output until a line is complete.
static void WriteConsole(const char *pBuffer, size_t nLength)
{
	static char s_Line[256];
	static size_t s_Used = 0;

	for (size_t i = 0; i < nLength; i++)
	{
		const char c = pBuffer[i];
		if (c != '\n' && s_Used < sizeof s_Line - 1)
		{
			if (c != '\r')
			{
				s_Line[s_Used++] = c;
			}
			continue;
		}

		s_Line[s_Used] = 0;
		if (s_Used > 0)
		{
			ra_libc_log(s_Line);
		}
		s_Used = 0;
		if (c != '\n')
		{
			s_Line[s_Used++] = c;
		}
	}
}

extern "C" {

void *__dso_handle = 0;

// Normally from crti.o; Circle runs the constructors itself and never returns
// through exit(), so there is nothing to finalize.
void _init(void)
{
}

void _fini(void)
{
}

// free(), realloc() and memalign() replace Circle's versions (the kernel links
// with --allow-multiple-definition and this object comes first); malloc() and
// calloc() stay Circle's.
void free(void *pBlock)
{
	if (!pBlock)
	{
		return;
	}

	void *pRaw;
	size_t nSize;
	ra_libc_heap_free(ReleaseAlignedBlock(pBlock, &pRaw, &nSize) ? pRaw : pBlock);
}

void *realloc(void *pBlock, size_t nSize)
{
	void *pRaw;
	size_t nOldSize;
	if (pBlock && ReleaseAlignedBlock(pBlock, &pRaw, &nOldSize))
	{
		void *pNew = ra_libc_heap_alloc(nSize);
		if (pNew)
		{
			memcpy(pNew, pBlock, nOldSize < nSize ? nOldSize : nSize);
		}
		ra_libc_heap_free(pRaw);
		return pNew;
	}
	return ra_libc_heap_realloc(pBlock, nSize);
}

void *memalign(size_t nAlign, size_t nSize)
{
	return AlignedAllocate(nAlign, nSize);
}

void *aligned_alloc(size_t nAlign, size_t nSize)
{
	return AlignedAllocate(nAlign, nSize);
}

int posix_memalign(void **ppBlock, size_t nAlign, size_t nSize)
{
	if (nAlign < sizeof(void *))
	{
		nAlign = sizeof(void *);
	}
	void *pBlock = AlignedAllocate(nAlign, nSize);
	if (!pBlock)
	{
		return errno;
	}
	*ppBlock = pBlock;
	return 0;
}

void *_malloc_r(struct _reent *, size_t nSize)
{
	return malloc(nSize);
}

void _free_r(struct _reent *, void *pBlock)
{
	free(pBlock);
}

void *_calloc_r(struct _reent *, size_t nBlocks, size_t nSize)
{
	return calloc(nBlocks, nSize);
}

void *_realloc_r(struct _reent *, void *pBlock, size_t nSize)
{
	return realloc(pBlock, nSize);
}

void *_memalign_r(struct _reent *, size_t nAlign, size_t nSize)
{
	return AlignedAllocate(nAlign, nSize);
}

void *_sbrk(ptrdiff_t)
{
	errno = ENOMEM;
	return (void *)-1;
}

int _open(const char *pPath, int nFlags, int)
{
	if (!pPath)
	{
		return SetErrno(EFAULT);
	}

	int nIndex = 0;
	while (nIndex < MAX_OPEN_FILES && s_Files[nIndex].used)
	{
		nIndex++;
	}
	if (nIndex == MAX_OPEN_FILES)
	{
		return SetErrno(EMFILE);
	}

	BYTE nMode;
	switch (nFlags & O_ACCMODE)
	{
	case O_RDONLY:	nMode = FA_READ;		break;
	case O_WRONLY:	nMode = FA_WRITE;		break;
	default:	nMode = FA_READ | FA_WRITE;	break;
	}
	if (nFlags & O_TRUNC)
	{
		nMode |= FA_CREATE_ALWAYS;
	}
	else if ((nFlags & O_CREAT) && (nFlags & O_EXCL))
	{
		nMode |= FA_CREATE_NEW;
	}
	else if (nFlags & O_APPEND)
	{
		nMode |= FA_OPEN_APPEND;
	}
	else if (nFlags & O_CREAT)
	{
		nMode |= FA_OPEN_ALWAYS;
	}

	char Path[320];
	VolumePath(Path, sizeof Path, pPath);
	const FRESULT Result = f_open(&s_Files[nIndex].file, Path, nMode);
	if (Result != FR_OK)
	{
		return SetErrno(ErrnoFromResult(Result));
	}

	s_Files[nIndex].used = true;
	return FIRST_FILE_FD + nIndex;
}

int _close(int fd)
{
	FIL *pFile = FileFromFd(fd);
	if (!pFile)
	{
		return fd >= 0 && fd < FIRST_FILE_FD ? 0 : SetErrno(EBADF);
	}

	const FRESULT Result = f_close(pFile);
	s_Files[fd - FIRST_FILE_FD].used = false;
	return Result == FR_OK ? 0 : SetErrno(ErrnoFromResult(Result));
}

_ssize_t _read(int fd, void *pBuffer, size_t nCount)
{
	if (fd == STDIN_FILENO)
	{
		return 0;
	}

	FIL *pFile = FileFromFd(fd);
	if (!pFile)
	{
		return SetErrno(EBADF);
	}

	UINT nRead = 0;
	const FRESULT Result = f_read(pFile, pBuffer, (UINT)nCount, &nRead);
	return Result == FR_OK ? (_ssize_t)nRead : SetErrno(ErrnoFromResult(Result));
}

_ssize_t _write(int fd, const void *pBuffer, size_t nCount)
{
	if (fd == STDOUT_FILENO || fd == STDERR_FILENO)
	{
		WriteConsole((const char *)pBuffer, nCount);
		return (_ssize_t)nCount;
	}

	FIL *pFile = FileFromFd(fd);
	if (!pFile)
	{
		return SetErrno(EBADF);
	}

	UINT nWritten = 0;
	const FRESULT Result = f_write(pFile, pBuffer, (UINT)nCount, &nWritten);
	return Result == FR_OK ? (_ssize_t)nWritten : SetErrno(ErrnoFromResult(Result));
}

_off_t _lseek(int fd, _off_t nOffset, int nWhence)
{
	FIL *pFile = FileFromFd(fd);
	if (!pFile)
	{
		return SetErrno(fd >= 0 && fd < FIRST_FILE_FD ? ESPIPE : EBADF);
	}

	FSIZE_t nBase = 0;
	switch (nWhence)
	{
	case SEEK_SET:	nBase = 0;		break;
	case SEEK_CUR:	nBase = f_tell(pFile);	break;
	case SEEK_END:	nBase = f_size(pFile);	break;
	default:	return SetErrno(EINVAL);
	}

	if (nOffset < 0 && (FSIZE_t)-nOffset > nBase)
	{
		return SetErrno(EINVAL);
	}

	const FRESULT Result = f_lseek(pFile, nBase + nOffset);
	return Result == FR_OK ? (_off_t)f_tell(pFile) : SetErrno(ErrnoFromResult(Result));
}

int _fstat(int fd, struct stat *pStat)
{
	memset(pStat, 0, sizeof *pStat);
	if (fd >= 0 && fd < FIRST_FILE_FD)
	{
		pStat->st_mode = S_IFCHR;
		return 0;
	}

	FIL *pFile = FileFromFd(fd);
	if (!pFile)
	{
		return SetErrno(EBADF);
	}

	pStat->st_mode = S_IFREG | 0666;
	pStat->st_size = (off_t)f_size(pFile);
	pStat->st_blksize = 512;
	return 0;
}

int _stat(const char *pPath, struct stat *pStat)
{
	memset(pStat, 0, sizeof *pStat);

	char Path[320];
	VolumePath(Path, sizeof Path, pPath ? pPath : "");
	if (IsRoot(Path))
	{
		pStat->st_mode = S_IFDIR | 0777;
		return 0;
	}

	FILINFO Info;
	const FRESULT Result = f_stat(Path, &Info);
	if (Result != FR_OK)
	{
		return SetErrno(ErrnoFromResult(Result));
	}

	pStat->st_mode = (Info.fattrib & AM_DIR) ? (S_IFDIR | 0777) : (S_IFREG | 0666);
	pStat->st_size = (off_t)Info.fsize;
	pStat->st_blksize = 512;
	return 0;
}

int _isatty(int fd)
{
	return fd >= 0 && fd < FIRST_FILE_FD;
}

int _unlink(const char *pPath)
{
	char Path[320];
	VolumePath(Path, sizeof Path, pPath);
	const FRESULT Result = f_unlink(Path);
	return Result == FR_OK ? 0 : SetErrno(ErrnoFromResult(Result));
}

int _link(const char *, const char *)
{
	return SetErrno(EMLINK);
}

// newlib's rename() would fall back to _link() + _unlink().
int rename(const char *pOld, const char *pNew)
{
	char OldPath[320];
	char NewPath[320];
	VolumePath(OldPath, sizeof OldPath, pOld);
	VolumePath(NewPath, sizeof NewPath, pNew);
	f_unlink(NewPath);
	const FRESULT Result = f_rename(OldPath, NewPath);
	return Result == FR_OK ? 0 : SetErrno(ErrnoFromResult(Result));
}

int mkdir(const char *pPath, mode_t)
{
	char Path[320];
	VolumePath(Path, sizeof Path, pPath);
	const FRESULT Result = f_mkdir(Path);
	return Result == FR_OK ? 0 : SetErrno(ErrnoFromResult(Result));
}

int rmdir(const char *pPath)
{
	return _unlink(pPath);
}

int _getpid(void)
{
	return 1;
}

int _kill(int, int)
{
	return SetErrno(EINVAL);
}

void _exit(int nStatus)
{
	char Message[32];
	snprintf(Message, sizeof Message, "exit(%d) called", nStatus);
	ra_libc_panic(Message);
}

int _gettimeofday(struct timeval *pTime, void *)
{
	if (pTime)
	{
		pTime->tv_sec = (time_t)ra_libc_unix_time();
		pTime->tv_usec = (suseconds_t)(ra_libc_clock_usec() % 1000000);
	}
	return 0;
}

clock_t _times(struct tms *pTimes)
{
	const clock_t nTicks = (clock_t)(ra_libc_clock_usec() / (1000000 / CLOCKS_PER_SEC));
	if (pTimes)
	{
		pTimes->tms_utime = nTicks;
		pTimes->tms_stime = 0;
		pTimes->tms_cutime = 0;
		pTimes->tms_cstime = 0;
	}
	return nTicks;
}

#if defined(__aarch64__)
// AArch64 dynarecs (gpSP) call the kernel's code from their code buffer with
// BL, which reaches +/-128 MB. The heap starts behind the kernel's 256 MB
// region, too far away, so the executable mapping comes from this block in
// .bss right behind the kernel code instead (one at a time: gpSP's cache).
static const size_t JIT_ARENA_SIZE = 12 * 1024 * 1024;
static uint8_t s_JitArena[JIT_ARENA_SIZE] __attribute__((aligned(65536)));
static bool s_JitArenaUsed;
#endif

void *mmap(void *, size_t nLength, int nProt, int nFlags, int fd, off_t)
{
	if (!(nFlags & MAP_ANONYMOUS) || fd != -1 || (nFlags & MAP_FIXED))
	{
		errno = ENOTSUP;
		return MAP_FAILED;
	}

#if defined(__aarch64__)
	if ((nProt & PROT_EXEC) && !s_JitArenaUsed && nLength <= JIT_ARENA_SIZE
	    && ra_libc_make_executable(s_JitArena, nLength) == 0)
	{
		s_JitArenaUsed = true;
		memset(s_JitArena, 0, nLength);
		char Message[96];
		snprintf(Message, sizeof Message, "mmap: %u KB executable at %p (code area)",
			 (unsigned)(nLength / 1024), (void *)s_JitArena);
		ra_libc_log(Message);
		return s_JitArena;
	}
#endif

	void *pBlock = AlignedAllocate(4096, nLength);
	if (!pBlock)
	{
		errno = ENOMEM;
		return MAP_FAILED;
	}
	memset(pBlock, 0, nLength);

	if (nProt & PROT_EXEC)
	{
		const bool bExecutable = ra_libc_make_executable(pBlock, nLength) == 0;
		char Message[96];
		snprintf(Message, sizeof Message, "mmap: %u KB executable at %p: %s",
			 (unsigned)(nLength / 1024), pBlock, bExecutable ? "ok" : "failed");
		ra_libc_log(Message);
		if (!bExecutable)
		{
			free(pBlock);
			errno = ENOTSUP;
			return MAP_FAILED;
		}
	}
	return pBlock;
}

int munmap(void *pBlock, size_t)
{
#if defined(__aarch64__)
	if (pBlock == s_JitArena)
	{
		s_JitArenaUsed = false;
		return 0;
	}
#endif
	free(pBlock);
	return 0;
}

int mprotect(void *pBlock, size_t nLength, int nProt)
{
	if ((nProt & PROT_EXEC) && ra_libc_make_executable(pBlock, nLength) != 0)
	{
		return SetErrno(ENOTSUP);
	}
	return 0;
}

int madvise(void *, size_t, int)
{
	return 0;
}

DIR *opendir(const char *pName)
{
	char Path[320];
	VolumePath(Path, sizeof Path, pName ? pName : "");

	DIR *pDir = (DIR *)malloc(sizeof(DIR));
	if (!pDir)
	{
		errno = ENOMEM;
		return 0;
	}

	const FRESULT Result = f_opendir(&pDir->dir, Path);
	if (Result != FR_OK)
	{
		free(pDir);
		errno = ErrnoFromResult(Result);
		return 0;
	}
	return pDir;
}

struct dirent *readdir(DIR *pDir)
{
	if (!pDir)
	{
		errno = EBADF;
		return 0;
	}

	FILINFO Info;
	if (f_readdir(&pDir->dir, &Info) != FR_OK || Info.fname[0] == 0)
	{
		return 0;
	}

	strncpy(pDir->entry.d_name, Info.fname, sizeof pDir->entry.d_name - 1);
	pDir->entry.d_name[sizeof pDir->entry.d_name - 1] = 0;
	pDir->entry.d_type = (Info.fattrib & AM_DIR) ? DT_DIR : DT_REG;
	pDir->entry.d_ino = 0;
	return &pDir->entry;
}

void rewinddir(DIR *pDir)
{
	if (pDir)
	{
		f_readdir(&pDir->dir, 0);
	}
}

int closedir(DIR *pDir)
{
	if (!pDir)
	{
		return SetErrno(EBADF);
	}
	f_closedir(&pDir->dir);
	free(pDir);
	return 0;
}

int chdir(const char *pPath)
{
	if (!pPath)
	{
		return SetErrno(EFAULT);
	}
	char Path[sizeof s_Cwd];
	ResolvePath(Path, sizeof Path, pPath[0] == '/' ? "" : s_Cwd, pPath);
	if (Path[0])
	{
		char Full[320];
		VolumePath(Full, sizeof Full, "/");
		strncat(Full, Path, sizeof Full - strlen(Full) - 1);
		FILINFO Info;
		if (f_stat(Full, &Info) != FR_OK)
		{
			return SetErrno(ENOENT);
		}
		if (!(Info.fattrib & AM_DIR))
		{
			return SetErrno(ENOTDIR);
		}
	}
	strcpy(s_Cwd, Path);
	return 0;
}

// The absolute form of a path ("/" + path from the root); like FatFs, no
// links to resolve. The path need not exist.
char *realpath(const char *pPath, char *pResolved)
{
	if (!pPath)
	{
		errno = EINVAL;
		return 0;
	}
	char Path[256];
	ResolvePath(Path, sizeof Path, pPath[0] == '/' ? "" : s_Cwd, pPath);
	if (!pResolved)
	{
		pResolved = (char *)malloc(strlen(Path) + 2);
		if (!pResolved)
		{
			errno = ENOMEM;
			return 0;
		}
	}
	pResolved[0] = '/';
	strcpy(pResolved + 1, Path);
	return pResolved;
}

char *getcwd(char *pBuffer, size_t nSize)
{
	const size_t nLength = strlen(s_Cwd) + 2;	// "/" and NUL
	if (!pBuffer)
	{
		nSize = nSize > nLength ? nSize : nLength;
		pBuffer = (char *)malloc(nSize);
		if (!pBuffer)
		{
			errno = ENOMEM;
			return 0;
		}
	}
	if (nSize < nLength)
	{
		errno = ERANGE;
		return 0;
	}
	pBuffer[0] = '/';
	strcpy(pBuffer + 1, s_Cwd);
	return pBuffer;
}

int scandir(const char *pPath, struct dirent ***pppList,
	    int (*pFilter)(const struct dirent *),
	    int (*pCompare)(const struct dirent **, const struct dirent **))
{
	DIR *pDir = opendir(pPath);
	if (!pDir)
	{
		return -1;
	}

	struct dirent **ppList = 0;
	size_t nCount = 0;
	size_t nCapacity = 0;
	struct dirent *pEntry;
	while ((pEntry = readdir(pDir)) != 0)
	{
		if (pFilter && !pFilter(pEntry))
		{
			continue;
		}
		if (nCount == nCapacity)
		{
			nCapacity = nCapacity ? nCapacity * 2 : 32;
			struct dirent **ppNew = (struct dirent **)realloc(ppList, nCapacity * sizeof *ppList);
			if (!ppNew)
			{
				break;
			}
			ppList = ppNew;
		}
		struct dirent *pCopy = (struct dirent *)malloc(sizeof *pCopy);
		if (!pCopy)
		{
			break;
		}
		*pCopy = *pEntry;
		ppList[nCount++] = pCopy;
	}
	closedir(pDir);

	if (pCompare && nCount > 1)
	{
		qsort(ppList, nCount, sizeof *ppList,
		      (int (*)(const void *, const void *))pCompare);
	}
	*pppList = ppList;
	return (int)nCount;
}

int alphasort(const struct dirent **ppA, const struct dirent **ppB)
{
	return strcmp((*ppA)->d_name, (*ppB)->d_name);
}

}
