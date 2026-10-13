// stb_image + stb_image_write implementation TU for the Arcane DLL.
// Each module that uses stb must own exactly one TU with the
// IMPLEMENTATION define. The ArcaneTests exe has its own copy in
// VendorSmokeTest.cpp -- two static compilation units in separate
// binaries; no duplicate-symbol clash.

#include <Arcane/Modules/ArcaneAssetsApi.hpp>
#define STBIDEF ARC_ASSETS_API
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// The writer twin (WritePngRgba / WriteThumbnailPngRgba ride it). Same
// one-TU rule. Exported like the decoder: ArcaneTests' PNG fixtures call
// stbi_write_png* directly, and since P2 no static stb copy reaches that exe.
#define STBIWDEF extern "C" ARC_ASSETS_API   // stb_image_write.h's C++ default is extern "C"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
