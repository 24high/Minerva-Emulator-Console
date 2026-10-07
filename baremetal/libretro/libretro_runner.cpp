#include "libretro_runner.h"

#include "platform/circle/circle_platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef RA_BAREMETAL_CORE_BUNDLE
#include "libc/circle_bridge.h"
#endif

#ifndef RA_BAREMETAL_MAX_ROM_SIZE
#define RA_BAREMETAL_MAX_ROM_SIZE (16 * 1024 * 1024)
#endif

#define RA_ENVIRONMENT_BAREMETAL_PROGRESS (RETRO_ENVIRONMENT_PRIVATE | 0x4242)

// Core options in RetroArch's format (key = "value"), read at core start.
static const char OPTIONS_FILE[] = "minerva.cfg";

// Saves are checked once a second and written when the game has stopped
// changing them for one check (it is done saving), or after 30 checks if a
// game keeps changing its save memory all the time.
static const unsigned SAVE_CHECK_FRAMES = 60;
static const unsigned SAVE_MAX_CHANGED_CHECKS = 30;
// A save file left behind by a write that was interrupted (power loss).
static const char SAVE_TEMP_SUFFIX[] = ".tmp";

#define DECLARE_LIBRETRO_CORE(prefix) \
extern "C" { \
void prefix##_retro_init(void); \
void prefix##_retro_deinit(void); \
unsigned prefix##_retro_api_version(void); \
void prefix##_retro_get_system_info(struct retro_system_info *info); \
void prefix##_retro_get_system_av_info(struct retro_system_av_info *info); \
void prefix##_retro_set_environment(retro_environment_t cb); \
void prefix##_retro_set_video_refresh(retro_video_refresh_t cb); \
void prefix##_retro_set_audio_sample(retro_audio_sample_t cb); \
void prefix##_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb); \
void prefix##_retro_set_input_poll(retro_input_poll_t cb); \
void prefix##_retro_set_input_state(retro_input_state_t cb); \
void prefix##_retro_run(void); \
bool prefix##_retro_load_game(const struct retro_game_info *game); \
void prefix##_retro_unload_game(void); \
void *prefix##_retro_get_memory_data(unsigned id); \
size_t prefix##_retro_get_memory_size(unsigned id); \
}

#define LIBRETRO_CORE_ENTRY_EX(displayName, prefix, maxSize, aspect, options) \
{ \
	displayName, maxSize, false, \
	prefix##_retro_init, \
	prefix##_retro_deinit, \
	prefix##_retro_api_version, \
	prefix##_retro_get_system_info, \
	prefix##_retro_get_system_av_info, \
	prefix##_retro_set_environment, \
	prefix##_retro_set_video_refresh, \
	prefix##_retro_set_audio_sample, \
	prefix##_retro_set_audio_sample_batch, \
	prefix##_retro_set_input_poll, \
	prefix##_retro_set_input_state, \
	prefix##_retro_run, \
	prefix##_retro_load_game, \
	prefix##_retro_unload_game, \
	prefix##_retro_get_memory_data, \
	prefix##_retro_get_memory_size, \
	aspect, options \
}

#define LIBRETRO_CORE_ENTRY(displayName, prefix, maxSize, hasN64Options) \
{ \
	displayName, maxSize, hasN64Options, \
	prefix##_retro_init, \
	prefix##_retro_deinit, \
	prefix##_retro_api_version, \
	prefix##_retro_get_system_info, \
	prefix##_retro_get_system_av_info, \
	prefix##_retro_set_environment, \
	prefix##_retro_set_video_refresh, \
	prefix##_retro_set_audio_sample, \
	prefix##_retro_set_audio_sample_batch, \
	prefix##_retro_set_input_poll, \
	prefix##_retro_set_input_state, \
	prefix##_retro_run, \
	prefix##_retro_load_game, \
	prefix##_retro_unload_game, \
	prefix##_retro_get_memory_data, \
	prefix##_retro_get_memory_size \
}

#ifdef RA_BAREMETAL_MULTI
DECLARE_LIBRETRO_CORE(fceumm)
DECLARE_LIBRETRO_CORE(n64)

static const LibretroCore g_FceummCore = LIBRETRO_CORE_ENTRY("FCEUmm", fceumm, 0x1000000, false);
static const LibretroCore g_N64Core = LIBRETRO_CORE_ENTRY("Mupen64Plus-Next", n64, 0x4000000, true);
#elif defined(RA_BAREMETAL_CORE_BUNDLE)
// Cores pre-linked and isolated by baremetal/build.mjs (--core=all).
DECLARE_LIBRETRO_CORE(fceumm)
DECLARE_LIBRETRO_CORE(gambatte)
DECLARE_LIBRETRO_CORE(snes9x2002)
DECLARE_LIBRETRO_CORE(picodrive)
DECLARE_LIBRETRO_CORE(gpsp)

// Console games fill the 4:3 screen like on a TV; handheld systems keep the
// picture shape their core reports (see LibretroSystem::handheld).
static const float ASPECT_4_3 = 4.0f / 3.0f;

// Original Game Boy games in the green DMG palette instead of grey.
static const LibretroOption g_GambatteOptions[] = {
	{ "gambatte_gb_colorization", "internal" },
	{ "gambatte_gb_internal_palette", "GB - DMG" },
	{ 0, 0 }
};

static const LibretroCore g_FceummCore = LIBRETRO_CORE_ENTRY_EX("FCEUmm", fceumm, 0x1000000, ASPECT_4_3, 0);
static const LibretroCore g_GambatteCore = LIBRETRO_CORE_ENTRY_EX("Gambatte", gambatte, 0x1000000, 0.0f, g_GambatteOptions);
static const LibretroCore g_Snes9x2002Core = LIBRETRO_CORE_ENTRY_EX("Snes9x 2002", snes9x2002, 0x1000000, ASPECT_4_3, 0);
// The 6-button pad, so the games made for it get X, Y and Z (it works with
// almost all 3-button games too; "3 button pad" in minerva.cfg for the rest).
static const LibretroOption g_PicoDriveOptions[] = {
	{ "picodrive_input1", "6 button pad" },
	{ 0, 0 }
};

