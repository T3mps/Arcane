#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <Arcane/Version.hpp>

TEST_CASE("Core links and reports a version", "[core]")
{
    REQUIRE(Arcane::kVersionMajor == 0);
    REQUIRE(std::strlen(Arcane::VersionString()) > 0);
}
