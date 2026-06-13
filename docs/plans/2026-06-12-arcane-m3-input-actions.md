# Arcane Input Actions (M3 tail) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A snapshot-driven, Unity-style input action system in Arcane.dll (maps -> actions -> bindings with chords/composites/processors/interaction phases, context stack, ImGui capture suppression), feature-parity with the client oracle's core subset, wired into the Playground demo.

**Architecture:** Three layers behind one seam: a fixed-size POD `InputSnapshot` (public; fabricated by tests, serializable for future replay), an SDL-facing `InputDevices` sampler (owns the gamepad handle; capture flags passed in by the host), and the exported `InputActions` facade whose internal evaluator compiles `input_actions.json` at load (paths -> ControlIds, processor tokens -> ops) and evaluates compiled data per frame. Spec: `docs/superpowers/specs/2026-06-12-arcane-input-actions-design.md`. Oracle: `Client/src/services/Input.lua` (same observable semantics for the in-scope features; architecture deliberately improved — NOT a line-by-line port).

**Tech Stack:** SDL3 (keyboard/mouse/gamepad state + name lookups), nlohmann/json (`<Json.hpp>`), glm, Catch2. No new vendored deps.

**Deferred (designed-for, NOT in this plan):** rebinding + keybinds persistence, glyphs, event-order lookups, recording/replay. The compiled model keeps original path strings + group tags so they bolt on later.

---

## Constraints carried into every task

- UTF-8 no BOM, ASCII comments; Write/Edit tools. Never run `db-reset.bat` / `clean.bat --deep` / `docker compose down -v`. Do not modify `Server/`, `Client/`, `Tools/` (reading Client Lua is required — `Client/src/services/Input.lua` is the semantics oracle; READ the relevant section before implementing each feature).
- msbuild is NOT on PATH: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal` from `D:\dev\starworks\Gacha\Arcane`. Run `.\GenerateProjects.bat` first whenever files are added or premake changes.
- Run tests FROM `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\` (data/ and shaders/ resolve CWD-relative). Baseline entering: **491 assertions / 57 cases** green. All new input tests are CPU-only (`[input]` tag, no `[gpu]`) except the sampler smoke (`[input][platform]`, hidden window, no GPU device).
- API-adaptation rule: SDL3 signatures cited below were written from SDL3 knowledge, not verified against the vcpkg headers — check the actual header (`vcpkg installed x64-windows-static-md/include/SDL3/`) before use, adapt minimally, record deviations in the commit body.
- Established module pattern (Assets/TextSystem are the references): exported pure-virtual class + static `Create` + anonymous-namespace impl in the .cpp. Internal helpers that tests need directly must be header-only (AssetCache precedent); otherwise keep them in the .cpp.
- Commit per task, `type(scope):` + trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## File structure

```
Arcane/Arcane/src/Arcane/Input/InputSnapshot.hpp    NEW public POD seam (no export macro needed)
Arcane/Arcane/src/Arcane/Input/InputActions.hpp     NEW exported facade
Arcane/Arcane/src/Arcane/Input/InputActions.cpp     NEW compiled-asset model + evaluator (internal)
Arcane/Arcane/src/Arcane/Input/InputDevices.hpp     NEW exported SDL sampler
Arcane/Arcane/src/Arcane/Input/InputDevices.cpp     NEW
Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.{hpp,cpp} MODIFIED — WantCaptureKeyboard/Mouse accessors
Arcane/Arcane/src/Arcane/Platform/Window.{hpp,cpp}  MODIFIED — remove hardcoded ESC->quit
Arcane/Playground/data/input_actions.json           NEW demo asset (single source)
Arcane/Playground/src/main.cpp                      MODIFIED — quit/toggle_stats/move/swap_backend via actions
Arcane/premake5.lua                                 MODIFIED — postbuild copies of the demo asset
Arcane/Tests/src/InputActionsTest.cpp               NEW CPU characterization (grows across Tasks 1-5)
Arcane/Tests/src/InputDevicesTest.cpp               NEW sampler smoke
docs/superpowers/specs/2026-06-11-engine-architecture-design.md  MODIFIED (Task 7)
```

All testing goes through the exported facade (`LoadJson` + fabricated `InputSnapshot` + queries) — internals stay in the .cpp. The facade header is written COMPLETE in Task 1; later tasks replace trivial stub overrides with real logic (the impl must override all pure virtuals to compile — stubs return false/0/{} until their task lands, and each task's tests land with its features).

---

### Task 1: Snapshot + facade skeleton + button bindings (keyboard/mouse) + edges

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Input/InputSnapshot.hpp`, `Input/InputActions.hpp`, `Input/InputActions.cpp`, `Arcane/Tests/src/InputActionsTest.cpp`

