#include "circle_platform.h"

#include <string.h>

static const unsigned FS_ERROR_VALUE = 0xFFFFFFFFU;

static bool CharEqualNoCase(char a, char b)
{
	if (a >= 'A' && a <= 'Z')
	{
		a = (char)(a - 'A' + 'a');
	}
	if (b >= 'A' && b <= 'Z')
	{
		b = (char)(b - 'A' + 'a');
	}

	return a == b;
}

static bool HasExtension(const char *path, const char *extension)
{
	if (!path || !extension)
	{
		return false;
	}

	const char *dot = 0;
	for (const char *p = path; *p; p++)
	{
		if (*p == '.')
		{
			dot = p;
		}
	}

	if (!dot)
	{
		return false;
	}

	dot++;
	while (*dot && *extension)
	{
		if (!CharEqualNoCase(*dot++, *extension++))
		{
			return false;
		}
	}

	return *dot == 0 && *extension == 0;
}

static void CopyPath(char *dst, size_t dstSize, const char *src)
{
	if (!dst || dstSize == 0)
	{
		return;
	}

	size_t i = 0;
	if (src)
	{
		for (; src[i] && i + 1 < dstSize; i++)
		{
			dst[i] = src[i];
		}
	}
	dst[i] = 0;
}

#ifndef RA_BAREMETAL_FATFS

CircleFs::CircleFs(void)
:	m_pFileSystem(0),
	m_pLog(0)
{
}

void CircleFs::Init(CFATFileSystem *pFileSystem, CircleLog *pLog)
{
	m_pFileSystem = pFileSystem;
	m_pLog = pLog;
}

bool CircleFs::ListDirectory(const char *path, Entry *entries, unsigned maxEntries, unsigned *pCount,
			     TEntryFilter pFilter)
{
	if (pCount)
	{
		*pCount = 0;
	}

	if (!m_pFileSystem || !entries || maxEntries == 0 || !pCount)
	{
		return false;
	}

	TDirentry entry;
	TFindCurrentEntry current;
	memset(&entry, 0, sizeof(entry));
	memset(&current, 0, sizeof(current));

	unsigned found = m_pFileSystem->DirectoryFindFirst(path ? path : "", &entry, &current);
	while (found)
	{
		const bool isDirectory = (entry.nAttributes & FS_ATTRIB_DIRECTORY) != 0;
		if (strcmp(entry.chTitle, ".") != 0 && strcmp(entry.chTitle, "..") != 0
		    && (!pFilter || pFilter(entry.chTitle, isDirectory)))
		{
			if (*pCount < maxEntries)
			{
				Entry *pOut = &entries[*pCount];
				CopyPath(pOut->name, sizeof(pOut->name), entry.chTitle);
				pOut->size = entry.nSize;
				pOut->isDirectory = (entry.nAttributes & FS_ATTRIB_DIRECTORY) != 0;
				(*pCount)++;
			}
		}

		memset(&entry, 0, sizeof(entry));
		found = m_pFileSystem->DirectoryFindNext(&entry, &current);
	}

	return true;
}

bool CircleFs::ReadWholeFile(const char *path, uint8_t **ppData, size_t *pSize, size_t maxSize)
{
	if (ppData)
	{
		*ppData = 0;
	}

	if (pSize)
	{
		*pSize = 0;
	}

	if (!m_pFileSystem || !path || !ppData || !pSize || maxSize == 0)
	{
		return false;
	}

	if (m_pLog)
	{
		char message[64];
		CopyPath(message, sizeof(message), "Trying ROM: ");
		const size_t prefixLen = strlen(message);
		CopyPath(message + prefixLen, sizeof(message) - prefixLen, path);
		m_pLog->Notice(message);
	}

	unsigned hFile = m_pFileSystem->FileOpen(path);
	if (hFile == 0)
	{
		if (m_pLog)
		{
			char message[64];
			CopyPath(message, sizeof(message), "ROM open failed: ");
			const size_t prefixLen = strlen(message);
			CopyPath(message + prefixLen, sizeof(message) - prefixLen, path);
			m_pLog->Warn(message);
		}
		return false;
	}

	uint8_t *pData = new uint8_t[maxSize];
	if (!pData)
	{
		m_pFileSystem->FileClose(hFile);
		return false;
	}

	size_t used = 0;
	while (used < maxSize)
	{
		const unsigned toRead = (maxSize - used) > 4096 ? 4096 : (unsigned)(maxSize - used);
		const unsigned result = m_pFileSystem->FileRead(hFile, pData + used, toRead);
		if (result == 0)
		{
			break;
		}

		if (result == FS_ERROR_VALUE)
		{
			delete[] pData;
			m_pFileSystem->FileClose(hFile);
			return false;
		}

		used += result;
	}

	m_pFileSystem->FileClose(hFile);

	*ppData = pData;
	*pSize = used;

	if (m_pLog)
	{
		char message[64];
		CopyPath(message, sizeof(message), "loaded: ");
		const size_t prefixLen = strlen(message);
		CopyPath(message + prefixLen, sizeof(message) - prefixLen, path);
		m_pLog->Notice(message);
	}
	return true;
}

