#include "kernel.h"
#include "rom_browser.h"

#include <circle/string.h>
#ifdef RA_BAREMETAL_GPI_CASE
#include <circle/sound/pwmsoundbasedevice.h>
#else
#include <circle/sound/hdmisoundbasedevice.h>
#endif
#include <string.h>

#ifndef RA_BAREMETAL_ROM_PATH
#define RA_BAREMETAL_ROM_PATH "GAME.GB"
#endif

static const char FromKernel[] = "ra-circle";

// The log (see CircleBootLog) is written to the SD card a few seconds into
// the game and when the power switch is turned off, so problems on the
// device can be read on a PC afterwards. One file per system (minerva-GBA.log
// etc.), so testing another system does not overwrite it; minerva.log before
// a game is started.
static const char LOG_FILE[] = "minerva.log";
static const char SYSTEM_LOG_FILE[] = "minerva-%s.log";
static const uint64_t LOG_FILE_DELAY_USEC = 15000000;
static const uint64_t FRAME_STATS_USEC = 5000000;

// Holding Start+Select ends the game and returns to the ROM browser. Not in
// N64 builds, whose core is not meant to be restarted in place, and not
// without a gamepad (no browser).
#if !defined(RA_BAREMETAL_NO_USB) && !defined(RA_BAREMETAL_N64) && !defined(RA_BAREMETAL_MULTI)
#define RA_EXIT_TO_MENU 1
#endif
static const uint64_t EXIT_HOLD_USEC = 500000;

// The Pi has no battery-backed clock, but game clocks (Game Boy cartridges
// with a clock, GBA games like Pokemon Ruby) are based on the system time.
// The time is kept in a file so it continues from where it was at the last
// shutdown instead of starting at 1970 on every boot; time while the device
// is off is not counted.
static const char CLOCK_FILE[] = "minerva.time";
static const unsigned CLOCK_DEFAULT_TIME = 1767225600;	// 2026-01-01 00:00 UTC
static const uint64_t CLOCK_SAVE_USEC = 300000000;	// while a game runs
static const uint64_t SOUND_STOP_TIMEOUT_USEC = 500000;

#ifdef RA_BAREMETAL_SPLASH
// The boot image stays at least this long before the ROM browser appears.
static const uint64_t SPLASH_MIN_USEC = 1500000;

CKernel *CKernel::s_pThis = 0;
#endif
#ifdef RA_BAREMETAL_GPI_CASE
// The GPi Case feeds PWM audio (GPIO18/19) into its own amplifier.
static const char AUDIO_OUTPUT_NAME[] = "pwm";
static const unsigned PWM_AUDIO_CHUNK_SIZE = 2048;
#else
static const char AUDIO_OUTPUT_NAME[] = "hdmi";
static const unsigned HDMI_AUDIO_CHUNK_SIZE = 384 * 4;
#endif

#ifndef RA_BAREMETAL_SPLASH
static void ScreenWriteLine(CScreenDevice *pScreen, const char *pText)
{
	if (!pScreen || !pText)
	{
		return;
	}

	pScreen->Write(pText, strlen(pText));
	pScreen->Write("\n", 1);
	pScreen->Update();
}

static void RunnerProgress(void *pContext, const char *pMessage)
{
	ScreenWriteLine((CScreenDevice *)pContext, pMessage);
}
#endif

#ifdef RA_BAREMETAL_NO_USB
static void CopyString(char *pDst, size_t nDstSize, const char *pSrc)
{
	if (!pDst || nDstSize == 0)
	{
		return;
	}

	size_t i = 0;
	if (pSrc)
	{
		for (; pSrc[i] && i + 1 < nDstSize; i++)
		{
			pDst[i] = pSrc[i];
		}
	}
	pDst[i] = 0;
}
#endif