- [ ] **Step 1: Write `InputSnapshot.hpp`**

```cpp
#pragma once

// Per-frame device snapshot: the seam between SDL device state and the
// action evaluator (spec: 2026-06-12-arcane-input-actions-design.md).
// Fixed-size POD, memcpy-serializable -- the contract that keeps input
// recording/replay a later feature instead of a redesign. Tests fabricate
// these directly; InputDevices::Sample fills them from SDL.

#include <cstdint>
#include <type_traits>

namespace Arcane
{
    struct InputSnapshot
    {
        static constexpr uint32_t kScancodeWords   = 8;   // 512 scancodes
        static constexpr uint32_t kMaxKeycodesDown = 16;

        // Keyboard: physical scancodes as a bitset (SDL scancode index),
        // plus the layout-mapped keycodes currently down (SDL_Keycode
        // values; capped -- 16 simultaneous keys exceeds USB rollover).
        uint64_t scancodes[kScancodeWords]      = {};
        uint32_t keycodesDown[kMaxKeycodesDown] = {};
        uint32_t keycodeCount                   = 0;

        // Mouse: bit (sdlButton - 1): LMB=bit0, RMB=bit1, MMB=bit2, X1, X2.
        uint8_t mouseButtons = 0;

        // First connected gamepad. Button bits follow the GamepadButton
        // order in InputActions.cpp; axes are lx ly rx ry (-1..1, +y down)
        // then lt rt (0..1).
        bool     gamepadConnected = false;
        uint16_t gamepadButtons   = 0;
        float    gamepadAxes[6]   = {};

        // UI capture: when set, keyboard-/mouse-sourced controls read as
        // released (ImGui text fields / drags). Gamepad is never captured.
        bool wantCaptureKeyboard = false;
        bool wantCaptureMouse    = false;

        bool ScancodeDown(uint32_t sc) const
        {
            return sc < 512 && (scancodes[sc >> 6] >> (sc & 63)) & 1;
        }
        void SetScancode(uint32_t sc)
        {
            if (sc < 512) scancodes[sc >> 6] |= (uint64_t)1 << (sc & 63);
        }
        bool KeycodeDown(uint32_t kc) const
        {
            for (uint32_t i = 0; i < keycodeCount; ++i)
                if (keycodesDown[i] == kc) return true;
            return false;
        }
        void AddKeycode(uint32_t kc)
        {
            if (keycodeCount < kMaxKeycodesDown)
                keycodesDown[keycodeCount++] = kc;
        }
    };

    static_assert(std::is_trivially_copyable_v<InputSnapshot>,
                  "snapshot must stay POD (replay/serialization contract)");
}
```

- [ ] **Step 2: Write the COMPLETE facade header `InputActions.hpp`**

```cpp
#pragma once

// Input module facade: Unity-style action maps -> actions (Button|Value)
// -> bindings (simple paths, '+' chords, 2DVector/1DAxis composites),
// evaluated once per frame from an InputSnapshot. Feature-parity port of
// the client oracle's core subset (Client/src/services/Input.lua);
// snapshot-driven architecture per the 2026-06-12 input-actions spec.
// Deferred: rebinding/persistence, glyphs, event-order lookups, replay.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <Json.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Arcane
{
    enum class InputDevice : uint8_t { Kbm, Gamepad };

    class ARCANE_API InputActions
    {
    public:
        static std::unique_ptr<InputActions> Create();
        virtual ~InputActions() = default;

        // Parses an input_actions.json document (actionMaps[] schema, the
        // Tools InputEditor shape). FULL REPLACE: previous maps and the
        // context stack are reset. Returns false on malformed or
        // schema-violating input (one ARC_WARN). Unknown device/control
        // path tokens compile to constant-zero bindings with one
        // load-time warn naming map/action/path.
        virtual bool LoadJson(const nlohmann::json& doc) = 0;

        // Reads + parses the file. Relative paths resolve against the exe
        // (ShaderLibrary pattern). False on missing/unreadable/malformed.
        virtual bool LoadFile(const std::filesystem::path& path) = 0;

        // Evaluates every map's actions from the snapshot. Once per frame,
        // before queries. dt feeds hold/tap interaction timing.
        virtual void Update(double dt, const InputSnapshot& snap) = 0;

        // Context stack: queries resolve top-down; a 'blocking' map stops
        // fall-through. Unknown map names warn + no-op.
        virtual void PushContext(std::string_view map) = 0;
        virtual void PopContext() = 0;
        virtual void SetBaseContext(std::string_view map) = 0;  // resets stack
        virtual void SwapBaseContext(std::string_view map) = 0; // bottom only
        virtual std::string ActiveContext() const = 0;

        // Unresolvable actions return false / 0 / zero vector.
        virtual bool Down(std::string_view action) const = 0;      // held
        virtual bool Pressed(std::string_view action) const = 0;   // rising
        virtual bool Released(std::string_view action) const = 0;  // falling
        virtual bool Started(std::string_view action) const = 0;   // phase
        virtual bool Performed(std::string_view action) const = 0; // phase
        virtual bool Canceled(std::string_view action) const = 0;  // phase
        virtual float Strength(std::string_view action) const = 0;
        virtual glm::vec2 Axis(std::string_view action) const = 0;
        // Pressed within the last `frames` frames; consumes on success.
        virtual bool Buffered(std::string_view action, int frames = 6) = 0;

        virtual InputDevice ActiveDevice() const = 0;
    };
}
```

