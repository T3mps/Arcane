# Editor Colour Picker Unification — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One shared colour widget for the whole editor — four 0-255 sRGB channel
boxes plus a swatch that opens a radial-wheel picker with a pasteable hex box —
adopted by all four colour-edit sites, with sRGB authoring over linear storage.

**Architecture:** A pure encode/decode pair plus one thin ImGui wrapper
(`ColorField4`) in the existing `EditorWidgets` unit. Storage stays the linear
float it is today; the widget presents sRGB. Each call site keeps its own undo
bracketing — the helper only reports `changed`.

**Tech Stack:** C++23, ImGui (vendored 1.92), imgui-node-editor, Catch2 (not used
here — see Verification), MSBuild via VS 18.

Spec: `docs/superpowers/specs/2026-07-30-editor-color-picker-unification-design.md`

## How this arc is being executed (read first — written to survive a context clear)

**This file plus the spec are the whole brief. They assume no prior context.**

- The user clears context, then says **"go"**. Execute all four tasks in order via
  the repo's SDD flow (task brief -> implement -> report -> commit, logging to
  `.superpowers/sdd/progress.md`), one commit per task.
- **HARD STOP-GATE AFTER TASK 2.** Task 2 is the literal request. Stop there,
  hand the user Task 2's desk-check list, and do NOT start Task 3 until they have
  run their visual gate and said to continue.
- **No unit tests in this arc** — the user's explicit call, because it is a visual
  feature. Do not add them "for safety"; see the Verification section.
- **Branch:** work continues on whatever branch the tree is already on (it was
  `arcane-runtime-host-fold`). Run `git branch --show-current` before the first
  commit. **Do NOT switch or create branches:** a concurrent session shares this
  working tree, and a checkout under it scatters its commits. Never `git add -A`
  for the same reason -- stage the exact paths each task names.

### BLOCKER for the Task 2 gate, as of 2026-07-30

`Arcane/src/Arcane/Host/GpuContext.cpp:27` sets `wd.hidden = true` for every host
and **nothing calls `Window::Show()` yet** -- that is a concurrent arc's in-flight
state (`8d24d63c`, its own Task 8 lands the `Show()`). So the editor currently
launches with an INVISIBLE window and the visual gate cannot run.

This blocks only the gate, not the implementation. Before handing over the gate,
check whether `Show()` has a caller yet:

```bash
grep -rn "\.Show()\|->Show()" Arcane/ArcaneEditor/src Arcane/src | head
```

If it still has none, say so plainly at the stop-gate rather than asking the user
to verify something they cannot see, and offer the options: wait for that arc's
Task 8, or temporarily flip `wd.hidden = false` locally for the gate only (never
commit that -- it belongs to the other arc).

## Global Constraints

- **UTF-8 without BOM, ASCII comments only.** No em-dashes or non-ASCII glyphs in
  source; use `--`.
- **Storage is LINEAR and must not be migrated.** No task rewrites stored colour
  data. Stored values stay bit-identical; only the displayed numbers re-label.
- **Alpha is never gamma-encoded.** It is coverage, not colour.
- **The true sRGB piecewise curve, never `pow(2.2)`.** The curve must match what
  `nvrhi::Format::SRGBA8_UNORM` applies in hardware (`Assets.cpp:176`). The
  tonemap's `pow(1.0/2.2)` (`shaders/tonemap.hlsl:33`) is the display encode and
  is OUT OF SCOPE — do not touch it.
