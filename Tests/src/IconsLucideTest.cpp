// Guards IconsLucide.h against a bad regeneration ([editor], CPU-only).
#include <catch2/catch_test_macros.hpp>

#include "IconsLucide.h"

namespace
{
    // Decode a UTF-8 macro value to its codepoint (icons are 3-byte U+Exxx here).
    unsigned Utf8ToCp(const char* s)
    {
        const unsigned char* u = reinterpret_cast<const unsigned char*>(s);
        if (u[0] < 0x80u) return u[0];
        if ((u[0] >> 5) == 0x6u) return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
        if ((u[0] >> 4) == 0xEu)
            return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
        return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12)
             | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
    }
}

TEST_CASE("IconsLucide.h codepoints are stable", "[editor]")
{
    CHECK(Utf8ToCp(ICON_LC_PLAY)         == 0xE13Cu);
    CHECK(Utf8ToCp(ICON_LC_PAUSE)        == 0xE12Eu);
    CHECK(Utf8ToCp(ICON_LC_SQUARE)       == 0xE167u);
    CHECK(Utf8ToCp(ICON_LC_STEP_FORWARD) == 0xE3EAu);
    CHECK(Utf8ToCp(ICON_LC_UNDO)         == 0xE19Bu);
    CHECK(Utf8ToCp(ICON_LC_REDO)         == 0xE143u);
    CHECK(Utf8ToCp(ICON_LC_MOVE)         == 0xE121u);
    CHECK(Utf8ToCp(ICON_LC_ROTATE_3D)    == 0xE2EAu);
    CHECK(Utf8ToCp(ICON_LC_SCALE_3D)     == 0xE2EBu);
    CHECK(Utf8ToCp(ICON_LC_BOX)          == 0xE061u);
    CHECK(Utf8ToCp(ICON_LC_GLOBE)        == 0xE0E8u);
    CHECK(ICON_LC_MIN == 0xE038u);
    CHECK(ICON_LC_MAX == 0xE6FBu);
}
