// miniaudio implementation TU. MA_NO_DEVICE_IO is deliberately NOT set --
// the engine needs device IO in M2+; this TU proves the full implementation
// compiles. No device is opened by the smoke test.
// C4244 (size_t truncation): upstream narrowing inside miniaudio's own
// implementation. Vendored single-header code -- not ours to patch; scope
// the suppression to just this include, not our surrounding TU.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