- **Build with the VS 18 MSBuild, not PATH's:**
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"Arcane;ArcaneTests;ArcaneEditor" /v:minimal`
  from `Arcane/`, via PowerShell (Bash mangles `/p:`). Run it in the background and
  poll in the same turn.
- **No new source files in this plan**, so `GenerateProjects.bat` is NOT needed.
  If you add one anyway, you MUST run it or the file is silently not compiled.
- **`LNK1168` means the user's editor is running.** Rename the locked file aside
  (`ArcaneEditor.exe.running-lock-N`) and relink. With the editor running, its
  postbuild also cannot overwrite `Arcane.dll`, `Sandbox.dll`, `dxcompiler.dll` or
  `dxil.dll` — rename each aside the same way.
- **Never kill a running `ArcaneTests.exe`** — it is probably another session's
  gate. Queue behind it.

## Verification approach — READ THIS

**User's explicit call (2026-07-30): no in-depth tests for this arc. It is a
visual feature and the user's visual gate is the verification.** The spec's
original Verification section is superseded and has been updated to match.

So no task below writes a unit test. What replaces them:

- Every task ends with a clean build.
- Every task lists **desk-check items** for the user's visual gate.
- Task 1 carries three one-line numeric sanity `assert`-style checks as a scratch
  `--frames`-free console print, because a slightly-wrong transfer curve looks
  *plausible* on screen and is the one part of this arc a visual gate cannot
  catch. It is deleted before commit (Step 5). If you would rather skip even
  that, skip it — it is not load-bearing to the feature.

## File Structure

| file | responsibility | change |
|---|---|---|
| `Arcane/ArcaneEditor/src/EditorWidgets.hpp` | the editor's shared widget vocabulary | ADD colour section (2 conversions, 2 pure buffer converters, 1 widget) |
| `Arcane/ArcaneEditor/src/EditorWidgets.cpp` | their implementations | ADD the same |
| `Arcane/ArcaneEditor/src/InspectorView.cpp` | component field rows | REPLACE the `isColor` single-select arm; ENCODE the multi-select row |
| `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` | material params panel + graph canvas | REPLACE the material `ColorEdit` arm; REPLACE the `ConstColor` dead swatch |

Task order is deliberate: Task 2 is the literal request, so the user can run their
visual gate after Task 2 without waiting for the two shader-editor sites.

---

### Task 1: The shared colour widget

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditorWidgets.hpp` (append a colour section before the closing `}` of `namespace Arcane::Editor`)
- Modify: `Arcane/ArcaneEditor/src/EditorWidgets.cpp` (add `#include <cmath>`; append the implementations inside `namespace Arcane::Editor`)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, and Tasks 2-4 call exactly these:
  - `float SrgbToLinear(float srgb) noexcept`
  - `float LinearToSrgb(float linear) noexcept`
  - `void ColorDisplayFromLinear(const float linear[4], bool hdr, float outDisplay[4]) noexcept`
  - `void ColorLinearFromDisplay(const float display[4], bool hdr, float outLinear[4]) noexcept`
  - `bool ColorField4(const char* label, float linear[4], bool hdr = false)`

- [ ] **Step 1: Declare the colour section in `EditorWidgets.hpp`**

Append inside `namespace Arcane::Editor` (the file's existing style: a comment
block stating the WHY, then the declaration):

```cpp
    // ---- colour ---------------------------------------------------------------
    // sRGB <-> linear, the IEC 61966-2-1 piecewise curve. This is the SAME
    // transfer nvrhi::Format::SRGBA8_UNORM applies in hardware when a texture is
    // sampled (Assets.cpp:176), and that is the whole point: before this, a tint
    // and a texture pixel authored as the same number meant DIFFERENT colours in
    // the same multiply (#808080 -> linear 0.216 as a pixel, 0.502 as a tint).
    //
    // Deliberately NOT pow(2.2): that is the tonemap's display encode
    // (shaders/tonemap.hlsl:33, byte-matched to the retired client) and answers a
    // different question. Values outside [0,1] pass through monotonically so the
    // hdr path below cannot clamp anything.
    [[nodiscard]] float SrgbToLinear(float srgb) noexcept;
    [[nodiscard]] float LinearToSrgb(float linear) noexcept;

    // The pure halves of ColorField4, split out so the policy is stated in one
    // place and is obvious on inspection: RGB converts, ALPHA NEVER DOES (it is
    // coverage, not colour -- same rule as UE/Unity), and hdr passes all four
    // channels through untouched, because a value that may exceed 1 has no
    // meaningful sRGB encoding.
    void ColorDisplayFromLinear(const float linear[4], bool hdr, float outDisplay[4]) noexcept;
    void ColorLinearFromDisplay(const float display[4], bool hdr, float outLinear[4]) noexcept;

    // THE colour widget for the editor. Four 0-255 sRGB channel boxes plus a
    // swatch that opens ImGui's picker: radial hue wheel, alpha bar, and
    // RGB/HSV/Hex rows -- the hex box accepts a paste. Reads and writes LINEAR;
    // the encode/decode is entirely inside here.
    //
    // `linear` is float[4] rather than glm::vec4& so every call site binds
    // unchanged (the Inspector's glm::vec4 via &v.x, a material param's value.f,
    // a graph node's n.value). hdr = true presents raw linear floats instead.
    //
    // Returns true only on frames ImGui reported a change. The caller owns undo
    // bracketing -- call BeginGestureIfActivated / gestureBegin immediately after,
    // exactly as for any other widget.
    //
    // `label` is passed to ColorEdit4 verbatim; keep whatever each site already
    // uses. The Inspector needs a hidden "##id" because it draws its own label
    // column, while the material panel passes a visible name (which ImGui also
    // uses as the picker popup's title, imgui_widgets.cpp:5970-5973).
    bool ColorField4(const char* label, float linear[4], bool hdr = false);
```

