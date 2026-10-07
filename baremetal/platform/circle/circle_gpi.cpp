#include "circle_gpi.h"

#include <circle/timer.h>

// GPIO assignment of the GPi Case (see RetroFlag SafeShutdown_gpi.py and the
// dpi24-gpi overlay).
static const unsigned GPIO_POWER_SWITCH = 26;	// input, low = switched off
static const unsigned GPIO_POWER_LATCH = 27;	// output, high = keep powered

// DPI output format 6 (RGB666, mode 2): clock/DE/VSYNC/HSYNC on GPIO0-3,
// colour data on GPIO4-9, 12-17 and 20-25. GPIO10/11 are switched to ALT2
// like the original overlay does; GPIO18/19 stay with PWM audio.
static const unsigned DPI_PIN_RANGES[][2] = {
	{ 0, 17 },
	{ 20, 25 },
};

static const uint64_t SWITCH_ARM_USEC = 200000;
static const uint64_t SWITCH_DEBOUNCE_USEC = 50000;

CircleGpiCase::CircleGpiCase(void)
:	m_Armed(false),
	m_SwitchOnSinceUsec(0),
	m_SwitchOffSinceUsec(0)
{
}

void CircleGpiCase::Initialize(void)
{
	// config.txt already drives GPIO27 high from the firmware. Switching the
	// pin without bInitPin keeps that level instead of pulsing it low.
	m_PowerLatch.AssignPin(GPIO_POWER_LATCH);
	m_PowerLatch.SetMode(GPIOModeOutput, FALSE);
	m_PowerLatch.Write(HIGH);

	m_PowerSwitch.AssignPin(GPIO_POWER_SWITCH);
	m_PowerSwitch.SetMode(GPIOModeInputPullUp);

	// Repeats the gpio= lines of config.txt, so the panel also works with a
	// stock config.txt. CGPIOPin leaves the mode untouched on destruction.
	for (unsigned range = 0; range < sizeof(DPI_PIN_RANGES) / sizeof(DPI_PIN_RANGES[0]); range++)
	{
		for (unsigned pin = DPI_PIN_RANGES[range][0]; pin <= DPI_PIN_RANGES[range][1]; pin++)
		{
			CGPIOPin DpiPin(pin, GPIOModeAlternateFunction2);
		}
	}
}

bool CircleGpiCase::PowerSwitchOff(void)
{
	const uint64_t now = CTimer::GetClockTicks64();

	if (m_PowerSwitch.Read() == HIGH)
	{
		m_SwitchOffSinceUsec = 0;
		if (m_SwitchOnSinceUsec == 0)
		{
			m_SwitchOnSinceUsec = now;
		}
		// Only react to an on -> off transition, like the RetroFlag script,
		// so an input that reads low from boot on never powers off.
		if (now - m_SwitchOnSinceUsec >= SWITCH_ARM_USEC)
		{
			m_Armed = true;
		}
		return false;
	}

	m_SwitchOnSinceUsec = 0;
	if (!m_Armed)
	{
		return false;
	}

	if (m_SwitchOffSinceUsec == 0)
	{
		m_SwitchOffSinceUsec = now;
	}

	return now - m_SwitchOffSinceUsec >= SWITCH_DEBOUNCE_USEC;
}

void CircleGpiCase::PowerOff(void)
{
	m_PowerLatch.Write(LOW);
}