- [ ] **Step 3: Write the failing tests (first slice of `InputActionsTest.cpp`)**

```cpp
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
```

- [ ] **Step 4: Run to confirm compile failure** (`GenerateProjects.bat`, build — expect link/compile errors for the missing module).

- [ ] **Step 5: Implement `InputActions.cpp` — skeleton + this slice.** Anonymous-namespace impl (Assets.cpp is the structural reference). ALL pure virtuals overridden now; not-yet-implemented ones are trivial stubs (`return {};` etc.) replaced by their tasks. This task implements for real: `LoadJson`, `LoadFile` (file read via the local binary-ifstream + exe-relative-resolve pattern copied from Assets.cpp — implement locally, do not export), `Update` (per-action eval with prev/cur edges; threshold 0.5), `SetBaseContext`, `Down/Pressed/Released`, and resolution through a single-entry context stack. Internal model (design contract — write as REAL code):

```cpp
// enum class ControlSource : uint8_t { None, Scancode, Keycode, MouseButton,
//                                      GamepadButton, GamepadAxis, GamepadStick };
// struct ControlId { ControlSource source = ControlSource::None; uint32_t code = 0; };
//
// Path grammar (oracle: Input.lua parsePath/resolveSingle):
//   <Keyboard>/<lovename>            -> Keycode  (SDL_GetKeyFromName after
//                                       LOVE->SDL name translation, table below)
//   <Keyboard>/scancode/<lovename>   -> Scancode (SDL_GetScancodeFromName, same table)
//   <Mouse>/leftButton|rightButton|middleButton|button/N -> MouseButton bit 0|1|2|N-1
//   <Gamepad>/<token>                -> GamepadButton bit / GamepadAxis idx / GamepadStick idx
//   anything else                    -> None (constant zero) + one load-time
//                                       ARC_WARN("input: unknown control path '{}' in {}/{}")
//
// LOVE->SDL key-name translation (LOVE names appear in the asset; SDL
// lookups want SDL names). Table covers the divergent names; everything
// else passes through unchanged (letters, digits, f1..f24 resolve as-is):
//   lshift->"Left Shift"  rshift->"Right Shift"  lctrl->"Left Ctrl"
//   rctrl->"Right Ctrl"   lalt->"Left Alt"       ralt->"Right Alt"
//   lgui->"Left GUI"      rgui->"Right GUI"      return->"Return"
//   escape->"Escape"      space->"Space"         tab->"Tab"
//   backspace->"Backspace" up->"Up" down->"Down" left->"Left" right->"Right"
// (SDL_GetKeyFromName / SDL_GetScancodeFromName need no SDL_Init; verify
// exact SDL3 signatures in the vcpkg header and record.)
//
// Gamepad button tokens -> bit index (sampler uses the same order):
//   buttonSouth=0 buttonEast=1 buttonWest=2 buttonNorth=3
//   dpadUp=4 dpadDown=5 dpadLeft=6 dpadRight=7
//   leftShoulder=8 rightShoulder=9 start=10 back=11 guide=12
//   leftStickPress=13 rightStickPress=14
// Axis tokens -> index: leftStick/x=0 leftStick/y=1 rightStick/x=2
//   rightStick/y=3 leftTrigger=4 rightTrigger=5
// Stick tokens -> GamepadStick: leftStick=0 (axes 0,1), rightStick=1 (axes 2,3)
//
// struct CompiledBinding {
//     std::vector<ControlId> chord;     // size 1 = simple ('+'-split, oracle resolvePath)
//     std::string path;                 // original string (deferred features)
//     std::vector<ProcessorOp> processors;   // Task 2
//     bool isComposite = false;              // Task 3 fields join then
// };
// struct Action { name, type, controlType, std::vector<CompiledBinding>,
//                 interaction (Task 4), eval state: prevDown, curDown,
//                 strength, vec, started/performed/canceled, heldTime,
//                 lastPressFrame, bufConsumed, contribDevice };
// struct Map { name, blocking, std::unordered_map<std::string, Action> };
// Members: std::unordered_map<std::string, Map> m_maps;
//          std::vector<std::string> m_contextStack; uint64_t m_frame = 0;
//          InputDevice m_activeDevice = InputDevice::Kbm;
//
// ResolveControl(const ControlId&, const InputSnapshot&) -> float:
//   Scancode  -> snap.wantCaptureKeyboard ? 0 : snap.ScancodeDown(code)
//   Keycode   -> snap.wantCaptureKeyboard ? 0 : snap.KeycodeDown(code)
//   MouseButton -> snap.wantCaptureMouse ? 0 : (snap.mouseButtons >> code) & 1
//   GamepadButton/Axis/Stick -> Task 2/3. None -> 0.
//   (Capture suppression lives HERE and only here -- spec rule.)
// Chord value = 1 only if every part magnitude >= 0.5 (oracle resolvePath).
// Action eval this slice (oracle evalAction, button path): value = max
//   magnitude across bindings; prevDown=curDown; curDown = |v| >= 0.5;
//   Pressed = cur && !prev; Released = prev && !cur.
// Resolve(name): walk m_contextStack top-down; map has action -> it;
//   map.blocking -> stop (oracle resolve()).
// LoadJson: doc.contains("actionMaps") && is_array else warn+false; full
//   replace of m_maps + context stack reset; compile every binding path.
// LoadFile: read bytes (exe-relative resolve), parse non-throwing
//   (nlohmann::json::parse(bytes, nullptr, false)), is_discarded -> warn+false,
//   else LoadJson.
```

