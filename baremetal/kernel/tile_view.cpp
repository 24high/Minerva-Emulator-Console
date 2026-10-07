#include "tile_view.h"

#include <stdlib.h>
#include <string.h>

// PNG decoder for the tile pictures on the SD card (stb_image, public domain).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_ASSERT(x) ((void)0)
#define STBI_MAX_DIMENSIONS 4096
#include "third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

// Larger pictures are not decoded (memory: width * height * 4 bytes).
static const unsigned MAX_PICTURE_PIXELS = 2048 * 2048;

static const uint16_t BACKGROUND = TILE_RGB565(12, 15, 20);
static const uint16_t BAR = TILE_RGB565(22, 28, 35);
static const uint16_t TILE_BACKGROUND = TILE_RGB565(30, 36, 44);
static const uint16_t OUTLINE = TILE_RGB565(52, 62, 72);
static const uint16_t HIGHLIGHT = TILE_RGB565(217, 137, 48);
static const uint16_t TEXT = TILE_RGB565(232, 238, 230);
static const uint16_t MUTED = TILE_RGB565(140, 153, 156);
static const uint16_t FOLDER = TILE_RGB565(54, 190, 198);
static const uint16_t FOLDER_DARK = TILE_RGB565(32, 120, 128);

struct SystemColor
{
	const char *system;
	uint16_t color;
};

static uint16_t SystemAccent(const TileEntry &entry)
{
	static const SystemColor Colors[] = {
		{ "NES",  TILE_RGB565(118, 218, 135) },
		{ "N64",  TILE_RGB565(196, 114, 238) },
		{ "GB",   TILE_RGB565(176, 204, 84) },
		{ "GBC",  TILE_RGB565(240, 122, 176) },
		{ "GBA",  TILE_RGB565(176, 128, 236) },
		{ "SNES", TILE_RGB565(150, 150, 240) },
		{ "MD",   TILE_RGB565(236, 88, 88) },
		{ "32X",  TILE_RGB565(224, 128, 96) },
		{ "SMS",  TILE_RGB565(98, 152, 246) },
		{ "GG",   TILE_RGB565(246, 212, 84) },
		{ "SG",   TILE_RGB565(182, 182, 182) },
	};
	if (entry.isDirectory)
	{
		return FOLDER;
	}
	for (unsigned i = 0; i < sizeof Colors / sizeof Colors[0]; i++)
	{
		if (entry.system && strcmp(entry.system, Colors[i].system) == 0)
		{
			return Colors[i].color;
		}
	}
	return MUTED;
}

// --- Drawing primitives (clipped to the surface) ---------------------------

static void Fill(const TileSurface &surface, int x, int y, int w, int h, uint16_t color)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > (int)surface.width) w = (int)surface.width - x;
	if (y + h > (int)surface.height) h = (int)surface.height - y;
	for (int row = 0; row < h; row++)
	{
		uint16_t *pLine = surface.pixels + (size_t)(y + row) * surface.pitch + x;
		for (int column = 0; column < w; column++)
		{
			pLine[column] = color;
		}
	}
}

static void Frame(const TileSurface &surface, int x, int y, int w, int h, int thickness, uint16_t color)
{
	Fill(surface, x, y, w, thickness, color);
	Fill(surface, x, y + h - thickness, w, thickness, color);
	Fill(surface, x, y, thickness, h, color);
	Fill(surface, x + w - thickness, y, thickness, h, color);
}

static void Blit(const TileSurface &surface, int x, int y, unsigned w, unsigned h, const uint16_t *pPixels)
{
	for (unsigned row = 0; row < h; row++)
	{
		const int ty = y + (int)row;
		if (ty < 0 || ty >= (int)surface.height)
		{
			continue;
		}
		for (unsigned column = 0; column < w; column++)
		{
			const int tx = x + (int)column;
			if (tx >= 0 && tx < (int)surface.width)
			{
				surface.pixels[(size_t)ty * surface.pitch + tx] = pPixels[row * w + column];
			}
		}
	}
}