static const LibretroCore g_PicoDriveCore = LIBRETRO_CORE_ENTRY_EX("PicoDrive", picodrive, 0x1000000, ASPECT_4_3, g_PicoDriveOptions);
static const LibretroCore g_GpspCore = LIBRETRO_CORE_ENTRY_EX("gpSP", gpsp, 0x2000000, 0.0f, 0);

// Button layouts for the GPi Case, which has A, B, X and Y only (see
// button_mapper.h). Systems with 2 buttons use A and B as they are. The
// shoulder buttons L and R are on Y and X: directly on the GBA, which has 4
// buttons, and together with Select on systems with more than 4 buttons.
static const LibretroButtonRemap g_ShoulderButtons[] = {
	{ RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L },
	{ RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_R },
	{ BUTTON_REMAP_END, 0 }
};

// GBA: A, B, L, R. gpSP would use X and Y as turbo A and turbo B.
static const LibretroButtonLayout g_GbaButtons = { g_ShoulderButtons, 0 };

// SNES: A, B, X, Y as labelled, Select+Y = L, Select+X = R.
// Mega Drive 6-button pad: PicoDrive has A, B, C on Y, B, A, Y on X and X, Z
// on L, R, so Select+Y = X, Select+X = Z.
static const LibretroButtonLayout g_SixButtons = { 0, g_ShoulderButtons };

struct LibretroSystem
{
	const char *extension;
	const char *name;
	const LibretroCore *core;
	bool handheld;	// keep the core's aspect ratio instead of core->displayAspect
	const LibretroButtonLayout *buttons;	// 0: buttons as they are
};

static const LibretroSystem g_Systems[] = {
	{ "nes", "NES",  &g_FceummCore,      false, 0 },
	{ "gb",  "GB",   &g_GambatteCore,    true,  0 },
	{ "dmg", "GB",   &g_GambatteCore,    true,  0 },
	{ "gbc", "GBC",  &g_GambatteCore,    true,  0 },
	{ "gba", "GBA",  &g_GpspCore,        true,  &g_GbaButtons },
	{ "agb", "GBA",  &g_GpspCore,        true,  &g_GbaButtons },
	{ "sfc", "SNES", &g_Snes9x2002Core,  false, &g_SixButtons },
	{ "smc", "SNES", &g_Snes9x2002Core,  false, &g_SixButtons },
	{ "swc", "SNES", &g_Snes9x2002Core,  false, &g_SixButtons },
	{ "fig", "SNES", &g_Snes9x2002Core,  false, &g_SixButtons },
	{ "md",  "MD",   &g_PicoDriveCore,   false, &g_SixButtons },
	{ "gen", "MD",   &g_PicoDriveCore,   false, &g_SixButtons },
	{ "smd", "MD",   &g_PicoDriveCore,   false, &g_SixButtons },
	{ "bin", "MD",   &g_PicoDriveCore,   false, &g_SixButtons },
	{ "32x", "32X",  &g_PicoDriveCore,   false, &g_SixButtons },
	{ "sms", "SMS",  &g_PicoDriveCore,   false, 0 },
	{ "gg",  "GG",   &g_PicoDriveCore,   true,  0 },
	{ "sg",  "SG",   &g_PicoDriveCore,   false, 0 },
	{ 0, 0, 0, false, 0 }
};
#else
extern "C" {
void retro_init(void);
void retro_deinit(void);
unsigned retro_api_version(void);
void retro_get_system_info(struct retro_system_info *info);
void retro_get_system_av_info(struct retro_system_av_info *info);
void retro_set_environment(retro_environment_t cb);
void retro_set_video_refresh(retro_video_refresh_t cb);
void retro_set_audio_sample(retro_audio_sample_t cb);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void retro_set_input_poll(retro_input_poll_t cb);
void retro_set_input_state(retro_input_state_t cb);
void retro_run(void);
bool retro_load_game(const struct retro_game_info *game);
void retro_unload_game(void);
void *retro_get_memory_data(unsigned id);
size_t retro_get_memory_size(unsigned id);
}

static const LibretroCore g_DefaultCore = {
	"libretro", RA_BAREMETAL_MAX_ROM_SIZE,
#ifdef RA_BAREMETAL_N64
	true,
#else
	false,
#endif
	retro_init,
	retro_deinit,
	retro_api_version,
	retro_get_system_info,
	retro_get_system_av_info,
	retro_set_environment,
	retro_set_video_refresh,
	retro_set_audio_sample,
	retro_set_audio_sample_batch,
	retro_set_input_poll,
	retro_set_input_state,
	retro_run,
	retro_load_game,
	retro_unload_game,
	retro_get_memory_data,
	retro_get_memory_size
};
#endif

LibretroRunner *LibretroRunner::s_pActive = 0;

static bool StringEndsWith(const char *value, const char *suffix)
{
	if (!value || !suffix)
	{
		return false;
	}

	const size_t valueLen = strlen(value);
	const size_t suffixLen = strlen(suffix);
	if (suffixLen > valueLen)
	{
		return false;
	}

	return strcmp(value + valueLen - suffixLen, suffix) == 0;
}