- [ ] **Step 2: Implement in `EditorWidgets.cpp`**

Add `#include <cmath>` to the existing include block, then append inside
`namespace Arcane::Editor`:

```cpp
    float SrgbToLinear(float srgb) noexcept
    {
        // Guard the low end FIRST: std::pow of a negative base with a
        // fractional exponent is NaN, and a negative channel is reachable
        // (an HDR-authored value edited down, a script write).
        if (srgb <= 0.0f)     return srgb;
        if (srgb <= 0.04045f) return srgb / 12.92f;
        return std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    }

    float LinearToSrgb(float linear) noexcept
    {
        if (linear <= 0.0f)       return linear;
        if (linear <= 0.0031308f) return linear * 12.92f;
        return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    }

    void ColorDisplayFromLinear(const float linear[4], bool hdr, float outDisplay[4]) noexcept
    {
        if (hdr)
        {
            for (int i = 0; i < 4; ++i)
                outDisplay[i] = linear[i];
            return;
        }
        outDisplay[0] = LinearToSrgb(linear[0]);
        outDisplay[1] = LinearToSrgb(linear[1]);
        outDisplay[2] = LinearToSrgb(linear[2]);
        outDisplay[3] = linear[3];   // alpha: coverage, never encoded
    }

    void ColorLinearFromDisplay(const float display[4], bool hdr, float outLinear[4]) noexcept
    {
        if (hdr)
        {
            for (int i = 0; i < 4; ++i)
                outLinear[i] = display[i];
            return;
        }
        outLinear[0] = SrgbToLinear(display[0]);
        outLinear[1] = SrgbToLinear(display[1]);
        outLinear[2] = SrgbToLinear(display[2]);
        outLinear[3] = display[3];
    }

    bool ColorField4(const char* label, float linear[4], bool hdr)
    {
        float display[4];
        ColorDisplayFromLinear(linear, hdr, display);

        // Uint8 is ImGui's default today, but state it: the hdr branch needs
        // Float explicitly anyway, and a default is not a contract.
        // No NoPicker -- that flag is what this whole arc removes. The hex box
        // needs nothing from us: ColorEdit4 forwards PickerMask_ (so the wheel
        // carries into the popup) and force-sets DisplayMask_ on the picker
        // (imgui_widgets.cpp:5975-5976), and ColorPicker4 draws a hex ColorEdit4
        // whenever DisplayHex is in that mask (:6304-6305).
        const ImGuiColorEditFlags flags =
            (hdr ? (ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                 : ImGuiColorEditFlags_Uint8)
            | ImGuiColorEditFlags_AlphaBar
            | ImGuiColorEditFlags_PickerHueWheel;

        if (!ImGui::ColorEdit4(label, display, flags))
            return false;   // NO write on an unchanged frame. A blind round-trip
                            // every frame would re-quantise the stored float to
                            // 255ths continuously, so a colour set by script or
                            // animation would decay just from being looked at.

        ColorLinearFromDisplay(display, hdr, linear);
        return true;
    }
```

- [ ] **Step 3: Build**

```powershell
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"Arcane;ArcaneTests;ArcaneEditor" /v:minimal
```