#ifndef RA_BAREMETAL_FATFS
static boolean TryMountFatDevice(CDeviceNameService *pDeviceNameService, CLogger *pLogger,
	CFATFileSystem **ppFileSystem, const char *pDeviceName)
{
	if (!pDeviceNameService || !ppFileSystem || !pDeviceName)
	{
		return FALSE;
	}

	CDevice *pDevice = pDeviceNameService->GetDevice(pDeviceName, TRUE);
	if (pDevice == 0)
	{
		if (pLogger)
		{
			pLogger->Write(FromKernel, LogNotice, "FAT mount candidate missing: %s", pDeviceName);
		}
		return FALSE;
	}

	CFATFileSystem *pFileSystem = new CFATFileSystem;
	if (!pFileSystem)
	{
		if (pLogger)
		{
			pLogger->Write(FromKernel, LogError, "FAT filesystem allocation failed");
		}
		return FALSE;
	}

	if (!pFileSystem->Mount(pDevice))
	{
		if (pLogger)
		{
			pLogger->Write(FromKernel, LogWarning, "FAT mount failed on %s", pDeviceName);
		}
		delete pFileSystem;
		return FALSE;
	}

	*ppFileSystem = pFileSystem;
	if (pLogger)
	{
		pLogger->Write(FromKernel, LogNotice, "FAT mounted on %s", pDeviceName);
	}
	return TRUE;
}
#endif

#ifndef RA_BAREMETAL_SPLASH
static void DrawBootMarker(CScreenDevice *pScreen)
{
	if (!pScreen)
	{
		return;
	}

	const unsigned width = pScreen->GetWidth();
	const unsigned height = pScreen->GetHeight();
	if (width == 0 || height == 0)
	{
		return;
	}

	const TScreenColor colors[] = {
		BRIGHT_RED_COLOR,
		BRIGHT_GREEN_COLOR,
		BRIGHT_BLUE_COLOR,
		BRIGHT_WHITE_COLOR,
	};
	const unsigned markerHeight = height / 10 < 96 ? height / 10 : 96;
	const unsigned bandWidth = width / 4 ? width / 4 : 1;

	for (unsigned y = 0; y < markerHeight; y++)
	{
		for (unsigned x = 0; x < width; x++)
		{
			unsigned colorIndex = x / bandWidth;
			if (colorIndex >= 4)
			{
				colorIndex = 3;
			}
			pScreen->SetPixel(x, y, colors[colorIndex]);
		}
	}

	pScreen->Update();
}
#endif

CKernel::CKernel(void)
:	m_CPUThrottle(CPUSpeedMaximum),
	m_Screen(m_Options.GetWidth(), m_Options.GetHeight()),
	m_Timer(&m_Interrupt),
	m_Logger(m_Options.GetLogLevel(), &m_Timer),
#ifndef RA_BAREMETAL_NO_USB
	m_pUSBHCI(0),
#endif
	m_pEMMC(0),
#ifndef RA_BAREMETAL_FATFS
	m_pFileSystem(0),
#endif
	m_pSound(0),
	m_pLogSystem(0),
#ifndef RA_BAREMETAL_NO_USB
	m_Runner(&m_Log, &m_FrameTimer, &m_Video, &m_Audio, &m_Input, &m_Fs)
#else
	m_Runner(&m_Log, &m_FrameTimer, &m_Video, &m_Audio, 0, &m_Fs)
#endif
{
#ifdef RA_BAREMETAL_SPLASH
	m_SplashShownUsec = 0;
	s_pThis = this;
#endif
	m_ActLED.Blink(5);
}

CKernel::~CKernel(void)
{
	if (m_pSound)
	{
		m_pSound->Cancel();
		delete m_pSound;
		m_pSound = 0;
	}

#ifndef RA_BAREMETAL_FATFS
	delete m_pFileSystem;
	m_pFileSystem = 0;
#endif

	delete m_pEMMC;
	m_pEMMC = 0;

#ifndef RA_BAREMETAL_NO_USB
	delete m_pUSBHCI;
	m_pUSBHCI = 0;
#endif
}

