#ifndef RA_BAREMETAL_ROM_BROWSER_H
#define RA_BAREMETAL_ROM_BROWSER_H

#include <stddef.h>

class CScreenDevice;
class CTimer;
class CircleFs;
class CircleInput;
struct LibretroCore;

// Polled while the browser waits for input; returning true aborts the
// selection (e.g. the GPi Case power switch was turned off).
typedef bool (*TRomBrowserAbortPoll)(void *pContext);

bool SelectRomAtBoot(CScreenDevice *pScreen,
                     CTimer *pTimer,
                     CircleFs *pFs,
                     CircleInput *pInput,
                     char *romPath,
                     size_t romPathSize,
                     const LibretroCore **ppCore,
                     TRomBrowserAbortPoll pAbortPoll = 0,
                     void *pAbortContext = 0);

#endif
