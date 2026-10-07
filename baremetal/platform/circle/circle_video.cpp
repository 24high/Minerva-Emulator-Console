#include "circle_platform.h"

#include <string.h>

CircleVideo::CircleVideo(void)
:	m_pScreen(0),
	m_Format(RETRO_PIXEL_FORMAT_RGB565),
	m_DisplayAspectRatio(0.0f),
	m_LastScreenW(0),
	m_LastScreenH(0),
	m_LastOutW(0),
	m_LastOutH(0),
	m_LastOriginX(0),
	m_LastOriginY(0),
	m_XMapCount(0),
	m_XMapSourceWidth(0),
	m_pScaleBuffer(0),
	m_ScaleBufferPixels(0),
	m_FrameCount(0),
	m_SampleRequested(false),
	m_SampledLitPercent(0)
{
}

CircleVideo::~CircleVideo(void)
{
	delete[] m_pScaleBuffer;
	m_pScaleBuffer = 0;
	m_ScaleBufferPixels = 0;
}

void CircleVideo::Init(CScreenDevice *pScreen)
{
	m_pScreen = pScreen;
}

bool CircleVideo::SetPixelFormat(enum retro_pixel_format format)
{
	switch (format)
	{
	case RETRO_PIXEL_FORMAT_0RGB1555:
	case RETRO_PIXEL_FORMAT_XRGB8888:
	case RETRO_PIXEL_FORMAT_RGB565:
		m_Format = format;
		m_XMapCount = 0;
		return true;

	default:
		return false;
	}
}

void CircleVideo::SetDisplayAspectRatio(float aspectRatio)
{
	m_DisplayAspectRatio = aspectRatio > 0.1f ? aspectRatio : 0.0f;
	m_LastScreenW = 0;
}

TScreenColor CircleVideo::ConvertPixel(const void *pPixel) const
{
	if (m_Format == RETRO_PIXEL_FORMAT_RGB565)
	{
		uint16_t v;
		memcpy(&v, pPixel, sizeof(v));
		const unsigned r = (v >> 11) & 31;
		const unsigned g = (v >> 6) & 31;
		const unsigned b = v & 31;
		return COLOR16(r, g, b);
	}

	if (m_Format == RETRO_PIXEL_FORMAT_0RGB1555)
	{
		uint16_t v;
		memcpy(&v, pPixel, sizeof(v));
		const unsigned r = (v >> 10) & 31;
		const unsigned g = (v >> 5) & 31;
		const unsigned b = v & 31;
		return COLOR16(r, g, b);
	}

	uint32_t v;
	memcpy(&v, pPixel, sizeof(v));
	return COLOR16((v >> 19) & 31, (v >> 11) & 31, (v >> 3) & 31);
}

static uint16_t ConvertRGB565ToScreen16(uint16_t value)
{
	return value;
}

static uint16_t Convert0RGB1555ToRGB565(uint16_t value)
{
	const unsigned r = (value >> 10) & 31;
	const unsigned g = (value >> 5) & 31;
	const unsigned b = value & 31;
	return (uint16_t)((r << 11) | (g << 6) | b);
}