Expected: `Build succeeded`, 0 errors. `EditorWidgets.cpp` compiles into BOTH
`ArcaneEditor.exe` and `ArcaneTests.exe` (`premake5.lua:611`), so a compile error
here breaks the test exe too — that is expected and is the only coupling.

- [ ] **Step 4: curve sanity check -- DONE OUT-OF-TREE, no temp code needed**

The one thing the visual gate cannot catch is a plausible-but-wrong curve. This
step originally pasted a throwaway `ARC_INFO` into `ColorField4` to be deleted
before commit; that is unnecessary -- the curve is nine lines of pure arithmetic
with no ImGui or engine dependency, so it was evaluated in a scratch shell instead
and the editor source was never polluted. Results (2026-07-30):

| check | value | meaning |
|---|---|---|
| `SrgbToLinear(128/255)` | **0.215861** | LOAD-BEARING: matches the sRGB decode `SRGBA8_UNORM` applies in hardware, so a tint and a texture pixel authored alike now agree |
| `LinearToSrgb(0.5) * 255` | **187.516** | -- |
| stored `0.501961` displays as | **188** | this is the re-label the user sees at the gate |
| piecewise boundary `S2L(0.04045)` | 0.0031308 both sides | continuous, no visible seam at the knee |
| `S2L(L2S(0.3))`, `L2S(S2L(0.729))` | exact to 8 dp | round-trip stable |
| `S2L(-0.25)`, `L2S(-0.25)` | -0.25 | negatives pass through, no NaN from `pow` |
| `L2S(2.0)` | 1.3533, monotonic | the hdr path cannot clamp |

**CORRECTION to this plan's earlier numbers:** it previously expected `186`, and
the spec's Decision 1 and desk-check item 7 said an existing `128` would re-label
to `186`. That is wrong, and wrong in a telling way -- `186.084` is exactly
`pow(0.5, 1/2.2) * 255`, i.e. it was computed with the 2.2 display gamma this arc
explicitly forbids. The true sRGB curve gives **188**. Both documents are
corrected; the gate list now says `188`.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditorWidgets.hpp Arcane/ArcaneEditor/src/EditorWidgets.cpp
git commit -m "feat(arcane): one colour widget for the editor -- sRGB display, linear storage

ColorField4 plus the pure encode/decode it delegates to. sRGB in, linear stored,
using the IEC 61966-2-1 piecewise curve -- the SAME transfer SRGBA8_UNORM applies
in hardware (Assets.cpp:176), so a tint and a texture pixel authored as the same
number finally mean the same colour. Alpha is never encoded. hdr mode passes
through for values that may exceed 1.

Writes back ONLY when ImGui reports a change: a blind round-trip every frame
would re-quantise the stored float to 255ths continuously.

No call site adopts it yet -- that is the next commits."
```

**Desk-check items:** none yet (nothing calls it).

---

### Task 2: Inspector adoption — the actual request

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorView.cpp:636-653` (the `Multi()` arm of `FieldKind::Vec4`)
- Modify: `Arcane/ArcaneEditor/src/InspectorView.cpp:654-677` (the `isColor` single-select arm)

Both are inside `namespace Arcane::Editor`, and the file already includes
`EditorWidgets.hpp` (line 5), so calls are unqualified and need no new include.

**Interfaces:**
- Consumes: `ColorField4`, `ColorDisplayFromLinear`, `SrgbToLinear` from Task 1.
- Produces: nothing for later tasks.

- [ ] **Step 1: Replace the single-select `isColor` arm**

Replace the whole `if (isColor) { ... break; }` block (currently lines 654-677,
the one whose comment begins "NoPicker ON PURPOSE") with:

