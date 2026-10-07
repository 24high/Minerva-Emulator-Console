#include "rom_browser.h"

#include "libretro/libretro_runner.h"
#include "platform/circle/circle_platform.h"

#include <circle/2dgraphics.h>
#include <circle/screen.h>
#include <circle/timer.h>
#include <libretro.h>
#include <string.h>

static const unsigned MAX_BROWSER_ENTRIES = 96;

// Screens below this height (e.g. the 320x240 panel of the GPi Case) get
// the compact browser layout.
static const unsigned COMPACT_LAYOUT_MAX_HEIGHT = 400;

static const char BROWSER_TITLE[] = "MINERVA CONSOLE";
#if defined(RA_BAREMETAL_CORE_BUNDLE)
static const char BROWSER_SUBTITLE[] = "ROM SELECTION  -  NES GB GBA SNES MD SMS GG";
#elif defined(RA_BAREMETAL_MULTI)
static const char BROWSER_SUBTITLE[] = "ROM SELECTION  -  NES AND N64";
#elif defined(RA_BAREMETAL_N64)
static const char BROWSER_SUBTITLE[] = "ROM SELECTION  -  N64";
#elif defined(RA_BAREMETAL_FCEUMM)
static const char BROWSER_SUBTITLE[] = "ROM SELECTION  -  NES";
#else
static const char BROWSER_SUBTITLE[] = "ROM SELECTION";
#endif

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

	if (!pFs || !pFs->ListDirectory(directory, rawEntries, MAX_BROWSER_ENTRIES, &rawCount))
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

static unsigned TextScaleX(CCharGenerator::TFontFlags flags)
{
	return (flags & CCharGenerator::FontFlagsDoubleWidth) ? 2 : 1;
}

static unsigned TextScaleY(CCharGenerator::TFontFlags flags)
{
	return (flags & CCharGenerator::FontFlagsDoubleHeight) ? 2 : 1;
}

static unsigned TextHeightPixels(const TFont &font, CCharGenerator::TFontFlags flags)
{
	return (font.height + font.extra_height) * TextScaleY(flags);
}

static void CopyTextLimited(char *dst,
                            size_t dstSize,
                            const char *src,
                            unsigned maxPixels,
                            const TFont &font,
                            CCharGenerator::TFontFlags flags)
{
	if (!dst || dstSize == 0)
	{
		return;
	}

	dst[0] = 0;
	if (!src || maxPixels == 0)
	{
		return;
	}

	unsigned charWidth = font.width * TextScaleX(flags);
	unsigned maxChars = charWidth ? maxPixels / charWidth : 0;
	if (maxChars + 1 > dstSize)
	{
		maxChars = (unsigned)dstSize - 1;
	}
	if (maxChars == 0)
	{
		return;
	}

	size_t srcLen = strlen(src);
	size_t copyLen = srcLen < maxChars ? srcLen : maxChars;
	memcpy(dst, src, copyLen);
	dst[copyLen] = 0;

	if (srcLen > copyLen)
	{
		if (copyLen >= 3)
		{
			dst[copyLen - 3] = '.';
			dst[copyLen - 2] = '.';
			dst[copyLen - 1] = '.';
		}
		else
		{
			dst[copyLen - 1] = '~';
		}
	}
}

static void DrawRectClipped(C2DGraphics *pGraphics,
                            unsigned x,
                            unsigned y,
                            unsigned w,
                            unsigned h,
                            T2DColor color)
{
	if (!pGraphics || w == 0 || h == 0)
	{
		return;
	}

	const unsigned screenW = pGraphics->GetWidth();
	const unsigned screenH = pGraphics->GetHeight();
	if (x >= screenW || y >= screenH)
	{
		return;
	}
	if (x + w > screenW)
	{
		w = screenW - x;
	}
	if (y + h > screenH)
	{
		h = screenH - y;
	}
	if (w && h)
	{
		pGraphics->DrawRect(x, y, w, h, color);
	}
}

