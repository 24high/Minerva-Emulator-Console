#include "circle_splash.h"

#include <stdint.h>
#include <string.h>

#include <circle/bcmframebuffer.h>

// Generated into the build directory by baremetal/splash.mjs.
extern "C" const unsigned g_SplashWidth;
extern "C" const unsigned g_SplashHeight;
extern "C" const uint16_t g_SplashPixels[];

void CircleDrawSplash(CScreenDevice *pScreen)
{
	CBcmFrameBuffer *pFrameBuffer = pScreen ? pScreen->GetFrameBuffer() : 0;
	if (!pFrameBuffer || pFrameBuffer->GetDepth() != 16 || g_SplashWidth == 0 || g_SplashHeight == 0)
	{
		return;
	}

	const unsigned screenW = pFrameBuffer->GetWidth();
	const unsigned screenH = pFrameBuffer->GetHeight();
	unsigned outW = screenW;
	unsigned outH = (unsigned)((uint64_t)g_SplashHeight * screenW / g_SplashWidth);
	if (outH > screenH)
	{
		outH = screenH;
		outW = (unsigned)((uint64_t)g_SplashWidth * screenH / g_SplashHeight);
	}
	if (outW == 0 || outH == 0)
	{
		return;
	}

	const unsigned originX = (screenW - outW) / 2;
	const unsigned originY = (screenH - outH) / 2;
	uint8_t *pBase = (uint8_t *)(uintptr)pFrameBuffer->GetBuffer();
	const unsigned pitch = pFrameBuffer->GetPitch();

	// Nearest-neighbour fit; 1:1 when the image matches the screen (GPi Case).
	for (unsigned y = 0; y < screenH; y++)
	{
		uint16_t *pDst = (uint16_t *)(pBase + y * pitch);
		if (y < originY || y >= originY + outH)
		{
			memset(pDst, 0, screenW * sizeof(uint16_t));
			continue;
		}

		const uint16_t *pSrc = g_SplashPixels
			+ (uint64_t)(y - originY) * g_SplashHeight / outH * g_SplashWidth;
		for (unsigned x = 0; x < screenW; x++)
		{
			pDst[x] = x < originX || x >= originX + outW
				? 0
				: pSrc[(uint64_t)(x - originX) * g_SplashWidth / outW];
		}
	}
}

CircleBootLog::CircleBootLog(void)
:	m_pScreen(0),
	m_Revealed(false),
	m_Total(0)
{
}

void CircleBootLog::Init(CScreenDevice *pScreen)
{
	m_pScreen = pScreen;
}

int CircleBootLog::Write(const void *pBuffer, size_t nCount)
{
	const char *pChars = (const char *)pBuffer;
	m_SpinLock.Acquire();
	for (size_t i = 0; i < nCount; i++)
	{
		if (m_Total < HEAD_SIZE)
		{
			m_Head[m_Total] = pChars[i];
		}
		m_Ring[m_Total % RING_SIZE] = pChars[i];
		m_Total++;
	}
	m_SpinLock.Release();

	if (m_Revealed && m_pScreen)
	{
		// Reveal() switched the terminal to delayed updates, flush each write.
		m_pScreen->Write(pBuffer, nCount);
		m_pScreen->Update();
	}

	return (int)nCount;
}

void CircleBootLog::WriteLine(const char *pText)
{
	if (pText)
	{
		Write(pText, strlen(pText));
		Write("\n", 1);
	}
}

// Copies the characters from position nFrom up to the end of the log out of
// the ring (they must still be in it). Called with the spin lock held.
unsigned CircleBootLog::CopyRing(char *pText, unsigned nFrom)
{
	unsigned nCopied = 0;
	for (unsigned nPos = nFrom; nPos < m_Total; nPos++)
	{
		pText[nCopied++] = m_Ring[nPos % RING_SIZE];
	}
	return nCopied;
}

size_t CircleBootLog::CopyText(char *pText, size_t nMaxSize)
{
	static const char Gap[] = "[...]\n";
	if (!pText || nMaxSize < TEXT_SIZE)
	{
		return 0;
	}

	m_SpinLock.Acquire();
	size_t nLength = 0;
	if (m_Total <= RING_SIZE)
	{
		nLength = CopyRing(pText, 0);
	}
	else
	{
		// The first lines (core, video and audio setup), then the newest ones.
		const unsigned nHead = m_Total < HEAD_SIZE ? m_Total : HEAD_SIZE;
		memcpy(pText, m_Head, nHead);
		nLength = nHead;

		unsigned nFrom = m_Total - RING_SIZE;
		if (nFrom < nHead)
		{
			nFrom = nHead;
		}
		else if (nFrom > nHead)
		{
			memcpy(pText + nLength, Gap, sizeof Gap - 1);
			nLength += sizeof Gap - 1;
		}
		nLength += CopyRing(pText + nLength, nFrom);
	}
	m_SpinLock.Release();

	return nLength;
}

void CircleBootLog::Reveal(void)
{
	if (m_Revealed || !m_pScreen)
	{
		return;
	}

	static const char ClearScreen[] = "\x1b[2J\x1b[H\x1b[?25h";
	m_pScreen->Write(ClearScreen, sizeof ClearScreen - 1);

	m_SpinLock.Acquire();
	m_Revealed = true;
	unsigned nFrom = 0;
	if (m_Total > RING_SIZE)
	{
		// Start behind the oldest, partly overwritten line.
		nFrom = m_Total - RING_SIZE;
		while (nFrom < m_Total && m_Ring[nFrom % RING_SIZE] != '\n')
		{
			nFrom++;
		}
		nFrom++;
	}
	for (; nFrom < m_Total; nFrom++)
	{
		m_pScreen->Write(&m_Ring[nFrom % RING_SIZE], 1);
	}
	m_SpinLock.Release();

	m_pScreen->Update();
}