- [ ] **Step 6: Build + run `"[input]"` then full suite.** Expected: input cases green; full suite = 491 baseline + new assertions, all pass.
- [ ] **Step 7: Commit** `feat(arcane): InputActions skeleton - snapshot seam, button bindings, edges`

---

### Task 2: Gamepad controls, chords, processors, Strength

**Files:** Modify `Arcane/Arcane/src/Arcane/Input/InputActions.cpp`, `Arcane/Tests/src/InputActionsTest.cpp` (append).

- [ ] **Step 1: Append failing tests**

```cpp
namespace
{
    nlohmann::json PadAndChordDoc()
    {
        return nlohmann::json::parse(R"({
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
        })");
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
    CHECK(input->Strength("aim") == Catch::Approx(0.59375f).margin(1e-4));
    // invert then scale(2): 0.5 -> -0.5 -> -1.0
    CHECK(input->Strength("throttle") == Catch::Approx(-1.0f).margin(1e-4));
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
    CHECK(input->Strength("aim") == Catch::Approx(1.0f));
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
```

(Add `#include <catch2/catch_approx.hpp>` and `using Catch::Approx;` at the top of the test file.)

- [ ] **Step 2: Implement** (oracle: `resolveSingle` Gamepad branch, `resolvePath` chord loop, `parseProcessor`/`applyScalar`/`runProcessors`). Processor tokens parse at LOAD into `struct ProcessorOp { enum Kind { Invert, Scale, Deadzone, NormalizeVector2 } kind; float min, max, factor; }` with oracle defaults (deadzone 0.125/0.925, scale factor 1). Scalar application order = token order. `Strength(name)` returns the evaluated scalar (signed). Unknown processor names: ignored with one load-time warn (oracle silently no-ops; the warn is the agreed improvement).
- [ ] **Step 3: Build + `"[input]"` + full suite green.**
- [ ] **Step 4: Commit** `feat(arcane): input gamepad controls, chords, processors`

---

### Task 3: Composites (2DVector/1DAxis), Axis, vector processors, max-magnitude

**Files:** Modify `InputActions.cpp`, `InputActionsTest.cpp` (append).

- [ ] **Step 1: Append failing tests**

