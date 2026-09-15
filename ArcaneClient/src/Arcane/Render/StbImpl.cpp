// stb_image implementation TU for ArcaneClient.dll.
//
// THE TWO-COPIES RULE: stb is header-only, so every MODULE that calls it must
// own exactly one TU carrying the IMPLEMENTATION define -- a static copy per
// binary, never a shared export. The Core-DLL split moved Arcane/Assets/ (and
// with it the original Assets/StbImpl.cpp) into ArcaneCore.dll, leaving
// Platform/Window.cpp's stbi_load for the window icon as ArcaneClient's only
// remaining stb caller; this file is Client's own copy. The workspace already
// has the same shape three more times: ArcaneCore's Assets/StbImpl.cpp,
// ArcaneAssetPipeline's AssetPipeline/StbImpl.cpp, and the ArcaneTests exe's
// copy inside VendorSmokeTest.cpp. Separate binaries -- no duplicate-symbol
// clash, and no ODR question (nothing crosses the boundary; Core's image I/O
// reaches Client as exported ARCANE_CORE_API functions, not as stb symbols).
//
// Only the DECODER: nothing in ArcaneClient writes a PNG. The writer twin
// (stb_image_write, behind Arcane::WritePngRgba) lives in Core's copy, where
// its callers are.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
