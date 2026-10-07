// Renders the ROM browser screens of kernel/tile_view.cpp on the host into
// PPM files, see tile-preview.mjs.
//
// usage: tile_preview <output dir> <cover.png>
#include "kernel/tile_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static std::vector<uint8_t> ReadFile(const char *pPath)
{
	std::vector<uint8_t> data;
	FILE *pFile = fopen(pPath, "rb");
	if (pFile)
	{
		uint8_t buffer[65536];
		size_t n;
		while ((n = fread(buffer, 1, sizeof buffer, pFile)) > 0)
		{
			data.insert(data.end(), buffer, buffer + n);
		}
		fclose(pFile);
	}
	return data;
}

static void WritePpm(const char *pDir, const char *pName, const std::vector<uint16_t> &pixels,
		     unsigned width, unsigned height)
{
	char path[512];
	snprintf(path, sizeof path, "%s/%s.ppm", pDir, pName);
	FILE *pFile = fopen(path, "wb");
	fprintf(pFile, "P6\n%u %u\n255\n", width, height);
	for (uint16_t p : pixels)
	{
		const uint8_t rgb[3] = {
			(uint8_t)((p >> 11) * 255 / 31),
			(uint8_t)(((p >> 5) & 63) * 255 / 63),
			(uint8_t)((p & 31) * 255 / 31),
		};
		fwrite(rgb, 1, 3, pFile);
	}
	fclose(pFile);
	printf("%s\n", path);
}

struct Game
{
	const char *name;
	const char *system;
	bool isDirectory;
	bool cover;
};

// Some names like those on a real SD card; the cover picture stands for a
// <rom name>.png next to the game.
static const Game Games[] = {
	{ "GBA Hacks", "", true, false },
	{ "007 - NightFire (USA, Europe) (En,Fr,De).gba", "GBA", false, false },
	{ "1942 (USA, Europe).gbc", "GBC", false, false },
	{ "3 Ninjas Kick Back (USA).sfc", "SNES", false, false },
	{ "Addams Family, The - Pugsley's Scavenger Hunt (USA, Europe).gb", "GB", false, false },
	{ "Aerial Assault (Europe).sms", "SMS", false, false },
	{ "Dinosaur's Tale, A (USA).md", "MD", false, false },
	{ "Pokemon - FireRed Version (USA).gba", "GBA", false, true },
	{ "Sonic Chaos (USA, Europe).gg", "GG", false, false },
	{ "Star Trek - 25th Anniversary (Germany).nes", "NES", false, false },
	{ "Virtua Racing Deluxe (USA).32x", "32X", false, false },
	{ "Zaxxon (Japan).sg", "SG", false, false },
};
static const unsigned GameCount = sizeof Games / sizeof Games[0];

struct Screen
{
	unsigned width;
	unsigned height;
	TileLayout layout;
	std::vector<uint16_t> pixels;
	std::vector<std::vector<uint16_t>> pictures;
	TileEntry entries[GameCount];

	Screen(unsigned w, unsigned h, const std::vector<uint8_t> &cover, bool loaded)
	:	width(w), height(h), layout(ComputeTileLayout(w, h)), pixels(w * h), pictures(GameCount)
	{
		const unsigned tile = layout.tileSize;
		for (unsigned i = 0; i < GameCount; i++)
		{
			entries[i].name = Games[i].name;
			entries[i].system = Games[i].system;
			entries[i].isDirectory = Games[i].isDirectory;
			entries[i].pPicture = 0;
			if (!loaded || Games[i].isDirectory)
			{
				continue;
			}
			pictures[i].resize(tile * tile);
			bool ok = false;
			if (Games[i].cover)
			{
				ok = MakeTilePictureFromPng(cover.data(), cover.size(), tile, pictures[i].data());
			}
			const TSystemImage *pImage = FindSystemImage(Games[i].system);
			if (!ok && pImage)
			{
				MakeTilePictureFromRgb565(pImage->pixels, pImage->width, pImage->height, tile, pictures[i].data());
				ok = true;
			}
			entries[i].pPicture = ok ? pictures[i].data() : 0;
		}
	}

	TileSurface Surface(void)
	{
		TileSurface surface = { pixels.data(), width, height, width };
		return surface;
	}
};

int main(int argc, char **argv)
{
	if (argc < 3)
	{
		fprintf(stderr, "usage: tile_preview <output dir> <cover.png>\n");
		return 2;
	}
	const char *pDir = argv[1];
	const std::vector<uint8_t> cover = ReadFile(argv[2]);
	if (cover.empty())
	{
		fprintf(stderr, "cannot read %s\n", argv[2]);
		return 1;
	}

	{
		Screen screen(320, 240, cover, true);
		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "", screen.entries, GameCount, 1, 0);
		WritePpm(pDir, "gpi-browser", screen.pixels, screen.width, screen.height);

		const unsigned selected = 7;
		const unsigned first = TileFirstRow(screen.layout, selected, 0);
		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "GBA", screen.entries, GameCount, selected, first);
		WritePpm(pDir, "gpi-browser-scrolled", screen.pixels, screen.width, screen.height);

		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "", screen.entries, 0, 0, 0);
		WritePpm(pDir, "gpi-browser-empty", screen.pixels, screen.width, screen.height);

		DrawTileLaunch(screen.Surface(), screen.layout, screen.entries[7], "gpSP");
		WritePpm(pDir, "gpi-launch", screen.pixels, screen.width, screen.height);
	}
	{
		Screen screen(320, 240, cover, false);
		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "", screen.entries, GameCount, 3, 0);
		WritePpm(pDir, "gpi-browser-loading", screen.pixels, screen.width, screen.height);
	}
	{
		Screen screen(1280, 720, cover, true);
		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "", screen.entries, GameCount, 2, 0);
		WritePpm(pDir, "hdmi-browser", screen.pixels, screen.width, screen.height);
	}
	{
		// GPi Case 2 (CM4)
		Screen screen(640, 480, cover, true);
		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "", screen.entries, GameCount, 2, 0);
		WritePpm(pDir, "gpi2-browser", screen.pixels, screen.width, screen.height);
	}
	{
		// Pi 5 on a 1080p TV
		Screen screen(1920, 1080, cover, true);
		DrawTileBrowser(screen.Surface(), screen.layout, "MINERVA CONSOLE", "", screen.entries, GameCount, 2, 0);
		WritePpm(pDir, "rpi5-browser", screen.pixels, screen.width, screen.height);
	}
	return 0;
}
