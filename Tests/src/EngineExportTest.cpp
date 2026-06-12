// Proves the Arcane.dll boundary: an exported function is callable through
// the import lib and the DLL actually loads at run time.

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
    REQUIRE(std::strstr(info, "(M1)") != nullptr);
    REQUIRE(std::strstr(Arcane::VersionString(), "(M1)") != nullptr);
}
