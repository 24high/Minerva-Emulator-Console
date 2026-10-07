#include "rom_browser.h"
#include "tile_view.h"

#include "libretro/libretro_runner.h"
#include "platform/circle/circle_platform.h"

#include <circle/2dgraphics.h>
#include <circle/bcmframebuffer.h>
#include <circle/screen.h>
#include <circle/timer.h>
#include <libretro.h>
#include <string.h>

// Games and folders per folder (other files do not count, see LoadDirectory).
static const unsigned MAX_BROWSER_ENTRIES = 512;

static const char BROWSER_TITLE[] = "MINERVA CONSOLE";

// Tile pictures: <rom name>.png or <rom file name>.png next to the game.
static const size_t MAX_PICTURE_FILE_SIZE = 4 * 1024 * 1024;

struct BrowserEntry
{
	CircleFs::Entry entry;
	char path[256];
	const LibretroCore *pCore;
	const char *system;
};

static void CopyString(char *dst, size_t dstSize, const char *src)
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

static int CompareNoCase(const char *a, const char *b)
{
	while (*a || *b)
	{
		char ca = *a++;
		char cb = *b++;
		if (ca >= 'A' && ca <= 'Z')
		{
			ca = (char)(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z')
		{
			cb = (char)(cb - 'A' + 'a');
		}
		if (ca != cb)
		{
			return (int)(unsigned char)ca - (int)(unsigned char)cb;
		}
	}

	return 0;
}

static void JoinPath(char *dst, size_t dstSize, const char *directory, const char *name)
{
	if (!directory || directory[0] == 0)
	{
		CopyString(dst, dstSize, name);
		return;
	}

	CopyString(dst, dstSize, directory);
	const size_t len = strlen(dst);
	if (len + 1 < dstSize)
	{
		dst[len] = '/';
		dst[len + 1] = 0;
	}
	if (len + 1 < dstSize)
	{
		CopyString(dst + len + 1, dstSize - len - 1, name);
	}
}

static void ParentPath(char *path)
{
	if (!path || path[0] == 0)
	{
		return;
	}

	char *lastSlash = 0;
	for (char *p = path; *p; p++)
	{
		if (*p == '/' || *p == '\\')
		{
			lastSlash = p;
		}
	}

	if (lastSlash)
	{
		*lastSlash = 0;
	}
	else
	{
		path[0] = 0;
	}
}

static bool IsParentAvailable(const char *path)
{
	return path && path[0] != 0;
}

static void SortEntries(BrowserEntry *entries, unsigned count)
{
	for (unsigned i = 1; i < count; i++)
	{
		BrowserEntry value = entries[i];
		unsigned j = i;
		while (j > 0)
		{
			const bool valueDir = value.entry.isDirectory;
			const bool prevDir = entries[j - 1].entry.isDirectory;
			const bool after = (valueDir == prevDir)
				? CompareNoCase(entries[j - 1].entry.name, value.entry.name) > 0
				: (!prevDir && valueDir);
			if (!after)
			{
				break;
			}
			entries[j] = entries[j - 1];
			j--;
		}
		entries[j] = value;
	}
}

static unsigned LoadDirectory(CircleFs *pFs, const char *directory, BrowserEntry *entries, unsigned maxEntries)
{
	static CircleFs::Entry rawEntries[MAX_BROWSER_ENTRIES];
	unsigned rawCount = 0;
	unsigned count = 0;

	// Only folders and games are listed: pictures, saves, logs and the boot
	// files must not take the places of games.
	const CircleFs::TEntryFilter isGameOrFolder = [](const char *name, bool isDirectory)
	{
		return isDirectory || LibretroFindCoreForPath(name) != 0;
	};
	if (!pFs || !pFs->ListDirectory(directory, rawEntries, MAX_BROWSER_ENTRIES, &rawCount, isGameOrFolder))
	{
		return 0;
	}

	for (unsigned i = 0; i < rawCount && count < maxEntries; i++)
	{
		char fullPath[256];
		JoinPath(fullPath, sizeof(fullPath), directory, rawEntries[i].name);

		const LibretroCore *pCore = rawEntries[i].isDirectory ? 0 : LibretroFindCoreForPath(fullPath);
		if (!rawEntries[i].isDirectory && !pCore)
		{
			continue;
		}

		entries[count].entry = rawEntries[i];
		CopyString(entries[count].path, sizeof(entries[count].path), fullPath);
		entries[count].pCore = pCore;
		entries[count].system = pCore ? LibretroSystemForPath(fullPath) : "";
		count++;
	}

	SortEntries(entries, count);
	return count;
}

static void ScreenWrite(CScreenDevice *pScreen, const char *text)
{
	if (pScreen && text)
	{
		pScreen->Write(text, strlen(text));
	}
}

static void ScreenWriteLine(CScreenDevice *pScreen, const char *text)
{
	ScreenWrite(pScreen, text);
	ScreenWrite(pScreen, "\n");
}

static void DrawTextBrowser(CScreenDevice *pScreen,
                            const char *directory,
                            const BrowserEntry *entries,
                            unsigned count,
                            unsigned selected)
{
	if (!pScreen)
	{
		return;
	}

	ScreenWrite(pScreen, "\x1b[2J\x1b[H");
	ScreenWriteLine(pScreen, "MINERVA CONSOLE - ROM selection");
	ScreenWrite(pScreen, "Path: /");
	ScreenWriteLine(pScreen, directory && directory[0] ? directory : "");
	ScreenWriteLine(pScreen, "A: start/open  B: back  D-pad: navigate");
	ScreenWriteLine(pScreen, "");

	unsigned rows = pScreen->GetRows();
	unsigned visibleRows = rows > 7 ? rows - 6 : 18;
	if (visibleRows > 24)
	{
		visibleRows = 24;
	}

	unsigned first = 0;
	if (selected >= visibleRows)
	{
		first = selected - visibleRows + 1;
	}

	if (count == 0)
	{
		ScreenWriteLine(pScreen, "  No supported ROMs or folders found.");
	}
	else
	{
		for (unsigned row = 0; row < visibleRows && first + row < count; row++)
		{
			const BrowserEntry *entry = &entries[first + row];
			ScreenWrite(pScreen, first + row == selected ? "> " : "  ");
			ScreenWrite(pScreen, entry->entry.isDirectory ? "[" : " ");
			ScreenWrite(pScreen, entry->entry.name);
			ScreenWrite(pScreen, entry->entry.isDirectory ? "]" : " ");
			if (!entry->entry.isDirectory && entry->pCore)
			{
				ScreenWrite(pScreen, "  ");
				ScreenWrite(pScreen, entry->pCore->name);
			}
			ScreenWriteLine(pScreen, "");
		}
	}

	pScreen->Update();
}

// --- Tile pictures ------------------------------------------------------------
//
// Loaded one at a time while the browser waits for input, visible tiles first,
// and kept for the folder (also while a game runs), so the browser is back at
// once after a game.

enum TPictureState
{
	PictureUnknown,
	PictureCover,		// <rom name>.png from the SD card
	PictureSystem,		// placeholder of the system
	PictureNone		// folders and systems without a picture
};

struct SystemPicture
{
	const char *system;
	uint16_t *pPixels;
};

// Identifies the game a cached picture belongs to.
static uint32_t PathHash(const char *pPath)
{
	uint32_t hash = 2166136261U;	// FNV-1a
	for (; *pPath; pPath++)
	{
		hash = (hash ^ (uint8_t)*pPath) * 16777619U;
	}
	return hash;
}

class TilePictures
{
public:
	TilePictures(void)
	:	m_TileSize(0),
		m_nSystemPictures(0)
	{
		memset(m_State, 0, sizeof m_State);
		memset(m_pCover, 0, sizeof m_pCover);
		memset(m_Hash, 0, sizeof m_Hash);
		m_Directory[0] = 0;
	}

	// Keeps the pictures if folder and tile size are the same as before.
	void Use(const char *pDirectory, unsigned tileSize)
	{
		if (tileSize != m_TileSize)
		{
			for (unsigned i = 0; i < m_nSystemPictures; i++)
			{
				delete[] m_SystemPictures[i].pPixels;
			}
			m_nSystemPictures = 0;
			m_TileSize = tileSize;
			m_Directory[0] = 1;	// no folder name: forces Reset() below
		}
		if (strcmp(pDirectory, m_Directory) != 0)
		{
			Reset();
			CopyString(m_Directory, sizeof m_Directory, pDirectory);
		}
	}

	const uint16_t *Get(unsigned index, const BrowserEntry &entry)
	{
		if (!IsLoaded(index, entry))
		{
			return 0;
		}
		switch (m_State[index])
		{
		case PictureCover:	return m_pCover[index];
		case PictureSystem:	return System(entry.system);
		default:		return 0;
		}
	}

	// Pictures are kept by position in the list; a position that holds
	// another game now (the folder changed) counts as not loaded.
	bool IsLoaded(unsigned index, const BrowserEntry &entry) const
	{
		return m_State[index] != PictureUnknown && m_Hash[index] == PathHash(entry.path);
	}

	void Load(unsigned index, const BrowserEntry &entry, CircleFs *pFs)
	{
		delete[] m_pCover[index];
		m_pCover[index] = 0;
		m_Hash[index] = PathHash(entry.path);
		m_State[index] = PictureNone;
		if (entry.entry.isDirectory)
		{
			return;
		}

		// "GBA/Pokemon.gba" -> "GBA/Pokemon.png", then "GBA/Pokemon.gba.png"
		char candidates[2][270];
		CopyString(candidates[0], sizeof candidates[0], entry.path);
		char *pDot = 0;
		for (char *p = candidates[0]; *p; p++)
		{
			if (*p == '.')
			{
				pDot = p;
			}
			else if (*p == '/')
			{
				pDot = 0;
			}
		}
		if (pDot)
		{
			*pDot = 0;
		}
		CopyString(candidates[1], sizeof candidates[1], entry.path);
		for (unsigned i = 0; i < 2 && pFs; i++)
		{
			const size_t length = strlen(candidates[i]);
			CopyString(candidates[i] + length, sizeof candidates[i] - length, ".png");
			uint8_t *pData = 0;
			size_t size = 0;
			if (!pFs->FileExists(candidates[i])
			    || !pFs->ReadWholeFile(candidates[i], &pData, &size, MAX_PICTURE_FILE_SIZE))
			{
				continue;
			}
			uint16_t *pTile = new uint16_t[m_TileSize * m_TileSize];
			const bool ok = pTile && MakeTilePictureFromPng(pData, size, m_TileSize, pTile);
			delete[] pData;
			if (ok)
			{
				m_pCover[index] = pTile;
				m_State[index] = PictureCover;
				return;
			}
			delete[] pTile;
		}

		if (System(entry.system))
		{
			m_State[index] = PictureSystem;
		}
	}

private:
	void Reset(void)
	{
		for (unsigned i = 0; i < MAX_BROWSER_ENTRIES; i++)
		{
			delete[] m_pCover[i];
			m_pCover[i] = 0;
			m_State[i] = PictureUnknown;
		}
	}

	// The placeholder of a system, scaled to the tile size once.
	const uint16_t *System(const char *pSystem)
	{
		for (unsigned i = 0; i < m_nSystemPictures; i++)
		{
			if (strcmp(m_SystemPictures[i].system, pSystem) == 0)
			{
				return m_SystemPictures[i].pPixels;
			}
		}
		const TSystemImage *pImage = FindSystemImage(pSystem);
		if (!pImage || m_nSystemPictures >= MAX_SYSTEM_PICTURES)
		{
			return 0;
		}
		uint16_t *pPixels = new uint16_t[m_TileSize * m_TileSize];
		if (!pPixels)
		{
			return 0;
		}
		MakeTilePictureFromRgb565(pImage->pixels, pImage->width, pImage->height, m_TileSize, pPixels);
		m_SystemPictures[m_nSystemPictures].system = pImage->system;
		m_SystemPictures[m_nSystemPictures].pPixels = pPixels;
		m_nSystemPictures++;
		return pPixels;
	}

	static const unsigned MAX_SYSTEM_PICTURES = 16;

	unsigned m_TileSize;
	char m_Directory[256];
	uint8_t m_State[MAX_BROWSER_ENTRIES];
	uint32_t m_Hash[MAX_BROWSER_ENTRIES];
	uint16_t *m_pCover[MAX_BROWSER_ENTRIES];
	SystemPicture m_SystemPictures[MAX_SYSTEM_PICTURES];
	unsigned m_nSystemPictures;
};

static TilePictures s_Pictures;

// Loads the next missing picture: visible tiles first, then the others.
// Returns true if a visible tile changed.
static bool LoadNextPicture(const BrowserEntry *entries, unsigned count, unsigned firstVisible,
			    unsigned visibleCount, CircleFs *pFs)
{
	for (unsigned i = firstVisible; i < firstVisible + visibleCount && i < count; i++)
	{
		if (!s_Pictures.IsLoaded(i, entries[i]))
		{
			s_Pictures.Load(i, entries[i], pFs);
			return true;
		}
	}
	for (unsigned i = 0; i < count; i++)
	{
		if (!s_Pictures.IsLoaded(i, entries[i]))
		{
			s_Pictures.Load(i, entries[i], pFs);
			return false;
		}
	}
	return false;
}

static TileEntry TileFor(unsigned index, const BrowserEntry &entry)
{
	TileEntry tile;
	tile.name = entry.entry.name;
	tile.system = entry.system;
	tile.isDirectory = entry.entry.isDirectory;
	tile.pPicture = s_Pictures.Get(index, entry);
	return tile;
}

static void DrawTiles(C2DGraphics *pGraphics, const TileLayout &layout, const char *directory,
		      const BrowserEntry *entries, unsigned count, unsigned selected, unsigned firstRow)
{
	static TileEntry tiles[MAX_BROWSER_ENTRIES];
	for (unsigned i = 0; i < count; i++)
	{
		tiles[i] = TileFor(i, entries[i]);
	}
	const TileSurface surface = {
		static_cast<uint16_t *>(pGraphics->GetBuffer()),
		pGraphics->GetWidth(), pGraphics->GetHeight(), pGraphics->GetWidth()
	};
	DrawTileBrowser(surface, layout, BROWSER_TITLE, directory, tiles, count, selected, firstRow);
	pGraphics->UpdateDisplay();
}

static void DrawLaunchScreen(C2DGraphics *pGraphics,
                             CScreenDevice *pScreen,
                             const TileLayout &layout,
                             unsigned index,
                             const BrowserEntry &entry)
{
	if (pGraphics)
	{
		const TileSurface surface = {
			static_cast<uint16_t *>(pGraphics->GetBuffer()),
			pGraphics->GetWidth(), pGraphics->GetHeight(), pGraphics->GetWidth()
		};
		DrawTileLaunch(surface, layout, TileFor(index, entry), entry.pCore ? entry.pCore->name : "");
		pGraphics->UpdateDisplay();
		return;
	}

	ScreenWrite(pScreen, "\x1b[2J\x1b[H");
	ScreenWrite(pScreen, "ROM: ");
	ScreenWriteLine(pScreen, entry.path);
	ScreenWrite(pScreen, "Core: ");
	ScreenWriteLine(pScreen, entry.pCore ? entry.pCore->name : "");
	if (pScreen)
	{
		pScreen->Update();
	}
}

static unsigned ReadButtons(CircleInput *pInput)
{
	if (!pInput)
	{
		return 0;
	}

	pInput->Poll();

	unsigned buttons = 0;
	if (pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP))
	{
		buttons |= 1 << 0;
	}
	if (pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN))
	{
		buttons |= 1 << 1;
	}
	if (pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT))
	{
		buttons |= 1 << 2;
	}
	if (pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT))
	{
		buttons |= 1 << 3;
	}
	if (pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A))
	{
		buttons |= 1 << 4;
	}
	if (pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B))
	{
		buttons |= 1 << 5;
	}

	return buttons;
}