static char ToLowerAscii(char c)
{
	return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static bool StringEndsWithNoCase(const char *value, const char *suffix)
{
	if (!value || !suffix)
	{
		return false;
	}

	const size_t valueLen = strlen(value);
	const size_t suffixLen = strlen(suffix);
	if (suffixLen > valueLen)
	{
		return false;
	}

	value += valueLen - suffixLen;
	while (*suffix)
	{
		if (ToLowerAscii(*value++) != ToLowerAscii(*suffix++))
		{
			return false;
		}
	}

	return true;
}

#ifdef RA_BAREMETAL_CORE_BUNDLE
static const LibretroSystem *FindSystem(const char *path)
{
	for (const LibretroSystem *pSystem = g_Systems; pSystem->extension; pSystem++)
	{
		char suffix[8] = ".";
		strncat(suffix, pSystem->extension, sizeof suffix - 2);
		if (StringEndsWithNoCase(path, suffix))
		{
			return pSystem;
		}
	}
	return 0;
}
#endif

const LibretroCore *LibretroDefaultCore(void)
{
#if defined(RA_BAREMETAL_MULTI) || defined(RA_BAREMETAL_CORE_BUNDLE)
	return &g_FceummCore;
#else
	return &g_DefaultCore;
#endif
}

const LibretroCore *LibretroFindCoreForPath(const char *path)
{
#ifdef RA_BAREMETAL_CORE_BUNDLE
	const LibretroSystem *pSystem = FindSystem(path);
	return pSystem ? pSystem->core : 0;
#elif defined(RA_BAREMETAL_MULTI)
	if (StringEndsWithNoCase(path, ".nes"))
	{
		return &g_FceummCore;
	}
	if (StringEndsWithNoCase(path, ".z64") ||
	    StringEndsWithNoCase(path, ".n64") ||
	    StringEndsWithNoCase(path, ".v64"))
	{
		return &g_N64Core;
	}
	return 0;
#else
#ifdef RA_BAREMETAL_N64
	if (StringEndsWithNoCase(path, ".z64") ||
	    StringEndsWithNoCase(path, ".n64") ||
	    StringEndsWithNoCase(path, ".v64"))
	{
		return &g_DefaultCore;
	}
	return 0;
#elif defined(RA_BAREMETAL_FCEUMM)
	if (StringEndsWithNoCase(path, ".nes"))
	{
		return &g_DefaultCore;
	}
	return 0;
#else
	(void)path;
	return &g_DefaultCore;
#endif
#endif
}

const char *LibretroSystemForPath(const char *path)
{
#ifdef RA_BAREMETAL_CORE_BUNDLE
	const LibretroSystem *pSystem = FindSystem(path);
	return pSystem ? pSystem->name : "";
#else
	const LibretroCore *pCore = LibretroFindCoreForPath(path);
	if (!pCore)
	{
		return "";
	}
	return pCore->n64Options ? "N64" : "NES";
#endif
}

static bool GetN64Variable(struct retro_variable *var)
{
	if (!var || !var->key)
	{
		return false;
	}

	if (StringEndsWith(var->key, "-rdp-plugin"))
	{
		var->value = "angrylion";
		return true;
	}
	if (StringEndsWith(var->key, "-rsp-plugin"))
	{
		var->value = "cxd4";
		return true;
	}
	if (StringEndsWith(var->key, "-cpucore"))
	{
		var->value = "dynamic_recompiler";
		return true;
	}
	if (StringEndsWith(var->key, "-ThreadedRenderer"))
	{
		var->value = "False";
		return true;
	}
	if (StringEndsWith(var->key, "-angrylion-vioverlay"))
	{
		var->value = "Unfiltered";
		return true;
	}
	if (StringEndsWith(var->key, "-angrylion-sync"))
	{
		var->value = "Low";
		return true;
	}
	if (StringEndsWith(var->key, "-angrylion-multithread"))
	{
		var->value = "4";
		return true;
	}
	if (StringEndsWith(var->key, "-angrylion-overscan"))
	{
		var->value = "disabled";
		return true;
	}
	if (StringEndsWith(var->key, "-43screensize"))
	{
		var->value = "320x240";
		return true;
	}
	if (StringEndsWith(var->key, "-FrameDuping"))
	{
		var->value = "False";
		return true;
	}
	if (StringEndsWith(var->key, "-Framerate"))
	{
		var->value = "Fullspeed";
		return true;
	}
	if (StringEndsWith(var->key, "-virefresh"))
	{
		var->value = "Auto";
		return true;
	}
	if (StringEndsWith(var->key, "-CountPerOp"))
	{
		var->value = "1";
		return true;
	}
	if (StringEndsWith(var->key, "-CountPerOpDenomPot"))
	{
		var->value = "0";
		return true;
	}
	if (StringEndsWith(var->key, "-pak1"))
	{
		var->value = "memory";
		return true;
	}
	if (StringEndsWith(var->key, "-astick-deadzone"))
	{
		var->value = "12";
		return true;
	}
	if (StringEndsWith(var->key, "-astick-sensitivity"))
	{
		var->value = "100";
		return true;
	}

	return false;
}

LibretroRunner::LibretroRunner(CircleLog *pLog,
                               CircleTimer *pTimer,
                               CircleVideo *pVideo,
                               CircleAudio *pAudio,
                               CircleInput *pInput,
                               CircleFs *pFs)
:	m_pLog(pLog),
	m_pTimer(pTimer),
	m_pVideo(pVideo),
	m_pAudio(pAudio),
	m_pInput(pInput),
	m_pFs(pFs),
	m_pCore(0),
	m_CoreInitialized(false),
	m_GameLoaded(false),
	m_MappedButtons(0),
	m_pProgressContext(0),
	m_pProgressCallback(0),
	m_PixelFormat(RETRO_PIXEL_FORMAT_RGB565),
	m_SupportsNoGame(false),
	m_pContentOverrides(0),
	m_CoreOptionCount(0),
	m_LegacyDefaultsUsed(0),
	m_FileOptionCount(0),
	m_pRomData(0),
	m_RomSize(0),
	m_SaveCheckCountdown(SAVE_CHECK_FRAMES)
{
	static const SaveSlot Slots[SAVE_SLOTS] = {
		{ RETRO_MEMORY_SAVE_RAM, ".srm", false, false, false, 0, 0, 0, 0 },
		{ RETRO_MEMORY_RTC,      ".rtc", true,  false, false, 0, 0, 0, 0 },
	};
	memcpy(m_Saves, Slots, sizeof(m_Saves));
	memset(m_SaveBase, 0, sizeof(m_SaveBase));
	memset(&m_AvInfo, 0, sizeof(m_AvInfo));
	memset(&m_GameInfoExt, 0, sizeof(m_GameInfoExt));
	memset(m_GameFullPath, 0, sizeof(m_GameFullPath));
	memset(m_GameDir, 0, sizeof(m_GameDir));
	memset(m_GameName, 0, sizeof(m_GameName));
	memset(m_GameExt, 0, sizeof(m_GameExt));
}

LibretroRunner::~LibretroRunner(void)
{
	Shutdown();
}

void LibretroRunner::Shutdown(void)
{
	if (m_pCore)
	{
		if (m_GameLoaded)
		{
			FlushSaves();
			m_pCore->unload_game();
		}
		if (m_CoreInitialized)
		{
			m_pCore->deinit();
		}
	}
	m_pCore = 0;
	m_CoreInitialized = false;
	m_GameLoaded = false;
	m_ButtonMapper.SetLayout(0);
	m_MappedButtons = 0;
	ReleaseSaves();

	delete[] m_pRomData;
	m_pRomData = 0;
	m_RomSize = 0;

	// Everything the previous core declared or set.
	m_PixelFormat = RETRO_PIXEL_FORMAT_RGB565;
	m_SupportsNoGame = false;
	m_pContentOverrides = 0;
	m_CoreOptionCount = 0;
	m_LegacyDefaultsUsed = 0;
	m_FileOptionCount = 0;
	memset(&m_AvInfo, 0, sizeof(m_AvInfo));
	memset(&m_GameInfoExt, 0, sizeof(m_GameInfoExt));
	memset(m_GameFullPath, 0, sizeof(m_GameFullPath));
	memset(m_GameDir, 0, sizeof(m_GameDir));
	memset(m_GameName, 0, sizeof(m_GameName));
	memset(m_GameExt, 0, sizeof(m_GameExt));
}

bool LibretroRunner::Init(const char *romPath)
{
	return Init(LibretroDefaultCore(), romPath);
}

void LibretroRunner::SetProgressCallback(void *pContext, TProgressCallback pCallback)
{
	m_pProgressContext = pContext;
	m_pProgressCallback = pCallback;
}

void LibretroRunner::Progress(const char *pMessage)
{
	if (m_pProgressCallback && pMessage)
	{
		m_pProgressCallback(m_pProgressContext, pMessage);
	}
	if (m_pLog && pMessage)
	{
		m_pLog->Notice(pMessage);
	}
}

void LibretroRunner::ProgressValue(const char *pLabel, unsigned nValue)
{
	char message[96];
	snprintf(message, sizeof(message), "%s%u", pLabel ? pLabel : "", nValue);
	Progress(message);
}

#ifdef RA_BAREMETAL_CORE_BUNDLE
// Override entries list extensions as "a|b|c"; the list ends with extensions == 0.
static const struct retro_system_content_info_override *FindContentOverride(
	const struct retro_system_content_info_override *pOverrides, const char *path)
{
	for (; pOverrides && pOverrides->extensions; pOverrides++)
	{
		const char *pExtension = pOverrides->extensions;
		while (*pExtension)
		{
			char suffix[16] = ".";
			size_t length = 1;
			while (*pExtension && *pExtension != '|' && length < sizeof(suffix) - 1)
			{
				suffix[length++] = *pExtension++;
			}
			suffix[length] = 0;
			if (StringEndsWithNoCase(path, suffix))
			{
				return pOverrides;
			}
			while (*pExtension && *pExtension != '|')
			{
				pExtension++;
			}
			if (*pExtension == '|')
			{
				pExtension++;
			}
		}
	}
	return 0;
}
#endif

bool LibretroRunner::Init(const LibretroCore *pCore, const char *romPath)
{
	Progress("runner: begin init");
	Shutdown();
	s_pActive = this;
	m_pCore = pCore ? pCore : LibretroDefaultCore();
	if (!m_pCore)
	{
		return false;
	}

	char coreMessage[64];
	snprintf(coreMessage, sizeof(coreMessage), "runner: core %s", m_pCore->name);
	Progress(coreMessage);

	if (m_pCore->api_version() != RETRO_API_VERSION && m_pLog)
	{
		m_pLog->Warn("Core reports an unexpected libretro API version");
	}

	LoadOptionsFile();
	if (m_pVideo)
	{
		// The previous core's format must not stay active for this one.
		m_pVideo->SetPixelFormat(m_PixelFormat);
	}

	Progress("runner: set callbacks");
	m_pCore->set_environment(EnvironmentThunk);
	m_pCore->set_video_refresh(VideoRefreshThunk);
	m_pCore->set_audio_sample(AudioSampleThunk);
	m_pCore->set_audio_sample_batch(AudioSampleBatchThunk);
	m_pCore->set_input_poll(InputPollThunk);
	m_pCore->set_input_state(InputStateThunk);

	Progress("runner: retro_init begin");
	m_pCore->init();
	m_CoreInitialized = true;
	Progress("runner: retro_init done");

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	Progress("runner: system_info begin");
	m_pCore->get_system_info(&info);
	Progress("runner: system_info done");

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));

	bool haveGame = false;
	char loadedPath[256];
	memset(loadedPath, 0, sizeof(loadedPath));

	// Cores with need_fullpath open the file themselves; in the bundle build
	// that works through the newlib/FatFs layer (baremetal/libc).
	bool needFullpath = false;