static unsigned LineHeight(const TFont &font)
{
	return font.height + font.extra_height;
}

static unsigned TextWidth(const TFont &font, const char *pText)
{
	return (unsigned)strlen(pText) * font.width;
}

// Same glyph format as Circle's CCharGenerator.
static uint32_t GlyphLine(const TFont &font, unsigned char ch, unsigned y)
{
	if (ch < font.first_char || ch > font.last_char || y >= font.height)
	{
		return 0;
	}
	const unsigned index = (ch - font.first_char) * font.height + y;
	switch ((font.width + 7) / 8)
	{
	case 1:  return static_cast<const uint8_t *>(font.data)[index];
	case 2:  return static_cast<const uint16_t *>(font.data)[index];
	case 3: {
		const uint8_t *pData = static_cast<const uint8_t *>(font.data) + 3 * index;
		return pData[0] | pData[1] << 8 | pData[2] << 16;
		}
	default: return static_cast<const uint32_t *>(font.data)[index];
	}
}

static void DrawText(const TileSurface &surface, int x, int y, uint16_t color, const char *pText, const TFont &font)
{
	for (; *pText; pText++, x += (int)font.width)
	{
		for (unsigned gy = 0; gy < font.height; gy++)
		{
			const uint32_t line = GlyphLine(font, (unsigned char)*pText, gy);
			if (!line)
			{
				continue;
			}
			for (unsigned gx = 0; gx < font.width; gx++)
			{
				if (line & (1U << (font.width - 1 - gx)))
				{
					Fill(surface, x + (int)gx, y + (int)gy, 1, 1, color);
				}
			}
		}
	}
}

// Shortens pText to maxWidth pixels, ending in ".." if it had to be cut.
static void FitText(char *pOut, size_t outSize, const char *pText, unsigned maxWidth, const TFont &font)
{
	size_t maxChars = font.width ? maxWidth / font.width : 0;
	if (maxChars >= outSize)
	{
		maxChars = outSize - 1;
	}
	size_t length = strlen(pText);
	if (length <= maxChars)
	{
		memcpy(pOut, pText, length + 1);
		return;
	}
	if (maxChars < 3)
	{
		pOut[0] = 0;
		return;
	}
	memcpy(pOut, pText, maxChars - 2);
	// no blank before the dots
	size_t cut = maxChars - 2;
	while (cut > 1 && pOut[cut - 1] == ' ')
	{
		cut--;
	}
	pOut[cut] = '.';
	pOut[cut + 1] = '.';
	pOut[cut + 2] = 0;
}

static void DrawTextFitted(const TileSurface &surface, int x, int y, unsigned maxWidth, uint16_t color,
			   const char *pText, const TFont &font, bool centred)
{
	char text[160];
	FitText(text, sizeof text, pText, maxWidth, font);
	if (centred)
	{
		x += (int)(maxWidth - TextWidth(font, text)) / 2;
	}
	DrawText(surface, x, y, color, text, font);
}

// Name without the file extension ("Tetris (USA).gb" -> "Tetris (USA)").
static void DisplayName(char *pOut, size_t outSize, const TileEntry &entry)
{
	strncpy(pOut, entry.name ? entry.name : "", outSize - 1);
	pOut[outSize - 1] = 0;
	char *pDot = 0;
	for (char *p = pOut; *p; p++)
	{
		if (*p == '.')
		{
			pDot = p;
		}
	}
	if (!entry.isDirectory && pDot && pDot != pOut)
	{
		*pDot = 0;
	}
}

// Name for the small caption below a tile, also without the region and
// version tags at the end ("Tetris (USA) (Rev 1)" -> "Tetris").
static void CaptionName(char *pOut, size_t outSize, const TileEntry &entry)
{
	DisplayName(pOut, outSize, entry);
	if (entry.isDirectory)
	{
		return;
	}
	for (;;)
	{
		size_t length = strlen(pOut);
		while (length > 0 && pOut[length - 1] == ' ')
		{
			pOut[--length] = 0;
		}
		if (length == 0 || (pOut[length - 1] != ')' && pOut[length - 1] != ']'))
		{
			return;
		}
		const char open = pOut[length - 1] == ')' ? '(' : '[';
		size_t start = length - 1;
		while (start > 0 && pOut[start] != open)
		{
			start--;
		}
		if (start == 0)
		{
			return;	// the whole name is a tag: keep it
		}
		pOut[start] = 0;
	}
}

