// stb_image + stb_image_write implementation TU for ArcaneAssetPipeline. Task 3: stb decode
// moves into this lib (TextureImporter.cpp calls stbi_load_from_memory) -- the workspace's rule
// is exactly one static-lib TU carrying the IMPLEMENTATION define per final linked binary
// (StbImpl.cpp in ArcaneClient.dll follows the same rule; it stays untouched -- ArcaneClient is
// a SharedLib, so its copy is DLL-internal and never collides with this one). stb_image_write
// rides along for AssetPipelineImporterTest.cpp's in-test PNG fixture generation, which needs
// the encoder but has no implementation TU of its own.
//
// ArcaneTests.exe links this lib (ArcaneAssetPipeline) directly, so VendorSmokeTest.cpp's own
// stb "arrival gate" test no longer carries its own IMPLEMENTATION defines -- that would be a
// SECOND static copy of the same symbols merged into the same exe (LNK2005). It still exercises
// the real stb_write/stb_load round trip; it just links against the symbols this TU provides.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