boolean CKernel::Initialize(void)
{
	boolean bOK = TRUE;

#ifdef RA_BAREMETAL_GPI_CASE
	// Before the screen: hold the case power latch and route the DPI pins.
	m_GpiCase.Initialize();
#endif

	if (bOK)
	{
		bOK = m_Screen.Initialize();
		if (bOK)
		{
#ifdef RA_BAREMETAL_SPLASH
			// Hide the cursor first, otherwise it is drawn over the image.
			m_Screen.Write("\x1b[?25l", 6);
			CircleDrawSplash(&m_Screen);
			m_SplashShownUsec = CTimer::GetClockTicks64();
			m_BootLog.Init(&m_Screen);
#else
			DrawBootMarker(&m_Screen);
#endif
			Status("RetroArch bare-metal: screen ok");
		}
	}

#ifndef RA_BAREMETAL_GPI_CASE
	if (bOK)
	{
		Status("serial init...");
		bOK = m_Serial.Initialize(115200);
		Status(bOK ? "serial ok" : "serial failed");
	}
#endif

	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice(m_Options.GetLogDevice(), FALSE);
		if (pTarget == 0)
		{
			pTarget = &m_Screen;
		}
#ifdef RA_BAREMETAL_SPLASH
		if (pTarget == &m_Screen)
		{
			// Keep log output off the boot image until something fails.
			pTarget = &m_BootLog;
			m_Logger.RegisterPanicHandler(PanicHandler);
		}
#endif

		Status("logger init...");
		bOK = m_Logger.Initialize(pTarget);
		Status(bOK ? "logger ok" : "logger failed");
	}

	if (bOK)
	{
		Status("interrupt init...");
		bOK = m_Interrupt.Initialize();
		Status(bOK ? "interrupt ok" : "interrupt failed");
	}

	if (bOK)
	{
		Status("timer init...");
		bOK = m_Timer.Initialize();
		Status(bOK ? "timer ok" : "timer failed");
	}

	if (bOK)
	{
		Status("early init ok");
		m_CPUThrottle.DumpStatus(FALSE);
	}

	if (bOK)
	{
		Status("multicore init...");
		if (m_Parallel.Initialize())
		{
			Status("multicore ok");
		}
		else
		{
			Status("multicore failed; n64 single-core RDP");
		}
	}

	return bOK;
}

TShutdownMode CKernel::Run(void)
{
	m_Log.Init(&m_Logger);
	m_FrameTimer.Init(&m_Timer);
	m_Video.Init(&m_Screen);

	m_Logger.Write(FromKernel, LogNotice, "Compile time: " __DATE__ " " __TIME__);

#ifndef RA_BAREMETAL_NO_USB
	Status("alloc usb...");
	m_pUSBHCI = new CUSBHCIDevice(&m_Interrupt, &m_Timer, TRUE);
	if (!m_pUSBHCI)
	{
		Status("usb allocation failed; continuing without input");
	}
	else
	{
		Status("usb init...");
		if (m_pUSBHCI->Initialize())
		{
			Status("usb ok");
			m_Input.Init(&m_DeviceNameService, m_pUSBHCI, &m_Log);
		}
		else
		{
			Status("usb failed; continuing without input");
			delete m_pUSBHCI;
			m_pUSBHCI = 0;
		}
	}
#else
	Status("usb disabled for diagnosis");
#endif

	Status("alloc emmc...");
	m_pEMMC = new CEMMCDevice(&m_Interrupt, &m_Timer, &m_ActLED);
	if (!m_pEMMC)
	{
		Fatal("emmc allocation failed");
		return ShutdownHalt;
	}

	Status("emmc init...");
	if (!m_pEMMC->Initialize())
	{
		Fatal("emmc failed");
		return ShutdownHalt;
	}
	Status("emmc ok");

	m_Logger.Write(FromKernel, LogNotice, "Searching for FAT partition on SD");
	Status("mount sd fat...");

#ifdef RA_BAREMETAL_FATFS
	if (!m_Fs.Mount(&m_Log))
	{
		Status("no FAT16/FAT32 partition");
		Fatal("format SD boot partition as MBR FAT32");
		m_Logger.Write(FromKernel, LogPanic, "No mountable FAT16/FAT32 SD partition found");
		return ShutdownHalt;
	}
	Status("mount ok: SD:");
#else

	static const char *MountCandidates[] = {
		"emmc1-1", "emmc1-2", "emmc1-3", "emmc1-4",
		"emmc1",
		"emmc2-1", "emmc2-2", "emmc2-3", "emmc2-4",
		"emmc2",
		0
	};

	for (unsigned i = 0; MountCandidates[i] != 0; i++)
	{
		m_Logger.Write(FromKernel, LogNotice, "Trying FAT mount: %s", MountCandidates[i]);
		if (TryMountFatDevice(&m_DeviceNameService, &m_Logger, &m_pFileSystem, MountCandidates[i]))
		{
			CString Message;
			Message.Format("mount ok: %s", MountCandidates[i]);
			Status(Message);
			break;
		}
	}

	if (!m_pFileSystem)
	{
		Status("no FAT16/FAT32 partition");
		Fatal("format SD boot partition as MBR FAT32");
		m_Logger.Write(FromKernel, LogPanic, "No mountable FAT16/FAT32 SD partition found");
		return ShutdownHalt;
	}

	m_Fs.Init(m_pFileSystem, &m_Log);
#endif
	LoadClock();

#ifndef RA_BAREMETAL_NO_USB
	Status("select ROM...");
#ifdef RA_BAREMETAL_SPLASH
	WaitSplashMinimum();
#endif
#endif

	while (1)
	{
		char selectedRom[256];
		const LibretroCore *pSelectedCore = 0;
#ifndef RA_BAREMETAL_NO_USB
		if (!SelectRomAtBoot(&m_Screen, &m_Timer, &m_Fs, &m_Input,
			selectedRom, sizeof(selectedRom), &pSelectedCore, PowerOffPollThunk, this))
		{
			if (PowerOffRequested())
			{
				return PowerOff();
			}
			Fatal("ROM selection failed");
			m_Logger.Write(FromKernel, LogPanic, "ROM selection failed");
			return ShutdownHalt;
		}
#else
		CopyString(selectedRom, sizeof(selectedRom), RA_BAREMETAL_ROM_PATH);
		pSelectedCore = LibretroFindCoreForPath(selectedRom);
		if (!pSelectedCore)
		{
			pSelectedCore = LibretroDefaultCore();
		}
#endif

		switch (RunGame(pSelectedCore, selectedRom))
		{
		case GameEndExit:
			StopGame();
			break;

		case GameEndPowerOff:
			return PowerOff();

		case GameEndFailed:
		default:
			return ShutdownHalt;
		}
	}
}