```cpp
namespace
{
    constexpr uint32_t kScancodeA = 4;    // SDL_SCANCODE_A
    constexpr uint32_t kScancodeS2 = 22;  // SDL_SCANCODE_S
    constexpr uint32_t kScancodeD = 7;    // SDL_SCANCODE_D

    nlohmann::json MoveDoc()
    {
        return nlohmann::json::parse(R"({
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
        })");
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
    CHECK(v.x == Catch::Approx(0.0f));
    CHECK(v.y == Catch::Approx(-1.0f));    // up = -y (screen y-down, oracle)
    CHECK(input->Down("move"));            // magnitude >= 0.5

    InputSnapshot diag;
    diag.SetScancode(kScancodeW);
    diag.SetScancode(kScancodeD);          // up+right
    input->Update(1.0 / 60.0, diag);
    v = input->Axis("move");
    CHECK(v.x == Catch::Approx(0.70710678f).margin(1e-4));   // normalized
    CHECK(v.y == Catch::Approx(-0.70710678f).margin(1e-4));
    CHECK(input->Strength("move") == Catch::Approx(1.0f).margin(1e-4));
}

TEST_CASE("input: 1DAxis composite", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(MoveDoc()));
    input->SetBaseContext("world");

    InputSnapshot snap;
    snap.SetScancode(kScancodeS2);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Strength("zoom") == Catch::Approx(-1.0f));  // negative - positive
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
    CHECK(v.x == Catch::Approx(1.0f).margin(1e-4));
    CHECK(v.y == Catch::Approx(0.0f).margin(1e-4));

    // Weak stick + full WASD: the larger (keyboard) vector wins.
    InputSnapshot both;
    both.gamepadConnected = true;
    both.gamepadAxes[0] = 0.3f;
    both.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, both);
    CHECK(input->Axis("move").x == Catch::Approx(1.0f).margin(1e-4));
}
```

