// Smoke test driver: the real LibretroRunner and the real (isolated) cores,
// with the Circle platform classes replaced by stubs that record what the
// core produces. Built for the host (host-smoke.mjs) and for ARM Linux under
// qemu-arm (arm-smoke.mjs).
//
// usage: smoke <frames> <rom> [rom ...]
#include "libretro/libretro_runner.h"
#include "platform/circle/circle_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_Frames, g_DupFrames, g_Width, g_Height, g_Format = 99;
static unsigned long g_AudioFrames;
static float g_Aspect;
static unsigned g_FirstPixel, g_CenterPixel;

CircleLog::CircleLog(void) : m_pLogger(0) {}
void CircleLog::Init(CLogger *) {}
void CircleLog::Notice(const char *p) { if (getenv("SMOKE_VERBOSE")) printf("  [notice] %s\n", p); }
void CircleLog::Warn(const char *p) { printf("  [warn] %s\n", p); }
void CircleLog::Error(const char *p) { printf("  [error] %s\n", p); }

CircleVideo::CircleVideo(void) {}
CircleVideo::~CircleVideo(void) {}
void CircleVideo::Init(CScreenDevice *) {}
bool CircleVideo::SetPixelFormat(enum retro_pixel_format format) { g_Format = format; return true; }
void CircleVideo::SetDisplayAspectRatio(float aspect) { g_Aspect = aspect; }
bool CircleVideo::SubmitFrame(const void *frame, unsigned width, unsigned height, size_t pitch)
{
	if (!frame)
	{
		g_DupFrames++;
		return true;
	}
	const unsigned char *p = (const unsigned char *)frame;
	g_Frames++;
	g_Width = width;
	g_Height = height;
	g_FirstPixel = *(const unsigned short *)p;
	g_CenterPixel = *(const unsigned short *)(p + (height / 2) * pitch + (width / 2) * 2);
	return true;
}

CircleAudio::CircleAudio(void) : m_pSound(0) {}
void CircleAudio::WriteSample(int16_t, int16_t) { g_AudioFrames++; }
size_t CircleAudio::WriteFrames(const int16_t *, size_t frames) { g_AudioFrames += frames; return frames; }

// Pad buttons per frame from SMOKE_BUTTONS="mask@frames,mask@frames,...":
// each RetroPad mask (C notation, e.g. 0x204 = Select+X) is held for the
// given number of frames, the last one until the end.
static unsigned g_SmokeFrame;

static unsigned ScheduledButtons(unsigned frame)
{
	const char *pSchedule = getenv("SMOKE_BUTTONS");
	unsigned mask = 0;
	while (pSchedule && *pSchedule)
	{
		char *pEnd;
		mask = (unsigned)strtoul(pSchedule, &pEnd, 0);
		unsigned frames = ~0U;
		if (*pEnd == '@')
		{
			frames = (unsigned)strtoul(pEnd + 1, &pEnd, 0);
		}
		if (frame < frames)
		{
			break;
		}
		frame -= frames;
		pSchedule = *pEnd == ',' ? pEnd + 1 : 0;
	}
	return mask;
}

CircleInput::CircleInput(void) {}
void CircleInput::Poll(void) {}
int16_t CircleInput::State(unsigned port, unsigned device, unsigned, unsigned id) const
{
	if (port != 0 || device != RETRO_DEVICE_JOYPAD)
	{
		return 0;
	}
	const unsigned mask = ScheduledButtons(g_SmokeFrame);
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
	{
		return (int16_t)mask;
	}
	return id < 16 ? (int16_t)((mask >> id) & 1) : 0;
}

