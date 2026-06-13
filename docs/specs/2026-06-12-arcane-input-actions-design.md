# Arcane Input Actions — Design

**Date:** 2026-06-12
**Status:** Approved (brainstorm 2026-06-12)
**Scope:** Tail end of M3. Amends the engine architecture spec
(`2026-06-11-engine-architecture-design.md`): input becomes its own DLL module
folder (`Arcane/Input/`) rather than living inside Platform; Platform keeps
window/events/app lifecycle.

## Goal

A Unity-style input action system inside Arcane.dll: action maps -> actions
(Button | Value) -> bindings (simple paths, `+` chords, 2DVector/1DAxis
composites), driven by the same editor-authored `input_actions.json` the
client uses. **Feature parity with the client oracle's core subset; improved
architecture** — this is explicitly NOT a line-by-line port.

- **Oracle:** `Client/src/services/Input.lua` (805 lines). Same observable
  semantics for everything in scope; freely restructured internally.
- **Data contract:** the `input_actions.json` schema authored by the Tools
  InputEditor (`actionMaps[] -> actions[] -> bindings[]`, Unity
  `.inputactions` shape). Unchanged.

### In scope (M3 tail)

Maps/actions/bindings (simple + chord + composite), processors (deadzone,
invert, scale, normalizeVector2), interaction phases (press/hold/tap ->
started/performed/canceled), context stack with blocking maps, polling
queries (Down/Pressed/Released/Started/Performed/Canceled/Strength/Axis/
Buffered), active-device tracking with hysteresis, gamepad support, and
built-in ImGui capture suppression (this closes the M2b-documented
"ESC-quits-while-ImGui-focused" refinement).

### Deferred (designed-for, not built)

Rebinding + `keybinds.json` persistence, glyph labels, event-order
`actionForKey`-style lookups, input recording/replay. The compiled asset
model keeps mutable binding lists, original path strings, and group tags so
all four bolt on without re-architecting.

## Architecture decision

**Snapshot-driven evaluation core** (the AAA hybrid: OS events feed device
state; gameplay consumes a per-frame snapshot). SDL already does the
event->state half via `Window::PumpEvents`; we add the snapshot seam:

```
Window::PumpEvents()                          (SDL accumulates device state)
snap = devices->Sample(imgui->WantCaptureKeyboard(), imgui->WantCaptureMouse())
actions->Update(dt, snap)
consumers: actions->Pressed("quit"), actions->Axis("move"), ...
```

Rejected: direct-poll faithful port (welds the evaluator to SDL — untestable
without hardware, suppression smears across poll sites; the oracle's one
structural weakness); event-driven core (re-implements the event->state
layer SDL already provides, and frame-snapshot semantics are what the oracle
defines anyway).

## Components (all in `Arcane/Arcane/src/Arcane/Input/`)

### InputSnapshot (public header, plain data)

Fixed-size POD: keyboard scancode bitset (512 bits) + small fixed array of
down keycodes (so layout-dependent `<Keyboard>/w` and physical
`<Keyboard>/scancode/w` both resolve purely), mouse button mask, gamepad
connected flag + button mask + 6 axes (sticks + triggers), and
`wantCaptureKeyboard` / `wantCaptureMouse` flags.

**Rule: the snapshot stays fixed-size POD** (memcpy-serializable). That is
what makes input recording/replay a later feature instead of a redesign, and
it feeds the determinism story (same snapshot stream -> same sim).

### InputDevices (exported; SDL-facing sampler)

Pure-virtual + `Create` factory (established module pattern). Owns the
gamepad handle and hotplug (lazy re-scan on disconnect, like the oracle's
`cachedPad`). `Sample(bool captureKeyboard, bool captureMouse)` fills an
InputSnapshot from SDL keyboard/mouse/gamepad state. Capture flags are
passed in by the host — InputDevices never touches ImGui (no Input -> ImGui
module dependency). `ImGuiLayer` gains two const accessors:
`WantCaptureKeyboard()` / `WantCaptureMouse()`.

### InputActions (exported facade)

Pure-virtual + `Create` factory.

