# Colour Pipeline Correction + Dense Picker — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the engine's colour pipeline symmetric (true sRGB in and out), give
`EditGesture` the popup-lifetime boundary it lacks, and replace the four colour-edit
sites with one dense UE-shaped picker that names every colour space on screen.

**Architecture:** Three layers, done in order. The shader's display encode is fixed
first so every later encode agrees with the renderer. Then `EditGesture` gains a
popup boundary in its pure core, because owning a popup ends the ActiveId loan that
makes popup undo work today. Then the picker is built as the first consumer.

**Tech Stack:** C++23, HLSL (DXC -> DXIL + SPIR-V), ImGui 1.92 (vendored),
imgui-node-editor, Catch2, MSBuild via VS 18.

Spec: `docs/superpowers/specs/2026-07-30-ue-style-color-picker-design.md`

## How this arc is being executed (read first — written to survive a context clear)

**This file plus the spec are the whole brief. They assume no prior context.**

- **The user clears context, then says "go".** Execute via
  **superpowers:subagent-driven-development** — the user asked for SDD explicitly, so
  dispatching subagents is authorised for this arc. One task per subagent, review
  between tasks, one commit per task.
- **Read the spec too, not just this plan.** A prior arc's review found every task
  reviewer read the plan and never the spec, and this design has reasoning that only
  lives there — chiefly WHY the picker buffer is sRGB-encoded and why there is only
  ever one `ColorPicker4` call. Spec:
  `docs/superpowers/specs/2026-07-30-ue-style-color-picker-design.md`.
