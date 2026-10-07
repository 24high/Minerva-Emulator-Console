#ifndef RA_BAREMETAL_BUTTON_MAPPER_H
#define RA_BAREMETAL_BUTTON_MAPPER_H

#include <libretro.h>

// Fits the buttons of a system onto a pad with only A, B, X and Y (the GPi
// Case). Works on RetroPad button masks (bit n = RETRO_DEVICE_ID_JOYPAD_n),
// once per frame.
//
// - direct:     button "from" acts as button "to" (GBA: Y is L, X is R).
// - withSelect: while Select is held, button "from" acts as button "to"
//               (SNES: Select+Y is L, Select+X is R).
//
// With Select combinations Select itself is held back, so a combination does
// not also press Select in the game: tapped and released it reaches the game
// for SELECT_TAP_FRAMES after the release; held longer than
// SELECT_HOLD_FRAMES without a combination it is passed through.

struct LibretroButtonRemap
{
	unsigned from;
	unsigned to;
};
static const unsigned BUTTON_REMAP_END = ~0U;

struct LibretroButtonLayout
{
	const LibretroButtonRemap *direct;	// may be 0; ends with from == BUTTON_REMAP_END
	const LibretroButtonRemap *withSelect;	// may be 0; ends with from == BUTTON_REMAP_END
};

class LibretroButtonMapper
{
public:
	static const unsigned SELECT_HOLD_FRAMES = 15;	// ~1/4 s
	static const unsigned SELECT_TAP_FRAMES = 6;

	LibretroButtonMapper(void)
	:	m_pLayout(0)
	{
		Reset();
	}

	void SetLayout(const LibretroButtonLayout *pLayout)
	{
		m_pLayout = pLayout;
		Reset();
	}

	bool IsActive(void) const
	{
		return m_pLayout != 0;
	}

	// raw: buttons pressed on the pad; returns the buttons the game sees.
	unsigned Map(unsigned raw)
	{
		if (!m_pLayout)
		{
			return raw;
		}

		unsigned result = Apply(m_pLayout->direct, raw, raw);
		if (!m_pLayout->withSelect)
		{
			return result;
		}

		const unsigned select = 1U << RETRO_DEVICE_ID_JOYPAD_SELECT;
		result &= ~select;
		if (raw & select)
		{
			if (m_State == StateIdle)
			{
				m_State = StatePending;
				m_HeldFrames = 0;
			}
			if (raw & Sources(m_pLayout->withSelect))
			{
				m_State = StateCombination;
			}
			else if (m_State == StatePending && ++m_HeldFrames >= SELECT_HOLD_FRAMES)
			{
				m_State = StateHeld;
			}

			result = Apply(m_pLayout->withSelect, raw, result);
			if (m_State == StateHeld)
			{
				result |= select;
			}
		}
		else
		{
			if (m_State == StatePending)
			{
				m_TapFrames = SELECT_TAP_FRAMES;
			}
			m_State = StateIdle;
		}

		if (m_TapFrames > 0)
		{
			m_TapFrames--;
			result |= select;
		}
		return result;
	}

private:
	enum TState
	{
		StateIdle,		// Select not pressed
		StatePending,		// pressed, not yet known whether tapped, held or combined
		StateHeld,		// held without combination: passed through
		StateCombination	// used as modifier: not passed through until released
	};

	void Reset(void)
	{
		m_State = StateIdle;
		m_HeldFrames = 0;
		m_TapFrames = 0;
	}

	static unsigned Sources(const LibretroButtonRemap *pRemap)
	{
		unsigned mask = 0;
		for (; pRemap && pRemap->from != BUTTON_REMAP_END; pRemap++)
		{
			mask |= 1U << pRemap->from;
		}
		return mask;
	}

	// The "from" buttons stop acting as themselves; each pressed one in raw
	// presses its "to" button.
	static unsigned Apply(const LibretroButtonRemap *pRemap, unsigned raw, unsigned buttons)
	{
		unsigned result = buttons & ~Sources(pRemap);
		for (; pRemap && pRemap->from != BUTTON_REMAP_END; pRemap++)
		{
			if (raw & (1U << pRemap->from))
			{
				result |= 1U << pRemap->to;
			}
		}
		return result;
	}

	const LibretroButtonLayout *m_pLayout;
	TState m_State;
	unsigned m_HeldFrames;
	unsigned m_TapFrames;
};

#endif
