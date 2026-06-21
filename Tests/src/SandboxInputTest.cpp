#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Base/Runtime.hpp>

#include "Helpers/TestTypeContext.hpp"

TEST_CASE("input snapshot carries cursor position and stays POD", "[sandbox]")
{
    static_assert(std::is_trivially_copyable_v<Arcane::InputSnapshot>);
    Arcane::InputSnapshot s{};
    s.mouseX = 12.5f; s.mouseY = -3.0f;
    CHECK(s.mouseX == 12.5f);
    CHECK(s.mouseY == -3.0f);
}

TEST_CASE("Runtime stores and returns the latest input snapshot", "[sandbox]")
{
    // Use the process-wide shared TypeContext (test_main installs it) -- the
    // default Runtime ctor would overwrite the per-module context slot and leave
    // it dangling after rt is destroyed (order-dependent corruption for later tests).
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    Arcane::InputSnapshot s{}; s.mouseX = 7.0f; s.mouseButtons = 0x1;
    rt.SetInputSnapshot(s);
    CHECK(rt.Input().mouseX == 7.0f);
    CHECK(rt.Input().mouseButtons == 0x1);
}