- `LoadFile(path)` — exe-relative resolution (ShaderLibrary pattern);
  `LoadJson(const nlohmann::json&)` — the test seam. Re-load fully replaces
  state and resets the context stack (oracle `Input.load`).
- `Update(double dt, const InputSnapshot&)` — evaluates every map's actions.
- Queries: `Down/Pressed/Released/Started/Performed/Canceled(name)`,
  `Strength(name)`, `Axis(name) -> glm::vec2`, `Buffered(name, frames = 6)`.
- Context stack: `PushContext/PopContext/SetBaseContext/SwapBaseContext/
  ActiveContext`.
- `ActiveDevice() -> enum { Kbm, Gamepad }`.

### Internal evaluator (anonymous namespace)

**Compile at load, evaluate compiled data** — the deliberate improvement
over the oracle, which re-parses binding path strings with pattern matches
every frame per binding. At load: paths -> `ControlId` structs, processor
tokens -> parsed ops, interactions -> `{kind, args}`. Original path strings
and group tags are retained alongside the compiled form (the deferred
features address that model).

## Semantics (oracle-fidelity contract)

- Max-magnitude wins across an action's bindings; button threshold 0.5.
- Chords: `+`-joined paths, down only when every part is down.
- Composites: `2DVector` (`x = right - left`, `y = down - up`, screen-y-down
  like the client) and `1DAxis` (`positive - negative`); part strength =
  max across the part's binding array.
- Processors: `deadzone` (defaults min 0.125 / max 0.925; scalar form and
  radial vector form), `invert`, `scale(factor)`, `normalizeVector2`.
- Interactions: `press` (default; performed = rising edge, canceled =
  falling), `hold` (default 0.4 s; performed once at duration, canceled on
  early release), `tap` (default 0.2 s; performed on release within
  duration, else canceled).
- Context stack resolved top-down; a `blocking` map stops fall-through.
- `Buffered(action, n)`: pressed within the last n frames, consume-on-read.
- Active device: kbm activity wins immediately; gamepad requires
  magnitude > 0.5 (hysteresis against prompt flicker).
- ImGui suppression at evaluation input: `wantCaptureKeyboard` zeroes
  keyboard-sourced controls, `wantCaptureMouse` zeroes mouse-sourced ones,
  gamepad unaffected. Actions release cleanly — falling edges fire (no
  stuck-pressed actions when a text field grabs focus mid-hold).

## Error handling

- `LoadFile`/`LoadJson` return false on missing/malformed/schema-violating
  input, one `ARC_WARN`; the host decides severity (Playground exits).
- Unknown device/control tokens compile to constant-zero bindings with one
  load-time warn naming map/action/path (oracle silently zeroed at runtime,
  every frame).
- `PushContext` on unknown map: warn + no-op (oracle). Queries on
  unresolvable actions: false / 0 / zero-vector (oracle).

## Testing

All CPU, no `[gpu]` tag, fabricated snapshots. Characterization cases
written against `Input.lua` as the read-along reference: WASD composite
vector incl. normalize + diagonals; deadzone scalar + radial at boundary
values; chord all-parts requirement; hold/tap phase timing incl. early
release; blocking-map shadowing; buffered consume-once; device hysteresis;
capture suppression (kbm suppressed, pad not, clean release edges); loader
rejection of malformed JSON; unknown paths compile to zero with one warn;
round-trip load of the actual Playground `input_actions.json`.

## Playground demo wiring (the M3-tail integration proof)

Playground ships its own minimal `data/input_actions.json` (postbuild copy,
single-source under `Arcane/Playground/`): one `demo` map with `quit`
(Escape), `toggle_stats` (Tab), `move` (WASD + left-stick 2DVector with
deadzone + normalize, drives the orbit-circle offset so composites and
processors are visibly live), and `swap_backend`
(`<Keyboard>/lctrl+<Keyboard>/b` chord) — bound now, consumed by the M3
demo proper when runtime backend swap lands. **`Window`'s hardcoded ESC->quit handling is removed**; Playground
quits via `Pressed("quit")`. Manual acceptance: with an ImGui text field
focused, ESC and WASD do nothing; click away and they work again.

The client's `Client/data/input_actions.json` is game data and stays
untouched; the engine consumes whatever file the host hands it (no
canonical-path baking).