- [ ] **Step 2: Implement** (oracle: `resolveComposite`, `partStrength`, `applyVector`, `evalAction`'s best-vector/best-scalar split). Composite parsing at load: `parts.up/down/left/right` (2DVector) or `parts.positive/negative` (1DAxis), each an ARRAY of bindings; part strength = max magnitude across the part's array. 2DVector value: `x = right - left`, `y = down - up`. Vector processors: `normalizeVector2`, radial `deadzone`, `invert` (negates both components). `GamepadStick` controls resolve to a vector from axes (idx*2, idx*2+1). Per-action: track best vector (by length) AND best scalar (by magnitude) across bindings; `controlType == "Vector2"` actions report the vector (strength = length, curDown = length >= 0.5), scalar actions report the scalar (oracle `evalAction`). `Axis(name)` returns the vector.
- [ ] **Step 3: Build + `"[input]"` + full suite green.**
- [ ] **Step 4: Commit** `feat(arcane): input composites - 2DVector/1DAxis, vector processors, max-magnitude`

---

### Task 4: Interaction phases (press/hold/tap) + Buffered

**Files:** Modify `InputActions.cpp`, `InputActionsTest.cpp` (append).

- [ ] **Step 1: Append failing tests**

```cpp
namespace
{
    nlohmann::json PhasesDoc()
    {
        return nlohmann::json::parse(R"({
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
        })");
    }
}

TEST_CASE("input: hold fires performed once at duration", "[input]")
{
    auto input = InputActions::Create();
    REQUIRE(input->LoadJson(PhasesDoc()));
    input->SetBaseContext("demo");

    InputSnapshot down; down.AddKeycode(kKeycodeSpace);

    input->Update(0.1, down);
    CHECK(input->Started("charge"));
    CHECK_FALSE(input->Performed("charge"));
    input->Update(0.1, down);                  // held 0.1 -> heldTime 0.2
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

    input->Update(0.1, down);                  // press again
    input->Update(0.15, down);
    input->Update(0.1, down);                  // heldTime 0.35 > 0.2: tap invalid
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
```

- [ ] **Step 2: Implement** (oracle: `evalAction`'s interaction block + `Input.buffered`). Interactions parse at load from `interactions[0]` with the same `name(arg=val)` grammar as processors; kinds `press` (default), `hold` (duration default 0.4), `tap` (duration default 0.2). State per action: `heldTime`, `_perfFired`, `_tapValid`, `lastPressFrame`, `bufConsumed`. Buffered: `(m_frame - lastPressFrame) <= frames && !bufConsumed` -> consume + true.
- [ ] **Step 3: Build + `"[input]"` + full suite green.**
- [ ] **Step 4: Commit** `feat(arcane): input interaction phases (press/hold/tap) + buffered presses`

---

### Task 5: Context stack, active-device hysteresis, capture suppression

**Files:** Modify `InputActions.cpp`, `InputActionsTest.cpp` (append).

- [ ] **Step 1: Append failing tests**

```cpp
namespace
{
    nlohmann::json ContextDoc()
    {
        return nlohmann::json::parse(R"({
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
        })");
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
```

- [ ] **Step 2: Implement** (oracle: `pushContext/popContext/setBaseContext/swapBaseContext/resolve`, the device block at the end of `Input.update`). Suppression was already placed in `ResolveControl` in Task 1 — this task adds the tests proving it plus the contributing-device tracking: during eval record which device sourced the winning value when its magnitude clears 0.5 (kbm for Scancode/Keycode/MouseButton, gamepad for the Gamepad* sources); after evaluating all maps, kbm contribution -> `Kbm`, else max pad magnitude > 0.5 -> `Gamepad`, else unchanged.
- [ ] **Step 3: Build + `"[input]"` + full suite green.**
- [ ] **Step 4: Commit** `feat(arcane): input context stack, device hysteresis, ImGui capture suppression`

---

### Task 6: InputDevices SDL sampler + ImGuiLayer capture accessors

**Files:**
- Create: `Arcane/Arcane/src/Arcane/Input/InputDevices.hpp`, `Input/InputDevices.cpp`, `Arcane/Tests/src/InputDevicesTest.cpp`
- Modify: `Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.hpp`, `ImGuiLayer.cpp`

- [ ] **Step 1: Write `InputDevices.hpp`**

```cpp
#pragma once

// SDL-facing sampler: fills an InputSnapshot from SDL keyboard/mouse/
// gamepad state. Owns the gamepad handle (lazy re-scan on disconnect,
// oracle: Input.lua's cachedPad). Capture flags are passed in by the host
// (typically ImGuiLayer::WantCaptureKeyboard/Mouse) -- this module never
// touches ImGui. Requires the SDL video subsystem (a created Window).

#include <Arcane/Base/Api.hpp>
#include <Arcane/Input/InputSnapshot.hpp>

#include <memory>

namespace Arcane
{
    class ARCANE_API InputDevices
    {
    public:
        static std::unique_ptr<InputDevices> Create();
        virtual ~InputDevices() = default;

        virtual InputSnapshot Sample(bool captureKeyboard,
                                     bool captureMouse) = 0;
    };
}
```

- [ ] **Step 2: Write the failing smoke test `InputDevicesTest.cpp`**

```cpp
// Sampler smoke: with a hidden window (SDL video up) and no user input,
// Sample returns a sane snapshot. Headless-safe: asserts shape, not
// hardware state beyond "nothing is pressed in an unfocused hidden window".

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Input/InputDevices.hpp>
#include <Arcane/Platform/Window.hpp>

TEST_CASE("input devices: sample from a hidden window", "[input][platform]")
{
    Arcane::Window window;
    Arcane::WindowDesc desc;
    desc.hidden = true;
    desc.width = 64; desc.height = 64;
    REQUIRE(window.Create(desc));

    auto devices = Arcane::InputDevices::Create();
    REQUIRE(devices != nullptr);
    (void)window.PumpEvents();

    const auto snap = devices->Sample(false, false);
    CHECK(snap.keycodeCount <= Arcane::InputSnapshot::kMaxKeycodesDown);
    CHECK_FALSE(snap.wantCaptureKeyboard);

    const auto captured = devices->Sample(true, true);
    CHECK(captured.wantCaptureKeyboard);
    CHECK(captured.wantCaptureMouse);
}
```

- [ ] **Step 3: Implement `InputDevices.cpp`** (anonymous-namespace impl + factory). SDL3 calls (VERIFY each against the vcpkg SDL3 headers; record deviations):

```cpp
// Sample():
//   snap.wantCaptureKeyboard / wantCaptureMouse = the passed flags (stored
//     verbatim; suppression happens in the evaluator).
//   Keyboard: int n = 0; const bool* state = SDL_GetKeyboardState(&n);
//     for sc in [0, min(n,512)): if state[sc] { snap.SetScancode(sc);
//       SDL_Keycode kc = SDL_GetKeyFromScancode((SDL_Scancode)sc,
//                                               SDL_KMOD_NONE, false);
//       if (kc != SDLK_UNKNOWN) snap.AddKeycode((uint32_t)kc); }
//   Mouse: SDL_MouseButtonFlags f = SDL_GetMouseState(nullptr, nullptr);
//     snap.mouseButtons = bits for SDL_BUTTON_LMASK/RMASK/MMASK/X1MASK/X2MASK
//     mapped to bit 0..4 (LMB=0, RMB=1, MMB=2 -- matches the compiler).
//   Gamepad: ensure SDL_InitSubSystem(SDL_INIT_GAMEPAD) once in Create
//     (paired SDL_QuitSubSystem in the destructor). Cached SDL_Gamepad*:
//     if cached && !SDL_GamepadConnected(cached) { SDL_CloseGamepad; null }
//     if !cached { int c = 0; SDL_JoystickID* ids = SDL_GetGamepads(&c);
//       if (ids) { if (c > 0) cached = SDL_OpenGamepad(ids[0]); SDL_free(ids); } }
//     if cached: snap.gamepadConnected = true;
//       buttons: SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH/EAST/
//         WEST/NORTH/DPAD_UP/DPAD_DOWN/DPAD_LEFT/DPAD_RIGHT/LEFT_SHOULDER/
//         RIGHT_SHOULDER/START/BACK/GUIDE/LEFT_STICK/RIGHT_STICK) -> bits
//         0..14 IN THAT ORDER (must match the compiler's token table).
//       axes: SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX/.../RIGHTY)
//         / 32767.0f clamped to [-1,1] -> indices 0..3;
//         LEFT_TRIGGER/RIGHT_TRIGGER / 32767.0f clamped [0,1] -> 4,5.
```

- [ ] **Step 4: ImGuiLayer accessors.** `ImGuiLayer.hpp` — add after `Render(...)`:

```cpp
        // ImGui capture state for the input layer: pass these into
        // InputDevices::Sample so kbm actions release while ImGui owns the
        // keyboard/mouse (text fields, drags). Valid between frames.
        virtual bool WantCaptureKeyboard() const = 0;
        virtual bool WantCaptureMouse() const = 0;
```

`ImGuiLayer.cpp` — in `ImGuiLayerImpl`:

```cpp
            bool WantCaptureKeyboard() const override
            {
                return ImGui::GetIO().WantCaptureKeyboard;
            }
            bool WantCaptureMouse() const override
            {
                return ImGui::GetIO().WantCaptureMouse;
            }
```

- [ ] **Step 5: GenerateProjects + build + run `"[input]"` + full suite green** (the sampler test creates a hidden window only — no GPU device; confirm `~[gpu]` still passes with no "device created" lines).
- [ ] **Step 6: Commit** `feat(arcane): InputDevices SDL sampler + ImGuiLayer capture accessors`

---

### Task 7: Playground integration + Window ESC removal + docs

**Files:**
- Create: `Arcane/Playground/data/input_actions.json`
- Modify: `Arcane/Playground/src/main.cpp`, `Arcane/Arcane/src/Arcane/Platform/Window.{hpp,cpp}`, `Arcane/premake5.lua`, `docs/superpowers/specs/2026-06-11-engine-architecture-design.md`

- [ ] **Step 1: Write `Arcane/Playground/data/input_actions.json`** (the demo asset; single source under Playground):

```json
{
  "actionMaps": [
    {
      "name": "demo",
      "actions": [
        {
          "name": "quit",
          "type": "Button",
          "bindings": [ { "path": "<Keyboard>/escape" } ]
        },
        {
          "name": "toggle_stats",
          "type": "Button",
          "bindings": [
            { "path": "<Keyboard>/tab" },
            { "path": "<Gamepad>/back" }
          ]
        },
        {
          "name": "swap_backend",
          "type": "Button",
          "bindings": [ { "path": "<Keyboard>/lctrl+<Keyboard>/b" } ]
        },
        {
          "name": "move",
          "type": "Value",
          "controlType": "Vector2",
          "bindings": [
            {
              "composite": "2DVector",
              "parts": {
                "up":    [ { "path": "<Keyboard>/scancode/w" } ],
                "down":  [ { "path": "<Keyboard>/scancode/s" } ],
                "left":  [ { "path": "<Keyboard>/scancode/a" } ],
                "right": [ { "path": "<Keyboard>/scancode/d" } ]
              },
              "processors": [ "normalizeVector2" ]
            },
            {
              "path": "<Gamepad>/leftStick",
              "processors": [ "deadzone(min=0.125,max=0.925)" ]
            }
          ]
        }
      ]
    }
  ]
}
```

- [ ] **Step 2: premake postbuild copies.** In `Arcane/premake5.lua`, add to BOTH the Playground and ArcaneTests `postbuildcommands` (next to the existing font copy lines):

```lua
        '{COPYFILE} "%{wks.location}/Playground/data/input_actions.json" "%{cfg.buildtarget.directory}/data/input_actions.json"',
```

(`data/` already exists via the font `{MKDIR}`. The ArcaneTests copy feeds the round-trip test below.)

- [ ] **Step 3: Round-trip test (append to `InputActionsTest.cpp`)**

```cpp
TEST_CASE("input: round-trip load of the Playground demo asset", "[input]")
{
    auto input = Arcane::InputActions::Create();
    REQUIRE(input->LoadFile("data/input_actions.json"));
    input->SetBaseContext("demo");

    InputSnapshot snap;
    snap.SetScancode(kScancodeW);
    snap.SetScancode(kScancodeD);
    input->Update(1.0 / 60.0, snap);
    CHECK(input->Strength("move") == Catch::Approx(1.0f).margin(1e-4));
    CHECK_FALSE(input->Down("quit"));
}
```

- [ ] **Step 4: Remove Window's hardcoded ESC.** `Window.cpp` `PumpEvents`: delete the whole `case SDL_EVENT_KEY_DOWN:` block (lines 79-82). `Window.hpp`: update the `WindowEvents.quitRequested` comment to `// SDL_EVENT_QUIT or window close (ESC is the host's business via InputActions)` and the file-top module comment (`// Platform module: SDL3 window + event pump. Quit/resize surface only; input lives in Arcane/Input (snapshot-driven action system).`). Grep `Arcane/Tests` for any test relying on ESC-quit (none expected; WindowTest only pumps) — report if found.

- [ ] **Step 5: Wire Playground (`main.cpp`).** Includes: `<Arcane/Input/InputActions.hpp>`, `<Arcane/Input/InputDevices.hpp>`. Setup after the imgui layer creation:

```cpp
    auto inputDevices = Arcane::InputDevices::Create();
    auto input = Arcane::InputActions::Create();
    if (!inputDevices || !input || !input->LoadFile("data/input_actions.json"))
    {
        std::fprintf(stderr, "error: input system init failed\n");
        return 1;
    }
    input->SetBaseContext("demo");
    bool showStats = true;
    glm::vec2 moveOffset(0.0f);
    auto lastFrameTime = std::chrono::steady_clock::now();
```

Frame loop, directly after `auto events = window.PumpEvents();` / quit/resize handling:

```cpp
        const auto nowInput = std::chrono::steady_clock::now();
        const double frameDt =
            std::chrono::duration<double>(nowInput - lastFrameTime).count();
        lastFrameTime = nowInput;

        input->Update(frameDt,
                      inputDevices->Sample(/* the layer owns capture state */
                                           imgui->WantCaptureKeyboard(),
                                           imgui->WantCaptureMouse()));
        if (input->Pressed("quit"))
            break;
        if (input->Pressed("toggle_stats"))
            showStats = !showStats;
        if (input->Pressed("swap_backend"))
            ARC_INFO("swap_backend pressed (runtime backend swap lands with the M3 demo proper)");
        // WASD / left stick nudges the orbit center: composites +
        // processors visibly live in the demo.
        const auto mv = input->Axis("move");
        moveOffset += glm::vec2(mv.x, mv.y) * (float)(300.0 * frameDt);
```

Gate the stats window on `showStats` (wrap the existing `ImGui::Begin("Arcane Stats") ... End()` block in `if (showStats) { ... }`) and change the orbit center line to `const glm::vec2 middle(w * 0.5f + moveOffset.x, h * 0.5f + moveOffset.y);`. NOTE: `imgui->BeginFrame()` must still run every frame regardless of `showStats` (only the stats window is conditional), and `imgui->Render(...)` stays unconditional. Adapt anchors to the real file — read it first; the loop was last touched by M2b Task 8.

- [ ] **Step 6: Amend the engine architecture spec** (`docs/superpowers/specs/2026-06-11-engine-architecture-design.md`):
  - Module list (~line 68): change `**Platform**: SDL3 window/events/ input (input_actions.json semantics), app lifecycle.` to `**Platform**: SDL3 window/events, app lifecycle. **Input**: snapshot-driven action system (input_actions.json semantics; design: 2026-06-12-arcane-input-actions-design.md).`
  - M3 line (~line 185): append `+ input action system (snapshot-driven, ImGui-capture-aware; landed as the M3 tail)` to the milestone description.

- [ ] **Step 7: Full verification.**
  - `GenerateProjects.bat` + Debug build; full suite from the ArcaneTests exe dir — all green (record the new count).
  - Release build + Release suite green.
  - Scripted: `.\Playground.exe --backend dx12 --frames 240 --no-vsync` and `--backend vulkan ...` from the Playground exe dir — both exit 0, zero validation.
  - Manual acceptance (HUMAN does this; note in the report, do not attempt): ESC quits; Tab toggles the stats window; WASD/stick moves the orbit center smoothly (normalized diagonals); with an ImGui text field focused (demo window -> any InputText), ESC and WASD do nothing, click away and they work; ctrl+b logs the swap message.
- [ ] **Step 8: Commit** `feat(playground): input actions demo - quit/toggle/move via InputActions, Window ESC removed`

---

## Exit criteria

- InputActions: full core-subset semantics green via fabricated snapshots ([input] CPU suite — no GPU, no hardware).
- Round-trip: the editor-shaped demo asset loads and evaluates through `LoadFile`.
- InputDevices samples real SDL state behind the snapshot seam; ImGuiLayer exposes capture flags; suppression proven by test.
- Window no longer hardcodes ESC; Playground quits/toggles/moves through actions only.
- Full suite green Debug + Release; both Playground backends exit 0 scripted with validation silent.
- Engine architecture spec reflects the Input module split + M3 tail.

Out of scope (deferred per spec): rebinding + keybinds.json, glyphs, event-order lookups, recording/replay, multiple gamepads, mouse position/wheel controls.