static uint16_t ConvertXRGB8888ToRGB565(uint32_t value)
{
	const unsigned r = (value >> 16) & 0xFF;
	const unsigned g = (value >> 8) & 0xFF;
	const unsigned b = value & 0xFF;
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void FillRect16(CBcmFrameBuffer *pFrameBuffer,
	unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color)
{
	if (!pFrameBuffer || w == 0 || h == 0 || pFrameBuffer->GetDepth() != 16)
	{
		return;
	}

	const unsigned screenW = pFrameBuffer->GetWidth();
	const unsigned screenH = pFrameBuffer->GetHeight();
	if (x >= screenW || y >= screenH)
	{
		return;
	}

	if (x + w > screenW)
	{
		w = screenW - x;
	}
	if (y + h > screenH)
	{
		h = screenH - y;
	}

	uint8_t *pBase = (uint8_t *)(uintptr)pFrameBuffer->GetBuffer();
	const unsigned pitch = pFrameBuffer->GetPitch();
	for (unsigned row = 0; row < h; row++)
	{
		uint16_t *pDst = (uint16_t *)(pBase + (y + row) * pitch) + x;
		for (unsigned col = 0; col < w; col++)
		{
			pDst[col] = color;
		}
	}
}

unsigned CircleVideo::LitPercent(const void *frame, unsigned width, unsigned height, size_t pitch) const
{
	unsigned nLit = 0;
	unsigned nTotal = 0;
	for (unsigned y = 0; y < height; y += 4)
	{
		const uint8_t *pRow = (const uint8_t *)frame + y * pitch;
		for (unsigned x = 0; x < width; x += 4)
		{
			uint32_t nPixel;
			if (m_Format == RETRO_PIXEL_FORMAT_XRGB8888)
			{
				nPixel = ((const uint32_t *)pRow)[x] & 0xFFFFFF;
			}
			else
			{
				nPixel = ((const uint16_t *)pRow)[x];
				if (m_Format == RETRO_PIXEL_FORMAT_0RGB1555)
				{
					nPixel &= 0x7FFF;
				}
			}
			nLit += nPixel != 0;
			nTotal++;
		}
	}
	return nTotal ? nLit * 100 / nTotal : 0;
}

bool CircleVideo::SubmitFrame(const void *frame, unsigned width, unsigned height, size_t pitch)
{
	if (!m_pScreen || !frame || width == 0 || height == 0)
	{
		return false;
	}

	m_FrameCount++;
	if (m_SampleRequested)
	{
		m_SampledLitPercent = LitPercent(frame, width, height, pitch);
		m_SampleRequested = false;
	}

	const unsigned screenW = m_pScreen->GetWidth();
	const unsigned screenH = m_pScreen->GetHeight();
	const float aspect = m_DisplayAspectRatio > 0.1f
		? m_DisplayAspectRatio
		: (float)width / (float)height;
	unsigned outH = screenH;
	unsigned outW = (unsigned)((float)screenH * aspect + 0.5f);
	if (outW == 0)
	{
		outW = 1;
	}
	if (outW > screenW)
	{
		outW = screenW;
		outH = (unsigned)((float)screenW / aspect + 0.5f);
		if (outH == 0)
		{
			outH = 1;
		}
	}

	if (outW >= 16)
	{
		outW &= ~15U;
	}

	const unsigned originX = screenW > outW ? (screenW - outW) / 2 : 0;
	const unsigned originY = screenH > outH ? (screenH - outH) / 2 : 0;
	const unsigned bytesPerPixel = m_Format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
	CBcmFrameBuffer *pFrameBuffer = m_pScreen->GetFrameBuffer();

	if (m_LastScreenW != screenW || m_LastScreenH != screenH
		|| m_LastOutW != outW || m_LastOutH != outH
		|| m_LastOriginX != originX || m_LastOriginY != originY)
	{
		if (pFrameBuffer && pFrameBuffer->GetDepth() == 16)
		{
			FillRect16(pFrameBuffer, 0, 0, screenW, screenH, BLACK_COLOR);
		}
		else
		{
			for (unsigned y = 0; y < screenH; y++)
			{
				for (unsigned x = 0; x < screenW; x++)
				{
					m_pScreen->SetPixel(x, y, BLACK_COLOR);
				}
			}
		}

		m_LastScreenW = screenW;
		m_LastScreenH = screenH;
		m_LastOutW = outW;
		m_LastOutH = outH;
		m_LastOriginX = originX;
		m_LastOriginY = originY;
	}

	if (pFrameBuffer && pFrameBuffer->GetDepth() == 16
		&& m_Format == RETRO_PIXEL_FORMAT_RGB565
		&& outW == width && outH == height)
	{
		if (pitch == outW * sizeof(uint16_t))
		{
			CDisplay::TArea area = {
				originX,
				originX + outW - 1,
				originY,
				originY + outH - 1
			};
			pFrameBuffer->SetArea(area, frame);
			return true;
		}

		for (unsigned y = 0; y < outH && originY + y < screenH; y++)
		{
			CDisplay::TArea area = {
				originX,
				originX + outW - 1,
				originY + y,
				originY + y
			};
			pFrameBuffer->SetArea(area, (const uint8_t *)frame + y * pitch);
		}

		return true;
	}

	if (pFrameBuffer && pFrameBuffer->GetDepth() == 16
		&& outW <= sizeof(m_XMap) / sizeof(m_XMap[0]))
	{
		if (m_XMapCount != outW || m_XMapSourceWidth != width)
		{
			for (unsigned x = 0; x < outW; x++)
			{
				m_XMap[x] = (uint16_t)((uint64_t)x * width / outW);
			}

			m_XMapCount = outW;
			m_XMapSourceWidth = width;
		}

		uint8_t *pBase = (uint8_t *)(uintptr)pFrameBuffer->GetBuffer();
		const unsigned dstPitch = pFrameBuffer->GetPitch();
		const uint64_t srcYStep = ((uint64_t)height << 32) / outH;
		unsigned previousSrcY = (unsigned)-1;
		uint16_t *pPreviousLine = 0;

		for (unsigned y = 0; y < outH && originY + y < screenH; y++)
		{
			const unsigned srcY = (unsigned)((srcYStep * y) >> 32);
			uint16_t *pDst = (uint16_t *)(pBase + (originY + y) * dstPitch) + originX;

			if (srcY == previousSrcY && pPreviousLine)
			{
				memcpy(pDst, pPreviousLine, outW * sizeof(uint16_t));
				continue;
			}

			const uint8_t *pSrcLine = (const uint8_t *)frame + srcY * pitch;
			if (m_Format == RETRO_PIXEL_FORMAT_RGB565)
			{
				const uint16_t *pSrc = (const uint16_t *)pSrcLine;
				for (unsigned x = 0; x < outW; x++)
				{
					pDst[x] = ConvertRGB565ToScreen16(pSrc[m_XMap[x]]);
				}
			}
			else if (m_Format == RETRO_PIXEL_FORMAT_0RGB1555)
			{
				const uint16_t *pSrc = (const uint16_t *)pSrcLine;
				for (unsigned x = 0; x < outW; x++)
				{
					pDst[x] = Convert0RGB1555ToRGB565(pSrc[m_XMap[x]]);
				}
			}
			else
			{
				const uint32_t *pSrc = (const uint32_t *)pSrcLine;
				for (unsigned x = 0; x < outW; x++)
				{
					pDst[x] = ConvertXRGB8888ToRGB565(pSrc[m_XMap[x]]);
				}
			}

			previousSrcY = srcY;
			pPreviousLine = pDst;
		}

		return true;
	}

	if (pFrameBuffer && pFrameBuffer->GetDepth() == 16)
	{
		uint8_t *pBase = (uint8_t *)(uintptr)pFrameBuffer->GetBuffer();
		const unsigned dstPitch = pFrameBuffer->GetPitch();
		const uint64_t srcXStep = ((uint64_t)width << 32) / outW;
		const uint64_t srcYStep = ((uint64_t)height << 32) / outH;
		uint64_t srcYAcc = 0;

		for (unsigned y = 0; y < outH && originY + y < screenH; y++)
		{
			const unsigned srcY = (unsigned)(srcYAcc >> 32);
			const uint8_t *srcLine = (const uint8_t *)frame + srcY * pitch;
			uint16_t *pDst = (uint16_t *)(pBase + (originY + y) * dstPitch) + originX;
			uint64_t srcXAcc = 0;

			for (unsigned x = 0; x < outW && originX + x < screenW; x++)
			{
				const unsigned srcX = (unsigned)(srcXAcc >> 32);
				pDst[x] = (uint16_t)ConvertPixel(srcLine + srcX * bytesPerPixel);
				srcXAcc += srcXStep;
			}

			srcYAcc += srcYStep;
		}

		return true;
	}

	for (unsigned y = 0; y < outH && originY + y < screenH; y++)
	{
		const unsigned srcY = (uint64_t)y * height / outH;
		const uint8_t *srcLine = (const uint8_t *)frame + srcY * pitch;

		for (unsigned x = 0; x < outW && originX + x < screenW; x++)
		{
			const unsigned srcX = (uint64_t)x * width / outW;
			const void *pPixel = srcLine + srcX * bytesPerPixel;
			m_pScreen->SetPixel(originX + x, originY + y, ConvertPixel(pPixel));
		}
	}

	m_pScreen->Update();
	return true;
}