bool CircleFs::FindFirstWithExtension(const char *extension, char *path, size_t pathSize)
{
	if (path && pathSize)
	{
		path[0] = 0;
	}

	if (!m_pFileSystem || !extension || !path || pathSize == 0)
	{
		return false;
	}

	TDirentry entry;
	TFindCurrentEntry current;
	memset(&entry, 0, sizeof(entry));
	memset(&current, 0, sizeof(current));

	unsigned found = m_pFileSystem->RootFindFirst(&entry, &current);
	while (found)
	{
		if (m_pLog)
		{
			char message[64];
			CopyPath(message, sizeof(message), "SD root entry: ");
			const size_t prefixLen = strlen(message);
			CopyPath(message + prefixLen, sizeof(message) - prefixLen, entry.chTitle);
			m_pLog->Notice(message);
		}

		if (HasExtension(entry.chTitle, extension))
		{
			CopyPath(path, pathSize, entry.chTitle);
			return true;
		}

		memset(&entry, 0, sizeof(entry));
		found = m_pFileSystem->RootFindNext(&entry, &current);
	}

	return false;
}

bool CircleFs::FileExists(const char *path)
{
	if (!m_pFileSystem || !path)
	{
		return false;
	}

	const unsigned hFile = m_pFileSystem->FileOpen(path);
	if (hFile == 0)
	{
		return false;
	}
	m_pFileSystem->FileClose(hFile);
	return true;
}

bool CircleFs::WriteWholeFile(const char *path, const void *pData, size_t size)
{
	if (!m_pFileSystem || !path || (!pData && size))
	{
		return false;
	}

	unsigned hFile = m_pFileSystem->FileCreate(path);
	if (hFile == 0)
	{
		return false;
	}

	size_t written = 0;
	while (written < size)
	{
		const unsigned chunk = (size - written) > 4096 ? 4096 : (unsigned)(size - written);
		const unsigned result = m_pFileSystem->FileWrite(hFile, (const uint8_t *)pData + written, chunk);
		if (result == FS_ERROR_VALUE || result == 0)
		{
			m_pFileSystem->FileClose(hFile);
			return false;
		}

		written += result;
	}

	return m_pFileSystem->FileClose(hFile) != 0;
}

#else // RA_BAREMETAL_FATFS

static const char FATFS_VOLUME[] = "SD:";

// "GAMES/NES/A.NES" -> "SD:/GAMES/NES/A.NES"
static void VolumePath(char *dst, size_t dstSize, const char *path)
{
	CopyPath(dst, dstSize, FATFS_VOLUME);
	size_t len = strlen(dst);
	if (len + 1 < dstSize)
	{
		dst[len++] = '/';
		dst[len] = 0;
	}

	if (path)
	{
		while (*path == '/' || *path == '\\')
		{
			path++;
		}
		CopyPath(dst + len, dstSize - len, path);
	}
}

// Hidden/system entries and macOS "._" resource forks are not ROMs.
static bool IsListable(const FILINFO &info)
{
	return (info.fattrib & (AM_HID | AM_SYS)) == 0
		&& !(info.fname[0] == '.' && info.fname[1] == '_');
}

// Names that do not fit an Entry fall back to the 8.3 alias, which FatFs
// opens just the same.
static const char *EntryName(const FILINFO &info, size_t maxLen)
{
	return strlen(info.fname) < maxLen || info.altname[0] == 0 ? info.fname : info.altname;
}

CircleFs::CircleFs(void)
:	m_Mounted(false),
	m_pLog(0)
{
	memset(&m_FatFs, 0, sizeof(m_FatFs));
}

bool CircleFs::Mount(CircleLog *pLog)
{
	m_pLog = pLog;
	m_Mounted = f_mount(&m_FatFs, FATFS_VOLUME, 1) == FR_OK;
	if (!m_Mounted && m_pLog)
	{
		m_pLog->Error("FatFs mount of SD: failed");
	}
	return m_Mounted;
}

bool CircleFs::ListDirectory(const char *path, Entry *entries, unsigned maxEntries, unsigned *pCount,
			     TEntryFilter pFilter)
{
	if (pCount)
	{
		*pCount = 0;
	}

	if (!m_Mounted || !entries || maxEntries == 0 || !pCount)
	{
		return false;
	}

	char fullPath[320];
	VolumePath(fullPath, sizeof(fullPath), path);

	DIR dir;
	if (f_opendir(&dir, fullPath) != FR_OK)
	{
		return false;
	}

	FILINFO info;
	while (*pCount < maxEntries && f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0)
	{
		if (!IsListable(info))
		{
			continue;
		}
		const char *pName = EntryName(info, sizeof(entries[0].name));
		if (pFilter && !pFilter(pName, (info.fattrib & AM_DIR) != 0))
		{
			continue;
		}

		Entry *pOut = &entries[*pCount];
		CopyPath(pOut->name, sizeof(pOut->name), EntryName(info, sizeof(pOut->name)));
		pOut->size = (unsigned)info.fsize;
		pOut->isDirectory = (info.fattrib & AM_DIR) != 0;
		(*pCount)++;
	}

	f_closedir(&dir);
	return true;
}

