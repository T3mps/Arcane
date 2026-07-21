# Grimoire Editor Icon Font + Roboto — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Grimoire sim-time toolbar's text buttons with lucide icon buttons, on a reusable editor-wide icon-font foundation (Roboto base + lucide merged into the ImGui atlas).

**Architecture:** All Grimoire-side, zero engine change. A generated `IconsLucide.h` maps icon names to UTF-8 codepoint macros. Grimoire installs Roboto (16 px) as the base font and merges `lucide.ttf` into the editor ImGui atlas, on the editor context before the first frame. The toolbar then uses `ImGui::Button(ICON_LC_*)` with tooltips.

**Tech Stack:** C++23, Dear ImGui (1.92, `RendererHasTextures`), Catch2, premake5, Python (offline header generation). Spec: `docs/superpowers/specs/2026-07-21-grimoire-editor-icon-font-design.md`.

## Global Constraints

- **Zero engine change:** the engine `ImGuiLayer` is NOT modified. Grimoire configures `io.Fonts` on the editor ImGui context itself. Do not add font code to Arcane.
- **`IconsLucide.h` macros are RAW UTF-8 byte-escape NARROW string literals** — e.g. `#define ICON_LC_PLAY "\xEE\x84\xBC"`. NOT `u8"..."` (that is `const char8_t*` in C++20, won't convert to the `const char*` ImGui wants) and NOT `"\uXXXX"` (implementation-defined narrow encoding).
- **Font size 16 px.** Base = `Roboto-Regular.ttf`; merged icons = `lucide.ttf`, glyph range `[ICON_LC_MIN, ICON_LC_MAX]` = `[0xE038, 0xE6FB]`.
- **Preserve the toolbar ordering gotcha:** in `DrawSimTimeToolbar`, `runtime.Loop()` is fetched AFTER the Play/Stop handling (`play.Stop` replaces the RunLoop via `RestoreRegistry`). Do not hoist the `loop` fetch above the Play/Stop block.
- **/MD everywhere; UTF-8 without BOM; ASCII-only comments** (the generated header's string values are UTF-8 byte escapes, which are ASCII source).
- **Build (PowerShell, VS18 MSBuild — NOT the Bash tool):**
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "D:\dev\starworks\Gacha\Arcane\Arcane.slnx" /t:<Target> /p:Configuration=Debug /m /nologo /v:minimal`
  New source/test files → run `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` once first. Targets: `ArcaneTests`, `Grimoire`. A premake postbuild change also needs `GenerateProjects.bat`.
- **Run headless tests** from the exe dir: `cd "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests"` then `.\ArcaneTests.exe "<filter>"`.
- **`[gpu]` / Grimoire.exe are DESK-ONLY** (Parsec GPU-driver crash hazard headless). Run only `~[gpu]` filters headless. The font-atlas rendering + the toolbar visuals are desk-verified in Task 4.
- **Clang/clangd diagnostics are NOISE** (wrong toolchain). MSVC build is the sole source of truth.
- **Commits:** single-line `type(scope): summary`, NO body, NO AI trailers.
- **Baselines (must not drop):** `~[gpu]` 27849/344, `[grimoire]` 84/12. Task 1 adds a `[grimoire]` case (grows both counts).

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/scripts/gen_icons_lucide.py` | Create | Offline generator: `codepoints.json` → `IconsLucide.h`. |
| `Arcane/Grimoire/src/IconsLucide.h` | Create (generated) | 1,979 `ICON_LC_*` macros + `ICON_LC_MIN`/`ICON_LC_MAX`. |
| `Arcane/Tests/src/IconsLucideTest.cpp` | Create | `[grimoire]` codepoint tripwire test. |
| `Arcane/Grimoire/src/EditorFonts.hpp` | Create | `Grimoire::InstallEditorFonts(float)` declaration. |
| `Arcane/Grimoire/src/EditorFonts.cpp` | Create | Roboto + merged lucide install; exe-relative paths. |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Call `InstallEditorFonts()` in `Init` (editor ctx current, pre-first-frame). |
| `Arcane/premake5.lua` | Modify | Postbuild copy of the two `.ttf` into Grimoire's output. |
| `Arcane/Grimoire/src/EditorPanels.cpp` | Modify | `DrawSimTimeToolbar`: text buttons → icon buttons + tooltips + tinted-active. |

---

## Task 1: Generate `IconsLucide.h` + headless codepoint test (TDD)

The reusable icon-name→codepoint header, guarded by a headless tripwire so a bad regen is caught. GPU-free.

**Files:** Create `Arcane/scripts/gen_icons_lucide.py`, `Arcane/Grimoire/src/IconsLucide.h` (generated), `Arcane/Tests/src/IconsLucideTest.cpp`.

**Interfaces:**
- Produces: macros `ICON_LC_PLAY`, `ICON_LC_PAUSE`, `ICON_LC_SQUARE`, `ICON_LC_STEP_FORWARD`, `ICON_LC_UNDO`, `ICON_LC_REDO`, `ICON_LC_MOVE`, `ICON_LC_ROTATE_3D`, `ICON_LC_SCALE_3D`, `ICON_LC_BOX`, `ICON_LC_GLOBE`, … (all 1,979) and `ICON_LC_MIN` (`0xE038u`) / `ICON_LC_MAX` (`0xE6FBu`).

- [ ] **Step 0 (read-first):** Confirm `Arcane/data/font/lucide/codepoints.json` maps `name -> decimal codepoint`. Confirm `ArcaneTests` can `#include "IconsLucide.h"` from `Grimoire/src` — `Arcane/premake5.lua` already lists `"%{wks.location}/Grimoire/src"` in the `ArcaneTests` `includedirs` (used by existing `[grimoire]` tests) and globs `Arcane/Tests/src/**.cpp`. Confirm the `[grimoire]` tag is the one those tests use.

- [ ] **Step 1: Write the failing test** — `Arcane/Tests/src/IconsLucideTest.cpp`:

```cpp
// Guards IconsLucide.h against a bad regeneration ([grimoire], CPU-only).
#include <catch2/catch_test_macros.hpp>

#include "IconsLucide.h"

namespace
{
    // Decode a UTF-8 macro value to its codepoint (icons are 3-byte U+Exxx here).
    unsigned Utf8ToCp(const char* s)
    {
        const unsigned char* u = reinterpret_cast<const unsigned char*>(s);
        if (u[0] < 0x80u) return u[0];
        if ((u[0] >> 5) == 0x6u) return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
        if ((u[0] >> 4) == 0xEu)
            return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
        return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12)
             | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
    }
}

TEST_CASE("IconsLucide.h codepoints are stable", "[grimoire]")
{
    CHECK(Utf8ToCp(ICON_LC_PLAY)         == 0xE13Cu);
    CHECK(Utf8ToCp(ICON_LC_PAUSE)        == 0xE12Eu);
    CHECK(Utf8ToCp(ICON_LC_SQUARE)       == 0xE167u);
    CHECK(Utf8ToCp(ICON_LC_STEP_FORWARD) == 0xE3EAu);
    CHECK(Utf8ToCp(ICON_LC_UNDO)         == 0xE19Bu);
    CHECK(Utf8ToCp(ICON_LC_REDO)         == 0xE143u);
    CHECK(Utf8ToCp(ICON_LC_MOVE)         == 0xE121u);
    CHECK(Utf8ToCp(ICON_LC_ROTATE_3D)    == 0xE2EAu);
    CHECK(Utf8ToCp(ICON_LC_SCALE_3D)     == 0xE2EBu);
    CHECK(Utf8ToCp(ICON_LC_BOX)          == 0xE061u);
    CHECK(Utf8ToCp(ICON_LC_GLOBE)        == 0xE0E8u);
    CHECK(ICON_LC_MIN == 0xE038u);
    CHECK(ICON_LC_MAX == 0xE6FBu);
}
```

- [ ] **Step 2: Run headless, verify fail.** `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` (new test file), build `ArcaneTests` → FAIL to compile (`IconsLucide.h` not found).

- [ ] **Step 3: Write the generator** `Arcane/scripts/gen_icons_lucide.py`:

```python
# Generate Arcane/Grimoire/src/IconsLucide.h from the lucide codepoints map.
# Run from the repo root: python Arcane/scripts/gen_icons_lucide.py
import json, re, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # repo root
CP   = os.path.join(ROOT, "Arcane", "data", "font", "lucide", "codepoints.json")
OUT  = os.path.join(ROOT, "Arcane", "Grimoire", "src", "IconsLucide.h")

cps = json.load(open(CP, encoding="utf-8"))
values = [int(v) for v in cps.values()]
lo, hi = min(values), max(values)

lines = [
    "#pragma once",
    "// GENERATED from Arcane/data/font/lucide/codepoints.json by",
    "// Arcane/scripts/gen_icons_lucide.py -- do not edit by hand.",
    "// Lucide icon-font codepoint macros for ImGui (raw UTF-8 byte-escape narrow literals).",
    "",
    "#define ICON_LC_MIN 0x%04Xu" % lo,
    "#define ICON_LC_MAX 0x%04Xu" % hi,
    "",
]
for name in sorted(cps):
    cp    = int(cps[name])
    macro = "ICON_LC_" + re.sub(r"[^0-9A-Za-z]", "_", name).upper()
    utf8  = "".join("\\x%02X" % b for b in chr(cp).encode("utf-8"))
    lines.append('#define %s "%s"' % (macro, utf8))
lines.append("")

with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(lines))
print("wrote", OUT, "(%d icons, U+%04X..U+%04X)" % (len(cps), lo, hi))
```

Run it: `python "D:\dev\starworks\Gacha\Arcane\scripts\gen_icons_lucide.py"` → writes `IconsLucide.h`. Spot-check: `grep "ICON_LC_PLAY " IconsLucide.h` shows `#define ICON_LC_PLAY "\xEE\x84\xBC"`.

- [ ] **Step 4: Build + run, verify PASS.** `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` (new header), build `ArcaneTests`; `.\ArcaneTests.exe "[grimoire]"` → all pass (84/12 + this case = 84+13/13 or as counted; record the exact new count). `.\ArcaneTests.exe "~[gpu]"` grows by the same case.

- [ ] **Step 5: Commit** — `feat(grimoire): generate IconsLucide.h (lucide icon-font codepoints)`.

---

## Task 2: Font install — `EditorFonts` + premake copy

Grimoire loads Roboto (16 px) as the base editor font and merges the lucide glyphs, on the editor context before the first frame. Build-verified; desk-verified in Task 4.

**Files:** Create `EditorFonts.{hpp,cpp}`; modify `GrimoireApp.cpp`, `Arcane/premake5.lua`.

**Interfaces:**
- Consumes: `ICON_LC_MIN`/`ICON_LC_MAX` (Task 1), ImGui, the editor context (current in `Init`).
- Produces: `void Grimoire::InstallEditorFonts(float sizePx = 16.0f)`.

- [ ] **Step 0 (read-first):** In `GrimoireApp.cpp` find the exact point in `Init` where the editor `ImGuiLayer` is up (via `m_gpu`) and `ImGui::GetCurrentContext()` is the editor context, BEFORE `MainLoop`'s first `BeginFrame` — the game-imgui redirect (`m_runtime->SetImGui(...)`) region is the landmark; `InstallEditorFonts()` goes near there while the editor context is current (and before/after the `OffscreenImGuiLayer` creation is fine since `Create` restores the editor context). Confirm `<imgui.h>` include path used elsewhere in Grimoire; confirm `ImWchar` is 16-bit (values `0xE038`/`0xE6FB` fit). In `premake5.lua` read the Grimoire `postbuildcommands` block (~451-457) + the `{MKDIR}`/`{COPYFILE}` token style.

- [ ] **Step 1: Write `EditorFonts.hpp`:**

```cpp
#pragma once

namespace Grimoire
{
    // Install the editor's fonts on the CURRENT ImGui context: Roboto base + merged
    // lucide icon glyphs. Call once in Init, after the editor ImGuiLayer is up and its
    // context is current, before the first frame. Font paths resolve exe-relative.
    void InstallEditorFonts(float sizePx = 16.0f);
}
```

- [ ] **Step 2: Write `EditorFonts.cpp`:**

```cpp
#include "EditorFonts.hpp"

#include "IconsLucide.h"

#include <imgui.h>

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace Grimoire
{
    namespace
    {
        std::filesystem::path ExeDir()
        {
#ifdef _WIN32
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, buf, MAX_PATH) != 0)
                return std::filesystem::path(buf).parent_path();
#endif
            return std::filesystem::current_path();
        }
    }

    void InstallEditorFonts(float sizePx)
    {
        ImGuiIO& io = ImGui::GetIO();
        const std::filesystem::path dir = ExeDir();
        const std::string roboto = (dir / "data" / "font" / "Roboto-Regular.ttf").string();
        const std::string lucide = (dir / "data" / "font" / "lucide" / "lucide.ttf").string();

        // Base font first -> becomes the default (ProggyClean is never added).
        io.Fonts->AddFontFromFileTTF(roboto.c_str(), sizePx);

        // Merge the lucide icon glyphs into the same atlas.
        static const ImWchar range[] = { ICON_LC_MIN, ICON_LC_MAX, 0 };
        ImFontConfig cfg;
        cfg.MergeMode        = true;
        cfg.GlyphMinAdvanceX = sizePx;   // monospace icon cell
        cfg.GlyphOffset.y    = 3.0f;     // baseline nudge; tune at desk (Task 4)
        io.Fonts->AddFontFromFileTTF(lucide.c_str(), sizePx, &cfg, range);
    }
}
```

- [ ] **Step 3: Call it in `GrimoireApp::Init`** at the point from Step 0 (add `#include "EditorFonts.hpp"`):

```cpp
        // Editor fonts: Roboto base + merged lucide icons, on the editor context
        // (current here), before the first frame. Zero engine change.
        Grimoire::InstallEditorFonts();
```

- [ ] **Step 4: Copy the fonts to Grimoire's output** — in the Grimoire `postbuildcommands` (`Arcane/premake5.lua`, the block that copies shaders/data), add (adapt token style to Step 0):

```lua
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/lucide"',
        '{COPYFILE} "%{wks.location}/data/font/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/lucide/lucide.ttf" "%{cfg.buildtarget.directory}/data/font/lucide/lucide.ttf"',
```

- [ ] **Step 5: Regenerate + build, verify clean.** `& "D:\dev\starworks\Gacha\Arcane\GenerateProjects.bat"` (new files + premake change); build `Grimoire` and `ArcaneTests`; zero errors. Confirm the two `.ttf` landed: `ls "D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\Grimoire\data\font\Roboto-Regular.ttf"` and `.../data/font/lucide/lucide.ttf` exist. `.\ArcaneTests.exe "~[gpu]"` unchanged from Task 1; `.\ArcaneTests.exe "[grimoire]"` unchanged from Task 1.

- [ ] **Step 6: Commit** — `feat(grimoire): install Roboto + merged lucide icon font on the editor atlas`.

---

## Task 3: Toolbar conversion — icon buttons in `DrawSimTimeToolbar`

Swap every text button for an icon button with a tooltip; tinted-background active for the T/R/S gizmo modes. Build-verified; desk-verified in Task 4.

**Files:** Modify `Arcane/Grimoire/src/EditorPanels.cpp`.

**Interfaces:**
- Consumes: the `ICON_LC_*` macros (Task 1), the merged font (Task 2).

- [ ] **Step 0 (read-first):** Read `DrawSimTimeToolbar` (`EditorPanels.cpp:40-108`) in full: the exact `play.Play/Stop`, `loop.SetPaused/RequestSingleStep/SetTimeScale`, `undo.Undo/Redo`, `mode`/`space` assignment calls, the `BeginDisabled`/tooltip on Undo/Redo, and the `SameLine()` layout — so the icon rewrite preserves every action and the loop-fetch-after-Play/Stop ordering. Note the includes at the top of the file (add `#include "IconsLucide.h"`).

- [ ] **Step 1: Add local icon-button helpers** near the top of `DrawSimTimeToolbar` (or file-local lambdas), then rewrite each control. Add `#include "IconsLucide.h"`.

```cpp
    // Icon button with a hover tooltip (icons need discoverable labels).
    auto iconBtn = [](const char* icon, const char* tip) -> bool
    {
        const bool clicked = ImGui::Button(icon);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };
    // Toggle-style icon button: tinted background when active (gizmo T/R/S).
    auto iconToggle = [](const char* icon, bool active, const char* tip) -> bool
    {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool clicked = ImGui::Button(icon);
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return clicked;
    };
```

Rewrite the controls (keep the exact actions + the `loop`-after-Play/Stop ordering from Step 0):

```cpp
    // Play / Stop (mutually exclusive; loop is fetched AFTER this block, unchanged).
    if (play.IsPlaying())
    {
        if (iconBtn(ICON_LC_SQUARE, "Stop"))  play.Stop(runtime, plugin);
    }
    else
    {
        if (iconBtn(ICON_LC_PLAY, "Play"))    play.Play(runtime, plugin);
    }

    // ... (existing: Arcane::RunLoop& loop = runtime.Loop();  -- keep here) ...

    ImGui::SameLine();
    if (iconBtn(loop.IsPaused() ? ICON_LC_PLAY : ICON_LC_PAUSE,
                loop.IsPaused() ? "Resume" : "Pause"))
        loop.SetPaused(!loop.IsPaused());

    ImGui::SameLine();
    if (iconBtn(ICON_LC_STEP_FORWARD, "Step")) loop.RequestSingleStep();

    // time-scale slider stays (unchanged).

    ImGui::SameLine();
    ImGui::BeginDisabled(!undo.CanUndo());
    if (iconBtn(ICON_LC_UNDO, undo.CanUndo() ? undo.UndoLabel() : "Undo")) undo.Undo();
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!undo.CanRedo());
    if (iconBtn(ICON_LC_REDO, undo.CanRedo() ? undo.RedoLabel() : "Redo")) undo.Redo();
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (iconToggle(ICON_LC_MOVE,      mode == Arcane::GizmoMode::Translate, "Translate"))
        mode = Arcane::GizmoMode::Translate;
    ImGui::SameLine();
    if (iconToggle(ICON_LC_ROTATE_3D, mode == Arcane::GizmoMode::Rotate,    "Rotate"))
        mode = Arcane::GizmoMode::Rotate;
    ImGui::SameLine();
    if (iconToggle(ICON_LC_SCALE_3D,  mode == Arcane::GizmoMode::Scale,     "Scale"))
        mode = Arcane::GizmoMode::Scale;

    ImGui::SameLine();
    const bool local = (space == Arcane::GizmoSpace::Local);
    if (iconBtn(local ? ICON_LC_BOX : ICON_LC_GLOBE,
                local ? "Local space" : "World space"))
        space = local ? Arcane::GizmoSpace::World : Arcane::GizmoSpace::Local;
```

(Adapt to the exact existing control order, `SameLine()` placement, and enum-qualified names from Step 0. Keep the `undo.UndoLabel()`/`RedoLabel()` tooltip content; the `CanUndo/Redo ? label : fallback` guards a possibly-empty label while disabled.)

- [ ] **Step 2: Build `Grimoire;ArcaneTests`, verify clean + no regression.** Build both; zero errors. `.\ArcaneTests.exe "~[gpu]"` + `.\ArcaneTests.exe "[grimoire]"` unchanged from Task 1 (no new tests here).

- [ ] **Step 3: Commit** — `feat(grimoire): sim-time toolbar uses lucide icon buttons`.

---

## Task 4: Desk-verify

- [ ] **Step 1: Headless gate** (here): `.\ArcaneTests.exe "~[gpu]"` at/above the Task-1 count; `.\ArcaneTests.exe "[grimoire]"` green. Record counts.
- [ ] **Step 2: Desk interactive** (Grimoire, at the desk — GPU hazard headless): the sim-time toolbar shows crisp lucide icons; hovering each shows the correct tooltip; **Play** swaps to a **stop square** while playing and back; **Pause** swaps to a play glyph when paused ("Resume"); **Step** fires; **Undo/Redo** show undo/redo glyphs, disable when empty, and carry the label tooltip; the active **T/R/S** button shows a tinted background and switching modes moves the tint; **Local/World** swaps box/globe and flips gizmo space; all other editor text (panels, hierarchy, inspector, console) is now **Roboto** (not ProggyClean); icons are vertically centered (adjust `GlyphOffset.y` if not); no ImGui atlas / NVRHI validation errors in the log; hot-reload (F5) still fine.
- [ ] **Step 3:** Append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** generated `IconsLucide.h` (raw UTF-8 byte escapes, all 1,979 + MIN/MAX) → Task 1; headless codepoint tripwire → Task 1; Roboto base + merged lucide install on the editor context before the first frame, exe-relative paths, zero engine change → Task 2; premake postbuild font copy → Task 2; toolbar conversion of ALL buttons with tooltips + tinted-active T/R/S + Play/Stop & Pause/Resume swaps + Undo/Redo disable-gating + Local/World box↔globe + time-scale-stays-slider + loop-ordering preserved → Task 3; desk-verify (crisp icons, tooltips, Roboto base, alignment, no validation noise) → Task 4; non-goals (other panels, DPI, in-scene text, game-context fonts) untouched. Covered.

**Placeholder scan:** `IconsLucide.h` is generated by the Task 1 script (not a hand-written placeholder — it is a build product with a concrete generator). Task 2 Step 0 / Task 3 Step 0 are read-first adaptations against named files/lines (the Init insertion point; the exact toolbar control order) with the intended code shown — same convention as this repo's prior plans. No "TBD"/"add error handling"/uncoded steps.

**Type consistency:** `Grimoire::InstallEditorFonts(float sizePx = 16.0f)`; `ICON_LC_MIN`/`ICON_LC_MAX` (`0xE038u`/`0xE6FBu`) used both in the merge glyph range (Task 2) and asserted (Task 1); `ICON_LC_PLAY/PAUSE/SQUARE/STEP_FORWARD/UNDO/REDO/MOVE/ROTATE_3D/SCALE_3D/BOX/GLOBE` used in the toolbar (Task 3) and asserted (Task 1); `Arcane::GizmoMode::Translate/Rotate/Scale`, `Arcane::GizmoSpace::Local/World` per the existing toolbar. Consistent across tasks and matching the spec.
