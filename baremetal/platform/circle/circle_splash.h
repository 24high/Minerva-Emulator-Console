#ifndef RA_BAREMETAL_CIRCLE_SPLASH_H
#define RA_BAREMETAL_CIRCLE_SPLASH_H

#include <stddef.h>

#include <circle/device.h>
#include <circle/screen.h>
#include <circle/spinlock.h>

// Draws the boot image (baremetal/assets/splash.png, converted by
// baremetal/splash.mjs) centred and scaled to fit the screen.
void CircleDrawSplash(CScreenDevice *pScreen);

// Logger target while the boot image is shown: keeps the beginning and the
// most recent part of the log and only puts it on the screen once Reveal() is
// called, e.g. on a fatal error or a panic, so errors do not stay hidden
// behind the image. CopyText() returns it for the log file on the SD card.
class CircleBootLog : public CDevice
{
public:
	CircleBootLog(void);

	void Init(CScreenDevice *pScreen);
	int Write(const void *pBuffer, size_t nCount) override;
	void WriteLine(const char *pText);
	void Reveal(void);

	static const unsigned TEXT_SIZE = 16384;
	size_t CopyText(char *pText, size_t nMaxSize);

private:
	unsigned CopyRing(char *pText, unsigned nFrom);

	static const unsigned HEAD_SIZE = 6144;
	static const unsigned RING_SIZE = 8192;

	CScreenDevice *m_pScreen;
	volatile bool m_Revealed;
	CSpinLock m_SpinLock;
	char m_Head[HEAD_SIZE];
	char m_Ring[RING_SIZE];
	unsigned m_Total;	// characters written so far
};

#endif