static void DrawFolder(const TileSurface &surface, int x, int y, unsigned size)
{
	Fill(surface, x, y, (int)size, (int)size, TILE_BACKGROUND);
	const int w = (int)size * 5 / 8;
	const int h = (int)size * 7 / 16;
	const int fx = x + ((int)size - w) / 2;
	const int fy = y + ((int)size - h) / 2 + (int)size / 16;
	Fill(surface, fx, fy - (int)size / 12, w * 2 / 5, (int)size / 12 + 1, FOLDER_DARK);	// tab
	Fill(surface, fx, fy, w, h, FOLDER);
	Fill(surface, fx, fy, w, (int)size / 24 + 1, FOLDER_DARK);
}

static void DrawTriangle(const TileSurface &surface, int cx, int y, int size, bool up, uint16_t color)
{
	for (int i = 0; i < size; i++)
	{
		const int half = up ? i : size - 1 - i;
		Fill(surface, cx - half, y + i, 2 * half + 1, 1, color);
	}
}

// --- Layout -------------------------------------------------------------------

TileLayout ComputeTileLayout(unsigned screenWidth, unsigned screenHeight)
{
	TileLayout layout;
	const bool compact = screenHeight < 400;
	layout.pTitleFont = compact ? &Font8x16 : &Font12x22;
	layout.pTextFont = compact ? &Font8x8 : &Font8x16;
	layout.pCaptionFont = compact ? &Font6x7 : &Font8x16;
	layout.headerHeight = compact ? 18 : 44;
	layout.footerHeight = compact ? 16 : 40;
	layout.gridY = layout.headerHeight;
	layout.rows = compact ? 2 : (screenHeight >= 900 ? 3 : 2);

	const unsigned captionHeight = LineHeight(*layout.pCaptionFont);
	const unsigned gridHeight = screenHeight - layout.headerHeight - layout.footerHeight;
	layout.rowHeight = gridHeight / layout.rows;
	// picture, 4 pixels above and between picture and caption, caption
	unsigned tile = layout.rowHeight > captionHeight + 10 ? layout.rowHeight - captionHeight - 10 : 16;
	if (!compact && tile > 280)
	{
		tile = 280;
	}

	const unsigned minGap = compact ? 12 : 24;
	layout.columns = compact ? 3 : (screenWidth - minGap) / (tile + minGap);
	if (layout.columns == 0)
	{
		layout.columns = 1;
	}
	if (layout.columns * tile + (layout.columns + 1) * minGap > screenWidth)
	{
		tile = (screenWidth - (layout.columns + 1) * minGap) / layout.columns;
	}
	layout.tileSize = tile & ~1U;
	layout.gapX = (screenWidth - layout.columns * layout.tileSize) / (layout.columns + 1);
	return layout;
}

unsigned TileFirstRow(const TileLayout &layout, unsigned selected, unsigned firstRow)
{
	const unsigned row = selected / layout.columns;
	if (row < firstRow)
	{
		return row;
	}
	if (row >= firstRow + layout.rows)
	{
		return row - layout.rows + 1;
	}
	return firstRow;
}

static void TilePosition(const TileLayout &layout, unsigned row, unsigned column, int *pX, int *pY)
{
	*pX = (int)(layout.gapX + column * (layout.tileSize + layout.gapX));
	*pY = (int)(layout.gridY + row * layout.rowHeight + 6);
}

// --- Screens -------------------------------------------------------------------

