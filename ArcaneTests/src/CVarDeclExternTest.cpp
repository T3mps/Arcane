// ARC_CVAR_EXTERN (settings spec 2026-10-03 s4.3): another translation unit
// names the handle CVarDeclTest.cpp defines, so the definition has external
// linkage and both read the one registry value.
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Config/CVarDecl.hpp>

namespace Arcane::Test
{
    ARC_CVAR_EXTERN(cvar_declProbe, std::int32_t);
}

TEST_CASE("ARC_CVAR_EXTERN names a handle defined in another translation unit", "[cvar]")
{
    CHECK(Arcane::Test::cvar_declProbe.Name() == "tests.decl.probe");
    CHECK(Arcane::Test::cvar_declProbe.Handle() == Arcane::CVarRegistry::Get().Find("tests.decl.probe"));
    CHECK(Arcane::Test::cvar_declProbe.Get() == 11);
}