static void DrawOutlineClipped(C2DGraphics *pGraphics,
                               unsigned x,
                               unsigned y,
                               unsigned w,
                               unsigned h,
                               T2DColor color)
{
	if (!pGraphics || w < 2 || h < 2)
	{
		return;
	}

	DrawRectClipped(pGraphics, x, y, w, 1, color);
	DrawRectClipped(pGraphics, x, y + h - 1, w, 1, color);
	DrawRectClipped(pGraphics, x, y, 1, h, color);
	DrawRectClipped(pGraphics, x + w - 1, y, 1, h, color);
}

static void DrawTextLimited(C2DGraphics *pGraphics,
                            unsigned x,
                            unsigned y,
                            unsigned maxPixels,
                            T2DColor color,
                            const char *text,
                            const TFont &font,
                            CCharGenerator::TFontFlags flags = CCharGenerator::FontFlagsNone)
{
	char buffer[128];
	CopyTextLimited(buffer, sizeof(buffer), text, maxPixels, font, flags);
	if (buffer[0])
	{
		pGraphics->DrawText(x, y, color, buffer, C2DGraphics::AlignLeft, font, flags);
	}
}

static const char *EntryKind(const BrowserEntry *entry)
{
	if (!entry)
	{
		return "";
	}
	if (entry->entry.isDirectory)
	{
		return "DIR";
	}
	return entry->system;
}

struct SystemColor
{
	const char *system;
	T2DColor color;
};

static T2DColor EntryAccentColor(const BrowserEntry *entry)
{
	if (!entry)
	{
		return COLOR2D(140, 148, 160);
	}
	if (entry->entry.isDirectory)
	{
		return COLOR2D(54, 190, 198);
	}

	static const SystemColor Colors[] = {
		{ "NES",  COLOR2D(118, 218, 135) },
		{ "N64",  COLOR2D(196, 114, 238) },
		{ "GB",   COLOR2D(176, 204, 84) },
		{ "GBC",  COLOR2D(240, 122, 176) },
		{ "GBA",  COLOR2D(176, 128, 236) },
		{ "SNES", COLOR2D(150, 150, 240) },
		{ "MD",   COLOR2D(236, 88, 88) },
		{ "32X",  COLOR2D(224, 128, 96) },
		{ "SMS",  COLOR2D(98, 152, 246) },
		{ "GG",   COLOR2D(246, 212, 84) },
		{ "SG",   COLOR2D(182, 182, 182) },
	};
	for (unsigned i = 0; i < sizeof Colors / sizeof Colors[0]; i++)
	{
		if (strcmp(entry->system, Colors[i].system) == 0)
		{
			return Colors[i].color;
		}
	}
	return COLOR2D(140, 148, 160);
}

static void FormatPath(char *dst, size_t dstSize, const char *directory)
{
	if (!dst || dstSize == 0)
	{
		return;
	}

	if (!directory || directory[0] == 0)
	{
		CopyString(dst, dstSize, "/");
		return;
	}

	CopyString(dst, dstSize, "/");
	const size_t len = strlen(dst);
	CopyString(dst + len, dstSize - len, directory);
}