```cpp
                        if (isColor)
                        {
                            // The editor's one colour widget: 0-255 sRGB channel
                            // boxes plus a swatch that opens the radial-wheel
                            // picker with a pasteable hex box. The STORED value
                            // stays the linear float vec4; ColorField4 owns the
                            // encode/decode (EditorWidgets.hpp).
                            //
                            // The picker popup DOES land inside undo, which the
                            // NoPicker comment this replaces got backwards. It
                            // reasoned that popup edits happen on the popup's own
                            // widgets so the gesture would never OPEN -- but ImGui
                            // rewrites g.LastItemData.ID to the picker's ActiveId
                            // while the popup is live, expressly so IsItemActive()
                            // keeps working on ColorEdit4 (imgui_widgets.cpp:
                            // 6044-6046), so IsItemActivated() fires and
                            // BeginGestureIfActivated parks that id. The end is
                            // the half that differs: on release ActiveId drops to
                            // 0, EvaluateEnd's ownership guard correctly declines
                            // the foreign id, and EditGesture::ScopeGuard's
                            // abandonment path commits it that same frame. Net:
                            // one drag = one undo step, same as the channel boxes.
                            bool changed = ColorField4(widgetId.c_str(), &v.x);
                            BeginGestureIfActivated(rawName, instance);
                            if (changed)
                                ForEachTarget(instance, [&](Astra::Entity, void* d)
                                              { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) *p = v; });
                            break;
                        }
```

- [ ] **Step 2: Encode the multi-select row**

Replace the whole `if (Multi()) { ... break; }` block immediately above it
(currently lines 636-653) with:

```cpp
                        if (Multi())
                        {
                            // Colour multi-select now shows the SAME 0-255 sRGB
                            // numbers the single-select row does. Before this the
                            // two disagreed INSIDE ONE FIELD: single-select drew
                            // 0-255 while this row drew raw linear floats
                            // (integral == false), so selecting a second entity
                            // silently changed what the numbers meant.
                            double cur[4];
                            if (isColor)
                            {
                                float disp[4];
                                ColorDisplayFromLinear(&v.x, /*hdr*/ false, disp);
                                for (int i = 0; i < 4; ++i)
                                    cur[i] = disp[i] * 255.0;
                            }
                            else
                            {
                                for (int i = 0; i < 4; ++i)
                                    cur[i] = v[i];
                            }
                            double out = 0.0;
                            // Axis strips only on the NON-colour flavour: XYZW
                            // colours under RGBA channels would label the values
                            // as something they are not. Colours are integral --
                            // they are 0-255 now, and a "128.000" box is the
                            // crowding the single-select row already rejected.
                            const int c = MultiScalarRow(widgetId.c_str(), 4, cur, MixedFor(f),
                                                         /*integral*/ isColor,
                                                         /*axisColors*/ !isColor, out);
                            if (c >= 0)
                            {
                                // 0-255 sRGB typed back -> linear storage. Alpha
                                // (channel 3) is coverage: it only rescales, and
                                // must NOT go through the curve.
                                const float fv = isColor
                                    ? (c == 3 ? static_cast<float>(out / 255.0)
                                              : SrgbToLinear(static_cast<float>(out / 255.0)))
                                    : static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
```

- [ ] **Step 3: Build**

Same command as Task 1 Step 3. Expected: `Build succeeded`, 0 errors.

Then confirm the editor actually relinked — a green build of `Arcane`/`ArcaneTests`
does NOT prove it, because neither compiles `InspectorView.cpp`:

```powershell
Get-Item D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe |
    Select-Object LastWriteTime
```

Expected: a timestamp from this build, not an older one.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorView.cpp
git commit -m "feat(arcane): the Inspector's colour swatch opens the picker

Clicking a colour field's swatch now opens the radial hue-wheel picker with a
pasteable hex box; the four 0-255 RGBA boxes are unchanged. Retires NoPicker,
whose comment concluded wrongly that a popup edit could not open the undo
gesture -- ImGui rewrites the item id while the picker is live precisely so
IsItemActive() keeps working (imgui_widgets.cpp:6044), and ScopeGuard's
abandonment path commits on release. One drag = one undo step.