bool CircleFs::ReadWholeFile(const char *path, uint8_t **ppData, size_t *pSize, size_t maxSize)
{
	if (ppData)
	{
		*ppData = 0;
	}

	if (pSize)
	{
		*pSize = 0;
	}

	if (!m_Mounted || !path || !ppData || !pSize || maxSize == 0)
	{
		return false;
	}

	char fullPath[320];
	VolumePath(fullPath, sizeof(fullPath), path);

	FIL file;
	if (f_open(&file, fullPath, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		if (m_pLog)
		{
			char message[160];
			CopyPath(message, sizeof(message), "open failed: ");
			const size_t prefixLen = strlen(message);
			CopyPath(message + prefixLen, sizeof(message) - prefixLen, fullPath);
			m_pLog->Warn(message);
		}
		return false;
	}

	const FSIZE_t fileSize = f_size(&file);
	if (fileSize == 0 || fileSize > maxSize)
	{
		if (m_pLog)
		{
			m_pLog->Warn(fileSize ? "file larger than allowed" : "file is empty");
		}
		f_close(&file);
		return false;
	}

	uint8_t *pData = new uint8_t[(size_t)fileSize];
	if (!pData)
	{
		f_close(&file);
		return false;
	}

	UINT bytesRead = 0;
	const FRESULT result = f_read(&file, pData, (UINT)fileSize, &bytesRead);
	f_close(&file);
	if (result != FR_OK || bytesRead != fileSize)
	{
		delete[] pData;
		if (m_pLog)
		{
			m_pLog->Warn("file read failed");
		}
		return false;
	}

	*ppData = pData;
	*pSize = bytesRead;

	if (m_pLog)
	{
		char message[96];
		CopyPath(message, sizeof(message), "loaded: ");
		const size_t prefixLen = strlen(message);
		CopyPath(message + prefixLen, sizeof(message) - prefixLen, path);
		m_pLog->Notice(message);
	}
	return true;
}

bool CircleFs::FindFirstWithExtension(const char *extension, char *path, size_t pathSize)
{
	if (path && pathSize)
	{
		path[0] = 0;
	}

	if (!m_Mounted || !extension || !path || pathSize == 0)
	{
		return false;
	}

	char rootPath[8];
	VolumePath(rootPath, sizeof(rootPath), "");

	DIR dir;
	if (f_opendir(&dir, rootPath) != FR_OK)
	{
		return false;
	}

	bool found = false;
	FILINFO info;
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0] != 0)
	{
		if (IsListable(info) && !(info.fattrib & AM_DIR) && HasExtension(info.fname, extension))
		{
			CopyPath(path, pathSize, EntryName(info, pathSize));
			found = true;
			break;
		}
	}

	f_closedir(&dir);
	return found;
}

bool CircleFs::FileExists(const char *path)
{
	if (!m_Mounted || !path)
	{
		return false;
	}

	char fullPath[320];
	VolumePath(fullPath, sizeof(fullPath), path);
	FILINFO info;
	return f_stat(fullPath, &info) == FR_OK;
}

bool CircleFs::WriteWholeFile(const char *path, const void *pData, size_t size)
{
	if (!m_Mounted || !path || (!pData && size))
	{
		return false;
	}

	char fullPath[320];
	VolumePath(fullPath, sizeof(fullPath), path);
	char tempPath[330];
	CopyPath(tempPath, sizeof(tempPath), fullPath);
	const size_t length = strlen(tempPath);
	CopyPath(tempPath + length, sizeof(tempPath) - length, ".tmp");

	// A power loss (the GPi Case runs on batteries) must not leave a half
	// written file behind, e.g. a game save: write a new file and replace
	// the old one only when it is complete.
	FIL file;
	if (f_open(&file, tempPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return false;
	}

	UINT written = 0;
	const FRESULT result = f_write(&file, pData, (UINT)size, &written);
	const FRESULT closeResult = f_close(&file);
	if (result != FR_OK || closeResult != FR_OK || written != size)
	{
		f_unlink(tempPath);
		return false;
	}

	const FRESULT unlinkResult = f_unlink(fullPath);
	if (unlinkResult != FR_OK && unlinkResult != FR_NO_FILE)
	{
		return false;
	}
	return f_rename(tempPath, fullPath) == FR_OK;
}

#endif // RA_BAREMETAL_FATFS
