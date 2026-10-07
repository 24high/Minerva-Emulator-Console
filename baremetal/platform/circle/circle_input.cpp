#include "circle_platform.h"

#include <stdio.h>
#include <string.h>

CircleInput *CircleInput::s_pThis = 0;

CircleInput::CircleInput(void)
:	m_pNameService(0),
	m_pUSBHCI(0),
	m_pLog(0),
	m_pGamePad(0),
	m_LoggedNoGamePad(false),
	m_GamePadKnown(false)
{
	memset((void *)&m_State, 0, sizeof(m_State));
}

void CircleInput::Init(CDeviceNameService *pNameService, CUSBHCIDevice *pUSBHCI)
{
	Init(pNameService, pUSBHCI, 0);
}

void CircleInput::Init(CDeviceNameService *pNameService, CUSBHCIDevice *pUSBHCI, CircleLog *pLog)
{
	m_pNameService = pNameService;
	m_pUSBHCI = pUSBHCI;
	m_pLog = pLog;
	s_pThis = this;
	AttachFirstGamePad();
}

void CircleInput::AttachFirstGamePad(void)
{
	if (!m_pNameService || m_pGamePad)
	{
		return;
	}

	m_pGamePad = (CUSBGamePadDevice *)m_pNameService->GetDevice("upad", 1, FALSE);
	if (!m_pGamePad)
	{
		if (m_pLog && !m_LoggedNoGamePad)
		{
			m_pLog->Notice("No USB gamepad found yet");
			m_LoggedNoGamePad = true;
		}
		return;
	}

	m_LoggedNoGamePad = false;
	m_GamePadKnown = (m_pGamePad->GetProperties() & GamePadPropertyIsKnown) != 0;
	if (m_pLog)
	{
		m_pLog->Notice(m_GamePadKnown ? "USB gamepad attached: known mapping" : "USB gamepad attached: generic HID mapping");
	}

	const TGamePadState *pInitial = m_pGamePad->GetInitialState();
	if (pInitial)
	{
		memcpy((void *)&m_State, pInitial, sizeof(m_State));
	}

	if (m_pLog && pInitial)
	{
		char message[96];
		snprintf(message, sizeof(message), "USB gamepad layout: axes=%d hats=%d buttons=%d",
			pInitial->naxes, pInitial->nhats, pInitial->nbuttons);
		m_pLog->Notice(message);
	}

	m_pGamePad->RegisterStatusHandler(GamePadStatusHandler);
	m_pGamePad->RegisterRemovedHandler(GamePadRemovedHandler, this);
}

void CircleInput::Poll(void)
{
	if (m_pUSBHCI)
	{
		m_pUSBHCI->UpdatePlugAndPlay();
	}

	if (!m_pGamePad)
	{
		AttachFirstGamePad();
	}
}

void CircleInput::GamePadStatusHandler(unsigned nDeviceIndex, const TGamePadState *pState)
{
	(void)nDeviceIndex;

	if (s_pThis && pState)
	{
		memcpy((void *)&s_pThis->m_State, pState, sizeof(s_pThis->m_State));
	}
}

void CircleInput::GamePadRemovedHandler(CDevice *pDevice, void *pContext)
{
	(void)pDevice;

	CircleInput *pThis = (CircleInput *)pContext;
	if (pThis)
	{
		pThis->m_pGamePad = 0;
		pThis->m_LoggedNoGamePad = false;
		pThis->m_GamePadKnown = false;
		memset((void *)&pThis->m_State, 0, sizeof(pThis->m_State));
	}
}

bool CircleInput::DigitalButton(unsigned mask) const
{
	return (m_State.buttons & mask) != 0;
}

bool CircleInput::GenericButton(unsigned index) const
{
	if (index >= 32 || index >= (unsigned)m_State.nbuttons)
	{
		return false;
	}

	return (m_State.buttons & (1U << index)) != 0;
}

bool CircleInput::HatDirection(unsigned id) const
{
	if (m_State.nhats <= 0)
	{
		return false;
	}

	const int hat = m_State.hats[0];
	switch (id)
	{
	case RETRO_DEVICE_ID_JOYPAD_UP:    return hat == 0 || hat == 1 || hat == 7;
	case RETRO_DEVICE_ID_JOYPAD_RIGHT: return hat == 1 || hat == 2 || hat == 3;
	case RETRO_DEVICE_ID_JOYPAD_DOWN:  return hat == 3 || hat == 4 || hat == 5;
	case RETRO_DEVICE_ID_JOYPAD_LEFT:  return hat == 5 || hat == 6 || hat == 7;
	default:                           return false;
	}
}

