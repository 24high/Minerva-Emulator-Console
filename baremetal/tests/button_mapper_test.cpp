// Frame-by-frame test of libretro/button_mapper.h on the host:
//   node baremetal/tests/button-mapper-test.mjs
#include "libretro/button_mapper.h"

#include <stdio.h>

static const unsigned B = 1U << RETRO_DEVICE_ID_JOYPAD_B;
static const unsigned Y = 1U << RETRO_DEVICE_ID_JOYPAD_Y;
static const unsigned SELECT = 1U << RETRO_DEVICE_ID_JOYPAD_SELECT;
static const unsigned START = 1U << RETRO_DEVICE_ID_JOYPAD_START;
static const unsigned A = 1U << RETRO_DEVICE_ID_JOYPAD_A;
static const unsigned X = 1U << RETRO_DEVICE_ID_JOYPAD_X;
static const unsigned L = 1U << RETRO_DEVICE_ID_JOYPAD_L;
static const unsigned R = 1U << RETRO_DEVICE_ID_JOYPAD_R;

static const LibretroButtonRemap Shoulders[] = {
	{ RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_L },
	{ RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_R },
	{ BUTTON_REMAP_END, 0 }
};
static const LibretroButtonLayout Gba = { Shoulders, 0 };
static const LibretroButtonLayout Snes = { 0, Shoulders };

static int g_Failures;

// Feeds raw for the given number of frames; every frame must give expected.
static void Expect(LibretroButtonMapper &mapper, const char *what, unsigned raw, unsigned frames, unsigned expected)
{
	for (unsigned frame = 0; frame < frames; frame++)
	{
		const unsigned result = mapper.Map(raw);
		if (result != expected)
		{
			printf("FAIL  %s, frame %u: raw %04x gives %04x, expected %04x\n", what, frame, raw, result, expected);
			g_Failures++;
			return;
		}
	}
	printf("ok    %s\n", what);
}

int main(void)
{
	const unsigned HOLD = LibretroButtonMapper::SELECT_HOLD_FRAMES;
	const unsigned TAP = LibretroButtonMapper::SELECT_TAP_FRAMES;
	LibretroButtonMapper mapper;

	// Systems without a layout get the pad as it is.
	Expect(mapper, "no layout: Select+X unchanged", SELECT | X, 30, SELECT | X);

	mapper.SetLayout(&Gba);
	Expect(mapper, "GBA: Y is L", Y, 3, L);
	Expect(mapper, "GBA: X is R", X, 3, R);
	Expect(mapper, "GBA: A, B, Start as they are", A | B | START, 3, A | B | START);
	Expect(mapper, "GBA: Select at once, no combinations", SELECT | X, 3, SELECT | R);

	mapper.SetLayout(&Snes);
	Expect(mapper, "SNES: A, B, X, Y as they are", A | B | X | Y, 3, A | B | X | Y);
	Expect(mapper, "SNES: Select+X is R, without Select", SELECT | X, 30, R);
	Expect(mapper, "SNES: then Select released, X held: X", X, 3, X);
	Expect(mapper, "SNES: Select+Y is L, without Select", SELECT | Y, 30, L);
	Expect(mapper, "SNES: both released", 0, 3, 0);
	// Select+A has no function on the SNES: A acts as A, Select as tapped.
	Expect(mapper, "SNES: Select+A+B: A and B as they are", SELECT | A | B, 1, A | B);
	Expect(mapper, "SNES: released: Select tapped", 0, TAP, SELECT);
	Expect(mapper, "SNES: then nothing", 0, 3, 0);

	// Select held alone: held back first, then passed through.
	Expect(mapper, "SNES: Select held, first 1/4 s nothing", SELECT, HOLD - 1, 0);
	Expect(mapper, "SNES: Select held longer: Select", SELECT, 20, SELECT);
	Expect(mapper, "SNES: held Select, then X: R only", SELECT | X, 5, R);
	Expect(mapper, "SNES: X released, Select still held: nothing", SELECT, 5, 0);
	Expect(mapper, "SNES: all released", 0, 3, 0);

	// Select tapped: reaches the game after the release.
	Expect(mapper, "SNES: Select tapped (3 frames held back)", SELECT, 3, 0);
	Expect(mapper, "SNES: after release Select for the tap time", 0, TAP, SELECT);
	Expect(mapper, "SNES: then nothing", 0, 5, 0);

	// No tap after a combination.
	Expect(mapper, "SNES: Select+X tapped", SELECT | X, 2, R);
	Expect(mapper, "SNES: released, no Select tap", 0, TAP + 2, 0);

	// Start+Select (ends the game in the kernel): Start goes through.
	Expect(mapper, "SNES: Start+Select: Start, Select held back", SELECT | START, HOLD - 1, START);

	mapper.SetLayout(0);
	Expect(mapper, "layout removed: as it is", SELECT | Y, 3, SELECT | Y);

	printf(g_Failures ? "%d failures\n" : "all passed\n", g_Failures);
	return g_Failures ? 1 : 0;
}
