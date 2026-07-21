# Grimoire Editor Icon Font + Roboto (toolbar icons) — Design

**Date:** 2026-07-21
**Status:** Design — approved (editor-wide icon font; Roboto base + lucide merged; 16 px; convert all toolbar buttons; tinted-background active). Awaiting spec review.
**Layer:** Grimoire editor only. **Zero engine change** — the engine `ImGuiLayer` stays editor-free; Grimoire configures the editor ImGui font atlas itself.

---

## 1. Why

The Grimoire sim-time toolbar (`DrawSimTimeToolbar`) uses plain text buttons ("Play", "Stop", "Pause", "Step", "Undo", "Redo", "T"/"R"/"S", "Local"). We want icon buttons. The idiomatic ImGui-editor way — and the reusable, editor-wide foundation — is an **icon font merged into the ImGui atlas**, so an icon is just a text codepoint (`ImGui::Button(ICON_LC_PLAY)`), crisp at any size, recolorable as text, usable anywhere in the editor later. In the same pass we set a real base font (**Roboto**) in place of ImGui's built-in ProggyClean.

We already have every asset under `Arcane/data/font/`: `Roboto-Regular.ttf`, `lucide/lucide.ttf`, and `lucide/codepoints.json` (the icon name → codepoint map).

Rejected alternatives (from brainstorming): per-icon SVG-rasterized textures (`nanosvg` + `ImGui::ImageButton`) — localized but not reusable/scalable and doesn't upgrade the base font; runtime SVG rasterizer — a dependency for no gain over the font.

---

## 2. Grounding (verified against the tree, 2026-07-21)

- **Toolbar:** `Grimoire::DrawSimTimeToolbar` (`Arcane/Grimoire/src/EditorPanels.cpp:40-108`, declared `EditorPanels.hpp:34-37`), called from `GrimoireApp.cpp:589-591` inside `MainLoop`. It is a docked `ImGui::Begin("Sim")` window of plain `ImGui::Button`/`RadioButton`/`Checkbox`/`SliderFloat`, laid out horizontally with `ImGui::SameLine()`. Controls + actions:
  - **Play** `Button("Play")` (line 63, shown when `!play.IsPlaying()`) → `play.Play(runtime, plugin)`.
  - **Stop** `Button("Stop")` (line 48, shown when `play.IsPlaying()`) → `play.Stop(runtime, plugin)`.
  - **Pause/Resume** `Button(loop.IsPaused() ? "Resume" : "Pause")` (line 70) → `loop.SetPaused(!loop.IsPaused())`.
  - **Step** `Button("Step")` (line 72) → `loop.RequestSingleStep()`.
  - **time-scale** `SliderFloat("time-scale", ...)` (line 74-76) → `loop.SetTimeScale(scale)`.
  - **Undo** `Button("Undo")` (line 80, `BeginDisabled(!undo.CanUndo())`, tooltip `undo.UndoLabel()` line 82) → `undo.Undo()`.
  - **Redo** `Button("Redo")` (line 85, disabled `!undo.CanRedo()`, tooltip `undo.RedoLabel()`) → `undo.Redo()`.
  - **T / R / S** `RadioButton` (lines 92/95/98) → `mode = GizmoMode::Translate/Rotate/Scale`.
  - **Local** `Checkbox` (line 103) → toggles `space` between `GizmoSpace::Local`/`World`.
  - **Ordering gotcha (must preserve):** `Arcane::RunLoop& loop = runtime.Loop();` is fetched AFTER the Play/Stop handling (`EditorPanels.cpp:65-67`) because `play.Stop` → `Runtime::RestoreRegistry` replaces the loop object; do not hoist the `loop` fetch above the Play/Stop block.