bool SelectRomAtBoot(CScreenDevice *pScreen,
                     CTimer *pTimer,
                     CircleFs *pFs,
                     CircleInput *pInput,
                     char *romPath,
                     size_t romPathSize,
                     const LibretroCore **ppCore,
                     TRomBrowserAbortPoll pAbortPoll,
                     void *pAbortContext)
{
	if (romPath && romPathSize)
	{
		romPath[0] = 0;
	}
	if (ppCore)
	{
		*ppCore = 0;
	}
	if (!pFs || !romPath || romPathSize == 0 || !ppCore)
	{
		return false;
	}

	// Kept between calls: after a game the browser opens where it was started.
	static char directory[256];
	static BrowserEntry entries[MAX_BROWSER_ENTRIES];
	static unsigned selected = 0;
	static unsigned firstRow = 0;
	unsigned count = 0;
	// Buttons already held when the browser opens do not count as presses.
	unsigned lastButtons = ReadButtons(pInput);
	bool reload = true;
	bool redraw = true;
	C2DGraphics *pGraphics = 0;
#ifndef SCREEN_HEADLESS
	CBcmFrameBuffer *pFrameBuffer = pScreen ? pScreen->GetFrameBuffer() : 0;
	C2DGraphics graphics(pFrameBuffer);
	if (pFrameBuffer && pFrameBuffer->GetDepth() == 16 && graphics.Initialize())
	{
		pGraphics = &graphics;
	}
#endif
	const TileLayout layout = ComputeTileLayout(pGraphics ? pGraphics->GetWidth() : 320,
						    pGraphics ? pGraphics->GetHeight() : 240);
	// The text browser is a list, one entry per line.
	const unsigned columns = pGraphics ? layout.columns : 1;

	while (1)
	{
		if (reload)
		{
			count = LoadDirectory(pFs, directory, entries, MAX_BROWSER_ENTRIES);
			if (selected >= count)
			{
				selected = count ? count - 1 : 0;
			}
			s_Pictures.Use(directory, layout.tileSize);
			reload = false;
			redraw = true;
		}

		firstRow = TileFirstRow(layout, selected, firstRow);
		if (redraw)
		{
			if (pGraphics)
			{
				DrawTiles(pGraphics, layout, directory, entries, count, selected, firstRow);
			}
			else
			{
				DrawTextBrowser(pScreen, directory, entries, count, selected);
			}
			redraw = false;
		}

		const unsigned buttons = ReadButtons(pInput);
		const unsigned pressed = buttons & ~lastButtons;
		lastButtons = buttons;

		if ((pressed & (1 << 0)) && selected >= columns)
		{
			selected -= columns;
			redraw = true;
		}
		if (pressed & (1 << 1))
		{
			if (selected + columns < count)
			{
				selected += columns;
				redraw = true;
			}
			else if (count && selected / columns < (count - 1) / columns)
			{
				selected = count - 1;	// into the shorter last row
				redraw = true;
			}
		}
		if ((pressed & (1 << 2)) && selected > 0)
		{
			selected--;
			redraw = true;
		}
		if ((pressed & (1 << 3)) && selected + 1 < count)
		{
			selected++;
			redraw = true;
		}
		if (pressed & (1 << 5))
		{
			if (IsParentAvailable(directory))
			{
				ParentPath(directory);
				selected = 0;
				firstRow = 0;
				reload = true;
			}
		}
		if ((pressed & (1 << 4)) && count > 0)
		{
			const BrowserEntry *entry = &entries[selected];
			if (entry->entry.isDirectory)
			{
				CopyString(directory, sizeof(directory), entry->path);
				selected = 0;
				firstRow = 0;
				reload = true;
			}
			else if (entry->pCore)
			{
				CopyString(romPath, romPathSize, entry->path);
				*ppCore = entry->pCore;
				if (!s_Pictures.IsLoaded(selected, *entry))
				{
					s_Pictures.Load(selected, *entry, pFs);
				}
				DrawLaunchScreen(pGraphics, pScreen, layout, selected, *entry);
				return true;
			}
		}

		if (pAbortPoll && pAbortPoll(pAbortContext))
		{
			return false;
		}

		// Pictures load while no button is pressed; otherwise wait a bit.
		if (!reload && !redraw && pGraphics && !pressed)
		{
			const unsigned visibleFirst = TileFirstRow(layout, selected, firstRow) * columns;
			if (LoadNextPicture(entries, count, visibleFirst, layout.rows * columns, pFs))
			{
				redraw = true;
			}
			else if (pTimer)
			{
				pTimer->MsDelay(10);
			}
		}
		else if (pTimer)
		{
			pTimer->MsDelay(10);
		}
	}
}