// Starts the game and runs it until it is ended with Start+Select or the
// power switch.
CKernel::TGameEnd CKernel::RunGame(const LibretroCore *pSelectedCore, const char *selectedRom)
{
	m_pLogSystem = LibretroSystemForPath(selectedRom);
	m_Logger.Write(FromKernel, LogNotice, "Starting %s (heap free %u KB)", selectedRom,
		       (unsigned)(CMemorySystem::Get()->GetHeapFreeSpace(HEAP_ANY) / 1024));
	Status("loading selected ROM");
#ifndef RA_BAREMETAL_SPLASH
	// Time to read the boot messages; behind the boot image they are hidden.
	m_Timer.MsDelay(1000);
	m_Runner.SetProgressCallback(&m_Screen, RunnerProgress);
#endif
	if (!m_Runner.Init(pSelectedCore, selectedRom))
	{
		Fatal("libretro runner init failed");
		m_Logger.Write(FromKernel, LogPanic, "libretro runner init failed");
		return GameEndFailed;
	}

	const unsigned audioSampleRate = m_Runner.SampleRate();
	m_Logger.Write(FromKernel, LogNotice, "Initializing %s audio at %u Hz", AUDIO_OUTPUT_NAME, audioSampleRate);
	CString AudioMessage;
	AudioMessage.Format("audio %s init...", AUDIO_OUTPUT_NAME);
	Status(AudioMessage);
	m_pSound = CreateSoundDevice(audioSampleRate);
	if (!m_pSound)
	{
		Status("audio allocation failed; muted");
		m_Logger.Write(FromKernel, LogError, "Audio allocation failed");
	}
	else if (!m_Audio.Init(m_pSound, audioSampleRate))
	{
		AudioMessage.Format("audio %s failed; muted", AUDIO_OUTPUT_NAME);
		Status(AudioMessage);
		m_Logger.Write(FromKernel, LogError, "Audio initialization failed");
		delete m_pSound;
		m_pSound = 0;
	}
	else
	{
		CString Message;
		Message.Format("audio ok: %u Hz", audioSampleRate);
		Status(Message);
	}

	Status("libretro ok; entering frame loop");
	m_Logger.Write(FromKernel, LogNotice, "Entering libretro frame loop");
#ifndef RA_BAREMETAL_SPLASH
	m_Timer.MsDelay(1000);
#endif

	const bool uncapFrontendPacing = pSelectedCore && pSelectedCore->n64Options;
	if (uncapFrontendPacing)
	{
		m_Logger.Write(FromKernel, LogNotice, "N64 frontend frame pacing disabled");
	}

	const uint64_t loopStartUsec = CTimer::GetClockTicks64();
	uint64_t statsStartUsec = loopStartUsec;
	unsigned statsFrames = 0;
	unsigned statsDrawnFrames = m_Video.FrameCount();
	bool logFileWritten = false;
	uint64_t exitHeldSinceUsec = 0;
	uint64_t clockSavedUsec = loopStartUsec;
	m_Video.RequestSample();

	while (1)
	{
#ifndef RA_BAREMETAL_NO_USB
		m_Input.Poll();
#endif
		m_Runner.RunFrame();
		if (!uncapFrontendPacing)
		{
			m_FrameTimer.WaitNextFrame(m_Runner.FramesPerSecond());
		}
		m_CPUThrottle.Update();

		statsFrames++;
		const uint64_t nowUsec = CTimer::GetClockTicks64();
		if (nowUsec - statsStartUsec >= FRAME_STATS_USEC)
		{
			LogFrameStats(statsFrames, (unsigned)((nowUsec - statsStartUsec) / 1000),
				      m_Video.FrameCount() - statsDrawnFrames);
			statsStartUsec = nowUsec;
			statsFrames = 0;
			statsDrawnFrames = m_Video.FrameCount();
			m_Video.RequestSample();
		}
		if (!logFileWritten && nowUsec - loopStartUsec >= LOG_FILE_DELAY_USEC)
		{
			WriteLogFile();
			logFileWritten = true;
		}
		if (nowUsec - clockSavedUsec >= CLOCK_SAVE_USEC)
		{
			SaveClock();
			clockSavedUsec = nowUsec;
		}

		if (!ExitComboHeld())
		{
			exitHeldSinceUsec = 0;
		}
		else if (exitHeldSinceUsec == 0)
		{
			exitHeldSinceUsec = nowUsec;
		}
		else if (nowUsec - exitHeldSinceUsec >= EXIT_HOLD_USEC)
		{
			return GameEndExit;
		}

		if (PowerOffRequested())
		{
			return GameEndPowerOff;
		}
	}
}