Multi-select now encodes too: it was drawing raw linear floats while
single-select drew 0-255, so adding a second entity to the selection changed
what the numbers meant."
```

**Desk-check items (the visual gate):**
1. Select an entity with a `SpriteRenderer`. Click the `tint` swatch — a picker
   opens with a **round** hue wheel, an alpha bar, and RGB/HSV/Hex rows.
2. Drag on the wheel: the sprite updates live in the viewport.
3. Paste a hex (e.g. `C9A0DC`) into the picker's hex box — the swatch and the
   sprite both take that colour, and it looks like that colour.
4. One `Ctrl+Z` undoes one whole drag (not one step per frame). A second
   `Ctrl+Z` undoes the drag before it.
5. Click the swatch and click away without dragging: no undo step is created
   (`Ctrl+Z` still undoes whatever came before).
6. Select TWO entities with different tints: the channel boxes are blank for
   differing channels, and typing a number in one writes 0-255 sRGB to both.
7. **Expected re-label, not a bug:** an existing tint authored as `128` now reads
   `188`. The colour on screen must NOT change.

---

### Task 3: Material params adopt it

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp:5825-5827` (the `ParamWidget::ColorEdit` arm)
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` include block (add `#include "EditorWidgets.hpp"` — this file does NOT include it yet)

**Interfaces:**
- Consumes: `ColorField4` from Task 1.
- Produces: nothing.

- [ ] **Step 1: Add the include**

Add to the file's existing local-include group (the `#include "..."` block near
the top, alphabetically among its neighbours):

```cpp
#include "EditorWidgets.hpp"
```

- [ ] **Step 2: Replace the `ColorEdit` arm**

Replace:

```cpp
                case ParamWidget::ColorEdit:
                    edited = ImGui::ColorEdit4(d.name.c_str(), value.f);
                    break;
```

with:

```cpp
                case ParamWidget::ColorEdit:
                    // The same colour widget the Inspector uses: 0-255 sRGB plus
                    // the radial picker. This row used to be raw 0..1 floats with
                    // ImGui's square picker, so colour authored here and colour
                    // authored in the Inspector neither looked nor behaved alike.
                    //
                    // hdr = false (the default) on purpose: MatParamType::Color
                    // exists precisely because "the editor shows a color picker"
                    // (MaterialTypes.hpp:26), so it IS a colour and authors like
                    // one. Nothing declares a colour param as HDR -- ParamMeta
                    // carries only sliderMin/sliderMax (MaterialTypes.hpp:133-139),
                    // documented for the Float slider and ignored here, and every
                    // existing template leaves them 0..1. A param that wants an
                    // unclamped multiplier is a Float4, which keeps its DragFloat4
                    // above and is not a colour widget at all.
                    edited = ColorField4(d.name.c_str(), value.f);
                    break;
```

`value` is an `Arcane::MatParamValue` whose `.f` is a `float[4]`, so it binds
directly. The surrounding gesture bracket (the code after the `switch`, which
reads `edited`) is untouched.

- [ ] **Step 3: Build + confirm the editor relinked**

Same as Task 2 Step 3.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp
git commit -m "feat(arcane): material colour params use the shared colour widget

They were raw 0..1 floats with ImGui's square picker, so a colour authored in the
material panel neither looked nor behaved like the same colour authored in the
Inspector. Now 0-255 sRGB + the radial wheel, from one helper.

hdr = false: MatParamType::Color exists because the editor shows a picker for it,
so it is a colour. A param wanting an unclamped multiplier is a Float4."
```

**Desk-check items:**
1. Open a `.arcmat` document with a Color param. Its row is now four 0-255 boxes
   plus a swatch, matching the Inspector.
2. Click the swatch: the same wheel picker opens, with the param's name as the
   popup title.
3. Drag it: the material preview updates, and one `Ctrl+Z` undoes the drag.
4. The param's colour looks the same after this change as before it (values are
   re-labelled, not converted).

---

### Task 4: The `ConstColor` graph node gets a picker

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp:4868-4876` (the `ConstColor` swatch inside the node canvas)

**Interfaces:**
- Consumes: `ColorField4` from Task 1; the include added in Task 3.
- Produces: nothing.

- [ ] **Step 1: Replace the dead swatch**

Replace:

```cpp
                if (n.type == Arcane::GraphNodeType::ConstColor)
                {
                    ImGui::SameLine();
                    // Preview swatch only (LINEAR floats; the full picker is a
                    // popup and popups cannot open inside the canvas).
                    ImGui::ColorButton("##swatch",
                                       ImVec4(n.value[0], n.value[1], n.value[2], n.value[3]),
                                       ImGuiColorEditFlags_NoTooltip, ImVec2(18, 18));
                }
```

with:

```cpp
                if (n.type == Arcane::GraphNodeType::ConstColor)
                {
                    ImGui::SameLine();
                    // A LIVE swatch now: clicking it opens the shared picker.
                    //
                    // The comment this replaces said popups cannot open inside
                    // the canvas. They can, and this very file already does it --
                    // ed::Suspend()/ed::Resume() brackets at :2848, :3916 and
                    // :3987 exist for exactly this, because a popup has to escape
                    // the node editor's transformed coordinate space.
                    //
                    // hdr = true: a ConstColor feeds raw shader maths and may
                    // legitimately exceed 1, where sRGB encoding is meaningless,
                    // so it shows linear floats. The DragFloat4 above stays the
                    // numeric entry; this is the graphical one.
                    ed::Suspend();
                    const bool swatchEdited = ColorField4("##swatch", n.value, /*hdr*/ true);
                    gestureBegin("Edit Color");
                    if (swatchEdited)
                        valueEdited();
                    gestureEnd();
                    ed::Resume();
                }
```

The `gestureBegin` / `valueEdited` / `gestureEnd` order mirrors the `DragFloat4`
immediately above (`:4864-4867`) — `gestureBegin` must follow the widget, since
`EditGesture::BeginOnActivate` reads the just-submitted item, and
`EndOnDeactivate` is documented safe to call per row per frame.

- [ ] **Step 2: Build + confirm the editor relinked**

Same as Task 2 Step 3.

- [ ] **Step 3: Commit**

```bash
git add Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp
git commit -m "feat(arcane): the ConstColor graph node's swatch opens the picker

It was a dead preview swatch, on the belief that popups cannot open inside the
node canvas -- but this file already opens canvas popups via ed::Suspend()/
ed::Resume() in three places, which is exactly what that needs. hdr = true: a
ConstColor is raw shader maths and may exceed 1."
```

**Desk-check items:**
1. Open a shader graph, add/select a `ConstColor` node. Click its swatch — the
   picker opens **above the canvas** and is usable (not clipped, not offset, and
   panning the canvas while it is open does not strand it).
2. The picker shows linear floats (not 0-255) — correct for this node.
3. Drag the wheel: the graph regenerates and the preview updates.
4. One `Ctrl+Z` undoes the drag.
5. The node's `DragFloat4` still works and still shows the same numbers.

---

## Self-Review

**Spec coverage:**

| spec item | task |
|---|---|
| Decision 1 (sRGB in, linear stored) | 1 |
| Decision 2 (true sRGB curve, not pow 2.2) | 1 (+ Global Constraints) |
| Decision 3 (alpha never encoded) | 1, and Task 2's multi-select channel-3 branch |
| Decision 4 (hdr mode) | 1 (mode), 3 (`false`), 4 (`true`) |
| Decision 5 (radial wheel) | 1 (`PickerHueWheel`) |
| Decision 6 (helper does not own undo) | 1 (returns `changed`); 2/3/4 keep their brackets |
| helper in `EditorWidgets`, `float[4]` signature | 1 |
| label passed verbatim per site | 1 (contract), 2 (`##id`), 3 (visible name) |
| site: Inspector single-select | 2 |
| site: Inspector multi-select | 2 |
| site: material params | 3 |
| site: `ConstColor` node + Suspend/Resume | 4 |
| undo reasoning (ImGui id rewrite + ScopeGuard) | 2, Step 1 comment |
| **Verification** | **SUPERSEDED by user's call — no unit tests; desk-check lists per task, and the spec has been updated to match so plan and spec do not diverge** |
| non-goals (no `Arcane::Color` type, no tonemap change, no data migration) | respected; Global Constraints forbids the last two |

**Placeholder scan:** none — every code step carries complete code, every build
step an exact command and expected output.

**Type consistency:** `ColorField4(const char*, float[4], bool)`,
`ColorDisplayFromLinear(const float[4], bool, float[4])`, `SrgbToLinear(float)`
are spelled identically in Task 1's declarations, implementations, Interfaces
blocks, and in every call in Tasks 2-4. `MultiScalarRow(const char*, int, const
double*, const FieldMixedMask&, bool integral, bool axisColors, double&)` matches
`InspectorView.cpp:263-265`. `MatParamValue::f` is `float[4]`.