#ifdef RA_BAREMETAL_CORE_BUNDLE
	needFullpath = info.need_fullpath;
	const struct retro_system_content_info_override *pOverride =
		romPath ? FindContentOverride(m_pContentOverrides, romPath) : 0;
	if (pOverride)
	{
		needFullpath = pOverride->need_fullpath;
	}
#endif

	if (romPath && romPath[0] && m_pFs && needFullpath)
	{
		Progress("runner: core loads the ROM from its path");
		strncpy(loadedPath, romPath, sizeof(loadedPath) - 1);
		PrepareGameInfoExt(loadedPath);
		game.path = loadedPath;
		haveGame = true;
	}
	else if (romPath && romPath[0] && m_pFs)
	{
		Progress("runner: read ROM begin");
		haveGame = m_pFs->ReadWholeFile(romPath, &m_pRomData, &m_RomSize, m_pCore->maxRomSize);
		if (haveGame)
		{
			strncpy(loadedPath, romPath, sizeof(loadedPath) - 1);
			ProgressValue("runner: ROM bytes ", (unsigned)m_RomSize);
		}
		else
		{
			Progress("runner: selected ROM read failed");
#ifndef RA_BAREMETAL_CORE_BUNDLE
			// Single-core builds fall back to the first ROM in the SD root.
			const char *extensionsN64[] = { "z64", "n64", "v64", 0 };
			const char *extensionsNES[] = { "nes", 0 };
			const char **extensions = m_pCore->n64Options ? extensionsN64 : extensionsNES;
			char foundPath[256];
			for (unsigned i = 0; extensions[i] && !haveGame; i++)
			{
				if (m_pFs->FindFirstWithExtension(extensions[i], foundPath, sizeof(foundPath)))
				{
					if (m_pLog)
					{
						m_pLog->Notice(m_pCore->n64Options
							? "Falling back to first N64 file in SD root"
							: "Falling back to first NES file in SD root");
					}
					haveGame = m_pFs->ReadWholeFile(foundPath, &m_pRomData, &m_RomSize, m_pCore->maxRomSize);
					if (haveGame)
					{
						strncpy(loadedPath, foundPath, sizeof(loadedPath) - 1);
						ProgressValue("runner: fallback ROM bytes ", (unsigned)m_RomSize);
					}
				}
			}
#endif
		}

		if (haveGame)
		{
			Progress("runner: prepare game info");
			PrepareGameInfoExt(loadedPath);
			game.path = loadedPath;
			game.data = m_pRomData;
			game.size = m_RomSize;
			game.meta = 0;
		}
	}

	if (!haveGame && !m_SupportsNoGame)
	{
		if (m_pLog)
		{
			m_pLog->Error("No ROM loaded and core does not support no-game mode");
		}
		return false;
	}

	Progress("runner: retro_load_game begin");
	if (!m_pCore->load_game(haveGame ? &game : 0))
	{
		if (m_pLog)
		{
			m_pLog->Error("retro_load_game failed");
		}
		Progress("runner: retro_load_game failed");
		return false;
	}
	Progress("runner: retro_load_game done");
	m_GameLoaded = true;
	// Before the first frame, like RetroArch: some cores only report the
	// size of their save memory until the game has run.
	LoadSaves(romPath);
