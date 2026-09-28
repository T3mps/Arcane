// Characterization of InputActions against the client oracle
// (Client/src/services/Input.lua): same observable semantics for the core
// subset; snapshot-driven so every case runs headless on fabricated
// snapshots. Grows across the input tasks.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputActions.hpp>
#include <Arcane/Input/InputActionAsset.hpp>

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

TEST_CASE("input: <Keyboard>/grave resolves to the backquote keycode", "[input]")
{
    // The console toggle ships on <Keyboard>/grave (data/EngineConfig/input.json).
    // SDL names that key by its glyph, so the LOVE->SDL table must hand
    // SDL_GetKeyFromName the backquote string, not "Grave" (hygiene pass
    // 2026-09-28: the binding was silently dropped at boot).
    constexpr uint32_t kKeycodeGrave = 96;   // SDLK_GRAVE = '`'
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(nlohmann::json::parse(R"({
      "actionMaps": [ { "name": "engine", "actions": [
        { "name": "console_toggle", "type": "Button",
          "bindings": [ { "path": "<Keyboard>/grave" } ] } ] } ]
    })")));
    input->SetBaseContext("engine");
    InputSnapshot snap;
    snap.AddKeycode(kKeycodeGrave);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Down("console_toggle"));
    CHECK(input->Pressed("console_toggle"));
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

// Task 3: Composites, vector processors, max-magnitude

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

// Task 4: Interaction phases (press/hold/tap) + Buffered

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

// Task 5: Context stack, active-device hysteresis, capture suppression

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

// Task 7: Round-trip load of the Playground demo asset

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

namespace
{
    nlohmann::json NativeActionDoc()
    {
        return nlohmann::json::parse(R"JSON({
            "version": 1,
            "id": "11111111-1111-4111-8111-111111111111",
            "defaultMap": "22222222-2222-4222-8222-222222222222",
            "controlSchemes": [
                { "id": "44444444-4444-4444-8444-444444444444",
                  "name": "KeyboardMouse", "bindingGroup": "KeyboardMouse" },
                { "id": "55555555-5555-4555-8555-555555555555",
                  "name": "Gamepad", "bindingGroup": "Gamepad" }
            ],
            "actionMaps": [
                { "id": "22222222-2222-4222-8222-222222222222",
                  "name": "Player", "actions": [
                    { "id": "66666666-6666-4666-8666-666666666666",
                      "name": "Move", "type": "Axis1D", "bindings": [
                        { "id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
                          "composite": "1DAxis", "groups": ["KeyboardMouse"],
                          "parts": [
                            { "id": "ffffffff-ffff-4fff-8fff-ffffffffffff",
                              "name": "positive", "path": "<Keyboard>/scancode/d" },
                            { "id": "99999999-9999-4999-8999-999999999999",
                              "name": "negative", "path": "<Keyboard>/scancode/a" }
                          ] },
                        { "id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
                          "path": "<Gamepad>/leftStick/x", "groups": ["Gamepad"] }
                      ] },
                    { "id": "77777777-7777-4777-8777-777777777777",
                      "name": "Confirm", "type": "Button", "bindings": [
                        { "id": "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
                          "path": "<Keyboard>/space", "groups": ["KeyboardMouse"] }
                      ] },
                    { "id": "88888888-8888-4888-8888-888888888888",
                      "name": "Jump", "type": "Button", "bindings": [
                        { "id": "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
                          "path": "<Keyboard>/space" }
                      ] }
                  ] },
                { "id": "33333333-3333-4333-8333-333333333333",
                  "name": "Menu", "blocking": true, "actions": [
                    { "id": "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
                      "name": "Confirm", "type": "Button", "bindings": [
                        { "id": "abababab-abab-4aba-8aba-abababababab",
                          "path": "<Keyboard>/space" }
                      ] }
                  ] }
            ]
        })JSON");
    }
}

TEST_CASE("input: native asset queries by stable action ID", "[input][native]")
{
    auto asset = Arcane::InputActionAsset::FromJson(NativeActionDoc());
    REQUIRE(asset);
    auto input = InputActions::Create();
    REQUIRE(input->LoadAsset(*asset));
    input->SetBaseContext("Player");

    const auto move = input->FindAction("Player", "Move");
    REQUIRE(move);
    CHECK(move->ToString() == "66666666-6666-4666-8666-666666666666");
    CHECK(input->FindAction("Move") == move);

    InputSnapshot snap;
    snap.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, snap);
    const auto value = input->Value(*move);
    CHECK(value.type == Arcane::InputActionType::Axis1D);
    CHECK(value.down);
    CHECK(value.scalar == Approx(1.0f));
    REQUIRE(input->ScalarValue(*move));
    CHECK(*input->ScalarValue(*move) == Approx(1.0f));

    const auto maps = input->Maps();
    REQUIRE(maps.size() == 2);
    CHECK(maps[0].name == "Player");
    CHECK(maps[1].name == "Menu");
    const auto actions = input->Actions(maps[0].id);
    REQUIRE(actions.size() == 3);
    CHECK(actions[0].id == *move);
    const auto bindings = input->Bindings(*move);
    REQUIRE(bindings.size() == 2);
    CHECK(bindings[0].composite == "1DAxis");
    CHECK(bindings[1].authoredPath == "<Gamepad>/leftStick/x");
}