- **State readers:** `play.IsPlaying()` (`PlayMode.hpp:30`), `loop.IsPaused()`, `loop.TimeScale()`, `undo.CanUndo()/CanRedo()`, and the by-ref `mode`/`space` enums (no getters — read the params directly each frame).
- **ImGui font setup:** there is NONE today. `ImGuiLayerImpl::Init` (`Arcane/Arcane/src/Arcane/ImGui/ImGuiLayer.cpp:38-67`) never touches `io.Fonts` — ImGui uses its implicit default (ProggyClean); the atlas is built lazily via the 1.92 `ImGuiBackendFlags_RendererHasTextures` protocol (`ImGuiNvrhi.cpp` `UpdateTexture`) on first `NewFrame`. So adding fonts to `io.Fonts` **before the first frame** is all that's needed; no manual `Build()`/upload.
- **Editor context access:** Grimoire already links/uses ImGui (`DrawSimTimeToolbar` calls `ImGui::*`), and already reads the editor context via `ImGui::GetCurrentContext()` (the game-imgui redirect used exactly that). After the editor `ImGuiLayer` is created in `GrimoireApp::Init` (its `Init` leaves the editor context current), `ImGui::GetCurrentContext()` is the editor context — the point at which Grimoire installs fonts.
- **Fonts on disk:** `Arcane/data/font/Roboto-Regular.ttf` (146 KB); `Arcane/data/font/lucide/lucide.ttf` (824 KB) + `lucide/codepoints.json` (name → **decimal** codepoint; 1,979 icons; contiguous PUA block **U+E038–U+E6FB**). Sample: `play`=U+E13C, `pause`=U+E12E, `square`=U+E167, `step-forward`=U+E3EA, `undo`=U+E19B, `redo`=U+E143, `move`=U+E121, `rotate-3d`=U+E2EA, `scale-3d`=U+E2EB, `box`=U+E061, `globe`=U+E0E8.
- **Runtime data resolution:** relative asset paths resolve **exe-relative** (`Assets::ResolveAssetPath` = `GetModuleFileNameW` parent, `Assets.cpp:26-36`), not CWD. Grimoire's data is copied to its output by `postbuildcommands` in `Arcane/premake5.lua:451-457` (DLLs `{COPYFILE}`, shaders `{COPYDIR}`, one JSON `{MKDIR}`+`{COPYFILE}`). There is **no** copy step for `Arcane/data/font` today.

---

## 3. Design

Two halves in one pass: a reusable font foundation, then the toolbar as its first consumer. All Grimoire-side.

### 3a. Font foundation
- **`IconsLucide.h`** (`Arcane/Grimoire/src/IconsLucide.h`, generated + committed): from `codepoints.json`, one macro per icon as a RAW UTF-8 byte-escape NARROW string literal -- e.g. `#define ICON_LC_PLAY "\xEE\x84\xBC"` (U+E13C). NOT `u8"..."` (that is `const char8_t*` in C++20 and will not convert to the `const char*` ImGui wants), and NOT a bare `"\uXXXX"` (narrow-literal encoding is implementation-defined). 1,979 entries, plus range constants `ICON_LC_MIN (0xE038)` / `ICON_LC_MAX (0xE6FB)`. Generated once by a small script from `codepoints.json` (kebab-name -> ICON_LC_UPPER_SNAKE; each decimal codepoint -> its 3-byte UTF-8 escape); regeneration is offline, the header is checked in. (Matches how IconFontCppHeaders ships static headers.)
- **Font install** (Grimoire, in `Init`, on the editor context, before the first frame): a small helper `Grimoire::InstallEditorFonts()`:
  1. `io.Fonts->AddFontFromFileTTF("<exe>/data/font/Roboto-Regular.ttf", 16.0f)` — the base editor font (becomes the default since it is the first font added; ProggyClean is not added).
  2. `ImFontConfig cfg; cfg.MergeMode = true; cfg.GlyphMinAdvanceX = 16.0f;` (monospace icon advance) `+ GlyphOffset` for vertical centering; `static const ImWchar range[] = { ICON_LC_MIN, ICON_LC_MAX, 0 };` `io.Fonts->AddFontFromFileTTF("<exe>/data/font/lucide/lucide.ttf", 16.0f, &cfg, range)` — merges the icon glyphs into the Roboto atlas.
  - Font file paths are resolved **exe-relative** (a tiny `GetModuleFileNameW`-parent helper, mirroring `Assets::ResolveAssetPath`) so the VS-debugger CWD doesn't matter.
  - Called after the editor `ImGuiLayer` exists and while the editor context is current (`ImGui::GetCurrentContext()`), before `MainLoop`'s first `BeginFrame`. **No engine change.**
- **Layering:** the font files + the choice are Grimoire's; the engine `ImGuiLayer` is untouched (stays editor-free). Only the editor context gets these fonts; the game (`OffscreenImGuiLayer`) context is unaffected (its debug UI is the plugin's concern).

### 3b. Toolbar conversion (`DrawSimTimeToolbar`, all buttons)
Every text button → an icon button (`ImGui::Button(ICON_LC_*)`), **each with a tooltip** carrying the old label (`if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play")`), since icons need discoverable hover labels:

| Control | Icon (state) | Tooltip | Behavior kept |
|---|---|---|---|
| Play / Stop | `ICON_LC_PLAY` / `ICON_LC_SQUARE` | "Play" / "Stop" | swap on `IsPlaying()` |
| Pause / Resume | `ICON_LC_PAUSE` / `ICON_LC_PLAY` | "Pause" / "Resume" | swap on `IsPaused()` |
| Step | `ICON_LC_STEP_FORWARD` | "Step" | momentary |
| Undo / Redo | `ICON_LC_UNDO` / `ICON_LC_REDO` | `UndoLabel()` / `RedoLabel()` | keep `CanUndo/Redo` `BeginDisabled` |
| Gizmo T / R / S | `ICON_LC_MOVE` / `ICON_LC_ROTATE_3D` / `ICON_LC_SCALE_3D` | "Translate"/"Rotate"/"Scale" | radio semantics; **active = tinted background** |
| Local / World | `ICON_LC_BOX` (Local) / `ICON_LC_GLOBE` (World) | "Local space" / "World space" | toggles `space` |
| time-scale | *stays a `SliderFloat`* (optionally a small `ICON_LC_GAUGE` label) | — | unchanged |