- **Task 1 is the tonemap and it goes FIRST** (user's call, 2026-07-30). Everything
  downstream encodes colour; the transfer must be right before any of it lands.
- **Suggested checkpoint after Task 1** so the user can eyeball the shadow change
  before anything builds on it. Ask rather than assume.
- **The tonemap change is NOT behaviour-preserving.** Rendered output changes: deep
  shadows darken (linear 0.002 encodes 0.059 -> 0.026, ~8/255). Do not describe it
  as a refactor in commits.
- **No visual gate for Task 1** (user's call): "nothing is built with the engine
  other than our test projects, and I can tell you myself if something is off." The
  existing golden test is the objective check.
- **Tests:** the gesture PURE CORE gets unit tests (Task 2) because it is a decision
  table with an existing headless harness and a stranded transaction disables undo
  editor-wide. The ImGui-facing skin and the widget do NOT — matching
  `EditGesture.hpp`'s own note that the skin "stays thin" and is not unit-driven.
  Do not add widget tests "for safety".
- **Branch:** work continues on whatever branch the tree is on (it was
  `arcane-runtime-host-fold`). Run `git branch --show-current` before the first
  commit. **Do NOT switch or create branches** — a concurrent session shares this
  working tree. Never `git add -A`; stage the exact paths each task names.

## Global Constraints

- **UTF-8 without BOM, ASCII comments only.** No em-dashes or non-ASCII glyphs in
  source; use `--`.
- **The true IEC 61966-2-1 sRGB curve everywhere, never `pow(2.2)`.** Knee at
  `0.0031308` (encode) / `0.04045` (decode), scale `1.055`, offset `0.055`,
  exponent `2.4`, linear segment `12.92`.
- **Three copies of that curve will exist after Task 1** — `EditorWidgets.cpp`
  (C++, branching), `shaders/tonemap.hlsl` (HLSL, branchless `min`), and
  `TonemapTest.cpp` (C++ mirror, branchless). Each MUST carry a comment naming the
  other two. Same drift hazard as the `kPxRange`/`kAtlasSize` pair between
  `msdf.hlsl` and `TextSystem.cpp`.
- **Alpha is never gamma-encoded.** It is coverage.
- **Storage stays LINEAR and is never migrated.** No task rewrites stored data.
- **Build with the VS 18 MSBuild, not PATH's**, from `Arcane/`, via PowerShell
  (Bash mangles `/p:`):
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"Arcane;ArcaneTests;ArcaneEditor" /v:minimal`
  Run it in the background and poll in the same turn.
- **Tasks 4 and 5 ADD SOURCE FILES, so `GenerateProjects.bat` MUST be run** or they
  are silently not compiled. Tasks 1-3 add none.
- **A green `Arcane`/`ArcaneTests` build does NOT prove the editor relinked** —
  neither compiles `InspectorView.cpp`. After any editor change, check:
  `Get-Item ...\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe | Select-Object LastWriteTime`
- **`LNK1168` means the editor is running.** Rename the locked file aside
  (`ArcaneEditor.exe.running-lock-N`) and relink; with the editor running its
  postbuild also cannot overwrite `Arcane.dll`, `Sandbox.dll`, `dxcompiler.dll`,
  `dxil.dll` — rename each aside the same way.
- **Never kill a running `ArcaneTests.exe`** — probably another session's gate.
- **A full-suite verdict is unreliable while a concurrent build runs.** Check
  `Get-Process MSBuild,cl` first; prefer targeted tags (`[editor]`, `[gpu]`).

## File Structure

| file | responsibility | change |
|---|---|---|
| `Arcane/shaders/tonemap.hlsl` | linear HDR -> display encode | MODIFY the transfer |
| `Arcane/Tests/src/TonemapTest.cpp` | golden colour-pipeline test | MODIFY the CPU mirror |
| `Arcane/ArcaneEditor/src/EditGesture.hpp/.cpp` | the one gesture bracket | ADD the popup boundary (pure core + skin) |
| `Arcane/Tests/src/EditGestureTest.cpp` | pure-core decision table | ADD popup cases |
| `Arcane/ArcaneEditor/src/CanvasPopupScope.hpp` | **NEW** — names the node-canvas popup rule | CREATE |
| `Arcane/ArcaneEditor/src/ColorPickerPopup.hpp/.cpp` | **NEW** — the dense picker | CREATE |
| `Arcane/ArcaneEditor/src/InspectorView.cpp` | component field rows | MODIFY the Vec4 colour arms |
| `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` | material params + node canvas | MODIFY two sites |
| `Arcane/ArcaneEditor/src/EditorWidgets.hpp/.cpp` | shared widget vocabulary | RETIRE `ColorField4` and its two buffer converters; KEEP `SrgbToLinear`/`LinearToSrgb` |

`CanvasPopupScope` is its own header rather than part of `EditorWidgets` on purpose:
`EditorWidgets.hpp` currently includes only `imgui.h` plus `Astra::Range`, and
pulling `imgui_node_editor.h` into it would couple the whole widget vocabulary to
the node editor. `ColorPickerPopup` is its own unit because the dense body is
substantial and `EditorWidgets.cpp` is already 621 lines of unrelated widgets.

---

### Task 1: Tonemap display encode -> true sRGB

**Files:**
- Modify: `Arcane/shaders/tonemap.hlsl:1-3` (header comment) and `:30-35` (`ps_main`)
- Modify: `Arcane/Tests/src/TonemapTest.cpp:1-2` (header comment) and `:23-27` (`ExpectedByte`)

**Interfaces:**
- Consumes: nothing.
- Produces: nothing callable. Establishes the curve every later task encodes with.

- [ ] **Step 1: Replace the shader's transfer**

In `Arcane/shaders/tonemap.hlsl`, replace lines 1-3 (the header comment) with:

```hlsl
// Output pass: linear HDR canvas -> ACES filmic -> TRUE sRGB encode ->
// display-referred backbuffer. Fullscreen triangle from SV_VertexID -- no
// vertex buffer.
//
// The encode is the IEC 61966-2-1 piecewise curve, NOT pow(1/2.2). That is the
// same transfer nvrhi::Format::SRGBA8_UNORM applies in HARDWARE when a texture
// is sampled (Assets.cpp:176), so the pipeline is symmetric: true sRGB in, true
// sRGB out. It is also what UE uses on output
// (GammaCorrectionCommon.ush LinearToSrgbBranchless).
//
// It previously used pow(1/2.2) to byte-match the RETIRED LOVE client's
// post_process.glsl. That oracle is gone and byte-identity is explicitly not a
// goal, so input and output no longer disagree about the transfer function.
//
// CURVE IS MIRRORED in two other places -- keep all three in step:
//   Tests/src/TonemapTest.cpp   (CPU golden reference, same branchless form)
//   ArcaneEditor/src/EditorWidgets.cpp (LinearToSrgb, branching form)
```

Then add this function immediately after `ACESFilmic` (after line 28's closing
brace) and change `ps_main`:

```hlsl
// Branchless sRGB encode, UE's form: below the knee the linear segment is the
// smaller value, above it the power segment is, so min() selects correctly and
// is continuous at 0.0031308. Input here is already saturated to [0,1] by
// ACESFilmic, so no negative-base pow is reachable.
float3 LinearToSrgb(float3 lin)
{
    return min(lin * 12.92, pow(max(lin, 0.0031308), 1.0 / 2.4) * 1.055 - 0.055);
}

float4 ps_main(VSOutput input) : SV_Target0
{
    float3 linearColor = g_Scene.Sample(g_Sampler, input.uv).rgb;
    float3 display = LinearToSrgb(ACESFilmic(linearColor));
    return float4(display, 1.0);
}
```

- [ ] **Step 2: Update the golden test's CPU mirror**

In `Arcane/Tests/src/TonemapTest.cpp`, replace lines 1-2 with:

```cpp
// Color-pipeline golden test: linear HDR canvas -> ACES -> TRUE sRGB encode must
// byte-match the CPU reference. The curve is the IEC 61966-2-1 piecewise one,
// deliberately NOT pow(1/2.2) -- it matches what SRGBA8_UNORM applies in hardware
// on input, so the pipeline is symmetric. MIRRORS shaders/tonemap.hlsl; keep the
// two in step (and ArcaneEditor/src/EditorWidgets.cpp's branching copy).
```

and replace `ExpectedByte` (lines 23-27) with:

```cpp
    // Same branchless algebra as the shader, so the only difference between the
    // two is float evaluation order -- which the +/-2 byte tolerance below
    // absorbs. Do NOT "simplify" this to the branching form: an identical
    // expression is what makes this a drift detector rather than a second
    // opinion.
    uint8_t ExpectedByte(float linearChannel)
    {
        const float lin = AcesFilmic(linearChannel);
        const float display = std::min(lin * 12.92f,
                                       std::pow(std::max(lin, 0.0031308f), 1.0f / 2.4f)
                                           * 1.055f - 0.055f);
        return (uint8_t)std::lround(display * 255.0f);
    }
```

- [ ] **Step 3: Build (recompiles shaders via the prebuild step)**

```powershell
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"Arcane;ArcaneTests;ArcaneEditor" /v:minimal
```

Expected: `Build succeeded`, 0 errors, and a `Shaders compiled to ...\shaders\generated` line.

- [ ] **Step 4: Run the golden test**

```powershell
cd D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[tonemap]"
```

Expected: `All tests passed`. If the tag does not select it, run
`.\ArcaneTests.exe "*Tonemap*"` and use whichever selector matches.

If it FAILS with the bytes off by more than 2, the shader and the mirror have
diverged — compare the two expressions character by character before changing the
tolerance. **Do not widen the tolerance to make it pass.**

- [ ] **Step 5: Commit**

```bash
git add Arcane/shaders/tonemap.hlsl Arcane/Tests/src/TonemapTest.cpp
git commit -m "fix(arcane): display encode is the true sRGB curve, not pow(1/2.2)

Textures load as SRGBA8_UNORM, so hardware decodes input with the true IEC
61966-2-1 curve while tonemap.hlsl encoded output with a 2.2 power -- the pipeline
disagreed with itself about the transfer function. The 2.2 existed only to
byte-match the RETIRED LOVE client's post_process.glsl, and byte-identity is
explicitly not a goal. UE encodes with the true curve too
(GammaCorrectionCommon.ush LinearToSrgbBranchless).

NOT a behaviour-preserving refactor: rendered output changes by design. Midtones
move under 1/255 (linear 0.5: 0.7297 -> 0.7354) but deep shadows change materially
(linear 0.002: 0.059 -> 0.026, ~8/255) because the 2.2 power lifted them.
Everything on screen gains slightly deeper shadows. That is the fix.

TonemapTest's CPU mirror is updated in the same commit, in the identical branchless
form, so the golden test stays a drift detector rather than a second opinion."
```

**Desk-check items:** none — the golden test is the check, per the user's call. The
user will report if anything looks off.

---

### Task 2: `EditGesture` popup boundary — pure core, test-first

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditGesture.hpp` (declare, in the PURE CORE block before the `---- ImGui-facing skin ----` divider)
- Modify: `Arcane/ArcaneEditor/src/EditGesture.cpp` (implement, in the first `namespace` block above the `#include <imgui.h>` at line 28)
- Modify: `Arcane/Tests/src/EditGestureTest.cpp` (add cases)

**Interfaces:**
- Consumes: existing `Slots`, `Arcane::TransactionId`.
- Produces, and Task 3 calls exactly this:
  - `bool ShouldClosePopup(const Slots& s, std::uint32_t popupId, bool open, bool hasPendingCommit) noexcept`

- [ ] **Step 1: Write the failing tests**

Append to `Arcane/Tests/src/EditGestureTest.cpp` (it already has `using namespace
Arcane::Editor;` and `using Arcane::TransactionId;` at the top, so these need no
new includes):

```cpp
TEST_CASE("ShouldClosePopup: closes only when OUR popup stopped being open", "[editor]")
{
    Slots s;
    s.txn  = static_cast<TransactionId>(7);
    s.item = 1234;                      // the popup id that opened the gesture

    // Still open -> nothing to do. This is the every-frame no-op case.
    CHECK_FALSE(ShouldClosePopup(s, 1234, /*open*/ true,  /*hasPendingCommit*/ false));
    // Ours, and gone -> close.
    CHECK(      ShouldClosePopup(s, 1234, /*open*/ false, /*hasPendingCommit*/ false));

    // OWNERSHIP GUARD: a different site asking about ITS popup must never close
    // ours. Same rule EvaluateEnd enforces via lastItemId, and the reason is the
    // same -- Cancel/Commit on a foreign live token corrupts the owner's edit.
    CHECK_FALSE(ShouldClosePopup(s, 9999, /*open*/ false, /*hasPendingCommit*/ false));
}

TEST_CASE("ShouldClosePopup: nothing parked means nothing to close", "[editor]")
{
    Slots s;                            // txn == None, item == 0

    CHECK_FALSE(ShouldClosePopup(s, 1234, /*open*/ false, /*hasPendingCommit*/ false));

    // JOINED gesture: txn is None because another consumer owned the stack, but a
    // built command is still owed. It must still close, or the command is lost.
    s.item = 1234;
    CHECK(ShouldClosePopup(s, 1234, /*open*/ false, /*hasPendingCommit*/ true));
    // ...and not while it is still open.
    CHECK_FALSE(ShouldClosePopup(s, 1234, /*open*/ true, /*hasPendingCommit*/ true));
}
```

- [ ] **Step 2: Run to verify they fail**

```powershell
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"ArcaneTests" /v:minimal
```

Expected: **compile error**, `'ShouldClosePopup': identifier not found`. That is the
failing state — a link/compile failure is the correct "red" for a function that does
not exist yet.

- [ ] **Step 3: Declare it in the pure core**

In `EditGesture.hpp`, insert immediately before the
`// ---- ImGui-facing skin (thin; NOT unit-driven -- the pure core above is) ----`
divider:

```cpp
    // Popup-lifetime close test -- the FOURTH edit boundary. `open` is whether the
    // popup that owns the parked gesture is still open this frame; `popupId` is
    // the asking site's own popup id.
    //
    // Why a popup needs its own shape rather than reusing the activation pair: a
    // popup's edits are made by FOREIGN widgets we do not submit, so there is no
    // item of ours to observe activating or deactivating. Worse, the reason the
    // activation pair appears to work for ImGui's OWN colour popup is a loan --
    // ColorEdit4 does `g.LastItemData.ID = g.ActiveId` while its picker window is
    // active (imgui_widgets.cpp, "so IsItemActive() will function on
    // ColorEdit4()"), and that only happens when ImGui opened the popup itself. A
    // hand-rolled popup gets no such loan, so IsItemActivated() never fires and
    // the edits would land outside undo entirely.
    //
    // There is no Cancel counterpart on purpose: for a popup, closed IS the end,
    // and the layer's invariant is that abandonment COMMITS -- the edits were
    // applied live and the user watched them happen. Escape and click-away
    // therefore keep the edit and leave exactly one step; Ctrl+Z is the way back.
    [[nodiscard]] bool ShouldClosePopup(const Slots& s, std::uint32_t popupId,
                                        bool open, bool hasPendingCommit) noexcept;
```

- [ ] **Step 4: Implement it**

In `EditGesture.cpp`, add inside the FIRST `namespace Arcane::Editor::EditGesture`
block (the pure-core one, above the `#include <imgui.h>` at line 28), after
`ShouldCloseStaleOnActivate`:

```cpp
    bool ShouldClosePopup(const Slots& s, std::uint32_t popupId,
                          bool open, bool hasPendingCommit) noexcept
    {
        if (s.txn == Arcane::TransactionId::None && !hasPendingCommit)
            return false;          // nothing parked
        if (s.item != popupId)
            return false;          // not ours -- see the ownership guard note
        return !open;
    }
```

- [ ] **Step 5: Run to verify they pass**

```powershell
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /t:"ArcaneTests" /v:minimal
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]"
```

Expected: `All tests passed`, with the case count up by 2 from the previous run.

- [ ] **Step 6: Prove the gate can fail**

Temporarily delete the `if (s.item != popupId) return false;` line, rebuild, and run
`[editor]` again. Expected: the ownership-guard `CHECK_FALSE` in the first case
FAILS. Restore the line and re-run to green.

A gate never seen to fail is not known to work, and this is the guard whose absence
would let one colour field close another's transaction.

- [ ] **Step 7: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditGesture.hpp Arcane/ArcaneEditor/src/EditGesture.cpp Arcane/Tests/src/EditGestureTest.cpp
git commit -m "feat(arcane): EditGesture gains a popup-lifetime boundary (pure core)

The layer modelled exactly ONE edit boundary -- item activated to item deactivated
-- and the editor has four shapes in play. Popup edits appear to work today only
because ColorEdit4 LENDS us its picker window's ActiveId (g.LastItemData.ID =
g.ActiveId, 'so IsItemActive() will function on ColorEdit4()'), which happens only
when ImGui opened the popup itself. Any hand-rolled popup gets no loan, so
IsItemActivated() never fires and its edits fall outside undo.

ShouldClosePopup joins the pure core beside EvaluateEnd and the two close tests, so
the decision table stays headlessly driveable. Same ownership guard as EvaluateEnd,
for the same reason: closing on a foreign live token corrupts the owner's edit.

No Cancel counterpart, deliberately -- for a popup, closed IS the end, and the
layer's invariant is that abandonment commits.

Ownership guard verified by deleting it and watching the test go red."
```

**Desk-check items:** none — pure core, covered by the tests above.

---

### Task 3: `EditGesture` popup skin

**Files:**
- Modify: `Arcane/ArcaneEditor/src/EditGesture.hpp` (declare, after `EndOnDeactivate`)
- Modify: `Arcane/ArcaneEditor/src/EditGesture.cpp` (implement, in the SECOND namespace block — the one after `#include <imgui.h>`)

**Interfaces:**
- Consumes: `ShouldClosePopup` (Task 2), existing `ClosePending`,
  `ShouldCloseStaleOnActivate`.
- Produces, and Tasks 6-7 call exactly these:
  - `void BeginOnPopupOpen(Arcane::CommandStack* stack, GestureState& st, std::uint32_t popupId, Arcane::FunctionRef<std::string()> label, Arcane::FunctionRef<std::function<void()>()> onOpened)`
  - `void EndOnPopupClose(Arcane::CommandStack* stack, GestureState& st, std::uint32_t popupId)`

- [ ] **Step 1: Declare both**

In `EditGesture.hpp`, insert after `EndOnDeactivate`'s declaration and before
`ClosePending`:

```cpp
    // The popup pair. Call BOTH every frame at a popup-hosted edit site, with the
    // site's own popup id (ImGui::GetID on a stable string, the same id passed to
    // ImGui::OpenPopup -- imgui.h:868 has the ImGuiID overload).
    //
    // BeginOnPopupOpen opens the gesture on the first frame the popup is observed
    // open and is a no-op every frame after, so the caller needs no edge tracking.
    // Liveness is `slots.item == popupId`: slots are cleared on close, and we park
    // item only for a popup whose gesture we opened -- so unlike the widget path,
    // where the header warns item is not an "is one open" flag, here it is exactly
    // that. `popupId == 0` is rejected; ImGui never mints a zero id for a real
    // popup and treating it as live would alias the cleared state.
    void BeginOnPopupOpen(Arcane::CommandStack* stack, GestureState& st,
                          std::uint32_t popupId,
                          Arcane::FunctionRef<std::string()> label,
                          Arcane::FunctionRef<std::function<void()>()> onOpened);

    // Owner-guarded close on the popup going away; safe to call every frame. Always
    // COMMITS (via ClosePending) -- see ShouldClosePopup on why there is no Cancel.
    void EndOnPopupClose(Arcane::CommandStack* stack, GestureState& st,
                         std::uint32_t popupId);
```

- [ ] **Step 2: Implement both**

In `EditGesture.cpp`, add to the SECOND `namespace Arcane::Editor::EditGesture`
block (after `EndOnDeactivate`, before `ScopeGuard::~ScopeGuard`). Add
`#include <imgui_internal.h>` is ALREADY present at line 29 — `IsPopupOpen(ImGuiID,
ImGuiPopupFlags)` is internal-only (`imgui_internal.h:3581`), so no new include is
needed:

```cpp
    void BeginOnPopupOpen(Arcane::CommandStack* stack, GestureState& st,
                          std::uint32_t popupId,
                          Arcane::FunctionRef<std::string()> label,
                          Arcane::FunctionRef<std::function<void()>()> onOpened)
    {
        if (!stack || popupId == 0)
            return;
        if (!ImGui::IsPopupOpen(static_cast<ImGuiID>(popupId), ImGuiPopupFlags_None))
            return;
        if (st.slots.item == popupId)
            return;                     // ours, already live -- every frame after the first
        if (ShouldCloseStaleOnActivate(st.slots, static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
        st.slots.txn  = stack->Begin(label());
        st.slots.item = popupId;
        st.pendingCommit = onOpened();
    }

    void EndOnPopupClose(Arcane::CommandStack* stack, GestureState& st,
                         std::uint32_t popupId)
    {
        if (!stack)
            return;
        if (ShouldClosePopup(st.slots, popupId,
                             ImGui::IsPopupOpen(static_cast<ImGuiID>(popupId),
                                                ImGuiPopupFlags_None),
                             static_cast<bool>(st.pendingCommit)))
            ClosePending(*stack, st);
    }
```

- [ ] **Step 3: Build**

Command as in Task 1 Step 3. Expected: `Build succeeded`, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/EditGesture.hpp Arcane/ArcaneEditor/src/EditGesture.cpp
git commit -m "feat(arcane): the ImGui-facing half of the popup gesture boundary

BeginOnPopupOpen / EndOnPopupClose, both safe to call every frame. Open is
edge-detected inside via slots.item == popupId rather than by the caller, and close
always commits.

Thin on purpose, matching the header's existing split: the decision table lives in
the pure core (Task 2, unit-tested headlessly) and this only supplies ImGui facts to
it. IsPopupOpen's ImGuiID overload is internal-only (imgui_internal.h:3581), which
this TU already includes."
```

**Desk-check items:** none yet — no call site adopts it until Task 6.

---

### Task 4: Name the node-canvas popup rule

**Files:**
- Create: `Arcane/ArcaneEditor/src/CanvasPopupScope.hpp`
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` — the three existing
  bracket pairs, currently at `:2849`/`:2957`, `:3917`/`:3984`, `:3988`/`:4305`
  (re-grep for `ed::Suspend()` before editing; earlier tasks in this arc do not move
  them, but confirm rather than trust these numbers)

**Interfaces:**
- Consumes: nothing.
- Produces, and Task 7 uses it:
  - `struct Arcane::Editor::CanvasPopupScope { CanvasPopupScope(); ~CanvasPopupScope(); }`

- [ ] **Step 1: Create the header**

```cpp
#pragma once

// Arcane::Editor::CanvasPopupScope -- the node-canvas popup rule, named.
//
// imgui-node-editor draws inside a TRANSFORMED coordinate space (pan and zoom),
// and an ImGui popup positions itself in screen space. Opening one from inside a
// canvas without leaving that space strands or misplaces it. ed::Suspend() exits
// the canvas's space and ed::Resume() re-enters it, which is why every canvas
// popup must be bracketed.
//
// This exists because the rule was FOLKLORE and was already got wrong in writing:
// a comment at the ConstColor node asserted that "popups cannot open inside the
// canvas" while three working Suspend/Resume brackets sat above it in the same
// file. One named type with the reason attached is cheaper than the next person
// re-deriving it wrongly.
//
// Its own header rather than part of EditorWidgets: that vocabulary includes only
// imgui.h plus Astra::Range, and pulling imgui_node_editor.h into it would couple
// every editor widget to the node editor.

#include <imgui_node_editor.h>

namespace Arcane::Editor
{
    namespace ed = ax::NodeEditor;

    struct CanvasPopupScope
    {
        CanvasPopupScope()  { ed::Suspend(); }
        ~CanvasPopupScope() { ed::Resume();  }
        CanvasPopupScope(const CanvasPopupScope&)            = delete;
        CanvasPopupScope& operator=(const CanvasPopupScope&) = delete;
    };
}
```

- [ ] **Step 2: Run GenerateProjects (a NEW FILE was added)**

```powershell
cd D:\dev\starworks\Gacha\Arcane
.\GenerateProjects.bat
```

Expected: it regenerates `Arcane.slnx`. **Skipping this silently leaves the header
uncompiled** — harmless for a header-only type, but the next task adds a `.cpp` and
the same run covers both, so do it now and confirm it succeeds.

- [ ] **Step 3: Adopt it at the three existing sites**

In `ShaderEditorDocument.cpp`, add `#include "CanvasPopupScope.hpp"` to the local
include group (alphabetically, immediately before `#include "EditorTheme.hpp"`).

Then at each of the three bracket pairs, replace the explicit calls with the scope.
The pattern, using the first pair as the worked example — `ed::Suspend();` on its
own line becomes:

```cpp
        const CanvasPopupScope canvasPopup;   // ed::Suspend/Resume, see the header
```

and the matching `ed::Resume();` is DELETED, because the dtor now runs it at the end
of the enclosing scope. **Check each site's braces before deleting:** if the
`Suspend`/`Resume` pair does not span exactly one scope, leave that site alone and
say so in the task report rather than restructuring control flow to fit the RAII.
This task is allowed to convert zero, one, two or three sites; converting fewer is
not a failure, and the new type still earns its place from Task 7's use.

- [ ] **Step 4: Build and confirm the editor relinked**

Command as in Task 1 Step 3, then:

```powershell
Get-Item D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe |
    Select-Object LastWriteTime
```

Expected: `Build succeeded`, and a timestamp from this build.

- [ ] **Step 5: Run the editor tests**

```powershell
cd D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]"
```

Expected: `All tests passed` — `ShaderEditorDocument.cpp` compiles into
`ArcaneTests.exe`, so its graph-canvas cases cover this refactor.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/CanvasPopupScope.hpp Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp
git commit -m "refactor(arcane): name the node-canvas popup rule

ed::Suspend/Resume appeared in ShaderEditorDocument.cpp and nowhere else in the
editor, which is how a comment at the ConstColor node came to assert that popups
cannot open inside the canvas while three working brackets sat above it in the same
file. CanvasPopupScope is that rule with the reason attached.

Its own header rather than part of EditorWidgets: that vocabulary includes only
imgui.h plus Astra::Range, and pulling in imgui_node_editor.h would couple every
editor widget to the node editor.

Behaviour-preserving -- the dtor runs the same Resume the deleted line did."
```

**Desk-check items:** the three converted sites still open their popups correctly —
right-click menus in the node canvas appear in the right place, unclipped, and
panning while one is open does not strand it.

---

### Task 5: The dense picker widget

**Files:**
- Create: `Arcane/ArcaneEditor/src/ColorPickerPopup.hpp`
- Create: `Arcane/ArcaneEditor/src/ColorPickerPopup.cpp`
- Modify: `Arcane/ArcaneEditor/src/EditorWidgets.hpp` — DELETE the `ColorField4`,
  `ColorDisplayFromLinear` and `ColorLinearFromDisplay` declarations; KEEP
  `SrgbToLinear` / `LinearToSrgb` and add the cross-reference comment the Global
  Constraints require
- Modify: `Arcane/ArcaneEditor/src/EditorWidgets.cpp` — DELETE those three
  definitions; KEEP the two conversions

**Interfaces:**
- Consumes: `SrgbToLinear`, `LinearToSrgb` (from `EditorWidgets.hpp`).
- Produces, and Tasks 6-7 call exactly these:
  - `ImGuiID ColorPopupId(const char* id)`
  - `bool ColorSwatchButton(const char* id, const float linear[4], ImVec2 size = ImVec2(0, 0))`
  - `bool ColorPopupBody(float linear[4], const float original[4], bool hdr)`

- [ ] **Step 1: Write the header**

```cpp
#pragma once

// Arcane::Editor -- THE colour picker for the editor. UE-shaped: one dense panel,
// no tabs, every colour space named on screen.
//
// THE DEFECT THIS EXISTS TO FIX is not a wrong curve -- the curve is correct. It is
// that no number on screen ever said which space it was in. That ambiguity produced
// a sanity check whose own expected output (128 -> 186) was computed with the
// pow(1/2.2) the work forbade; the true answer is 188.
//
// SPACE CONTRACT, and it is the whole design:
//   * STORAGE is linear. Callers pass linear and get linear back. Always.
//   * The four inline channel boxes at a property row show LINEAR, unconverted.
//   * ImGui's ColorPicker4 is handed sRGB-ENCODED values, and its rows are labelled
//     sRGB. This is forced, not chosen: the SV cursor position and picking response
//     derive from HSV of the buffer, so linear values put display 0.735-1.0 across
//     half the picking area and crush the whole shadow range into a sliver. Its
//     alpha-bar tint (imgui_widgets.cpp:350) and side preview (:397) read the buffer
//     too and render dark. Every colour picker works in gamma space for this reason.
//   * Both hexes are shown, always, each labelled. Better than UE, which switches
//     ONE field between modes (SColorPicker.cpp:378-413) and so still lets a reader
//     see the wrong number.
//   * SWATCHES ARE ALWAYS ENCODED. imgui.hlsl states its own contract -- "vertex
//     colors ... are display-referred; no linearization" -- so a swatch filled with
//     a raw linear value renders too dark. That is the original tint defect one
//     layer up. There is no toggle: UE's sRGB Preview checkbox solves this same
//     problem, but it is only needed because UE shows no linear/sRGB pair
//     numerically. We do, so the un-encoded view has no use.
//   * ALPHA IS NEVER ENCODED. Coverage, not colour.
//
// UNDO IS THE CALLER'S. These three functions report; they never touch the
// CommandStack. Bracket with EditGesture::BeginOnPopupOpen / EndOnPopupClose on
// ColorPopupId(id) -- the activation pair does NOT work here, because ImGui only
// lends its ActiveId to popups IT opened (EditGesture.hpp's ShouldClosePopup note).

#include <imgui.h>

namespace Arcane::Editor
{
    // The popup id for a site. Stable for a given `id` string within a window, and
    // the same value to pass to ImGui::OpenPopup (imgui.h:868's ImGuiID overload)
    // and to the EditGesture popup pair.
    [[nodiscard]] ImGuiID ColorPopupId(const char* id);

    // The property-row swatch. Fills with the ENCODED colour (see the contract
    // above) and returns true when clicked, so the caller opens the popup. Default
    // size follows the current frame height, square.
    [[nodiscard]] bool ColorSwatchButton(const char* id, const float linear[4],
                                         ImVec2 size = ImVec2(0, 0));

    // The dense popup body. Call INSIDE an already-open popup window.
    //
    // `linear` is read and written in linear. `original` is the colour as it was
    // when the popup opened, for the Old/New pair -- the caller latches it once on
    // open. `hdr` skips the sRGB encode and hides both hex rows, for values that may
    // exceed 1 (a ConstColor node feeds raw shader maths); the SV response is poor
    // for those, which is inherent and equally true in UE.
    //
    // Returns true only on frames ImGui reported a change.
    [[nodiscard]] bool ColorPopupBody(float linear[4], const float original[4], bool hdr);
}
```

- [ ] **Step 2: Write the implementation**

```cpp
#include "ColorPickerPopup.hpp"

#include "EditorWidgets.hpp"   // SrgbToLinear / LinearToSrgb

#include <cstdio>
#include <cstring>

namespace Arcane::Editor
{
    namespace
    {
        // Encode for display. RGB converts, alpha never does.
        void EncodeForDisplay(const float lin[4], bool hdr, float out[4]) noexcept
        {
            if (hdr)
            {
                for (int i = 0; i < 4; ++i) out[i] = lin[i];
                return;
            }
            out[0] = LinearToSrgb(lin[0]);
            out[1] = LinearToSrgb(lin[1]);
            out[2] = LinearToSrgb(lin[2]);
            out[3] = lin[3];
        }

        void DecodeFromDisplay(const float disp[4], bool hdr, float out[4]) noexcept
        {
            if (hdr)
            {
                for (int i = 0; i < 4; ++i) out[i] = disp[i];
                return;
            }
            out[0] = SrgbToLinear(disp[0]);
            out[1] = SrgbToLinear(disp[1]);
            out[2] = SrgbToLinear(disp[2]);
            out[3] = disp[3];
        }

        ImVec4 SwatchColor(const float lin[4]) noexcept
        {
            // ALWAYS encoded -- imgui.hlsl's colours are display-referred.
            return ImVec4(LinearToSrgb(lin[0]), LinearToSrgb(lin[1]),
                          LinearToSrgb(lin[2]), lin[3]);
        }

        void HexOf(const float c[4], char out[10]) noexcept
        {
            auto b = [](float v) -> int
            {
                const float s = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
                return static_cast<int>(s * 255.0f + 0.5f);
            };
            std::snprintf(out, 10, "%02X%02X%02X%02X", b(c[0]), b(c[1]), b(c[2]), b(c[3]));
        }

        // Returns true when `text` parsed as RRGGBB or RRGGBBAA. Alpha defaults to
        // the incoming value so a 6-digit paste does not silently clear it.
        bool ParseHex(const char* text, float out[4]) noexcept
        {
            unsigned r = 0, g = 0, b = 0, a = 255;
            const std::size_t n = std::strlen(text);
            if (n == 6)
            {
                if (std::sscanf(text, "%2x%2x%2x", &r, &g, &b) != 3) return false;
            }
            else if (n == 8)
            {
                if (std::sscanf(text, "%2x%2x%2x%2x", &r, &g, &b, &a) != 4) return false;
            }
            else
            {
                return false;
            }
            out[0] = r / 255.0f; out[1] = g / 255.0f;
            out[2] = b / 255.0f; out[3] = a / 255.0f;
            return true;
        }

        // One labelled hex row. `encode` selects which space the field speaks.
        bool HexRow(const char* label, float linear[4], bool encodeSrgb)
        {
            float shown[4];
            if (encodeSrgb) EncodeForDisplay(linear, /*hdr*/ false, shown);
            else            std::memcpy(shown, linear, sizeof(shown));

            char buf[10];
            HexOf(shown, buf);

            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            const bool committed = ImGui::InputText(label, buf, sizeof(buf),
                                                    ImGuiInputTextFlags_CharsHexadecimal
                                                    | ImGuiInputTextFlags_CharsUppercase
                                                    | ImGuiInputTextFlags_EnterReturnsTrue);
            if (!committed)
                return false;

            float parsed[4];
            if (!ParseHex(buf, parsed))
                return false;          // garbage typed -- reverts on the next frame's reseed

            if (encodeSrgb) DecodeFromDisplay(parsed, /*hdr*/ false, linear);
            else            std::memcpy(linear, parsed, sizeof(parsed));
            return true;
        }
    }

    ImGuiID ColorPopupId(const char* id)
    {
        return ImGui::GetID(id);
    }

    bool ColorSwatchButton(const char* id, const float linear[4], ImVec2 size)
    {
        if (size.x <= 0.0f) size.x = ImGui::GetFrameHeight();
        if (size.y <= 0.0f) size.y = ImGui::GetFrameHeight();
        // AlphaPreviewHalf so a translucent colour is legible; NoTooltip because the
        // popup is one click away and a tooltip over a button that opens it is noise.
        return ImGui::ColorButton(id, SwatchColor(linear),
                                  ImGuiColorEditFlags_AlphaPreviewHalf
                                  | ImGuiColorEditFlags_NoTooltip,
                                  size);
    }

    bool ColorPopupBody(float linear[4], const float original[4], bool hdr)
    {
        bool changed = false;

        // ---- Old / New, drawn by US -------------------------------------------
        // ImGui's own side preview reads the buffer and cannot be re-encoded from
        // outside, so it would be wrong per the space contract. NoSidePreview below.
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Old");
        ImGui::ColorButton("##old", SwatchColor(original),
                           ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(ImGui::GetFontSize() * 4.0f, ImGui::GetFontSize() * 1.5f));
        ImGui::TextUnformatted("New");
        ImGui::ColorButton("##new", SwatchColor(linear),
                           ImGuiColorEditFlags_AlphaPreviewHalf | ImGuiColorEditFlags_NoTooltip,
                           ImVec2(ImGui::GetFontSize() * 4.0f, ImGui::GetFontSize() * 1.5f));
        ImGui::EndGroup();
        ImGui::SameLine();

        // ---- the picker itself, on ENCODED values ------------------------------
        float display[4];
        EncodeForDisplay(linear, hdr, display);

        const ImGuiColorEditFlags pickerFlags =
            (hdr ? (ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
                 : ImGuiColorEditFlags_Uint8)
            | ImGuiColorEditFlags_DisplayRGB
            | ImGuiColorEditFlags_DisplayHSV
            | ImGuiColorEditFlags_AlphaBar
            | ImGuiColorEditFlags_PickerHueWheel
            | ImGuiColorEditFlags_NoSidePreview
            | ImGuiColorEditFlags_NoLabel;

        ImGui::BeginGroup();
        if (!hdr)
            ImGui::TextUnformatted("sRGB");
        if (ImGui::ColorPicker4("##picker", display, pickerFlags))
        {
            DecodeFromDisplay(display, hdr, linear);
            changed = true;
        }
        ImGui::EndGroup();

        // ---- the LINEAR readout, ours -----------------------------------------
        ImGui::Separator();
        ImGui::TextUnformatted("Linear");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        // NoPicker/NoSmallPreview: this row is a numeric readout of storage, not a
        // second picker. Float display because storage IS float and this is the row
        // that never lies about the space.
        if (ImGui::ColorEdit4("##linear", linear,
                              ImGuiColorEditFlags_Float
                              | ImGuiColorEditFlags_NoPicker
                              | ImGuiColorEditFlags_NoSmallPreview
                              | ImGuiColorEditFlags_NoLabel
                              | (hdr ? ImGuiColorEditFlags_HDR : 0)))
            changed = true;

        // ---- both hexes, each labelled ----------------------------------------
        if (!hdr)
        {
            if (HexRow("Hex sRGB",   linear, /*encodeSrgb*/ true))  changed = true;
            if (HexRow("Hex Linear", linear, /*encodeSrgb*/ false)) changed = true;
        }

        return changed;
    }
}
```

- [ ] **Step 3: Retire the superseded helpers from `EditorWidgets`**

In `EditorWidgets.hpp`, DELETE the declarations of `ColorDisplayFromLinear`,
`ColorLinearFromDisplay` and `ColorField4` together with their comment blocks, and
replace the `SrgbToLinear`/`LinearToSrgb` comment's final paragraph with the
cross-reference the Global Constraints require:

```cpp
    // CURVE IS MIRRORED in two other places -- keep all three in step:
    //   shaders/tonemap.hlsl        (HLSL, branchless min form)
    //   Tests/src/TonemapTest.cpp   (CPU golden reference, branchless)
    [[nodiscard]] float SrgbToLinear(float srgb) noexcept;
    [[nodiscard]] float LinearToSrgb(float linear) noexcept;
```

In `EditorWidgets.cpp`, DELETE the definitions of those same three functions. Keep
`SrgbToLinear` and `LinearToSrgb` untouched.

Also delete the now-unused `#include <cmath>` ONLY IF nothing else in the file uses
it — grep first (`grep -n "std::pow\|std::sqrt\|std::fabs" EditorWidgets.cpp`); the
two conversions use `std::pow`, so it stays.

- [ ] **Step 4: Run GenerateProjects (TWO new files)**

```powershell
cd D:\dev\starworks\Gacha\Arcane
.\GenerateProjects.bat
```

**Mandatory.** `ColorPickerPopup.cpp` is a new translation unit; without this it is
not compiled and Tasks 6-7 fail to link with unresolved externals.

- [ ] **Step 5: Build**

Command as in Task 1 Step 3.

Expected: `Build succeeded`. Compile errors naming `ColorField4` are EXPECTED at this
point if any call site still references it — Tasks 6 and 7 remove those. If the build
fails only on `InspectorView.cpp` / `ShaderEditorDocument.cpp` referencing
`ColorField4`, that is the correct intermediate state; proceed to Task 6 and do not
commit Task 5 alone in that case. If it builds clean, commit now.

- [ ] **Step 6: Commit (only if Step 5 was clean)**

```bash
git add Arcane/ArcaneEditor/src/ColorPickerPopup.hpp Arcane/ArcaneEditor/src/ColorPickerPopup.cpp Arcane/ArcaneEditor/src/EditorWidgets.hpp Arcane/ArcaneEditor/src/EditorWidgets.cpp
git commit -m "feat(arcane): the dense colour picker, UE-shaped

One panel, no tabs, every colour space named. Storage stays linear; the four inline
boxes show linear unconverted; ColorPicker4 is handed sRGB-ENCODED values and its
rows are labelled sRGB; both hexes are shown at once, each labelled; and swatches
are ALWAYS encoded.

Encoding the picker buffer is forced, not chosen. The SV cursor and picking response
derive from HSV of the buffer, so linear values put display 0.735-1.0 across half the
picking area and crush the shadow range into a sliver; the alpha-bar tint and side
preview read it too and render dark. Every colour picker works in gamma space for
this reason.

Showing both hexes is deliberately better than UE, which switches ONE field between
modes and so still lets a reader see the wrong number. That ambiguity is the actual
defect this arc fixes -- it is what produced a sanity check whose own expected output
was computed with the gamma the work forbade.

Swatches always encoded because imgui.hlsl states its colours are display-referred:
a raw linear fill is the original tint defect one layer up. No sRGB-preview toggle --
UE needs one because it shows no linear/sRGB pair numerically; we do.

Retires ColorField4 and its two buffer converters. SrgbToLinear/LinearToSrgb stay and
now carry the mirror cross-reference."
```

**Desk-check items:** none yet — nothing opens it until Task 6.

---

### Task 6: Inspector adoption

**Files:**
- Modify: `Arcane/ArcaneEditor/src/InspectorView.cpp` — the `Multi()` arm and the
  `isColor` arm of `FieldKind::Vec4` (re-grep for `IsColorFieldName` to locate them;
  they were at `:636-653` and `:654-681` before this arc's earlier edits)

**Interfaces:**
- Consumes: `ColorPopupId`, `ColorSwatchButton`, `ColorPopupBody` (Task 5);
  `EditGesture::BeginOnPopupOpen`, `EditGesture::EndOnPopupClose` (Task 3).
- Produces: nothing.

- [ ] **Step 1: Add the include**

`InspectorView.cpp` already includes `EditorWidgets.hpp` (line 5). Add immediately
after it:

```cpp
#include "ColorPickerPopup.hpp"
```

- [ ] **Step 2: Replace the multi-select arm**

Replace the whole `if (Multi()) { ... break; }` block inside `case
Arcane::Editor::FieldKind::Vec4:` with:

```cpp
                        if (Multi())
                        {
                            // LINEAR floats, both here and in the popup. The 0-255
                            // sRGB display this replaces made one stored value read
                            // two ways depending on how many entities were selected;
                            // showing storage directly cannot drift.
                            const double cur[4]{ v.x, v.y, v.z, v.w };
                            double out = 0.0;
                            // Axis strips only on the NON-colour flavour: XYZW
                            // colours under RGBA channels would label the values as
                            // something they are not.
                            const int c = MultiScalarRow(widgetId.c_str(), 4, cur, MixedFor(f),
                                                         /*integral*/ false,
                                                         /*axisColors*/ !isColor, out);
                            if (c >= 0)
                            {
                                const float fv = static_cast<float>(out);
                                ApplyImmediate(rawName, instance, [&](void* d)
                                               { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) (*p)[c] = fv; });
                            }
                            break;
                        }
```

- [ ] **Step 3: Replace the single-select colour arm**

Replace the whole `if (isColor) { ... break; }` block with:

```cpp
                        if (isColor)
                        {
                            // Four LINEAR float boxes -- storage, unconverted -- plus
                            // a swatch that opens the dense popup. The swatch is ours
                            // (NoSmallPreview) so it can be sRGB-ENCODED: imgui.hlsl's
                            // colours are display-referred, so a raw linear fill
                            // renders too dark.
                            const std::string popupKey = widgetId + "##colorpopup";
                            const ImGuiID popupId = ColorPopupId(popupKey.c_str());

                            bool changed = ImGui::ColorEdit4(widgetId.c_str(), &v.x,
                                                             ImGuiColorEditFlags_Float
                                                             | ImGuiColorEditFlags_NoSmallPreview
                                                             | ImGuiColorEditFlags_NoPicker
                                                             | ImGuiColorEditFlags_NoOptions);
                            // The BOX row keeps the activation gesture it always had.
                            BeginGestureIfActivated(rawName, instance);

                            ImGui::SameLine();
                            if (ColorSwatchButton("##sw", &v.x))
                            {
                                // Latch the open-time colour for the popup's Old
                                // swatch. It MUST live in InspectorState, not on this
                                // visitor: the visitor is rebuilt per frame (it holds
                                // only a POINTER to the gesture state, see
                                // InspectorView.cpp:143), so a member here would reset
                                // every frame and the Old swatch would track New.
                                // One slot is enough -- only one colour popup can be
                                // open at a time.
                                *originalColor = v;
                                ImGui::OpenPopup(popupId);
                            }

                            // POPUP-lifetime gesture, NOT the activation pair: ImGui
                            // only lends its ActiveId to popups it opened itself, and
                            // this one is ours (EditGesture.hpp, ShouldClosePopup).
                            //
                            // The onOpened body is BeginGestureIfActivated's fan-out
                            // verbatim (InspectorView.cpp:192-194): one Begin + N
                            // SnapshotComponent + one Commit = one undo step, and the
                            // pending-commit slot stays empty because the before-state
                            // rides the transaction.
                            EditGesture::BeginOnPopupOpen(
                                stack, *gesture, popupId,
                                [&] { return "Edit " + typeName + "." + rawName; },
                                [&]
                                {
                                    ForEachTarget(instance,
                                                  [&](Astra::Entity e, void*)
                                                  { stack->SnapshotComponent(e, descriptor); });
                                    return std::function<void()>{};
                                });

                            if (ImGui::BeginPopup(popupKey.c_str()))
                            {
                                if (ColorPopupBody(&v.x, &originalColor->x, /*hdr*/ false))
                                    changed = true;
                                ImGui::EndPopup();
                            }
                            EditGesture::EndOnPopupClose(stack, *gesture, popupId);

                            if (changed)
                                ForEachTarget(instance, [&](Astra::Entity, void* d)
                                              { if (glm::vec4* p = f.GetPtr<glm::vec4>(d)) *p = v; });
                            break;
                        }
```

**Two supporting edits this needs, both exact:**

1. **Add the persistent slot to `InspectorState`**, in
   `Arcane/ArcaneEditor/src/EditorPanels.hpp`, immediately after the
   `EditGesture::GestureState gesture;` member at `:225`:

```cpp
        // The colour a picker popup was opened on, for its Old/New pair. Lives HERE
        // rather than on the Inspector's per-field visitor because that visitor is
        // rebuilt every frame (it holds only a pointer to `gesture` above), so a
        // member there would reset each frame and Old would track New. One slot is
        // enough: only one colour popup can be open at a time.
        glm::vec4 colorPopupOriginal{ 1.0f, 1.0f, 1.0f, 1.0f };
```

   If `EditorPanels.hpp` does not already include glm's vec4, add
   `#include <glm/vec4.hpp>` to its include block.

2. **Thread it into the visitor** in `InspectorView.cpp`, beside the existing
   `EditGesture::GestureState* gesture = nullptr;` at `:143`:

```cpp
            glm::vec4*                        originalColor = nullptr;
```

   and set it wherever `gesture` is assigned from the owning `InspectorState`
   (grep `->gesture =` / `.gesture` at the construction site and mirror it exactly).
   Same lifetime, same owner, same shape — deliberately, so there is one rule to
   remember rather than two.

The names used in the code above — `stack`, `*gesture`, `typeName`, `descriptor`,
`ForEachTarget`, `SnapshotComponent` — are the visitor's real members and helpers as
of `InspectorView.cpp:143-196`, not placeholders. The `onOpened` body is
`BeginGestureIfActivated`'s own fan-out copied verbatim from `:192-194`.

- [ ] **Step 4: Build and confirm the editor relinked**

Command as in Task 1 Step 3, then the `Get-Item ... ArcaneEditor.exe` check from
Task 4 Step 4.

- [ ] **Step 5: Commit**

```bash
git add Arcane/ArcaneEditor/src/InspectorView.cpp
git commit -m "feat(arcane): the Inspector's colour row opens the dense picker

Four LINEAR float boxes -- storage, unconverted -- plus an sRGB-ENCODED swatch that
opens the dense popup. Multi-select shows the same linear floats, so a stored value
no longer reads two ways depending on selection size.

Undo is bracketed on the POPUP'S LIFETIME, not on item activation. The activation
pair works for ImGui's own colour popup only because ColorEdit4 lends it the
picker's ActiveId, and this popup is ours -- so it gets no loan. The box row keeps
the activation gesture it always had."
```

**Desk-check items (the visual gate):**
1. A colour row shows four linear-float boxes plus a swatch; the swatch opens the
   popup and typing in a box still edits without it.
2. **Row width judgement:** four `0.578` boxes are not crowded at the default label
   split and do not clip at a narrow panel width. This decides whether swatch-only
   rows are needed instead.
3. **The SV square feels right** — dragging gives an even perceptual spread with
   usable shadow range, not everything crammed bright. This is the justification for
   encoding the buffer; if it feels wrong, that decision is wrong.
4. The alpha bar's tint and the Old/New swatches look like the colour, not darker.
5. `sRGB` rows and the `Linear` readout are both correct: linear `0.5` reads ~`0.735`
   in the sRGB row.
6. Both hex fields visible and labelled, disagreeing in the expected direction.
7. Pasting `C9A0DC` into `Hex sRGB` sets the sprite to that colour and it *looks*
   like that colour.
8. One `Ctrl+Z` undoes one whole popup session, not one step per frame.
9. Editing a channel box in the row is still one undo step per drag.
10. Opening and closing the popup without editing creates no undo step.
11. `Escape` and click-away both KEEP the edit and leave exactly one undo step. This
    differs from UE deliberately — verify it is the intended behaviour, not a bug.
12. Drag value to black and back up — hue is preserved.

---

### Task 7: Material params and the `ConstColor` node

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` — the
  `ParamWidget::ColorEdit` arm (was `:5825-5841` after this arc's earlier edits) and
  the `ConstColor` swatch block inside the node canvas (was `:4869-4890`). Re-grep
  for `ParamWidget::ColorEdit` and `GraphNodeType::ConstColor` before editing.

**Interfaces:**
- Consumes: everything from Tasks 3, 4 and 5.
- Produces: nothing.

- [ ] **Step 1: Add the include**

Add to the local include group, alphabetically (it will sit between
`CanvasPopupScope.hpp` from Task 4 and `EditorTheme.hpp`):

```cpp
#include "ColorPickerPopup.hpp"
```

- [ ] **Step 2: Replace the material param arm**

Replace the whole `case ParamWidget::ColorEdit:` body with:

```cpp
                case ParamWidget::ColorEdit:
                {
                    // Same shape as the Inspector row: linear float boxes plus an
                    // encoded swatch opening the dense popup. hdr = false because
                    // MatParamType::Color exists precisely so "the editor shows a
                    // color picker" (MaterialTypes.hpp:26) -- it IS a colour, and
                    // nothing declares one as HDR (ParamMeta carries only
                    // sliderMin/sliderMax, MaterialTypes.hpp:133-139). A param
                    // wanting an unclamped multiplier is a Float4.
                    const std::string popupKey = d.name + "##colorpopup";
                    const ImGuiID popupId = ColorPopupId(popupKey.c_str());

                    edited = ImGui::ColorEdit4(d.name.c_str(), value.f,
                                               ImGuiColorEditFlags_Float
                                               | ImGuiColorEditFlags_NoSmallPreview
                                               | ImGuiColorEditFlags_NoPicker
                                               | ImGuiColorEditFlags_NoOptions);
                    ImGui::SameLine();
                    if (ColorSwatchButton("##sw", value.f))
                    {
                        std::memcpy(m_colorPopupOriginal, value.f, sizeof(m_colorPopupOriginal));
                        ImGui::OpenPopup(popupId);
                    }
                    if (ImGui::BeginPopup(popupKey.c_str()))
                    {
                        if (ColorPopupBody(value.f, m_colorPopupOriginal, /*hdr*/ false))
                            edited = true;
                        ImGui::EndPopup();
                    }
                    break;
                }
```

`m_colorPopupOriginal` is a NEW `float[4]` member on `ShaderEditorDocument`. The
existing gesture bracket after the `switch` (which reads `edited`) is untouched and
still covers the box row; add the popup pair alongside it:

```cpp
            EditGesture::BeginOnPopupOpen(m_services.undo, m_gesture,
                                          ColorPopupId((d.name + "##colorpopup").c_str()),
                                          [&] { return std::string("Edit ") + d.name; },
                                          [&] { return std::function<void()>(); });
            EditGesture::EndOnPopupClose(m_services.undo, m_gesture,
                                         ColorPopupId((d.name + "##colorpopup").c_str()));
```

Place these immediately after the existing `EditGesture::BeginOnActivate(...)` /
`EndOnDeactivate(...)` calls that follow the `switch`, and only for the
`ParamWidget::ColorEdit` case — guard with the same `WidgetFor(d.type) ==
ParamWidget::ColorEdit` test the switch used.

- [ ] **Step 3: Replace the `ConstColor` node block**

Replace the whole `if (n.type == Arcane::GraphNodeType::ConstColor) { ... }` block
with:

```cpp
                if (n.type == Arcane::GraphNodeType::ConstColor)
                {
                    ImGui::SameLine();
                    // hdr = true: a ConstColor feeds raw shader maths and may exceed
                    // 1, where an sRGB encode is meaningless and a hex is a clamped
                    // lie -- so the popup shows linear floats and no hex rows. The
                    // SV response is poor for HDR values; inherent, and equally true
                    // in UE.
                    const CanvasPopupScope canvasPopup;   // popups must leave the canvas space
                    const ImGuiID popupId = ColorPopupId("##constcolorpopup");
                    if (ColorSwatchButton("##swatch", n.value, ImVec2(18, 18)))
                    {
                        std::memcpy(m_colorPopupOriginal, n.value, sizeof(m_colorPopupOriginal));
                        ImGui::OpenPopup(popupId);
                    }
                    EditGesture::BeginOnPopupOpen(m_services.undo, m_gesture, popupId,
                                                  [&] { return std::string("Edit Color"); },
                                                  [&] { return std::function<void()>(); });
                    if (ImGui::BeginPopup("##constcolorpopup"))
                    {
                        if (ColorPopupBody(n.value, m_colorPopupOriginal, /*hdr*/ true))
                            valueEdited();
                        ImGui::EndPopup();
                    }
                    EditGesture::EndOnPopupClose(m_services.undo, m_gesture, popupId);
                }
```

Note the swatch is now ENCODED even here, because `ColorSwatchButton` always
encodes — correct: the swatch is a display object regardless of what the value feeds.

- [ ] **Step 4: Build and confirm the editor relinked**

Command as in Task 1 Step 3, then the `Get-Item ... ArcaneEditor.exe` check.

- [ ] **Step 5: Run the editor tests**

```powershell
cd D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor]"
```

Expected: `All tests passed`.

- [ ] **Step 6: Commit**

```bash
git add Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp
git commit -m "feat(arcane): material params and the ConstColor node use the dense picker

Both sites get the same shape as the Inspector row: linear float boxes plus an
encoded swatch opening the dense popup, with undo bracketed on popup lifetime.

ConstColor passes hdr = true -- it feeds raw shader maths and may exceed 1, where an
sRGB encode is meaningless and a hex would be a clamped lie, so the popup shows
linear floats and hides both hex rows. Its swatch is still encoded, because a swatch
is a display object whatever the value feeds. The canvas popup is bracketed by
CanvasPopupScope rather than by open-coded Suspend/Resume."
```

**Desk-check items:**
1. A `.arcmat` Color param row shows linear float boxes plus a swatch; the swatch
   opens the same popup the Inspector uses, and one `Ctrl+Z` undoes a drag.
2. The `ConstColor` popup opens **above the canvas**, unclipped, not offset, and
   panning the canvas while it is open does not strand it.
3. The `ConstColor` popup shows linear floats and **no** hex rows.
4. Its `DragFloat4` still works and still shows the same numbers.
5. The material preview and the graph regenerate live while dragging.

---

### Task 8: Per-channel gradient bars (cosmetic tier — CUT THIS IF THE ARC RUNS LONG)

**Files:**
- Modify: `Arcane/ArcaneEditor/src/ColorPickerPopup.cpp` (add a helper, call it
  under the Linear readout)

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing.

The spec marks this the cuttable tier. It is purely decorative: a gradient strip
under each channel showing that channel's axis, as UE draws. **If Tasks 1-7 landed
and the gate passed, stopping here is a legitimate end to the arc** — say so in the
report rather than implementing it reflexively.

- [ ] **Step 1: Add the bar helper**

In the anonymous namespace of `ColorPickerPopup.cpp`:

```cpp
        // A channel's axis as a gradient under its box: the colour with that one
        // channel swept 0..1, everything else held. Decorative only -- no
        // interaction, drawn AFTER the row so it cannot move layout.
        void ChannelAxisBar(const float linear[4], int channel)
        {
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const float  h   = ImMax(2.0f, ImGui::GetFontSize() * 0.18f);
            const ImVec2 p0(min.x, max.y);
            const ImVec2 p1(max.x, max.y + h);

            float lo[4], hi[4];
            for (int i = 0; i < 4; ++i) { lo[i] = linear[i]; hi[i] = linear[i]; }
            lo[channel] = 0.0f;
            hi[channel] = 1.0f;
            lo[3] = hi[3] = 1.0f;   // the bar is opaque; alpha is not the axis here

            const ImU32 c0 = ImGui::ColorConvertFloat4ToU32(SwatchColor(lo));
            const ImU32 c1 = ImGui::ColorConvertFloat4ToU32(SwatchColor(hi));
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(p0, p1, c0, c1, c1, c0);
        }
```

- [ ] **Step 2: Call it under the Linear readout**

Immediately after the `ColorEdit4("##linear", ...)` call in `ColorPopupBody`, add:

```cpp
        // One bar per RGB channel under the linear row. Alpha gets none: its axis is
        // the checkerboard alpha bar the picker already draws.
        for (int ch = 0; ch < 3; ++ch)
            ChannelAxisBar(linear, ch);
```

Note this draws all three bars under the SAME item rect, since `ColorEdit4` submits
one grouped item. If they overlap rather than sitting under their own box, the row
must be split into three `DragFloat` calls first — **if that is what you find, STOP
and report it instead of restructuring the readout**, because splitting it forfeits
the single-widget hue-restore guarantee the spec's Decision 7 depends on.

- [ ] **Step 3: Build, then eyeball**

Command as in Task 1 Step 3, plus the relink check.

- [ ] **Step 4: Commit**

```bash
git add Arcane/ArcaneEditor/src/ColorPickerPopup.cpp
git commit -m "feat(arcane): per-channel axis gradients under the linear readout

The cosmetic tier of the dense picker, as UE draws: each channel's axis swept 0..1
under its box. Decorative only, drawn after the row so it cannot move layout, and
encoded like every other swatch."
```

**Desk-check items:** the bars sit under their channels, run the right colours, and
do not shift the row's layout or overlap the hex fields.

---

## Self-Review

**Spec coverage:**

| spec item | task |
|---|---|
| Part 0 D1 (true sRGB tonemap, first, golden test updated, no visual gate) | 1 |
| Part 1 D2 (popup boundary in pure core) | 2 |
| Part 1 D3 (popup id is the ownership token) | 2 (`ShouldClosePopup`'s guard), 3 (parks it) |
| Part 1 D4 (abandonment = not open; ScopeGuard still the backstop) | 2, 3 |
| Part 1 D5 (canvas popup rule named) | 4 |
| Part 2 D6 (ColorPicker4 gets sRGB-encoded values) | 5 |
| Part 2 D7 (one ColorPicker4 call, hue restore) | 5 |
| Part 2 D8 (rows keep four linear-float boxes + swatch) | 6, 7 |
| Part 2 D9 (dense labelled readout: sRGB rows, Linear row, both hexes) | 5 |
| Part 2 D10 (swatches always encoded, no toggle) | 5 (`SwatchColor`) |
| Part 2 D11 (we draw Old/New, `NoSidePreview`) | 5 |
| Part 2 D12 (live commit, no OK/Cancel) | 2 (no Cancel path), 6 (gate item 11) |
| Part 2 D13 (`hdr` keeps floats, drops hexes) | 5 (`hdr` branches), 7 (`ConstColor` passes true) |
| Part 2 D14 (wheel only) | 5 (`PickerHueWheel`, no selection UI) |
| Part 2 D15 (no persisted preference, no settings handler) | none needed — nothing to persist, as designed |
| Part 2 D16 (gradient bars, cuttable) | 8 |
| reverts table (`ColorField4` retired, conversions survive) | 5 |

**Placeholder scan:** one deliberate exception. Task 8 Step 2 names the
grouped-item-rect risk and instructs a STOP rather than a workaround, because the
workaround would forfeit Decision 7's hue-restore guarantee. Everything else carries
complete code, exact commands and expected output.

An earlier revision of this plan asked Task 6's implementer to invent a
`SnapshotTargets` helper and to hang the Old-swatch colour off the Inspector's
visitor. Both were wrong and are now resolved against the source: the fan-out is
copied verbatim from `BeginGestureIfActivated` (`InspectorView.cpp:192-194`), and the
colour slot moved to `InspectorState` because the visitor is rebuilt every frame
(`:143` holds only a POINTER to the gesture state) — a member on it would have reset
each frame and made Old track New. That bug would have looked like a picker defect.

**Type consistency:** `ColorPopupId(const char*) -> ImGuiID`,
`ColorSwatchButton(const char*, const float[4], ImVec2) -> bool`,
`ColorPopupBody(float[4], const float[4], bool) -> bool`,
`ShouldClosePopup(const Slots&, std::uint32_t, bool, bool) -> bool`,
`BeginOnPopupOpen(CommandStack*, GestureState&, std::uint32_t, FunctionRef<std::string()>, FunctionRef<std::function<void()>()>)`,
`EndOnPopupClose(CommandStack*, GestureState&, std::uint32_t)` are spelled identically
in every declaration, definition, Interfaces block and call. `MatParamValue::f` is
`float[4]`; `GraphNode::value` is `float[4]` (both bind to `float[4]` parameters
directly). The Old-swatch slot differs per site by design and the call syntax follows
it: Task 6 uses `glm::vec4 InspectorState::colorPopupOriginal` reached through the
visitor's `glm::vec4* originalColor`, so it passes `&originalColor->x`; Task 7 uses
`float m_colorPopupOriginal[4]` directly on `ShaderEditorDocument` (which already owns
its own `m_gesture` at `ShaderEditorDocument.hpp:579`), so it passes it unqualified.
Undo handles likewise differ: the Inspector visitor exposes `stack` and
`EditGesture::GestureState* gesture`, while `ShaderEditorDocument` uses
`m_services.undo` and `m_gesture` — the plan's code uses each file's own names.

**Known ordering hazard:** Task 5 deletes `ColorField4` while Tasks 6-7 still call it,
so the tree does not build clean between Task 5 Step 3 and Task 6 Step 4. Task 5
Step 5 says so explicitly and defers its commit in that case. If you prefer a
green-at-every-commit history, do Task 5's deletions as part of Task 7 instead.