TEST_CASE("input: ambiguous unqualified action names do not resolve", "[input][native]")
{
    auto asset = Arcane::InputActionAsset::FromJson(NativeActionDoc());
    REQUIRE(asset);
    auto input = InputActions::Create();
    REQUIRE(input->LoadAsset(*asset));
    CHECK_FALSE(input->FindAction("Confirm"));
    const auto player = input->FindAction("Player", "Confirm");
    const auto menu = input->FindAction("Menu", "Confirm");
    REQUIRE(player);
    REQUIRE(menu);
    CHECK(*player != *menu);
    CHECK_FALSE(input->FindAction("Missing", "Confirm"));
}

TEST_CASE("input: typed queries reject action type mismatches", "[input][native]")
{
    auto asset = Arcane::InputActionAsset::FromJson(NativeActionDoc());
    REQUIRE(asset);
    auto input = InputActions::Create();
    REQUIRE(input->LoadAsset(*asset));
    const auto move = input->FindAction("Player", "Move");
    const auto jump = input->FindAction("Player", "Jump");
    REQUIRE(move);
    REQUIRE(jump);
    CHECK_FALSE(input->ButtonDown(*move));
    CHECK_FALSE(input->VectorValue(*move));
    CHECK_FALSE(input->ScalarValue(*jump));

    input->SetBaseContext("Menu");
    InputSnapshot snap;
    snap.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, snap);
    CHECK_FALSE(input->Value(*move).down);
    CHECK(input->Value(*move).scalar == 0.0f);
    CHECK_FALSE(input->Value(Arcane::Guid::Nil()).down);
}

TEST_CASE("input: scheme groups filter bindings", "[input][native]")
{
    auto asset = Arcane::InputActionAsset::FromJson(NativeActionDoc());
    REQUIRE(asset);
    auto input = InputActions::Create();
    REQUIRE(input->LoadAsset(*asset));
    input->SetBaseContext("Player");
    const auto move = input->FindAction("Player", "Move");
    const auto jump = input->FindAction("Player", "Jump");
    REQUIRE(move);
    REQUIRE(jump);

    InputSnapshot pad;
    pad.gamepadConnected = true;
    pad.gamepadAxes[0] = 0.8f;
    input->Update(1.0 / 60.0, pad);
    CHECK(input->Value(*move).scalar == Approx(0.8f));

    REQUIRE(input->SetControlScheme("KeyboardMouse"));
    input->Update(1.0 / 60.0, pad);
    CHECK(input->Value(*move).scalar == 0.0f);
    InputSnapshot key;
    key.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, key);
    CHECK(input->Value(*move).scalar == Approx(1.0f));

    REQUIRE(input->SetControlScheme("Gamepad"));
    input->Update(1.0 / 60.0, key);
    CHECK(input->Value(*move).scalar == 0.0f);
    input->Update(1.0 / 60.0, pad);
    CHECK(input->Value(*move).scalar == Approx(0.8f));
    InputSnapshot space;
    space.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, space);
    CHECK(input->Value(*jump).down); // ungrouped bindings stay eligible
    CHECK_FALSE(input->SetControlScheme("Missing"));
}

TEST_CASE("input: binding path override affects evaluation without mutating asset data", "[input][native]")
{
    auto asset = Arcane::InputActionAsset::FromJson(NativeActionDoc());
    REQUIRE(asset);
    auto input = InputActions::Create();
    REQUIRE(input->LoadAsset(*asset));
    input->SetBaseContext("Player");
    const auto jump = input->FindAction("Player", "Jump");
    const auto move = input->FindAction("Player", "Move");
    REQUIRE(jump);
    REQUIRE(move);
    const auto jumpBinding = Arcane::Guid::FromString("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    const auto positivePart = Arcane::Guid::FromString("ffffffff-ffff-4fff-8fff-ffffffffffff");
    REQUIRE(jumpBinding);
    REQUIRE(positivePart);
    REQUIRE(input->SetBindingPath(*jumpBinding, "<Keyboard>/scancode/w"));
    REQUIRE(input->SetBindingPath(*positivePart, "<Keyboard>/scancode/s"));

    InputSnapshot space;
    space.AddKeycode(kKeycodeSpace);
    input->Update(1.0 / 60.0, space);
    CHECK_FALSE(input->Value(*jump).down);
    InputSnapshot rebound;
    rebound.SetScancode(kScancodeW);
    rebound.SetScancode(kScancodeS2);
    input->Update(1.0 / 60.0, rebound);
    CHECK(input->Value(*jump).down);
    CHECK(input->Value(*move).scalar == Approx(1.0f));

    const auto jumpBindings = input->Bindings(*jump);
    REQUIRE(jumpBindings.size() == 1);
    CHECK(jumpBindings[0].authoredPath == "<Keyboard>/space");
    CHECK(jumpBindings[0].effectivePath == "<Keyboard>/scancode/w");
    CHECK_FALSE(input->BindingDisplayString(*jumpBinding).empty());
    CHECK(asset->ToJson()["actionMaps"][0]["actions"][2]["bindings"][0]["path"] == "<Keyboard>/space");
    CHECK_FALSE(input->SetBindingPath(Arcane::Guid::Nil(), "<Keyboard>/space"));
}
