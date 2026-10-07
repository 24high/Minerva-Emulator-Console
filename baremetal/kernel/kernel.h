#ifndef RA_BAREMETAL_KERNEL_H
#define RA_BAREMETAL_KERNEL_H

#include <circle/actled.h>
#include <circle/cputhrottle.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/koptions.h>
#include <circle/memory.h>
#include <circle/logger.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/sound/soundbasedevice.h>
#include <circle/timer.h>
#include <circle/types.h>
#include <circle/usb/usbhcidevice.h>
#include <SDCard/emmc.h>
#include <circle/fs/fat/fatfs.h>

#include "libretro/libretro_runner.h"
#include "platform/circle/circle_platform.h"
#include "platform/circle/circle_parallel.h"
#ifdef RA_BAREMETAL_GPI_CASE
#include "platform/circle/circle_gpi.h"
#endif
#ifdef RA_BAREMETAL_SPLASH
#include "platform/circle/circle_splash.h"
#endif

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel(void);
	~CKernel(void);

	boolean Initialize(void);
	TShutdownMode Run(void);

private:
	enum TGameEnd
	{
		GameEndExit,		// Start+Select: back to the ROM browser
		GameEndPowerOff,
		GameEndFailed
	};
	TGameEnd RunGame(const LibretroCore *pCore, const char *pRomPath);
	void StopGame(void);
	bool ExitComboHeld(void) const;

	void Status(const char *pText);
	void Fatal(const char *pText);
	CSoundBaseDevice *CreateSoundDevice(unsigned sampleRate);
	bool PowerOffRequested(void);
	TShutdownMode PowerOff(void);
	static bool PowerOffPollThunk(void *pContext);
	void LogFrameStats(unsigned nFrames, unsigned nMilliseconds, unsigned nDrawnFrames);
	void WriteLogFile(void);
	void LoadClock(void);
	void SaveClock(void);
#ifdef RA_BAREMETAL_SPLASH
	void WaitSplashMinimum(void);
	static void PanicHandler(void);
#endif

private:
	// Circle samples require this construction order.
	CActLED m_ActLED;
	CKernelOptions m_Options;
	CCPUThrottle m_CPUThrottle;
	CDeviceNameService m_DeviceNameService;
	CScreenDevice m_Screen;
#ifdef RA_BAREMETAL_GPI_CASE
	// No UART: its pins GPIO14/15 carry DPI data in the GPi Case.
	CircleGpiCase m_GpiCase;
#else
	CSerialDevice m_Serial;
#endif
	CExceptionHandler m_ExceptionHandler;
	CInterruptSystem m_Interrupt;
	CTimer m_Timer;
	CLogger m_Logger;
#ifndef RA_BAREMETAL_NO_USB
	CUSBHCIDevice *m_pUSBHCI;
#endif
	CEMMCDevice *m_pEMMC;
#ifndef RA_BAREMETAL_FATFS
	CFATFileSystem *m_pFileSystem;
#endif
	CSoundBaseDevice *m_pSound;
	const char *m_pLogSystem;	// system of the started game, names the log file

	CircleLog m_Log;
	CircleTimer m_FrameTimer;
	CircleVideo m_Video;
	CircleAudio m_Audio;
#ifndef RA_BAREMETAL_NO_USB
	CircleInput m_Input;
#endif
	CircleFs m_Fs;
	CircleParallel m_Parallel;
	LibretroRunner m_Runner;
#ifdef RA_BAREMETAL_SPLASH
	CircleBootLog m_BootLog;
	uint64_t m_SplashShownUsec;
	static CKernel *s_pThis;
#endif
};

#endif