static void DrawBars(const TileSurface &surface, const TileLayout &layout)
{
	Fill(surface, 0, 0, (int)surface.width, (int)surface.height, BACKGROUND);
	Fill(surface, 0, 0, (int)surface.width, (int)layout.headerHeight - 2, BAR);
	Fill(surface, 0, (int)layout.headerHeight - 2, (int)surface.width, 2, HIGHLIGHT);
	const int footerY = (int)(surface.height - layout.footerHeight);
	Fill(surface, 0, footerY, (int)surface.width, (int)layout.footerHeight, BAR);
}

void DrawTileBrowser(const TileSurface &surface, const TileLayout &layout,
		     const char *pTitle, const char *pDirectory,
		     const TileEntry *pEntries, unsigned count,
		     unsigned selected, unsigned firstRow)
{
	const TFont &titleFont = *layout.pTitleFont;
	const TFont &textFont = *layout.pTextFont;
	const TFont &captionFont = *layout.pCaptionFont;
	DrawBars(surface, layout);

	// Header: title, folder and position.
	const int titleY = ((int)layout.headerHeight - 2 - (int)titleFont.height) / 2;
	DrawText(surface, 6, titleY, TEXT, pTitle, titleFont);
	char counter[24];
	unsigned n = count ? selected + 1 : 0;
	char digits[12];
	unsigned length = 0;
	do { digits[length++] = (char)('0' + n % 10); n /= 10; } while (n && length < sizeof digits);
	unsigned out = 0;
	while (length) counter[out++] = digits[--length];
	counter[out++] = '/';
	n = count;
	do { digits[length++] = (char)('0' + n % 10); n /= 10; } while (n && length < sizeof digits);
	while (length) counter[out++] = digits[--length];
	counter[out] = 0;
	const unsigned titleEnd = 6 + TextWidth(titleFont, pTitle) + 12;
	const unsigned counterWidth = TextWidth(textFont, counter);
	const int infoY = ((int)layout.headerHeight - 2 - (int)textFont.height) / 2;
	if (pDirectory && pDirectory[0] && surface.width > titleEnd + counterWidth + 24)
	{
		char folder[160];
		folder[0] = '/';
		strncpy(folder + 1, pDirectory, sizeof folder - 2);
		folder[sizeof folder - 1] = 0;
		const unsigned folderWidth = surface.width - titleEnd - counterWidth - 18;
		char fitted[160];
		FitText(fitted, sizeof fitted, folder, folderWidth, textFont);
		DrawText(surface, (int)(surface.width - 6 - counterWidth - 12 - TextWidth(textFont, fitted)),
			 infoY, MUTED, fitted, textFont);
	}
	DrawText(surface, (int)(surface.width - 6 - counterWidth), infoY, MUTED, counter, textFont);

	const int footerY = (int)(surface.height - layout.footerHeight);
	const int footerTextY = footerY + ((int)layout.footerHeight - (int)textFont.height) / 2;

	if (count == 0)
	{
		const int y = (int)(layout.gridY + (surface.height - layout.gridY - layout.footerHeight) / 2);
		DrawTextFitted(surface, 0, y - (int)LineHeight(titleFont), surface.width, TEXT,
			       "NO ROMS FOUND", titleFont, true);
		DrawTextFitted(surface, 0, y + 4, surface.width, MUTED,
			       "COPY YOUR GAMES TO THE SD CARD", textFont, true);
		return;
	}

	// Tiles
	for (unsigned row = 0; row < layout.rows; row++)
	{
		for (unsigned column = 0; column < layout.columns; column++)
		{
			const unsigned index = (firstRow + row) * layout.columns + column;
			if (index >= count)
			{
				break;
			}
			const TileEntry &entry = pEntries[index];
			const bool isSelected = index == selected;
			int x, y;
			TilePosition(layout, row, column, &x, &y);
			const int size = (int)layout.tileSize;

			if (isSelected)
			{
				Frame(surface, x - 4, y - 4, size + 8, size + 8, 3, HIGHLIGHT);
			}
			else
			{
				Frame(surface, x - 1, y - 1, size + 2, size + 2, 1, OUTLINE);
			}

			if (entry.isDirectory)
			{
				DrawFolder(surface, x, y, layout.tileSize);
			}
			else if (entry.pPicture)
			{
				Blit(surface, x, y, layout.tileSize, layout.tileSize, entry.pPicture);
			}
			else
			{
				// Picture still loading: system name on the tile.
				Fill(surface, x, y, size, size, TILE_BACKGROUND);
				DrawTextFitted(surface, x, y + (size - (int)titleFont.height) / 2, layout.tileSize,
					       SystemAccent(entry), entry.system ? entry.system : "", titleFont, true);
			}

			char name[160];
			CaptionName(name, sizeof name, entry);
			DrawTextFitted(surface, x, y + size + 6, layout.tileSize,
				       isSelected ? TEXT : MUTED, name, captionFont, true);
		}
	}

	// More rows above or below
	const unsigned totalRows = (count + layout.columns - 1) / layout.columns;
	const int arrowX = (int)surface.width - (int)layout.gapX / 2 - 1;
	const int arrowSize = (int)layout.gapX / 3 > 3 ? (int)layout.gapX / 3 : 3;
	if (firstRow > 0)
	{
		DrawTriangle(surface, arrowX, (int)layout.gridY + 6, arrowSize, true, MUTED);
	}
	if (firstRow + layout.rows < totalRows)
	{
		DrawTriangle(surface, arrowX, footerY - 6 - arrowSize, arrowSize, false, MUTED);
	}

	// Footer: full name and system of the selected entry
	if (selected < count)
	{
		const TileEntry &entry = pEntries[selected];
		const char *pKind = entry.isDirectory ? "FOLDER" : (entry.system ? entry.system : "");
		const unsigned kindWidth = TextWidth(textFont, pKind);
		char name[160];
		DisplayName(name, sizeof name, entry);
		DrawTextFitted(surface, 6, footerTextY, surface.width - kindWidth - 24, TEXT, name, textFont, false);
		DrawText(surface, (int)(surface.width - 6 - kindWidth), footerTextY, SystemAccent(entry), pKind, textFont);
	}
}

