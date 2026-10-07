#include "circle_platform.h"

static const unsigned AudioQueueMilliseconds = 320;

CircleAudio::CircleAudio(void)
:	m_pSound(0),
	m_Step(0),
	m_Phase(0)
{
	m_Previous[0] = m_Previous[1] = 0;
}

bool CircleAudio::Init(CSoundBaseDevice *pSound, unsigned sampleRate, unsigned deviceRate)
{
	m_pSound = 0;
	m_Step = 0;
	m_Phase = 0;
	m_Previous[0] = m_Previous[1] = 0;
	if (!pSound)
	{
		return true;
	}

	if (deviceRate && sampleRate && deviceRate != sampleRate)
	{
		m_Step = (unsigned)(((unsigned long long)sampleRate << 16) / deviceRate);
		sampleRate = deviceRate;	// the device's queue is primed below
	}

	if (!pSound->AllocateQueue(AudioQueueMilliseconds))
	{
		return false;
	}

	pSound->SetWriteFormat(SoundFormatSigned16, 2);

	static const int16_t silence[512 * 2] = {0};
	if (sampleRate < 1000)
	{
		sampleRate = 48000;
	}
	unsigned framesToPrime = sampleRate * 60 / 1000;
	const unsigned maxPrime = pSound->GetQueueSizeFrames() / 2;
	if (framesToPrime > maxPrime)
	{
		framesToPrime = maxPrime;
	}
	while (framesToPrime > 0)
	{
		const unsigned chunkFrames = framesToPrime > 512 ? 512 : framesToPrime;
		const int written = pSound->Write(silence, chunkFrames * 2 * sizeof(int16_t));
		if (written <= 0)
		{
			break;
		}

		const unsigned writtenFrames = (unsigned)written / (2 * sizeof(int16_t));
		if (writtenFrames == 0)
		{
			break;
		}

		framesToPrime -= writtenFrames;
	}

	if (!pSound->Start())
	{
		return false;
	}

	m_pSound = pSound;
	return true;
}

void CircleAudio::WriteSample(int16_t left, int16_t right)
{
	int16_t frame[2];
	frame[0] = left;
	frame[1] = right;
	WriteFrames(frame, 1);
}

size_t CircleAudio::WriteFrames(const int16_t *samples, size_t frames)
{
	if (!m_pSound || !samples || frames == 0)
	{
		return frames;
	}

	if (m_Step)
	{
		WriteResampled(samples, frames);
		return frames;
	}

	const size_t bytes = frames * 2 * sizeof(int16_t);
	const int written = m_pSound->Write(samples, bytes);
	if (written <= 0)
	{
		return frames;
	}

	const size_t writtenFrames = (size_t)written / (2 * sizeof(int16_t));
	(void)writtenFrames;
	return frames;
}

// Linear interpolation from the core's rate to the device's: output frames
// are taken at steps of m_Step between the previous and the current input
// frame.
void CircleAudio::WriteResampled(const int16_t *samples, size_t frames)
{
	int16_t out[256 * 2];
	unsigned outFrames = 0;

	for (size_t i = 0; i < frames; i++)
	{
		const int16_t *current = samples + i * 2;
		while (m_Phase < 0x10000)
		{
			for (unsigned channel = 0; channel < 2; channel++)
			{
				const int from = m_Previous[channel];
				out[outFrames * 2 + channel] = (int16_t)(from + (((current[channel] - from) * (int)m_Phase) >> 16));
			}
			m_Phase += m_Step;
			if (++outFrames == sizeof out / sizeof out[0] / 2)
			{
				m_pSound->Write(out, outFrames * 2 * sizeof(int16_t));
				outFrames = 0;
			}
		}
		m_Phase -= 0x10000;
		m_Previous[0] = current[0];
		m_Previous[1] = current[1];
	}

	if (outFrames)
	{
		m_pSound->Write(out, outFrames * 2 * sizeof(int16_t));
	}
}