#ifdef RA_BAREMETAL_CORE_BUNDLE
	const LibretroSystem *pButtonSystem = romPath ? FindSystem(romPath) : 0;
	m_ButtonMapper.SetLayout(pButtonSystem ? pButtonSystem->buttons : 0);
#endif

#ifdef RA_BAREMETAL_CORE_BUNDLE
	// gpSP emits helper code while loading (BIOS division) that its own
	// cache syncs do not cover; make everything written so far fetchable.
	ra_libc_sync_code_caches();
#endif

	Progress("runner: av_info begin");
	m_pCore->get_system_av_info(&m_AvInfo);
	Progress("runner: av_info done");

	if (m_AvInfo.timing.fps < 1.0)
	{
		m_AvInfo.timing.fps = 60.0;
	}

	if (m_pVideo)
	{
		float aspect = m_pCore->displayAspect;
#ifdef RA_BAREMETAL_CORE_BUNDLE
		const LibretroSystem *pSystem = romPath ? FindSystem(romPath) : 0;
		if (pSystem && pSystem->handheld)
		{
			aspect = 0.0f;
		}
#endif
		m_pVideo->SetDisplayAspectRatio(aspect > 0.0f ? aspect : (float)m_AvInfo.geometry.aspect_ratio);
	}

	Progress("runner: init done");
	return true;
}

void LibretroRunner::PrepareGameInfoExt(const char *romPath)
{
	const char *path = romPath ? romPath : "GAME.NES";
	const char *base = path;
	const char *dot = 0;

	for (const char *p = path; *p; p++)
	{
		if (*p == '/' || *p == '\\')
		{
			base = p + 1;
		}
	}

	for (const char *p = base; *p; p++)
	{
		if (*p == '.')
		{
			dot = p;
		}
	}

	strncpy(m_GameFullPath, path, sizeof(m_GameFullPath) - 1);
	strncpy(m_GameDir, "/", sizeof(m_GameDir) - 1);

	if (dot && dot > base)
	{
		const size_t nameLen = (size_t)(dot - base) < sizeof(m_GameName) - 1
			? (size_t)(dot - base)
			: sizeof(m_GameName) - 1;
		memcpy(m_GameName, base, nameLen);
		m_GameName[nameLen] = 0;

		size_t extLen = 0;
		for (const char *p = dot + 1; *p && extLen < sizeof(m_GameExt) - 1; p++)
		{
			char c = *p;
			if (c >= 'A' && c <= 'Z')
			{
				c = (char)(c - 'A' + 'a');
			}
			m_GameExt[extLen++] = c;
		}
		m_GameExt[extLen] = 0;
	}
	else
	{
		strncpy(m_GameName, base, sizeof(m_GameName) - 1);
		m_GameExt[0] = 0;
	}

	m_GameInfoExt.full_path = m_GameFullPath;
	m_GameInfoExt.archive_path = 0;
	m_GameInfoExt.archive_file = 0;
	m_GameInfoExt.dir = m_GameDir;
	m_GameInfoExt.name = m_GameName;
	m_GameInfoExt.ext = m_GameExt;
	m_GameInfoExt.meta = 0;
	m_GameInfoExt.data = m_pRomData;
	m_GameInfoExt.size = m_RomSize;
	m_GameInfoExt.file_in_archive = false;
	m_GameInfoExt.persistent_data = true;
}

