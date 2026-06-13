// Characterization of InputActions against the client oracle
// (Client/src/services/Input.lua): same observable semantics for the core
// subset; snapshot-driven so every case runs headless on fabricated
// snapshots. Grows across the input tasks.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputActions.hpp>

#include <Json.hpp>

using Arcane::InputActions;
using Arcane::InputSnapshot;

namespace
{
    // SDL numeric constants used by fabricated snapshots. Mirrored from
    // SDL3 headers (test exe does not include SDL); a mismatch fails the
    // round-trip tests loudly.
    constexpr uint32_t kScancodeW    = 26;   // SDL_SCANCODE_W
    constexpr uint32_t kScancodeDown = 81;   // SDL_SCANCODE_DOWN
    constexpr uint32_t kKeycodeSpace = 32;   // SDLK_SPACE = ' '
    constexpr uint32_t kKeycodeS     = 115;  // SDLK_S = 's'
    constexpr uint32_t kKeycodeLCtrl = 0x400000E0u;  // SDLK_LCTRL

    nlohmann::json ButtonMapDoc()
    {
        return nlohmann::json::parse(R"({
          "actionMaps": [
            { "name": "demo", "actions": [
              { "name": "jump", "type": "Button",
                "bindings": [ { "path": "<Keyboard>/space" } ] },
              { "name": "scroll", "type": "Button",
                "bindings": [ { "path": "<Keyboard>/scancode/down" } ] },
              { "name": "fire", "type": "Button",
                "bindings": [ { "path": "<Mouse>/leftButton" } ] },
              { "name": "ghost", "type": "Button",
                "bindings": [ { "path": "<Wheel>/up" } ] }
            ] }
          ]
        })");
    }
}

TEST_CASE("input: keycode, scancode and mouse buttons with edges", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(ButtonMapDoc()));
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.AddKeycode(kKeycodeSpace);
    snap.SetScancode(kScancodeDown);
    snap.mouseButtons = 0x1;  // LMB

    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("jump"));
    CHECK(input->Pressed("jump"));        // rising edge on first frame
    CHECK(input->Down("scroll"));
    CHECK(input->Down("fire"));
    CHECK_FALSE(input->Released("jump"));

    input->Update(1.0 / 60.0, snap);      // held: no edge
    CHECK(input->Down("jump"));
    CHECK_FALSE(input->Pressed("jump"));

    input->Update(1.0 / 60.0, InputSnapshot{});  // all released
    CHECK_FALSE(input->Down("jump"));
    CHECK(input->Released("jump"));
    CHECK_FALSE(input->Pressed("jump"));
}

TEST_CASE("input: unknown action and unknown path token", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(ButtonMapDoc()));   // contains <Wheel>/up
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, snap);

    CHECK_FALSE(input->Down("nonexistent"));    // unresolvable -> false
    CHECK(input->Strength("nonexistent") == 0.0f);
    CHECK_FALSE(input->Down("ghost"));          // zero-compiled binding
}

TEST_CASE("input: load failures", "[input]")
{
    auto input = InputActions::Create();
    CHECK_FALSE(input->LoadJson(nlohmann::json::parse("{}")));      // no actionMaps
    CHECK_FALSE(input->LoadJson(nlohmann::json::parse("[1,2]")));   // wrong shape

    // A good load after a bad one works (full replace).
    CHECK(input->LoadJson(ButtonMapDoc()));
}