bool CircleInput::AxisDirection(unsigned id) const
{
	if (m_State.naxes < 2)
	{
		return false;
	}

	const unsigned axis = (id == RETRO_DEVICE_ID_JOYPAD_LEFT || id == RETRO_DEVICE_ID_JOYPAD_RIGHT)
		? GamePadAxisLeftX
		: GamePadAxisLeftY;
	if (axis >= (unsigned)m_State.naxes)
	{
		return false;
	}

	const int min = m_State.axes[axis].minimum;
	const int max = m_State.axes[axis].maximum;
	const int val = m_State.axes[axis].value;
	if (max <= min)
	{
		return false;
	}

	const int low = min + (max - min) / 3;
	const int high = max - (max - min) / 3;
	switch (id)
	{
	case RETRO_DEVICE_ID_JOYPAD_LEFT:
	case RETRO_DEVICE_ID_JOYPAD_UP:
		return val < low;
	case RETRO_DEVICE_ID_JOYPAD_RIGHT:
	case RETRO_DEVICE_ID_JOYPAD_DOWN:
		return val > high;
	default:
		return false;
	}
}

bool CircleInput::ButtonState(unsigned id) const
{
	const unsigned buttons = m_State.buttons;


	// Known pads report Xbox positions (A bottom, B right, X left, Y top).
	// RetroPad buttons are named by position on a SNES pad (B bottom, A right,
	// Y left, X top), so the labels of the GPi Case, which reports as an Xbox
	// 360 pad, end up on the RetroPad buttons with the same name.
	if (m_GamePadKnown)
	{
		switch (id)
		{
		case RETRO_DEVICE_ID_JOYPAD_B:      return (buttons & GamePadButtonA);
		case RETRO_DEVICE_ID_JOYPAD_Y:      return (buttons & GamePadButtonX);
		case RETRO_DEVICE_ID_JOYPAD_SELECT: return (buttons & GamePadButtonSelect) || (buttons & GamePadButtonMinus);
		case RETRO_DEVICE_ID_JOYPAD_START:  return (buttons & GamePadButtonStart) || (buttons & GamePadButtonPlus);
		case RETRO_DEVICE_ID_JOYPAD_UP:     return (buttons & GamePadButtonUp) || HatDirection(id) || AxisDirection(id);
		case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (buttons & GamePadButtonDown) || HatDirection(id) || AxisDirection(id);
		case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (buttons & GamePadButtonLeft) || HatDirection(id) || AxisDirection(id);
		case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (buttons & GamePadButtonRight) || HatDirection(id) || AxisDirection(id);
		case RETRO_DEVICE_ID_JOYPAD_A:      return (buttons & GamePadButtonB);
		case RETRO_DEVICE_ID_JOYPAD_X:      return (buttons & GamePadButtonY);
		case RETRO_DEVICE_ID_JOYPAD_L:      return (buttons & GamePadButtonLB);
		case RETRO_DEVICE_ID_JOYPAD_R:      return (buttons & GamePadButtonRB);
		case RETRO_DEVICE_ID_JOYPAD_L2:     return (buttons & GamePadButtonLT);
		case RETRO_DEVICE_ID_JOYPAD_R2:     return (buttons & GamePadButtonRT);
		case RETRO_DEVICE_ID_JOYPAD_L3:     return (buttons & GamePadButtonL3);
		case RETRO_DEVICE_ID_JOYPAD_R3:     return (buttons & GamePadButtonR3);
		default:                            return false;
		}
	}

	switch (id)
	{
	case RETRO_DEVICE_ID_JOYPAD_B:      return GenericButton(0);
	case RETRO_DEVICE_ID_JOYPAD_A:      return GenericButton(1);
	case RETRO_DEVICE_ID_JOYPAD_Y:      return GenericButton(2);
	case RETRO_DEVICE_ID_JOYPAD_X:      return GenericButton(3);
	case RETRO_DEVICE_ID_JOYPAD_L:      return GenericButton(4);
	case RETRO_DEVICE_ID_JOYPAD_R:      return GenericButton(5);
	case RETRO_DEVICE_ID_JOYPAD_SELECT: return GenericButton(6) || GenericButton(8);
	case RETRO_DEVICE_ID_JOYPAD_START:  return GenericButton(7) || GenericButton(9);
	case RETRO_DEVICE_ID_JOYPAD_L2:     return GenericButton(10);
	case RETRO_DEVICE_ID_JOYPAD_R2:     return GenericButton(11);
	case RETRO_DEVICE_ID_JOYPAD_L3:     return GenericButton(12);
	case RETRO_DEVICE_ID_JOYPAD_R3:     return GenericButton(13);
	case RETRO_DEVICE_ID_JOYPAD_UP:
	case RETRO_DEVICE_ID_JOYPAD_DOWN:
	case RETRO_DEVICE_ID_JOYPAD_LEFT:
	case RETRO_DEVICE_ID_JOYPAD_RIGHT:
		return HatDirection(id) || AxisDirection(id);
	default:
		return false;
	}
}