bool CKernel::ExitComboHeld(void) const
{
#ifdef RA_EXIT_TO_MENU
	return m_Input.State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START)
	    && m_Input.State(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
#else
	return false;
#endif
}

// Ends the running game so the next one can start: sound device (its sample
// rate depends on the core), core and game are released.
void CKernel::StopGame(void)
{
	m_Logger.Write(FromKernel, LogNotice, "Game ended, back to the ROM browser");
	WriteLogFile();

	if (m_pSound)
	{
		m_pSound->Cancel();
		const uint64_t cancelUsec = CTimer::GetClockTicks64();
		while (m_pSound->IsActive() && CTimer::GetClockTicks64() - cancelUsec < SOUND_STOP_TIMEOUT_USEC)
		{
			m_Timer.MsDelay(1);
		}
		m_Audio.Init(0, 0);
		if (!m_pSound->IsActive())
		{
			delete m_pSound;
		}
		else
		{
			// Deleting a running device would hit an assertion; leave it.
			m_Logger.Write(FromKernel, LogWarning, "Sound device did not stop");
		}
		m_pSound = 0;
	}

	m_Runner.Shutdown();	// writes changed saves
	SaveClock();
	m_pLogSystem = 0;
}

void CKernel::LoadClock(void)
{
	unsigned nTime = 0;
	uint8_t *pData = 0;
	size_t nSize = 0;
	if (m_Fs.FileExists(CLOCK_FILE) && m_Fs.ReadWholeFile(CLOCK_FILE, &pData, &nSize, 32))
	{
		for (size_t i = 0; i < nSize && pData[i] >= '0' && pData[i] <= '9'; i++)
		{
			nTime = nTime * 10 + (pData[i] - '0');
		}
		delete[] pData;
	}
	if (nTime < CLOCK_DEFAULT_TIME)
	{
		nTime = CLOCK_DEFAULT_TIME;
	}

	m_Timer.SetTime(nTime, FALSE);
	m_Logger.Write(FromKernel, LogNotice, "Clock set to %u", nTime);
}

