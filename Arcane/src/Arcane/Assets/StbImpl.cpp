// stb_image implementation TU for the Arcane DLL.
// Each module that uses stb must own exactly one TU with the
// IMPLEMENTATION define. The ArcaneTests exe has its own copy in
// VendorSmokeTest.cpp -- two static compilation units in separate
// binaries; no duplicate-symbol clash.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