- **Gizmo T/R/S active state:** the three become icon buttons; the one matching `mode` is drawn with a tinted background — push `ImGuiCol_Button` = the accent/active color when active, plain otherwise, and set `mode` on click. (Replaces `RadioButton`, keeps mutual-exclusion + a clear active look.)
- The time-scale `SliderFloat` stays a slider (not a button).

---

## 4. Build / asset plumbing

- **Copy the fonts to Grimoire's output** — add to the Grimoire `postbuildcommands` (`Arcane/premake5.lua:451-457`), following the shader `{COPYDIR}` pattern:
  - `{COPYFILE}` `data/font/Roboto-Regular.ttf` → `<out>/data/font/Roboto-Regular.ttf`
  - `{COPYFILE}` `data/font/lucide/lucide.ttf` → `<out>/data/font/lucide/lucide.ttf`
  - (only the two `.ttf` are needed at runtime; `codepoints.json` is a build-time input to `IconsLucide.h`, not shipped.)
- New files (`IconsLucide.h`, the font helper) → `GenerateProjects.bat` (premake globs) before building.

---

## 5. Testing

- **Headless (`[grimoire]` / CPU):** guard `IconsLucide.h` against a bad regen — assert a handful of known mappings decode correctly (e.g. `ICON_LC_PLAY` is the 3-byte UTF-8 for U+E13C; `ICON_LC_PAUSE` → U+E12E) and that `ICON_LC_MIN`/`ICON_LC_MAX` bound the block (`0xE038`/`0xE6FB`). Pure string/codepoint checks, no ImGui/GPU.
- **Desk (interactive, GPU hazard headless):** the toolbar shows crisp lucide icons; hover tooltips read correctly; Play↔Stop and Pause↔Resume swap icons with sim state; Step/Undo/Redo fire (Undo/Redo still disable when empty); the active gizmo mode (T/R/S) shows the tinted background and switches on click; Local↔World swaps box/globe; all other editor text is now Roboto (not ProggyClean); no ImGui atlas / NVRHI validation errors; two-context coexistence with the game ImGui unaffected.

---

## 6. Non-goals (v1)

- **Converting other editor panels** (hierarchy rows, inspector, console severity, menu items) — the foundation makes this trivial later; v1 converts only the sim-time toolbar.
- **DPI / font-size UI or runtime font switching** — 16 px fixed.
- **In-scene / world-space icons** — that is the separate MSDF `TextSystem`; this is ImGui-atlas only.
- **The game (`OffscreenImGuiLayer`) context fonts** — editor context only; the plugin owns its own debug-UI fonts.
- **Multiple font weights / italic** — one Roboto regular + lucide.

---

## 7. Impl-time verification points (shape unaffected; resolve while planning)

1. **Post-Init font install works with `RendererHasTextures`** — confirm that adding fonts to `io.Fonts` in `GrimoireApp::Init` (after the `ImGuiLayer` exists, editor context current, before the first `BeginFrame`) yields an atlas rebuilt with Roboto+lucide on the first frame, with no manual `Build()`/texture upload needed (the lazy-atlas protocol should handle it). Confirm the exact Init insertion point + that `ImGui::GetCurrentContext()` is the editor context there.
2. **`AddFontFromFileTTF` path + lifetime** — ImGui reads the `.ttf` file itself (no caller-owned buffer), so only a correct exe-relative path is needed; confirm the path helper and that the two `.ttf` land in `<exe>/data/font/...` via the new postbuild copy.
3. **Icon vertical alignment** — the `GlyphMinAdvanceX`/`GlyphOffset` values that center 16 px lucide glyphs on the Roboto baseline (tune at the desk).
4. **`IconsLucide.h` generation** — the exact kebab→`ICON_LC_` transform (e.g. `step-forward` → `ICON_LC_STEP_FORWARD`; digits like `rotate-3d` → `ICON_LC_ROTATE_3D`) and UTF-8 encoding of each PUA codepoint; the generator reads `codepoints.json` (decimal values).
5. **Gizmo active-tint colors** — the `ImGuiCol_Button` push value for the active T/R/S button (reuse an existing accent/theme color if one exists).
6. **Button sizing** — icon buttons want a squarer footprint than text buttons; confirm whether to pass an explicit `ImVec2` size or rely on frame padding so the toolbar stays tidy.
