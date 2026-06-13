// Characterization of InputActions against the client oracle
// (Client/src/services/Input.lua): same observable semantics for the core
// subset; snapshot-driven so every case runs headless on fabricated
// snapshots. Grows across the input tasks.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputActions.hpp>

#include <Json.hpp>

using Arcane::InputActions;
using Arcane::InputSnapshot;
using Catch::Approx;

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

namespace
{
    nlohmann::json PadAndChordDoc()
    {
        return nlohmann::json::parse(R"JSON({
          "actionMaps": [
            { "name": "demo", "actions": [
              { "name": "confirm", "type": "Button",
                "bindings": [ { "path": "<Gamepad>/buttonSouth" } ] },
              { "name": "aim", "type": "Value",
                "bindings": [ { "path": "<Gamepad>/leftTrigger",
                                "processors": [ "deadzone(min=0.125,max=0.925)" ] } ] },
              { "name": "save", "type": "Button",
                "bindings": [ { "path": "<Keyboard>/lctrl+<Keyboard>/s" } ] },
              { "name": "throttle", "type": "Value",
                "bindings": [ { "path": "<Gamepad>/rightTrigger",
                                "processors": [ "invert", "scale(factor=2)" ] } ] }
            ] }
          ]
        })JSON");
    }
}

TEST_CASE("input: gamepad buttons and trigger axes", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PadAndChordDoc()));
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.gamepadConnected = true;
    snap.gamepadButtons   = 1 << 0;   // buttonSouth
    snap.gamepadAxes[4]   = 0.6f;     // leftTrigger
    snap.gamepadAxes[5]   = 0.5f;     // rightTrigger

    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("confirm"));
    // deadzone(0.125, 0.925): 0.6 -> (0.6-0.125)/(0.925-0.125) = 0.59375
    CHECK(input->Strength("aim") == Approx(0.59375f).margin(1e-4));
    // invert then scale(2): 0.5 -> -0.5 -> -1.0
    CHECK(input->Strength("throttle") == Approx(-1.0f).margin(1e-4));
    CHECK(input->Down("throttle"));   // |strength| >= 0.5 counts as down
}

TEST_CASE("input: deadzone boundary values", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PadAndChordDoc()));
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.gamepadConnected = true;

    snap.gamepadAxes[4] = 0.1f;   // below min -> 0
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Strength("aim") == 0.0f);

    snap.gamepadAxes[4] = 0.95f;  // above max -> 1
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Strength("aim") == Approx(1.0f));
}

TEST_CASE("input: chord requires every part", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PadAndChordDoc()));
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.AddKeycode(kKeycodeS);
    input->Update(1.0 / 60.0, snap);
    CHECK_FALSE(input->Down("save"));          // s alone

    snap.AddKeycode(kKeycodeLCtrl);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("save"));                // lctrl+s
    CHECK(input->Pressed("save"));
}

// ── Task 3: Composites, vector processors, max-magnitude ─────────────────────

namespace
{
    constexpr uint32_t kScancodeA  = 4;   // SDL_SCANCODE_A
    constexpr uint32_t kScancodeS2 = 22;  // SDL_SCANCODE_S
    constexpr uint32_t kScancodeD  = 7;   // SDL_SCANCODE_D

    nlohmann::json MoveDoc()
    {
        // RAW-STRING NOTE: deadzone(min=0.125,max=0.925) contains )"
        // which would close a plain R"(...)". Use tagged delimiter R"JSON(...)JSON".
        return nlohmann::json::parse(R"JSON({
          "actionMaps": [
            { "name": "world", "actions": [
              { "name": "move", "type": "Value", "controlType": "Vector2",
                "bindings": [
                  { "composite": "2DVector",
                    "parts": {
                      "up":    [ { "path": "<Keyboard>/scancode/w" } ],
                      "down":  [ { "path": "<Keyboard>/scancode/s" } ],
                      "left":  [ { "path": "<Keyboard>/scancode/a" } ],
                      "right": [ { "path": "<Keyboard>/scancode/d" } ]
                    },
                    "processors": [ "normalizeVector2" ] },
                  { "path": "<Gamepad>/leftStick",
                    "processors": [ "deadzone(min=0.125,max=0.925)" ] }
                ] },
              { "name": "zoom", "type": "Value",
                "bindings": [
                  { "composite": "1DAxis",
                    "parts": {
                      "positive": [ { "path": "<Keyboard>/scancode/w" } ],
                      "negative": [ { "path": "<Keyboard>/scancode/s" } ]
                    } } ] }
            ] }
          ]
        })JSON");
    }
}

TEST_CASE("input: 2DVector composite with normalize", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(MoveDoc()));
    input->SetBaseContext("world");

    InputSnapshot snap;
    snap.SetScancode(kScancodeW);          // up only
    input->Update(1.0 / 60.0, snap);
    auto v = input->Axis("move");
    CHECK(v.x == Approx(0.0f));
    CHECK(v.y == Approx(-1.0f));    // up = -y (screen y-down, oracle)
    CHECK(input->Down("move"));            // magnitude >= 0.5

    InputSnapshot diag;
    diag.SetScancode(kScancodeW);
    diag.SetScancode(kScancodeD);          // up+right
    input->Update(1.0 / 60.0, diag);
    v = input->Axis("move");
    CHECK(v.x == Approx(0.70710678f).margin(1e-4));   // normalized
    CHECK(v.y == Approx(-0.70710678f).margin(1e-4));
    CHECK(input->Strength("move") == Approx(1.0f).margin(1e-4));
}

TEST_CASE("input: 1DAxis composite", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(MoveDoc()));
    input->SetBaseContext("world");

    InputSnapshot snap;
    snap.SetScancode(kScancodeS2);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Strength("zoom") == Approx(-1.0f));  // negative - positive
}

TEST_CASE("input: max-magnitude binding wins (stick vs wasd)", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(MoveDoc()));
    input->SetBaseContext("world");

    // Stick at full right, no keys: stick binding supplies the vector.
    InputSnapshot snap;
    snap.gamepadConnected = true;
    snap.gamepadAxes[0] = 1.0f;            // leftStick/x
    input->Update(1.0 / 60.0, snap);
    auto v = input->Axis("move");
    CHECK(v.x == Approx(1.0f).margin(1e-4));
    CHECK(v.y == Approx(0.0f).margin(1e-4));

    // Weak stick + full WASD: the larger (keyboard) vector wins.
    InputSnapshot both;
    both.gamepadConnected = true;
    both.gamepadAxes[0] = 0.3f;
    both.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, both);
    CHECK(input->Axis("move").x == Approx(1.0f).margin(1e-4));
}
