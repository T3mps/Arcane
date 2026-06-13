// BuildInfo() proves the Arcane.dll boundary (ARCANE_API export resolved
// through the import lib at run time). The VersionString() check is a
// Core-consistency pin, not a boundary check -- it is inline and compiles
// into this exe.

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include <Arcane/Base/Engine.hpp>
#include <Arcane/Version.hpp>

TEST_CASE("engine dll: BuildInfo exports and matches the Core version line", "[engine]")
{
    const char* info = Arcane::BuildInfo();
    REQUIRE(info != nullptr);
    REQUIRE(std::strlen(info) > 0);
    // Same milestone tag as Core's VersionString.
    REQUIRE(std::strstr(info, "(M2b)") != nullptr);
    REQUIRE(std::strstr(Arcane::VersionString(), "(M2b)") != nullptr);
}
