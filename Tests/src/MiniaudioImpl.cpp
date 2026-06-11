// miniaudio implementation TU. MA_NO_DEVICE_IO is deliberately NOT set --
// the engine needs device IO in M2+; this TU proves the full implementation
// compiles. No device is opened by the smoke test.
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