void CKernel::SaveClock(void)
{
	CString Text;
	Text.Format("%u\n", m_Timer.GetUniversalTime());
	if (!m_Fs.WriteWholeFile(CLOCK_FILE, (const char *)Text, Text.GetLength()))
	{
		m_Logger.Write(FromKernel, LogWarning, "Could not write %s", CLOCK_FILE);
	}
}

void CKernel::Status(const char *pText)
{
#ifdef RA_BAREMETAL_SPLASH
	m_BootLog.WriteLine(pText);
#else
	ScreenWriteLine(&m_Screen, pText);
#endif
}

void CKernel::Fatal(const char *pText)
{
	Status(pText);
	WriteLogFile();
#ifdef RA_BAREMETAL_SPLASH
	m_BootLog.Reveal();
#endif
}

// Emulated against real time, frames that reached the screen and how much of
// the last sampled frame was not black: tells a slow core from one that runs
// at full speed but shows nothing.
void CKernel::LogFrameStats(unsigned nFrames, unsigned nMilliseconds, unsigned nDrawnFrames)
{
	if (nMilliseconds == 0)
	{
		return;
	}
	const unsigned fpsTenths = (unsigned)((uint64_t)nFrames * 10000 / nMilliseconds);
	const unsigned targetTenths = (unsigned)(m_Runner.FramesPerSecond() * 10.0 + 0.5);
	m_Logger.Write(FromKernel, LogNotice, "speed %u.%u fps (core %u.%u), drawn %u, lit %u%%",
		       fpsTenths / 10, fpsTenths % 10, targetTenths / 10, targetTenths % 10,
		       nDrawnFrames, m_Video.SampledLitPercent());
}

void CKernel::WriteLogFile(void)
{
#ifdef RA_BAREMETAL_SPLASH
	static char Text[CircleBootLog::TEXT_SIZE];
	const size_t nLength = m_BootLog.CopyText(Text, sizeof Text);
	CString FileName(LOG_FILE);
	if (m_pLogSystem && m_pLogSystem[0])
	{
		FileName.Format(SYSTEM_LOG_FILE, m_pLogSystem);
	}
	if (nLength > 0 && !m_Fs.WriteWholeFile(FileName, Text, nLength))
	{
		m_Logger.Write(FromKernel, LogWarning, "Could not write %s", (const char *)FileName);
	}
#endif
}

#ifdef RA_BAREMETAL_SPLASH
void CKernel::WaitSplashMinimum(void)
{
	while (CTimer::GetClockTicks64() - m_SplashShownUsec < SPLASH_MIN_USEC
	       && !PowerOffRequested())
	{
		m_Timer.MsDelay(10);
	}
}

void CKernel::PanicHandler(void)
{
	if (s_pThis)
	{
		s_pThis->m_BootLog.Reveal();
	}
}
#endif

CSoundBaseDevice *CKernel::CreateSoundDevice(unsigned sampleRate)
{
#ifdef RA_BAREMETAL_GPI_CASE
	return new CPWMSoundBaseDevice(&m_Interrupt, sampleRate, PWM_AUDIO_CHUNK_SIZE);
#else
	return new CHDMISoundBaseDevice(&m_Interrupt, sampleRate, HDMI_AUDIO_CHUNK_SIZE);
#endif
}

bool CKernel::PowerOffRequested(void)
{
#ifdef RA_BAREMETAL_GPI_CASE
	return m_GpiCase.PowerSwitchOff();
#else
	return false;
#endif
}

bool CKernel::PowerOffPollThunk(void *pContext)
{
	return ((CKernel *)pContext)->PowerOffRequested();
}

TShutdownMode CKernel::PowerOff(void)
{
	m_Logger.Write(FromKernel, LogNotice, "Power switch turned off");
	if (m_pSound)
	{
		m_pSound->Cancel();
	}
	Status("power off...");
	m_Runner.FlushSaves();
	SaveClock();
	WriteLogFile();

#ifdef RA_BAREMETAL_GPI_CASE
	m_GpiCase.PowerOff();
#endif

	return ShutdownHalt;
}
