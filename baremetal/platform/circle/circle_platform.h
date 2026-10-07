#ifndef RA_BAREMETAL_CIRCLE_PLATFORM_H
#define RA_BAREMETAL_CIRCLE_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include <circle/devicenameservice.h>
#include <circle/fs/fat/fatfs.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbgamepad.h>
#include <circle/usb/usbhcidevice.h>

#include <libretro.h>

#ifdef RA_BAREMETAL_FATFS
// Circle's FatFs addon: subdirectories and long file names.
#include <fatfs/ff.h>
#define RA_BAREMETAL_FS_NAME_SIZE 128
#else
// Circle's native FAT driver: 8.3 names.
#define RA_BAREMETAL_FS_NAME_SIZE (FS_TITLE_LEN+1)
#endif

class CircleLog
{
public:
	CircleLog(void);
	void Init(CLogger *pLogger);
	void Notice(const char *pMessage);
	void Warn(const char *pMessage);
	void Error(const char *pMessage);

private:
	CLogger *m_pLogger;
};

class CircleTimer
{
public:
	CircleTimer(void);
	void Init(CTimer *pTimer);
	uint64_t NowUsec(void) const;
	void WaitNextFrame(double fps);

private:
	CTimer *m_pTimer;
	uint64_t m_NextFrameUsec;
};

class CircleVideo
{
public:
	CircleVideo(void);
	~CircleVideo(void);
	void Init(CScreenDevice *pScreen);
	bool SetPixelFormat(enum retro_pixel_format format);
	void SetDisplayAspectRatio(float aspectRatio);
	bool SubmitFrame(const void *frame, unsigned width, unsigned height, size_t pitch);

	// Diagnostics: frames drawn so far and the share of not black pixels in
	// the first frame drawn after RequestSample().
	unsigned FrameCount(void) const { return m_FrameCount; }
	void RequestSample(void) { m_SampleRequested = true; }
	unsigned SampledLitPercent(void) const { return m_SampledLitPercent; }

private:
	TScreenColor ConvertPixel(const void *pPixel) const;
	unsigned LitPercent(const void *frame, unsigned width, unsigned height, size_t pitch) const;

private:
	CScreenDevice *m_pScreen;
	enum retro_pixel_format m_Format;
	float m_DisplayAspectRatio;
	unsigned m_LastScreenW;
	unsigned m_LastScreenH;
	unsigned m_LastOutW;
	unsigned m_LastOutH;
	unsigned m_LastOriginX;
	unsigned m_LastOriginY;
	unsigned m_XMapCount;
	unsigned m_XMapSourceWidth;
	uint16_t m_XMap[2048];
	uint16_t *m_pScaleBuffer;
	size_t m_ScaleBufferPixels;
	unsigned m_FrameCount;
	bool m_SampleRequested;
	unsigned m_SampledLitPercent;
};

class CircleAudio
{
public:
	CircleAudio(void);
	// deviceRate: the rate the sound device runs at, if it differs from the
	// core's sampleRate (USB sound cards take only a few rates); 0 = the same.
	bool Init(CSoundBaseDevice *pSound, unsigned sampleRate, unsigned deviceRate = 0);
	void WriteSample(int16_t left, int16_t right);
	size_t WriteFrames(const int16_t *samples, size_t frames);

private:
	void WriteResampled(const int16_t *samples, size_t frames);

	CSoundBaseDevice *m_pSound;
	unsigned m_Step;		// input frames per output frame, 16.16; 0 = no resampling
	unsigned m_Phase;		// position between m_Previous and the next input frame, 16.16
	int16_t m_Previous[2];
};

class CircleInput
{
public:
	CircleInput(void);
	void Init(CDeviceNameService *pNameService, CUSBHCIDevice *pUSBHCI);
	void Init(CDeviceNameService *pNameService, CUSBHCIDevice *pUSBHCI, CircleLog *pLog);
	void Poll(void);
	int16_t State(unsigned port, unsigned device, unsigned index, unsigned id) const;

private:
	void AttachFirstGamePad(void);
	bool DigitalButton(unsigned mask) const;
	bool GenericButton(unsigned index) const;
	bool HatDirection(unsigned id) const;
	bool AxisDirection(unsigned id) const;
	bool ButtonState(unsigned id) const;
	unsigned JoypadMask(void) const;
	static void GamePadStatusHandler(unsigned nDeviceIndex, const TGamePadState *pState);
	static void GamePadRemovedHandler(CDevice *pDevice, void *pContext);

private:
	CDeviceNameService *m_pNameService;
	CUSBHCIDevice *m_pUSBHCI;
	CircleLog *m_pLog;
	CUSBGamePadDevice *m_pGamePad;
	bool m_LoggedNoGamePad;
	bool m_GamePadKnown;
	volatile TGamePadState m_State;

	static CircleInput *s_pThis;
};

class CircleFs
{
public:
	struct Entry
	{
		char name[RA_BAREMETAL_FS_NAME_SIZE];
		unsigned size;
		bool isDirectory;
	};

	CircleFs(void);
#ifdef RA_BAREMETAL_FATFS
	// Mounts the first FAT partition of the SD card (FatFs volume "SD:").
	bool Mount(CircleLog *pLog);
#else
	void Init(CFATFileSystem *pFileSystem, CircleLog *pLog);
#endif
	bool ReadWholeFile(const char *path, uint8_t **ppData, size_t *pSize, size_t maxSize);
	// pFilter (may be 0) decides which entries are kept; the others do not
	// count against maxEntries.
	typedef bool (*TEntryFilter)(const char *name, bool isDirectory);
	bool ListDirectory(const char *path, Entry *entries, unsigned maxEntries, unsigned *pCount,
			   TEntryFilter pFilter = 0);
	bool FindFirstWithExtension(const char *extension, char *path, size_t pathSize);
	bool FileExists(const char *path);
	// With FatFs the file is replaced only once the new contents are
	// completely written (written as <path>.tmp, then renamed).
	bool WriteWholeFile(const char *path, const void *pData, size_t size);

private:
#ifdef RA_BAREMETAL_FATFS
	FATFS m_FatFs;
	bool m_Mounted;
#else
	CFATFileSystem *m_pFileSystem;
#endif
	CircleLog *m_pLog;
};

#endif