void LibretroRunner::RunFrame(void)
{
#ifndef RA_BAREMETAL_NO_USB
	if (m_ButtonMapper.IsActive() && m_pInput)
	{
		// Once per frame: the Select handling counts frames.
		m_MappedButtons = m_ButtonMapper.Map(
			(uint16_t)m_pInput->State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
	}
#endif

	if (m_pCore)
	{
		m_pCore->run();
	}

	if (m_GameLoaded && --m_SaveCheckCountdown == 0)
	{
		m_SaveCheckCountdown = SAVE_CHECK_FRAMES;
		CheckSaves();
	}
}

static uint32_t SaveChecksum(const void *pData, size_t size)
{
	const uint8_t *pBytes = (const uint8_t *)pData;
	uint32_t hash = 2166136261U;	// FNV-1a
	for (size_t i = 0; i < size; i++)
	{
		hash = (hash ^ pBytes[i]) * 16777619U;
	}
	return hash;
}

void LibretroRunner::SavePath(const SaveSlot *pSlot, const char *pSuffix, char *pPath, size_t nSize) const
{
	snprintf(pPath, nSize, "%s%s%s", m_SaveBase, pSlot->extension, pSuffix ? pSuffix : "");
}

void LibretroRunner::LoadSaves(const char *romPath)
{
	ReleaseSaves();
	m_SaveCheckCountdown = SAVE_CHECK_FRAMES;
	if (!m_pCore || !m_pCore->get_memory_data || !m_pCore->get_memory_size || !m_pFs || !romPath)
	{
		return;
	}

	// "GBA/Pokemon.gba" -> "GBA/Pokemon"
	snprintf(m_SaveBase, sizeof(m_SaveBase), "%s", romPath);
	char *pDot = strrchr(m_SaveBase, '.');
	if (pDot && !strchr(pDot, '/'))
	{
		*pDot = 0;
	}

	for (unsigned i = 0; i < SAVE_SLOTS; i++)
	{
		SaveSlot *pSlot = &m_Saves[i];
		uint8_t *pMemory = (uint8_t *)m_pCore->get_memory_data(pSlot->memoryId);
		const size_t size = m_pCore->get_memory_size(pSlot->memoryId);
		if (!pMemory || size == 0)
		{
			continue;
		}

		char path[300];
		SavePath(pSlot, 0, path, sizeof(path));
		uint8_t *pFile = 0;
		size_t fileSize = 0;
		bool loaded = m_pFs->FileExists(path)
			&& m_pFs->ReadWholeFile(path, &pFile, &fileSize, 4 * 1024 * 1024);
		char tempPath[310];
		SavePath(pSlot, SAVE_TEMP_SUFFIX, tempPath, sizeof(tempPath));
		if (!loaded)
		{
			// Power was lost between writing the new file and renaming it.
			loaded = m_pFs->FileExists(tempPath)
				&& m_pFs->ReadWholeFile(tempPath, &pFile, &fileSize, 4 * 1024 * 1024);
			if (loaded)
			{
				snprintf(path, sizeof(path), "%s", tempPath);
			}
		}

		char message[360];
		if (loaded)
		{
			memcpy(pMemory, pFile, fileSize < size ? fileSize : size);
			delete[] pFile;
			pSlot->fileExists = true;
			snprintf(message, sizeof(message), "save loaded: %s (%u bytes)", path, (unsigned)fileSize);
		}
		else
		{
			snprintf(message, sizeof(message), "no save yet: %s", path);
		}
		if (m_pLog)
		{
			m_pLog->Notice(message);
		}

		pSlot->pShadow = new uint8_t[size];
		if (pSlot->pShadow)
		{
			memcpy(pSlot->pShadow, pMemory, size);
			pSlot->shadowSize = size;
		}
		pSlot->lastChecksum = SaveChecksum(pMemory, size);
	}
}

bool LibretroRunner::SaveChanged(const SaveSlot *pSlot, const void *pData, size_t size) const
{
	return !pSlot->pShadow
	    || size != pSlot->shadowSize
	    || memcmp(pData, pSlot->pShadow, size) != 0
	    || (pSlot->createAlways && !pSlot->fileExists);
}

void LibretroRunner::CheckSaves(void)
{
	if (!m_pCore || !m_pCore->get_memory_data || !m_pCore->get_memory_size)
	{
		return;
	}

	for (unsigned i = 0; i < SAVE_SLOTS; i++)
	{
		SaveSlot *pSlot = &m_Saves[i];
		const void *pMemory = m_pCore->get_memory_data(pSlot->memoryId);
		const size_t size = m_pCore->get_memory_size(pSlot->memoryId);
		if (!pMemory || size == 0 || pSlot->writeFailed)
		{
			continue;
		}
		if (!SaveChanged(pSlot, pMemory, size))
		{
			pSlot->changedChecks = 0;
			continue;
		}

		const uint32_t checksum = SaveChecksum(pMemory, size);
		if (checksum == pSlot->lastChecksum || ++pSlot->changedChecks >= SAVE_MAX_CHANGED_CHECKS)
		{
			WriteSave(pSlot, pMemory, size);
		}
		pSlot->lastChecksum = checksum;
	}
}

void LibretroRunner::FlushSaves(void)
{
	if (!m_GameLoaded || !m_pCore || !m_pCore->get_memory_data || !m_pCore->get_memory_size)
	{
		return;
	}

	for (unsigned i = 0; i < SAVE_SLOTS; i++)
	{
		SaveSlot *pSlot = &m_Saves[i];
		const void *pMemory = m_pCore->get_memory_data(pSlot->memoryId);
		const size_t size = m_pCore->get_memory_size(pSlot->memoryId);
		if (pMemory && size > 0 && SaveChanged(pSlot, pMemory, size))
		{
			WriteSave(pSlot, pMemory, size);
		}
	}
}

bool LibretroRunner::WriteSave(SaveSlot *pSlot, const void *pData, size_t size)
{
	char path[300];
	SavePath(pSlot, 0, path, sizeof(path));
	char message[340];
	if (!m_pFs || !m_pFs->WriteWholeFile(path, pData, size))
	{
		pSlot->writeFailed = true;
		snprintf(message, sizeof(message), "save write failed: %s", path);
		if (m_pLog)
		{
			m_pLog->Warn(message);
		}
		return false;
	}

	if (size != pSlot->shadowSize)
	{
		delete[] pSlot->pShadow;
		pSlot->pShadow = new uint8_t[size];
		pSlot->shadowSize = pSlot->pShadow ? size : 0;
	}
	if (pSlot->pShadow)
	{
		memcpy(pSlot->pShadow, pData, size);
	}
	pSlot->fileExists = true;
	pSlot->writeFailed = false;
	pSlot->changedChecks = 0;
	pSlot->lastChecksum = SaveChecksum(pData, size);

	snprintf(message, sizeof(message), "saved: %s (%u bytes)", path, (unsigned)size);
	if (m_pLog)
	{
		m_pLog->Notice(message);
	}
	return true;
}

void LibretroRunner::ReleaseSaves(void)
{
	for (unsigned i = 0; i < SAVE_SLOTS; i++)
	{
		SaveSlot *pSlot = &m_Saves[i];
		delete[] pSlot->pShadow;
		pSlot->pShadow = 0;
		pSlot->shadowSize = 0;
		pSlot->fileExists = false;
		pSlot->writeFailed = false;
		pSlot->lastChecksum = 0;
		pSlot->changedChecks = 0;
	}
	memset(m_SaveBase, 0, sizeof(m_SaveBase));
}

double LibretroRunner::FramesPerSecond(void) const
{
	return m_AvInfo.timing.fps > 1.0 ? m_AvInfo.timing.fps : 60.0;
}

unsigned LibretroRunner::SampleRate(void) const
{
	return m_AvInfo.timing.sample_rate > 1000.0
		? (unsigned)(m_AvInfo.timing.sample_rate + 0.5)
		: 48000;
}

bool LibretroRunner::Environment(unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RA_ENVIRONMENT_BAREMETAL_PROGRESS:
		if (data)
		{
			Progress((const char *)data);
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
		// The array stays valid for the lifetime of the core.
		m_pContentOverrides = (const struct retro_system_content_info_override *)data;
		return true;

	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		if (!data || !m_pVideo)
		{
			return false;
		}

		m_PixelFormat = *(const enum retro_pixel_format *)data;
		return m_pVideo->SetPixelFormat(m_PixelFormat);

	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data)
		{
			*(bool *)data = true;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
		if (data)
		{
			m_SupportsNoGame = *(const bool *)data;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
		if (data && m_pRomData && m_RomSize)
		{
			*(const struct retro_game_info_ext **)data = &m_GameInfoExt;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (data)
		{
			((struct retro_log_callback *)data)->log = LogPrintfThunk;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		if (data)
		{
			*(unsigned *)data = 2;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
		for (const struct retro_core_option_definition *pOption = (const struct retro_core_option_definition *)data;
		     pOption && pOption->key; pOption++)
		{
			AddCoreOption(pOption->key, pOption->default_value);
		}
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
		if (data)
		{
			return Environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS,
				((const struct retro_core_options_intl *)data)->us);
		}
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
		if (data)
		{
			for (const struct retro_core_option_v2_definition *pOption =
				((const struct retro_core_options_v2 *)data)->definitions;
			     pOption && pOption->key; pOption++)
			{
				AddCoreOption(pOption->key, pOption->default_value);
			}
		}
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
		if (data)
		{
			return Environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,
				((const struct retro_core_options_v2_intl *)data)->us);
		}
		return true;

	case RETRO_ENVIRONMENT_SET_VARIABLES:
		AddLegacyVariables((const struct retro_variable *)data);
		return true;

	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
	case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
	case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
	case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
	case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
	case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
	case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
	case RETRO_ENVIRONMENT_SET_MESSAGE:
	case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
	case RETRO_ENVIRONMENT_SET_GEOMETRY:
	case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
		return true;

	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data)
		{
			*(bool *)data = false;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
		if (data)
		{
			*(unsigned *)data = 1;
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;

	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
		if (data)
		{
			*(const char **)data = "/";
			return true;
		}
		return false;

	case RETRO_ENVIRONMENT_GET_VARIABLE:
		return m_pCore && m_pCore->n64Options
			? GetN64Variable((struct retro_variable *)data)
			: GetVariable((struct retro_variable *)data);

	default:
		return false;
	}
}

void LibretroRunner::AddCoreOption(const char *key, const char *defaultValue)
{
	if (!key || !defaultValue)
	{
		return;
	}

	for (unsigned i = 0; i < m_CoreOptionCount; i++)
	{
		if (strcmp(m_CoreOptions[i].key, key) == 0)
		{
			m_CoreOptions[i].defaultValue = defaultValue;
			return;
		}
	}

	if (m_CoreOptionCount < MAX_CORE_OPTIONS)
	{
		m_CoreOptions[m_CoreOptionCount].key = key;
		m_CoreOptions[m_CoreOptionCount].defaultValue = defaultValue;
		m_CoreOptionCount++;
	}
}

// Legacy values read "Description; first|second|..."; the first is the default.
void LibretroRunner::AddLegacyVariables(const struct retro_variable *variables)
{
	for (; variables && variables->key; variables++)
	{
		const char *value = variables->value ? strstr(variables->value, "; ") : 0;
		if (!value)
		{
			continue;
		}
		value += 2;

		size_t length = 0;
		while (value[length] && value[length] != '|')
		{
			length++;
		}
		if (m_LegacyDefaultsUsed + length + 1 > sizeof(m_LegacyDefaults))
		{
			continue;
		}

		char *copy = &m_LegacyDefaults[m_LegacyDefaultsUsed];
		memcpy(copy, value, length);
		copy[length] = 0;
		m_LegacyDefaultsUsed += length + 1;
		AddCoreOption(variables->key, copy);
	}
}

void LibretroRunner::LoadOptionsFile(void)
{
	m_FileOptionCount = 0;
	uint8_t *pData = 0;
	size_t nSize = 0;
	if (!m_pFs || !m_pFs->ReadWholeFile(OPTIONS_FILE, &pData, &nSize, sizeof(m_FileOptionText) - 1))
	{
		return;
	}

	memcpy(m_FileOptionText, pData, nSize);
	m_FileOptionText[nSize] = 0;
	delete[] pData;

	// Split "key = value" lines in place; quotes around the value are optional.
	char *pLine = m_FileOptionText;
	while (*pLine && m_FileOptionCount < MAX_FILE_OPTIONS)
	{
		char *pEnd = pLine;
		while (*pEnd && *pEnd != '\n' && *pEnd != '\r')
		{
			pEnd++;
		}
		char *pNext = *pEnd ? pEnd + 1 : pEnd;
		*pEnd = 0;

		char *pEqual = strchr(pLine, '=');
		if (pLine[0] != '#' && pEqual)
		{
			char *pKey = pLine;
			char *pKeyEnd = pEqual;
			while (*pKey == ' ' || *pKey == '\t') pKey++;
			while (pKeyEnd > pKey && (pKeyEnd[-1] == ' ' || pKeyEnd[-1] == '\t')) pKeyEnd--;
			*pKeyEnd = 0;

			char *pValue = pEqual + 1;
			while (*pValue == ' ' || *pValue == '\t' || *pValue == '"') pValue++;
			char *pValueEnd = pValue + strlen(pValue);
			while (pValueEnd > pValue && (pValueEnd[-1] == ' ' || pValueEnd[-1] == '\t' || pValueEnd[-1] == '"')) pValueEnd--;
			*pValueEnd = 0;

			if (*pKey)
			{
				m_FileOptions[m_FileOptionCount].key = pKey;
				m_FileOptions[m_FileOptionCount].value = pValue;
				m_FileOptionCount++;
				if (m_pLog)
				{
					char message[160];
					snprintf(message, sizeof(message), "%s: %s = %s", OPTIONS_FILE, pKey, pValue);
					m_pLog->Notice(message);
				}
			}
		}
		pLine = pNext;
	}
}

bool LibretroRunner::GetVariable(struct retro_variable *var) const
{
	if (!var || !var->key)
	{
		return false;
	}

	for (unsigned i = 0; i < m_FileOptionCount; i++)
	{
		if (strcmp(m_FileOptions[i].key, var->key) == 0)
		{
			var->value = m_FileOptions[i].value;
			return true;
		}
	}

	for (const LibretroOption *pOption = m_pCore ? m_pCore->options : 0; pOption && pOption->key; pOption++)
	{
		if (strcmp(pOption->key, var->key) == 0)
		{
			var->value = pOption->value;
			return true;
		}
	}

	for (unsigned i = 0; i < m_CoreOptionCount; i++)
	{
		if (strcmp(m_CoreOptions[i].key, var->key) == 0)
		{
			var->value = m_CoreOptions[i].defaultValue;
			return true;
		}
	}

	return false;
}

void LibretroRunner::VideoRefresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (m_pVideo)
	{
		m_pVideo->SubmitFrame(data, width, height, pitch);
	}
}

void LibretroRunner::AudioSample(int16_t left, int16_t right)
{
	if (m_pAudio)
	{
		m_pAudio->WriteSample(left, right);
	}
}

size_t LibretroRunner::AudioSampleBatch(const int16_t *data, size_t frames)
{
	if (!m_pAudio)
	{
		return frames;
	}

	return m_pAudio->WriteFrames(data, frames);
}

void LibretroRunner::InputPoll(void)
{
#ifndef RA_BAREMETAL_NO_USB
	if (m_pInput)
	{
		m_pInput->Poll();
	}
#endif
}

int16_t LibretroRunner::InputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
#ifdef RA_BAREMETAL_NO_USB
	(void)port;
	(void)device;
	(void)index;
	(void)id;
	return 0;
#else
	if (!m_pInput)
	{
		return 0;
	}

	if (m_ButtonMapper.IsActive() && port == 0 && device == RETRO_DEVICE_JOYPAD)
	{
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		{
			return (int16_t)m_MappedButtons;
		}
		return id < 16 ? (int16_t)((m_MappedButtons >> id) & 1) : 0;
	}

	return m_pInput->State(port, device, index, id);
#endif
}

bool LibretroRunner::EnvironmentThunk(unsigned cmd, void *data)
{
	return s_pActive ? s_pActive->Environment(cmd, data) : false;
}

void LibretroRunner::LogPrintfThunk(enum retro_log_level level, const char *format, ...)
{
	if (!s_pActive || !s_pActive->m_pLog || !format)
	{
		return;
	}

	char message[512];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = 0;
	for (char *p = message; *p; p++)
	{
		if (*p == '\r' || *p == '\n')
		{
			*p = 0;
			break;
		}
	}

	const bool n64Log = s_pActive->m_pCore && s_pActive->m_pCore->n64Options;
	const bool noteworthyN64Message = strstr(message, "M64CMD") || strstr(message, "failed");
	if (n64Log && level < RETRO_LOG_WARN && !noteworthyN64Message)
	{
		return;
	}

	switch (level)
	{
	case RETRO_LOG_ERROR:
		s_pActive->m_pLog->Error(message);
		break;
	case RETRO_LOG_WARN:
		s_pActive->m_pLog->Warn(message);
		break;
	default:
		s_pActive->m_pLog->Notice(message);
		break;
	}

	if (n64Log && noteworthyN64Message)
	{
		s_pActive->Progress(message);
	}
}

void LibretroRunner::VideoRefreshThunk(const void *data, unsigned width, unsigned height, size_t pitch)
{
	if (s_pActive)
	{
		s_pActive->VideoRefresh(data, width, height, pitch);
	}
}

void LibretroRunner::AudioSampleThunk(int16_t left, int16_t right)
{
	if (s_pActive)
	{
		s_pActive->AudioSample(left, right);
	}
}

size_t LibretroRunner::AudioSampleBatchThunk(const int16_t *data, size_t frames)
{
	return s_pActive ? s_pActive->AudioSampleBatch(data, frames) : frames;
}

void LibretroRunner::InputPollThunk(void)
{
	if (s_pActive)
	{
		s_pActive->InputPoll();
	}
}

int16_t LibretroRunner::InputStateThunk(unsigned port, unsigned device, unsigned index, unsigned id)
{
	return s_pActive ? s_pActive->InputState(port, device, index, id) : 0;
}
