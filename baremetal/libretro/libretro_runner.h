#ifndef RA_BAREMETAL_LIBRETRO_RUNNER_H
#define RA_BAREMETAL_LIBRETRO_RUNNER_H

#include <stddef.h>
#include <stdint.h>

#include <libretro.h>

#include "button_mapper.h"

class CircleAudio;
class CircleFs;
class CircleInput;
class CircleLog;
class CircleTimer;
class CircleVideo;

// Core option the frontend sets instead of the core's default.
struct LibretroOption
{
	const char *key;
	const char *value;
};

struct LibretroCore
{
	const char *name;
	size_t maxRomSize;
	bool n64Options;

	void (*init)(void);
	void (*deinit)(void);
	unsigned (*api_version)(void);
	void (*get_system_info)(struct retro_system_info *info);
	void (*get_system_av_info)(struct retro_system_av_info *info);
	void (*set_environment)(retro_environment_t cb);
	void (*set_video_refresh)(retro_video_refresh_t cb);
	void (*set_audio_sample)(retro_audio_sample_t cb);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t cb);
	void (*set_input_poll)(retro_input_poll_t cb);
	void (*set_input_state)(retro_input_state_t cb);
	void (*run)(void);
	bool (*load_game)(const struct retro_game_info *game);
	void (*unload_game)(void);
	void *(*get_memory_data)(unsigned id);
	size_t (*get_memory_size)(unsigned id);

	// Display aspect ratio forced by the frontend, 0 = the core's own.
	float displayAspect;
	// Option overrides, terminated by { 0, 0 }; may be 0.
	const LibretroOption *options;
	// Set: called with RETRO_DEVICE_JOYPAD for port 0 after loading, for
	// cores that ignore the pad until the frontend names the device.
	void (*set_controller_port_device)(unsigned port, unsigned device);
	// Ports that read the one pad of the GPi Case (0 counts as 1).
	unsigned padPorts;
};

const LibretroCore *LibretroFindCoreForPath(const char *path);
const LibretroCore *LibretroDefaultCore(void);
// Short system name for a ROM path ("NES", "GB", "SNES", "MD", ...).
const char *LibretroSystemForPath(const char *path);

class LibretroRunner
{
public:
	typedef void (*TProgressCallback)(void *pContext, const char *pMessage);

	LibretroRunner(CircleLog *pLog,
	               CircleTimer *pTimer,
	               CircleVideo *pVideo,
	               CircleAudio *pAudio,
	               CircleInput *pInput,
	               CircleFs *pFs);
	~LibretroRunner(void);

	bool Init(const char *romPath);
	bool Init(const LibretroCore *pCore, const char *romPath);
	// Unloads the game and deinitializes the core, so Init() can start the
	// next one (also with another core). Writes changed saves first.
	void Shutdown(void);
	// Writes the game's battery saves to the SD card if they changed since
	// they were loaded or last written (also done on its own while running).
	void FlushSaves(void);
	void SetProgressCallback(void *pContext, TProgressCallback pCallback);
	void RunFrame(void);
	double FramesPerSecond(void) const;
	unsigned SampleRate(void) const;

private:
	void Progress(const char *pMessage);
	void ProgressValue(const char *pLabel, unsigned nValue);
	bool Environment(unsigned cmd, void *data);
	void AddCoreOption(const char *key, const char *defaultValue);
	void AddLegacyVariables(const struct retro_variable *variables);
	void LoadOptionsFile(void);
	bool GetVariable(struct retro_variable *var) const;
	void PrepareGameInfoExt(const char *romPath);
	void VideoRefresh(const void *data, unsigned width, unsigned height, size_t pitch);
	void AudioSample(int16_t left, int16_t right);
	size_t AudioSampleBatch(const int16_t *data, size_t frames);
	void InputPoll(void);
	int16_t InputState(unsigned port, unsigned device, unsigned index, unsigned id);

	struct SaveSlot;
	void LoadSaves(const char *romPath);
	void CheckSaves(void);
	bool SaveChanged(const SaveSlot *pSlot, const void *pData, size_t size) const;
	bool WriteSave(SaveSlot *pSlot, const void *pData, size_t size);
	void ReleaseSaves(void);
	void SavePath(const SaveSlot *pSlot, const char *pSuffix, char *pPath, size_t nSize) const;

	static bool EnvironmentThunk(unsigned cmd, void *data);
	static void LogPrintfThunk(enum retro_log_level level, const char *format, ...);
	static void VideoRefreshThunk(const void *data, unsigned width, unsigned height, size_t pitch);
	static void AudioSampleThunk(int16_t left, int16_t right);
	static size_t AudioSampleBatchThunk(const int16_t *data, size_t frames);
	static void InputPollThunk(void);
	static int16_t InputStateThunk(unsigned port, unsigned device, unsigned index, unsigned id);

private:
	CircleLog *m_pLog;
	CircleTimer *m_pTimer;
	CircleVideo *m_pVideo;
	CircleAudio *m_pAudio;
	CircleInput *m_pInput;
	CircleFs *m_pFs;
	const LibretroCore *m_pCore;
	bool m_CoreInitialized;
	bool m_GameLoaded;
	LibretroButtonMapper m_ButtonMapper;	// active for systems with a button layout
	unsigned m_MappedButtons;		// what the game sees this frame
	void *m_pProgressContext;
	TProgressCallback m_pProgressCallback;

	enum retro_pixel_format m_PixelFormat;
	bool m_SupportsNoGame;
	const struct retro_system_content_info_override *m_pContentOverrides;

	// Options declared by the core (key and default value).
	struct CoreOption
	{
		const char *key;
		const char *defaultValue;
	};
	static const unsigned MAX_CORE_OPTIONS = 256;
	CoreOption m_CoreOptions[MAX_CORE_OPTIONS];
	unsigned m_CoreOptionCount;
	// Defaults parsed out of RETRO_ENVIRONMENT_SET_VARIABLES strings.
	char m_LegacyDefaults[4096];
	unsigned m_LegacyDefaultsUsed;

	// Options from minerva.cfg on the SD card, they win over everything else.
	static const unsigned MAX_FILE_OPTIONS = 64;
	LibretroOption m_FileOptions[MAX_FILE_OPTIONS];
	unsigned m_FileOptionCount;
	char m_FileOptionText[4096];
	uint8_t *m_pRomData;
	size_t m_RomSize;
	struct retro_system_av_info m_AvInfo;
	struct retro_game_info_ext m_GameInfoExt;
	char m_GameFullPath[256];
	char m_GameDir[64];
	char m_GameName[128];
	char m_GameExt[16];

	// Battery-backed memory of the game (cartridge saves, Game Boy clock),
	// kept next to the ROM like RetroArch does: <rom name>.srm and .rtc.
	struct SaveSlot
	{
		unsigned memoryId;	// RETRO_MEMORY_SAVE_RAM or RETRO_MEMORY_RTC
		const char *extension;
		bool createAlways;	// write once even if unchanged (clock base time)
		bool fileExists;
		bool writeFailed;	// no more automatic attempts, only FlushSaves()
		uint8_t *pShadow;	// what the file holds
		size_t shadowSize;
		uint32_t lastChecksum;	// of the memory at the previous check
		unsigned changedChecks;
	};
	static const unsigned SAVE_SLOTS = 2;
	SaveSlot m_Saves[SAVE_SLOTS];
	char m_SaveBase[256];		// ROM path without extension
	unsigned m_SaveCheckCountdown;

	static LibretroRunner *s_pActive;
};

#endif