static int16_t NormalizeAxisToLibretro(const TGamePadState &state, unsigned axis)
{
	if (axis >= (unsigned)state.naxes)
	{
		return 0;
	}

	const int min = state.axes[axis].minimum;
	const int max = state.axes[axis].maximum;
	const int val = state.axes[axis].value;
	if (max <= min)
	{
		return 0;
	}

	int scaled = (((val - min) * 65535) / (max - min)) - 32768;
	if (scaled < -32768)
	{
		scaled = -32768;
	}
	else if (scaled > 32767)
	{
		scaled = 32767;
	}

	return (int16_t)scaled;
}

static int AbsInt16(int16_t value)
{
	return value < 0 ? -value : value;
}

unsigned CircleInput::JoypadMask(void) const
{
	unsigned mask = 0;
	for (unsigned id = 0; id <= RETRO_DEVICE_ID_JOYPAD_R3; id++)
	{
		if (ButtonState(id))
		{
			mask |= 1U << id;
		}
	}

	return mask;
}

int16_t CircleInput::State(unsigned port, unsigned device, unsigned index, unsigned id) const
{
	if (port != 0)
	{
		return 0;
	}

	if (device == RETRO_DEVICE_JOYPAD)
	{
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
		{
			return (int16_t)JoypadMask();
		}

		return ButtonState(id) ? 1 : 0;
	}

	if (device == RETRO_DEVICE_ANALOG)
	{
		if (id != RETRO_DEVICE_ID_ANALOG_X && id != RETRO_DEVICE_ID_ANALOG_Y)
		{
			return 0;
		}

		TGamePadState state;
		memcpy(&state, (const void *)&m_State, sizeof(state));

		const unsigned axis = index == RETRO_DEVICE_INDEX_ANALOG_LEFT
			? (id == RETRO_DEVICE_ID_ANALOG_X ? GamePadAxisLeftX : GamePadAxisLeftY)
			: (id == RETRO_DEVICE_ID_ANALOG_X ? GamePadAxisRightX : GamePadAxisRightY);
		int16_t value = NormalizeAxisToLibretro(state, axis);

		if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT && AbsInt16(value) < 4096)
		{
			const unsigned altAxis = id == RETRO_DEVICE_ID_ANALOG_X
				? GamePadAxisRightX
				: GamePadAxisRightY;
			const int16_t altValue = NormalizeAxisToLibretro(state, altAxis);
			if (AbsInt16(altValue) > AbsInt16(value))
			{
				value = altValue;
			}
		}

		if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT && AbsInt16(value) < 4096)
		{
			if (id == RETRO_DEVICE_ID_ANALOG_X)
			{
				if (ButtonState(RETRO_DEVICE_ID_JOYPAD_LEFT))
				{
					return -32768;
				}
				if (ButtonState(RETRO_DEVICE_ID_JOYPAD_RIGHT))
				{
					return 32767;
				}
			}
			else
			{
				if (ButtonState(RETRO_DEVICE_ID_JOYPAD_UP))
				{
					return -32768;
				}
				if (ButtonState(RETRO_DEVICE_ID_JOYPAD_DOWN))
				{
					return 32767;
				}
			}
		}

		return value;
	}

	return 0;
}
