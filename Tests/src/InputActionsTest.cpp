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

// ── Task 4: Interaction phases (press/hold/tap) + Buffered ───────────────────

namespace
{
    nlohmann::json PhasesDoc()
    {
        // RAW-STRING GOTCHA: hold(duration=0.3) and tap(duration=0.2) embed )"
        // which terminates a plain R"(...)". Use tagged delimiter R"JSON(...)JSON".
        return nlohmann::json::parse(R"JSON({
          "actionMaps": [
            { "name": "demo", "actions": [
              { "name": "charge", "type": "Button",
                "interactions": [ "hold(duration=0.3)" ],
                "bindings": [ { "path": "<Keyboard>/space" } ] },
              { "name": "flick", "type": "Button",
                "interactions": [ "tap(duration=0.2)" ],
                "bindings": [ { "path": "<Keyboard>/space" } ] },
              { "name": "jump", "type": "Button",
                "bindings": [ { "path": "<Keyboard>/space" } ] }
            ] }
          ]
        })JSON");
    }
}

TEST_CASE("input: hold fires performed once at duration", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PhasesDoc()));
    input->SetBaseContext("demo");

    InputSnapshot down; down.AddKeycode(kKeycodeSpace);

    input->Update(0.1, down);                  // rising frame: heldTime stays 0
    CHECK(input->Started("charge"));
    CHECK_FALSE(input->Performed("charge"));
    input->Update(0.2, down);                  // heldTime 0.2 (rising adds nothing)
    CHECK_FALSE(input->Performed("charge"));
    input->Update(0.15, down);                 // heldTime 0.35 >= 0.3
    CHECK(input->Performed("charge"));
    input->Update(0.1, down);                  // fires only once
    CHECK_FALSE(input->Performed("charge"));
    input->Update(0.1, InputSnapshot{});       // release after performing
    CHECK_FALSE(input->Canceled("charge"));    // _perfFired -> no cancel
}

TEST_CASE("input: hold canceled on early release; tap is the inverse", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PhasesDoc()));
    input->SetBaseContext("demo");

    InputSnapshot down; down.AddKeycode(kKeycodeSpace);

    input->Update(0.1, down);
    input->Update(0.1, InputSnapshot{});       // released at 0.1 < 0.3
    CHECK(input->Canceled("charge"));
    CHECK(input->Performed("flick"));          // 0.1 <= tap 0.2 -> performed

    input->Update(0.1, down);                  // press again (rising: heldTime 0)
    input->Update(0.15, down);                 // heldTime 0.15
    input->Update(0.1, down);                  // heldTime 0.25 > 0.2: tap invalid
    input->Update(0.1, InputSnapshot{});
    CHECK(input->Canceled("flick"));

    // Default press interaction: performed == rising, canceled == falling.
    input->Update(0.1, down);
    CHECK(input->Performed("jump"));
    input->Update(0.1, InputSnapshot{});
    CHECK(input->Canceled("jump"));
}

TEST_CASE("input: buffered press consumes once within window", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PhasesDoc()));
    input->SetBaseContext("demo");

    InputSnapshot down; down.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, down);
    input->Update(1.0 / 60.0, InputSnapshot{});
    input->Update(1.0 / 60.0, InputSnapshot{});   // 2 frames since press

    CHECK(input->Buffered("jump", 6));
    CHECK_FALSE(input->Buffered("jump", 6));      // consumed

    input->Update(1.0 / 60.0, down);              // new press
    for (int i = 0; i < 8; ++i)
        input->Update(1.0 / 60.0, InputSnapshot{});
    CHECK_FALSE(input->Buffered("jump", 6));      // outside window
}

TEST_CASE("input: radial deadzone scales a stick vector", "[input]")
{
    // leftStick binding in MoveDoc has processors: [ "deadzone(min=0.125,max=0.925)" ]
    // only -- no normalizeVector2 (that lives on the WASD composite).
    // Input: (0.3, 0), len=0.3 inside [0.125, 0.925].
    //   scaled = (0.3 - 0.125) / (0.925 - 0.125) = 0.175 / 0.8 = 0.21875
    //   k      = scaled / len  = 0.21875 / 0.3
    //   result = (0.3 * k, 0)  = (0.21875, 0)
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(MoveDoc()));
    input->SetBaseContext("world");

    InputSnapshot snap;
    snap.gamepadConnected = true;
    snap.gamepadAxes[0] = 0.3f;   // leftStick/x
    snap.gamepadAxes[1] = 0.0f;   // leftStick/y
    input->Update(1.0 / 60.0, snap);

    auto v = input->Axis("move");
    CHECK(v.x == Approx(0.21875f).margin(1e-4));
    CHECK(v.y == Approx(0.0f).margin(1e-4));
}