void DrawTileLaunch(const TileSurface &surface, const TileLayout &layout,
		    const TileEntry &entry, const char *pCoreName)
{
	const TFont &titleFont = *layout.pTitleFont;
	const TFont &textFont = *layout.pTextFont;
	DrawBars(surface, layout);

	const int size = (int)layout.tileSize;
	const int x = ((int)surface.width - size) / 2;
	const int y = (int)layout.gridY + 14;
	Frame(surface, x - 4, y - 4, size + 8, size + 8, 3, HIGHLIGHT);
	if (entry.pPicture)
	{
		Blit(surface, x, y, layout.tileSize, layout.tileSize, entry.pPicture);
	}
	else
	{
		Fill(surface, x, y, size, size, TILE_BACKGROUND);
	}

	int textY = y + size + 12;
	DrawTextFitted(surface, 0, textY, surface.width, TEXT, "LOADING", titleFont, true);
	textY += (int)LineHeight(titleFont) + 2;
	char name[160];
	DisplayName(name, sizeof name, entry);
	DrawTextFitted(surface, 8, textY, surface.width - 16, MUTED, name, textFont, true);

	const int footerY = (int)(surface.height - layout.footerHeight);
	const int footerTextY = footerY + ((int)layout.footerHeight - (int)textFont.height) / 2;
	DrawTextFitted(surface, 0, footerTextY, surface.width, SystemAccent(entry), pCoreName ? pCoreName : "",
		       textFont, true);
}

// --- Pictures ------------------------------------------------------------------