static void FormatCount(char *dst, size_t dstSize, unsigned selected, unsigned count)
{
	if (!dst || dstSize == 0)
	{
		return;
	}

	if (count == 0)
	{
		CopyString(dst, dstSize, "0 / 0");
		return;
	}

	unsigned current = selected + 1;
	char left[12];
	char right[12];
	unsigned li = 0;
	unsigned ri = 0;

	do
	{
		left[li++] = (char)('0' + current % 10);
		current /= 10;
	}
	while (current && li + 1 < sizeof(left));
	do
	{
		right[ri++] = (char)('0' + count % 10);
		count /= 10;
	}
	while (count && ri + 1 < sizeof(right));

	unsigned out = 0;
	while (li && out + 1 < dstSize)
	{
		dst[out++] = left[--li];
	}
	if (out + 3 < dstSize)
	{
		dst[out++] = ' ';
		dst[out++] = '/';
		dst[out++] = ' ';
	}
	while (ri && out + 1 < dstSize)
	{
		dst[out++] = right[--ri];
	}
	dst[out] = 0;
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

static void DrawKeyHint(C2DGraphics *pGraphics,
                        unsigned x,
                        unsigned y,
                        T2DColor keyColor,
                        const char *key,
                        const char *label,
                        unsigned *pNextX)
{
	const unsigned charW = Font8x8.width;
	const unsigned keyW = (unsigned)strlen(key) * charW + 4;
	DrawRectClipped(pGraphics, x, y, keyW, 10, keyColor);
	DrawTextLimited(pGraphics, x + 2, y + 1, keyW, COLOR2D(8, 10, 10), key, Font8x8);
	DrawTextLimited(pGraphics, x + keyW + 3, y + 1, (unsigned)strlen(label) * charW, COLOR2D(232, 238, 230), label, Font8x8);
	*pNextX = x + keyW + 3 + (unsigned)strlen(label) * charW + 9;
}

static void DrawCompactBrowser(C2DGraphics *pGraphics,
                               const char *directory,
                               const BrowserEntry *entries,
                               unsigned count,
                               unsigned selected)
{
	const unsigned screenW = pGraphics->GetWidth();
	const unsigned screenH = pGraphics->GetHeight();
	const unsigned headerH = 22;
	const unsigned pathY = headerH + 2;
	const unsigned pathH = TextHeightPixels(Font8x8, CCharGenerator::FontFlagsNone) + 2;
	const unsigned footerH = 14;
	const unsigned footerY = screenH > footerH ? screenH - footerH : 0;
	const unsigned rowH = 18;
	const unsigned listX = 2;
	const unsigned listY = pathY + pathH + 2;
	const unsigned listBottom = footerY > listY + rowH ? footerY - 2 : listY + rowH;
	unsigned visibleRows = (listBottom - listY) / rowH;
	if (visibleRows == 0)
	{
		visibleRows = 1;
	}
	const unsigned scrollW = count > visibleRows ? 5 : 0;
	const unsigned listW = screenW > listX * 2 + scrollW ? screenW - listX * 2 - scrollW : screenW;

	unsigned first = 0;
	if (selected >= visibleRows)
	{
		first = selected - visibleRows + 1;
	}

	const T2DColor bg = COLOR2D(9, 12, 16);
	const T2DColor header = COLOR2D(18, 24, 31);
	const T2DColor panel = COLOR2D(25, 31, 38);
	const T2DColor panelAlt = COLOR2D(30, 37, 44);
	const T2DColor text = COLOR2D(232, 238, 230);
	const T2DColor muted = COLOR2D(140, 153, 156);
	const T2DColor selectedBg = COLOR2D(217, 137, 48);
	const T2DColor selectedText = COLOR2D(16, 18, 20);
	const T2DColor cyan = COLOR2D(50, 202, 210);
	const T2DColor magenta = COLOR2D(203, 86, 214);
	const T2DColor green = COLOR2D(100, 220, 130);

	pGraphics->ClearScreen(bg);
	DrawRectClipped(pGraphics, 0, 0, screenW, headerH, header);
	DrawRectClipped(pGraphics, 0, 0, screenW, 2, cyan);
	DrawRectClipped(pGraphics, 0, headerH - 2, screenW, 2, magenta);
	DrawTextLimited(pGraphics, 6, 3, screenW > 80 ? screenW - 80 : screenW, text, BROWSER_TITLE, Font8x16);

	char countText[32];
	FormatCount(countText, sizeof(countText), selected, count);
	pGraphics->DrawText(screenW - 6, 8, muted, countText, C2DGraphics::AlignRight, Font8x8);

	char pathText[256];
	FormatPath(pathText, sizeof(pathText), directory);
	DrawTextLimited(pGraphics, 6, pathY + 1, screenW - 12, muted, pathText, Font8x8);

	if (count == 0)
	{
		DrawRectClipped(pGraphics, listX, listY, listW, rowH * 2, panel);
		DrawTextLimited(pGraphics, listX + 6, listY + 5, listW - 12, text, "NO ROMS FOUND", Font8x8);
		DrawTextLimited(pGraphics, listX + 6, listY + 21, listW - 12, muted, "COPY YOUR GAMES TO THE SD CARD", Font8x8);
	}

	for (unsigned row = 0; row < visibleRows && first + row < count; row++)
	{
		const unsigned index = first + row;
		const BrowserEntry *entry = &entries[index];
		const bool isSelected = index == selected;
		const unsigned y = listY + row * rowH;
		const T2DColor rowColor = isSelected ? selectedBg : (row & 1 ? panelAlt : panel);
		const T2DColor accent = isSelected ? selectedText : EntryAccentColor(entry);
		const unsigned nameX = listX + 42;

		DrawRectClipped(pGraphics, listX, y, listW, rowH - 1, rowColor);
		DrawRectClipped(pGraphics, listX, y, 3, rowH - 1, accent);
		DrawTextLimited(pGraphics, listX + 7, y + 5, 32, accent, EntryKind(entry), Font8x8);
		DrawTextLimited(pGraphics, nameX, y + 1, listW > nameX + 2 ? listW - nameX - 2 : 8,
			isSelected ? selectedText : text, entry->entry.name, Font8x16);
	}

	if (scrollW)
	{
		const unsigned trackX = screenW - scrollW + 1;
		const unsigned trackH = visibleRows * rowH - 1;
		unsigned thumbH = trackH * visibleRows / count;
		if (thumbH < 6)
		{
			thumbH = 6;
		}
		const unsigned thumbY = listY + (trackH - thumbH) * first / (count - visibleRows);
		DrawRectClipped(pGraphics, trackX, listY, scrollW - 2, trackH, panel);
		DrawRectClipped(pGraphics, trackX, thumbY, scrollW - 2, thumbH, cyan);
	}

	DrawRectClipped(pGraphics, 0, footerY, screenW, footerH, COLOR2D(15, 19, 24));
	unsigned hintX = 4;
	DrawKeyHint(pGraphics, hintX, footerY + 2, green, "A", "START", &hintX);
	DrawKeyHint(pGraphics, hintX, footerY + 2, magenta, "B", "BACK", &hintX);
	DrawKeyHint(pGraphics, hintX, footerY + 2, cyan, "<>", "PAGE", &hintX);

	pGraphics->UpdateDisplay();
}

static void DrawGraphicBrowser(C2DGraphics *pGraphics,
                               const char *directory,
                               const BrowserEntry *entries,
                               unsigned count,
                               unsigned selected)
{
	if (!pGraphics)
	{
		return;
	}

	if (pGraphics->GetHeight() < COMPACT_LAYOUT_MAX_HEIGHT)
	{
		DrawCompactBrowser(pGraphics, directory, entries, count, selected);
		return;
	}

	const unsigned screenW = pGraphics->GetWidth();
	const unsigned screenH = pGraphics->GetHeight();
	const unsigned margin = screenW >= 1280 ? 64 : 28;
	const unsigned headerH = screenH >= 900 ? 150 : 118;
	const unsigned footerH = screenH >= 900 ? 82 : 64;
	const unsigned pathH = screenH >= 900 ? 46 : 38;
	const unsigned rowH = screenH >= 900 ? 42 : 34;
	const unsigned smallTextY = TextHeightPixels(Font8x16, CCharGenerator::FontFlagsNone);
	const unsigned titleFlags = CCharGenerator::MakeFlags(screenW >= 1280, screenH >= 900);
	const unsigned listX = margin;
	const unsigned listW = screenW > margin * 2 ? screenW - margin * 2 : screenW;
	const unsigned pathY = headerH + 18;
	const unsigned listY = pathY + pathH + 16;
	const unsigned listBottom = screenH > footerH + 18 ? screenH - footerH - 18 : screenH;
	const unsigned listH = listBottom > listY ? listBottom - listY : rowH;
	unsigned visibleRows = listH / rowH;
	if (visibleRows == 0)
	{
		visibleRows = 1;
	}

	unsigned first = 0;
	if (selected >= visibleRows)
	{
		first = selected - visibleRows + 1;
	}

	const T2DColor bg = COLOR2D(9, 12, 16);
	const T2DColor header = COLOR2D(18, 24, 31);
	const T2DColor panel = COLOR2D(25, 31, 38);
	const T2DColor panelAlt = COLOR2D(30, 37, 44);
	const T2DColor outline = COLOR2D(70, 86, 96);
	const T2DColor text = COLOR2D(232, 238, 230);
	const T2DColor muted = COLOR2D(140, 153, 156);
	const T2DColor selectedBg = COLOR2D(217, 137, 48);
	const T2DColor selectedText = COLOR2D(16, 18, 20);
	const T2DColor cyan = COLOR2D(50, 202, 210);
	const T2DColor magenta = COLOR2D(203, 86, 214);
	const T2DColor green = COLOR2D(100, 220, 130);

	pGraphics->ClearScreen(bg);
	DrawRectClipped(pGraphics, 0, 0, screenW, headerH, header);
	DrawRectClipped(pGraphics, 0, 0, screenW, 8, cyan);
	DrawRectClipped(pGraphics, 0, headerH - 5, screenW, 5, magenta);
	DrawRectClipped(pGraphics, margin, 32, 10, headerH > 70 ? headerH - 66 : 36, selectedBg);

	DrawTextLimited(pGraphics, margin + 28, 28, listW > 28 ? listW - 28 : listW,
		text, BROWSER_TITLE, Font12x22, (CCharGenerator::TFontFlags)titleFlags);
	DrawTextLimited(pGraphics, margin + 30, headerH > 56 ? headerH - 50 : 72,
		listW > 30 ? listW - 30 : listW, muted,
		BROWSER_SUBTITLE, Font8x16);

	char pathText[256];
	FormatPath(pathText, sizeof(pathText), directory);
	DrawRectClipped(pGraphics, listX, pathY, listW, pathH, panel);
	DrawOutlineClipped(pGraphics, listX, pathY, listW, pathH, outline);
	DrawTextLimited(pGraphics, listX + 16, pathY + (pathH > smallTextY ? (pathH - smallTextY) / 2 : 0),
		listW > 32 ? listW - 32 : listW, muted, pathText, Font8x16);

	if (count == 0)
	{
		DrawRectClipped(pGraphics, listX, listY, listW, rowH * 3, panelAlt);
		DrawOutlineClipped(pGraphics, listX, listY, listW, rowH * 3, outline);
		DrawTextLimited(pGraphics, listX + 22, listY + rowH, listW > 44 ? listW - 44 : listW,
			text, "NO SUPPORTED ROMS OR FOLDERS FOUND", Font8x16);
	}
	else
	{
		for (unsigned row = 0; row < visibleRows && first + row < count; row++)
		{
			const unsigned index = first + row;
			const BrowserEntry *entry = &entries[index];
			const bool isSelected = index == selected;
			const unsigned y = listY + row * rowH;
			const T2DColor rowColor = isSelected ? selectedBg : (row & 1 ? panelAlt : panel);
			const T2DColor rowText = isSelected ? selectedText : text;
			const T2DColor metaText = isSelected ? selectedText : muted;
			const T2DColor accent = isSelected ? selectedText : EntryAccentColor(entry);
			const unsigned tagW = screenW >= 1280 ? 72 : 54;
			const unsigned metaW = screenW >= 1280 ? 190 : 118;
			const unsigned nameX = listX + tagW + 28;
			const unsigned metaX = listX + listW > metaW + 20 ? listX + listW - metaW - 16 : listX + listW;
			const unsigned nameW = metaX > nameX ? metaX - nameX - 12 : 80;

			DrawRectClipped(pGraphics, listX, y, listW, rowH - 2, rowColor);
			DrawRectClipped(pGraphics, listX, y, 7, rowH - 2, accent);
			DrawOutlineClipped(pGraphics, listX, y, listW, rowH - 2, isSelected ? selectedText : COLOR2D(43, 52, 60));
			DrawTextLimited(pGraphics, listX + 18, y + (rowH > smallTextY ? (rowH - smallTextY) / 2 : 0),
				tagW, accent, EntryKind(entry), Font8x16);
			DrawTextLimited(pGraphics, nameX, y + (rowH > smallTextY ? (rowH - smallTextY) / 2 : 0),
				nameW, rowText, entry->entry.name, Font8x16);

			const char *meta = entry->entry.isDirectory
				? "FOLDER"
				: (entry->pCore ? entry->pCore->name : "");
			if (metaX < listX + listW)
			{
				DrawTextLimited(pGraphics, metaX, y + (rowH > smallTextY ? (rowH - smallTextY) / 2 : 0),
					metaW, metaText, meta, Font8x16);
			}
		}
	}

	char countText[32];
	FormatCount(countText, sizeof(countText), selected, count);
	DrawTextLimited(pGraphics, listX, listBottom + 2, listW / 3, muted, countText, Font8x16);

	const unsigned footerY = screenH > footerH ? screenH - footerH : 0;
	DrawRectClipped(pGraphics, 0, footerY, screenW, footerH, COLOR2D(15, 19, 24));
	DrawRectClipped(pGraphics, margin, footerY + 18, 34, 28, green);
	DrawTextLimited(pGraphics, margin + 10, footerY + 23, 16, COLOR2D(8, 10, 10), "A", Font8x16);
	DrawTextLimited(pGraphics, margin + 44, footerY + 23, 180, text, "START/OPEN", Font8x16);
	DrawRectClipped(pGraphics, margin + 244, footerY + 18, 34, 28, magenta);
	DrawTextLimited(pGraphics, margin + 254, footerY + 23, 16, COLOR2D(8, 10, 10), "B", Font8x16);
	DrawTextLimited(pGraphics, margin + 288, footerY + 23, 140, text, "BACK", Font8x16);
	DrawRectClipped(pGraphics, margin + 446, footerY + 18, 74, 28, cyan);
	DrawTextLimited(pGraphics, margin + 456, footerY + 23, 60, COLOR2D(8, 10, 10), "D-PAD", Font8x16);
	DrawTextLimited(pGraphics, margin + 530, footerY + 23, listW > 530 ? listW - 530 : 120,
		text, "NAVIGATE", Font8x16);

	pGraphics->UpdateDisplay();
}

static void DrawBrowser(C2DGraphics *pGraphics,
                        CScreenDevice *pScreen,
                        const char *directory,
                        const BrowserEntry *entries,
                        unsigned count,
                        unsigned selected)
{
	if (pGraphics)
	{
		DrawGraphicBrowser(pGraphics, directory, entries, count, selected);
	}
	else
	{
		DrawTextBrowser(pScreen, directory, entries, count, selected);
	}
}

static void DrawLaunchScreen(C2DGraphics *pGraphics,
                             CScreenDevice *pScreen,
                             const char *romPath,
                             const LibretroCore *pCore)
{
	if (pGraphics)
	{
		const unsigned screenW = pGraphics->GetWidth();
		const unsigned screenH = pGraphics->GetHeight();
		const bool compact = screenH < COMPACT_LAYOUT_MAX_HEIGHT;
		const unsigned margin = compact ? 12 : (screenW >= 1280 ? 96 : 32);
		const unsigned boxW = screenW > margin * 2 ? screenW - margin * 2 : screenW;
		const unsigned boxH = compact ? 110 : (screenH >= 900 ? 230 : 170);
		const unsigned boxY = screenH > boxH ? (screenH - boxH) / 2 : 0;
		const unsigned textX = compact ? margin + 20 : margin + 38;
		const unsigned textW = boxW > (textX - margin) * 2 ? boxW - (textX - margin) * 2 : boxW;

		pGraphics->ClearScreen(COLOR2D(8, 11, 15));
		DrawRectClipped(pGraphics, 0, 0, screenW, 8, COLOR2D(50, 202, 210));
		DrawRectClipped(pGraphics, 0, screenH > 8 ? screenH - 8 : 0, screenW, 8, COLOR2D(217, 137, 48));
		DrawRectClipped(pGraphics, margin, boxY, boxW, boxH, COLOR2D(24, 30, 37));
		DrawOutlineClipped(pGraphics, margin, boxY, boxW, boxH, COLOR2D(80, 96, 108));
		DrawRectClipped(pGraphics, margin, boxY, 10, boxH, COLOR2D(100, 220, 130));

		if (compact)
		{
			DrawTextLimited(pGraphics, textX, boxY + 16, textW,
				COLOR2D(232, 238, 230), "LOADING GAME", Font8x16);
		}
		else
		{
			DrawTextLimited(pGraphics, margin + 36, boxY + 34, boxW > 72 ? boxW - 72 : boxW,
				COLOR2D(232, 238, 230), "LOADING GAME", Font12x22,
				CCharGenerator::MakeFlags(screenW >= 1280, FALSE));
		}
		DrawTextLimited(pGraphics, textX, boxY + (compact ? 48 : 98), textW,
			COLOR2D(170, 185, 190), romPath, Font8x16);
		DrawTextLimited(pGraphics, textX, boxY + (compact ? 76 : 130), textW,
			COLOR2D(217, 137, 48), pCore ? pCore->name : "CORE", Font8x16);
		pGraphics->UpdateDisplay();
		return;
	}

	ScreenWrite(pScreen, "\x1b[2J\x1b[H");
	ScreenWrite(pScreen, "ROM: ");
	ScreenWriteLine(pScreen, romPath);
	ScreenWrite(pScreen, "Core: ");
	ScreenWriteLine(pScreen, pCore ? pCore->name : "");
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
	unsigned count = 0;
	// Buttons already held when the browser opens do not count as presses.
	unsigned lastButtons = ReadButtons(pInput);
	bool reload = true;
	bool redraw = true;
	C2DGraphics *pGraphics = 0;
#ifndef SCREEN_HEADLESS
	CBcmFrameBuffer *pFrameBuffer = pScreen ? pScreen->GetFrameBuffer() : 0;
	C2DGraphics graphics(pFrameBuffer);
	if (pFrameBuffer && graphics.Initialize())
	{
		pGraphics = &graphics;
	}
#endif

	while (1)
	{
		if (reload)
		{
			count = LoadDirectory(pFs, directory, entries, MAX_BROWSER_ENTRIES);
			if (selected >= count)
			{
				selected = count ? count - 1 : 0;
			}
			reload = false;
			redraw = true;
		}

		if (redraw)
		{
			DrawBrowser(pGraphics, pScreen, directory, entries, count, selected);
			redraw = false;
		}

		const unsigned buttons = ReadButtons(pInput);
		const unsigned pressed = buttons & ~lastButtons;
		lastButtons = buttons;

		if ((pressed & (1 << 0)) && selected > 0)
		{
			selected--;
			redraw = true;
		}
		if ((pressed & (1 << 1)) && selected + 1 < count)
		{
			selected++;
			redraw = true;
		}
		if ((pressed & (1 << 2)) && selected > 5)
		{
			selected -= 5;
			redraw = true;
		}
		if (pressed & (1 << 3))
		{
			selected = selected + 5 < count ? selected + 5 : (count ? count - 1 : 0);
			redraw = true;
		}
		if (pressed & (1 << 5))
		{
			if (IsParentAvailable(directory))
			{
				ParentPath(directory);
				selected = 0;
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
				reload = true;
			}
			else if (entry->pCore)
			{
				CopyString(romPath, romPathSize, entry->path);
				*ppCore = entry->pCore;
				DrawLaunchScreen(pGraphics, pScreen, romPath, entry->pCore);
				return true;
			}
		}

		if (pAbortPoll && pAbortPoll(pAbortContext))
		{
			return false;
		}

		if (pTimer)
		{
			pTimer->MsDelay(30);
		}
	}
}