#ifndef SMOKE_FATFS
// Linux file access; with SMOKE_FATFS the real platform/circle/circle_fs.cpp
// is linked instead.
CircleFs::CircleFs(void) : m_Mounted(true), m_pLog(0) {}
bool CircleFs::FileExists(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		return false;
	}
	fclose(f);
	return true;
}
extern "C" int ra_smoke_write_file(const char *path, const void *data, unsigned long size);
bool CircleFs::WriteWholeFile(const char *path, const void *pData, size_t size)
{
	return ra_smoke_write_file(path, pData, size) != 0;
}
bool CircleFs::ReadWholeFile(const char *path, uint8_t **ppData, size_t *pSize, size_t maxSize)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size <= 0 || (size_t)size > maxSize)
	{
		fclose(f);
		return false;
	}
	*ppData = new uint8_t[size];
	*pSize = fread(*ppData, 1, size, f);
	fclose(f);
	return *pSize == (size_t)size;
}
bool CircleFs::FindFirstWithExtension(const char *, char *, size_t) { return false; }
#endif

#ifdef SMOKE_RUN_CONSTRUCTORS
// newlib's linux-crt0 calls main() without running static constructors,
// which Circle's sysinit does on the device (Gambatte needs them).
extern "C" void __libc_init_array(void);
#endif

// Calls of ra_libc_clear_cache() (arm_*.c test shims): a dynarec core that
// shows "syncs=0" would run stale code on the device.
extern "C" unsigned long ra_smoke_cache_syncs __attribute__((weak));


static unsigned Rgb(unsigned p)
{
	return (((p >> 11) & 31) << 19) | (((p >> 5) & 63) << 10) | ((p & 31) << 3);
}

static void ResetCounters(void)
{
	g_Frames = g_DupFrames = g_Width = g_Height = 0;
	g_Format = 99;
	g_AudioFrames = 0;
	g_Aspect = 0.0f;
	g_FirstPixel = g_CenterPixel = 0;
}

// Runs every ROM with the same runner, one after another (Init, frames,
// Shutdown), like the kernel does when a game is ended with Start+Select.
int main(int argc, char **argv)
{
	if (argc < 3)
	{
		printf("usage: smoke <frames> <rom> [rom ...]\n");
		return 2;
	}
	const int frames = atoi(argv[1]);
#ifdef SMOKE_RUN_CONSTRUCTORS
	__libc_init_array();
#endif
	CircleLog log;
	CircleVideo video;
	CircleAudio audio;
	CircleInput input;
	CircleFs fs;
#ifdef SMOKE_FATFS
	if (!fs.Mount(&log))
	{
		printf("FatFs mount of SD: failed\n");
		return 1;
	}
#endif
	LibretroRunner runner(&log, 0, &video, &audio, &input, &fs);

	int failed = 0;
	for (int arg = 2; arg < argc; arg++)
	{
		const char *path = argv[arg];
		const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
		const LibretroCore *core = LibretroFindCoreForPath(path);
		const unsigned long syncsBefore = &ra_smoke_cache_syncs ? ra_smoke_cache_syncs : 0UL;
		ResetCounters();
		if (!core || !runner.Init(core, path))
		{
			printf("%-9s %-4s %-11s INIT FAILED\n", name, LibretroSystemForPath(path), core ? core->name : "-");
			runner.Shutdown();
			failed = 1;
			continue;
		}
		for (int i = 0; i < frames; i++)
		{
			g_SmokeFrame = (unsigned)i;
			runner.RunFrame();
		}
		const unsigned long syncs = (&ra_smoke_cache_syncs ? ra_smoke_cache_syncs : 0UL) - syncsBefore;
		printf("%-9s %-4s %-11s %ux%u aspect=%.3f format=%u first=#%06x center=#%06x frames=%u+%u audio=%lu@%u syncs=%lu\n",
		       name, LibretroSystemForPath(path), core->name, g_Width, g_Height, g_Aspect, g_Format,
		       Rgb(g_FirstPixel), Rgb(g_CenterPixel), g_Frames, g_DupFrames, g_AudioFrames, runner.SampleRate(),
		       syncs);
		fflush(stdout);
		runner.Shutdown();
	}
	return failed;
}