// Scales a picture to fit the square tile (box filter), centred on the tile
// background; transparent pixels show the background.
template <typename TPixel>
static void ScaleToTile(unsigned width, unsigned height, unsigned tileSize, uint16_t *pTile, TPixel pixel)
{
	for (unsigned i = 0; i < tileSize * tileSize; i++)
	{
		pTile[i] = TILE_BACKGROUND;
	}

	const unsigned scaledW = width >= height ? tileSize : (unsigned)(((uint64_t)width * tileSize + height / 2) / height);
	const unsigned scaledH = height >= width ? tileSize : (unsigned)(((uint64_t)height * tileSize + width / 2) / width);
	const unsigned offsetX = (tileSize - (scaledW ? scaledW : 1)) / 2;
	const unsigned offsetY = (tileSize - (scaledH ? scaledH : 1)) / 2;
	const unsigned bgR = (TILE_BACKGROUND >> 11) << 3;
	const unsigned bgG = ((TILE_BACKGROUND >> 5) & 63) << 2;
	const unsigned bgB = (TILE_BACKGROUND & 31) << 3;

	for (unsigned dy = 0; dy < scaledH; dy++)
	{
		const unsigned y0 = (unsigned)((uint64_t)dy * height / scaledH);
		unsigned y1 = (unsigned)((uint64_t)(dy + 1) * height / scaledH);
		if (y1 <= y0) y1 = y0 + 1;
		for (unsigned dx = 0; dx < scaledW; dx++)
		{
			const unsigned x0 = (unsigned)((uint64_t)dx * width / scaledW);
			unsigned x1 = (unsigned)((uint64_t)(dx + 1) * width / scaledW);
			if (x1 <= x0) x1 = x0 + 1;

			uint32_t r = 0, g = 0, b = 0, a = 0;
			for (unsigned sy = y0; sy < y1; sy++)
			{
				for (unsigned sx = x0; sx < x1; sx++)
				{
					unsigned pr, pg, pb, pa;
					pixel(sx, sy, &pr, &pg, &pb, &pa);
					r += pr * pa;
					g += pg * pa;
					b += pb * pa;
					a += pa;
				}
			}
			const uint32_t count = (y1 - y0) * (x1 - x0);
			const uint32_t full = 255 * count;
			r = (r + bgR * (full - a)) / full;
			g = (g + bgG * (full - a)) / full;
			b = (b + bgB * (full - a)) / full;
			pTile[(offsetY + dy) * tileSize + offsetX + dx] = TILE_RGB565(r, g, b);
		}
	}
}

bool MakeTilePictureFromPng(const uint8_t *pData, size_t size, unsigned tileSize, uint16_t *pTile)
{
	int width, height, components;
	if (!pData || size == 0 || size > 0x7FFFFFFF
	    || !stbi_info_from_memory(pData, (int)size, &width, &height, &components)
	    || width <= 0 || height <= 0 || (uint64_t)width * height > MAX_PICTURE_PIXELS)
	{
		return false;
	}

	unsigned char *pRgba = stbi_load_from_memory(pData, (int)size, &width, &height, &components, 4);
	if (!pRgba)
	{
		return false;
	}
	const unsigned w = (unsigned)width;
	ScaleToTile((unsigned)width, (unsigned)height, tileSize, pTile,
		[pRgba, w](unsigned x, unsigned y, unsigned *r, unsigned *g, unsigned *b, unsigned *a)
		{
			const unsigned char *p = pRgba + ((size_t)y * w + x) * 4;
			*r = p[0]; *g = p[1]; *b = p[2]; *a = p[3];
		});
	stbi_image_free(pRgba);
	return true;
}

void MakeTilePictureFromRgb565(const uint16_t *pPixels, unsigned width, unsigned height,
			       unsigned tileSize, uint16_t *pTile)
{
	ScaleToTile(width, height, tileSize, pTile,
		[pPixels, width](unsigned x, unsigned y, unsigned *r, unsigned *g, unsigned *b, unsigned *a)
		{
			const uint16_t p = pPixels[(size_t)y * width + x];
			*r = (p >> 11) * 255 / 31;
			*g = ((p >> 5) & 63) * 255 / 63;
			*b = (p & 31) * 255 / 31;
			*a = 255;
		});
}

const TSystemImage *FindSystemImage(const char *pSystem)
{
	for (unsigned i = 0; pSystem && i < g_SystemImageCount; i++)
	{
		if (strcmp(g_SystemImages[i].system, pSystem) == 0)
		{
			return &g_SystemImages[i];
		}
	}
	return 0;
}
