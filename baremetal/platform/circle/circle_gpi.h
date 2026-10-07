#ifndef RA_BAREMETAL_CIRCLE_GPI_H
#define RA_BAREMETAL_CIRCLE_GPI_H

#include <stdint.h>

#include <circle/gpiopin.h>
#include <circle/types.h>

// Hardware glue for the Retroflag GPi Case with a Raspberry Pi Zero / Zero W.
// Under Linux the case needs the dpi24-gpi overlay for the screen and
// SafeShutdown_gpi.py for the power switch; this class does both on bare metal.
// The controller is a plain USB HID gamepad (see CircleInput).
class CircleGpiCase
{
public:
	CircleGpiCase(void);

	// Keeps the case powered and routes the DPI panel signals to the GPIOs.
	void Initialize(void);

	// Polled from the main loops. Returns true once the power switch has
	// been turned off (after it was seen on, debounced).
	bool PowerSwitchOff(void);

	// Releases the power latch, the case then cuts the supply.
	void PowerOff(void);

private:
	CGPIOPin m_PowerSwitch;
	CGPIOPin m_PowerLatch;
	bool m_Armed;
	uint64_t m_SwitchOnSinceUsec;
	uint64_t m_SwitchOffSinceUsec;
};

#endif