// ── Task 5: Context stack, active-device hysteresis, capture suppression ──────

namespace
{
    nlohmann::json ContextDoc()
    {
        return nlohmann::json::parse(R"JSON({
          "actionMaps": [
            { "name": "world", "actions": [
              { "name": "interact", "type": "Button",
                "bindings": [ { "path": "<Keyboard>/space" } ] },
              { "name": "fire", "type": "Button",
                "bindings": [ { "path": "<Mouse>/leftButton" } ] },
              { "name": "move", "type": "Value", "controlType": "Vector2",
                "bindings": [ { "path": "<Gamepad>/leftStick" } ] }
            ] },
            { "name": "menu", "blocking": true, "actions": [
              { "name": "confirm", "type": "Button",
                "bindings": [ { "path": "<Keyboard>/space" } ] }
            ] }
          ]
        })JSON");
    }
}

TEST_CASE("input: blocking map shadows lower contexts", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(ContextDoc()));
    input->SetBaseContext("world");

    InputSnapshot snap; snap.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("interact"));

    input->PushContext("menu");                  // blocking
    CHECK(input->ActiveContext() == "menu");
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("confirm"));
    CHECK_FALSE(input->Down("interact"));        // blocked below the menu

    input->PopContext();
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("interact"));

    input->PushContext("nope");                  // unknown: warn + no-op
    CHECK(input->ActiveContext() == "world");

    input->PushContext("menu");
    input->SwapBaseContext("world");             // bottom swap keeps the menu
    CHECK(input->ActiveContext() == "menu");
}

TEST_CASE("input: active-device hysteresis", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(ContextDoc()));
    input->SetBaseContext("world");

    CHECK(input->ActiveDevice() == Arcane::InputDevice::Kbm);   // default

    InputSnapshot stick;
    stick.gamepadConnected = true;
    stick.gamepadAxes[0] = 0.9f;                 // strong stick
    input->Update(1.0 / 60.0, stick);
    CHECK(input->ActiveDevice() == Arcane::InputDevice::Gamepad);

    InputSnapshot weak;
    weak.gamepadConnected = true;
    weak.gamepadAxes[0] = 0.3f;                  // below 0.5: no flip back
    input->Update(1.0 / 60.0, weak);
    CHECK(input->ActiveDevice() == Arcane::InputDevice::Gamepad);

    InputSnapshot key; key.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, key);              // kbm wins immediately
    CHECK(input->ActiveDevice() == Arcane::InputDevice::Kbm);
}

TEST_CASE("input: ImGui capture suppresses kbm, not pad, with clean edges", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(ContextDoc()));
    input->SetBaseContext("world");

    InputSnapshot snap;
    snap.AddKeycode(kKeycodeSpace);
    snap.gamepadConnected = true;
    snap.gamepadAxes[0] = 0.9f;
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("interact"));

    snap.wantCaptureKeyboard = true;             // text field grabs focus mid-hold
    input->Update(1.0 / 60.0, snap);
    CHECK_FALSE(input->Down("interact"));
    CHECK(input->Released("interact"));          // falling edge fires (no stuck press)
    CHECK(input->Axis("move").x > 0.5f);         // gamepad unaffected

    snap.wantCaptureKeyboard = false;
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Pressed("interact"));           // returns as a fresh press

    // Mouse capture is independent of keyboard capture.
    snap.mouseButtons = 0x1;                     // LMB
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("fire"));
    snap.wantCaptureMouse = true;                // ImGui drag grabs the mouse
    input->Update(1.0 / 60.0, snap);
    CHECK_FALSE(input->Down("fire"));
    CHECK(input->Released("fire"));
    CHECK(input->Down("interact"));              // keyboard unaffected
}

// ── Task 7: Round-trip load of the Playground demo asset ─────────────────────

TEST_CASE("input: round-trip load of the Playground demo asset", "[input]")
{
    auto input = Arcane::InputActions::Create();
    REQUIRE(input->LoadFile("data/input_actions.json"));
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.SetScancode(kScancodeW);
    snap.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Strength("move") == Approx(1.0f).margin(1e-4));
    CHECK_FALSE(input->Down("quit"));
}
