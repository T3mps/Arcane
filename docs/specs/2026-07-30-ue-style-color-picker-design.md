# Colour Pipeline Correction + UE-Style Dense Picker — Design

**Status:** proposed, 2026-07-30 (rev 3)
**Supersedes:** `2026-07-30-editor-popup-gesture-and-picker-layouts-design.md`
(tabbed geometry selection).
**Revises shipped work:** `tonemap.hlsl`, and partially `77fabd6b`, `0a571b0e`,
`deed9abe`.

## Problem

Three defects, one root cause: **the engine is inconsistent about which colour
space a value is in, and nothing in the UI ever says.**

1. **The display encode is the wrong curve.** `tonemap.hlsl:33` does
   `pow(ACESFilmic(x), 1.0/2.2)`. Textures load as `SRGBA8_UNORM`, so hardware
   decodes input with the *true* piecewise sRGB curve. Input and output therefore
   use different transfer functions. The 2.2 exists only to byte-match the
   **retired** Love2D prototype's `post_process.glsl`, and byte-identity is
   explicitly no longer a goal.
2. **Colour swatches in the editor are wrong.** `imgui.hlsl` states its own
   contract: *"Vertex colors and the RGBA8 (non-sRGB) font atlas are
   display-referred; no linearization -- ImGui draws POST-tonemap into the
   display-referred backbuffer."* A swatch filled with a raw linear value feeds a
   scene-referred number into a display-referred channel, so it renders too dark.
   This is structurally the same error as the original tint defect, one layer up.
3. **No number on screen says which space it is in.** That ambiguity already cost
   real time: a curve-sanity check written to catch a wrong transfer carried
   `128 -> 186` as its own expected output, computed with the very `pow(1/2.2)`
   the work forbade. True answer 188. A widget that named its spaces would have
   made that unwriteable.

### And a fourth: one gesture boundary where four are needed

`EditGesture` models exactly ONE edit boundary — *item activated -> item
deactivated*. Four shapes are in play; only that one is named and tested:

| boundary shape | owner today | status |
|---|---|---|
| item activate -> deactivate | `EditGesture` | named, pure core unit-tested |
| single-shot commit | `StableTextEdit`, `ApplyImmediate` | named, untested |
| canvas-scoped popup | `ed::Suspend()/Resume()`, inline, 3 sites in 1 file | folklore |
| **popup lifetime** | **nothing** | **missing** |

Popup edits land inside undo TODAY only because ImGui *lends* us its popup's
active id:

```cpp
// When picker is being actively used, use its active id so IsItemActive() will function on ColorEdit4().
if (picker_active_window && g.ActiveId != 0 && g.ActiveIdWindow == picker_active_window)
    g.LastItemData.ID = g.ActiveId;
```

`picker_active_window` is set only when ImGui itself opened the popup. A dense
custom panel means we open our own, the loan ends, and popup edits fall outside
undo. **The hardening is a precondition, not a companion.**

This also means the `NoPicker` comment retired in `77fabd6b` was **half right** —
false for ImGui's built-in popup, true for a hand-rolled one. That commit message
overstated its wrongness.

## What UE actually does — verified from vendored 5.6 source

- **True piecewise sRGB for the display encode**, `GammaCorrectionCommon.ush:16-44`:
  `min(lin * 12.92, pow(max(lin, 0.00313067), 1.0/2.4) * 1.055 - 0.055)`.
  Character-for-character the curve shipped in `8f2e7aac`.
- **Encodes in the shader, not via an sRGB backbuffer** — necessarily, since UE
  supports output devices sRGB hardware cannot express (Rec709, ST2084/PQ). Our
  manual encode is the same, more general, choice.
- **UI composites after post-processing**, as ours does. Not a divergence.
- **Slate carries a `SOURCE_IN_LINEAR_SPACE` shader permutation**
  (`SlateElementPixelShader.usf:56,515`) so a linear-authored colour can be
  encoded at draw time. We have no equivalent; `imgui.hlsl`'s `ps_main` is
  `input.col * texture`, no transfer.
- **`sRGB Preview` is preview-only** — tooltip: *"the preview swatch uses sRGB
  encoding to correct the colors for display."*
- **ONE mode-switched hex field**, dropdown + textbox (`SColorPicker.cpp:378-413`),
  label toggling between `Hex sRGB` and `Hex Linear`. Not two simultaneous fields.
- **Preferences persist via `GConfig`** in `GEditorPerProjectIni`.
- **The Details-panel row is a swatch only** — `SColorBlock`
  (`ColorStructCustomization.cpp:134`).

Cross-check that UE's curve is ours: the reference screenshot's `R 0.57758` with
`Hex Linear 931B12FF` gives `0x93 = 147`, `147/255 = 0.576`; and
`LinearToSrgb(0.57758) * 255 = 200.0 = 0xC8`, matching `Hex sRGB C85B4BFF`.

## Part 0 — Tonemap correction (DO THIS FIRST)

1. **Replace `pow(1.0/2.2)` in `tonemap.hlsl:33` with the true piecewise sRGB
   encode**, matching what `SRGBA8_UNORM` applies on input and what UE uses on
   output. After this the engine is symmetric: true sRGB in, true sRGB out.

   **This is NOT a behaviour-preserving refactor.** Rendered output changes by
   design. Midtones shift under 1/255 (linear 0.5: 0.7297 -> 0.7354), but deep
   shadows change materially (linear 0.002: 0.059 -> 0.026, roughly 8/255) because
   the 2.2 power lifts them. Everything on screen gets slightly deeper shadows.
   That is the fix, not a regression.

   **Ordering:** first, before any picker work, so every later encode target
   agrees with the renderer.

   **`TonemapTest.cpp:25` must be updated in the same commit.** It is a golden
   test mirroring the curve CPU-side (`std::pow(AcesFilmic(x), 1.0f/2.2f)`) and
   asserting GPU output within +/-2 bytes. Updating it is not "adding a test" —
   it is keeping an existing one honest, and it gives this change objective
   verification for free.

   **No visual gate** (user's call, 2026-07-30): nothing ships on this engine but
   test projects, and the golden test plus the user's eye are the check. Both sides
   of the mirror get a comment pointing at the other, since a CPU/GPU pair like
   this is a drift risk — same hazard as the `kPxRange`/`kAtlasSize` pair between
   `msdf.hlsl` and `TextSystem.cpp`.

## Part 1 — Gesture hardening (precondition for Part 2)

2. **Add a popup-lifetime boundary to `EditGesture`, a peer of `ScopeGuard`.**
   Opens when the popup opens, closes on the frame it is gone. Honours the two
   existing invariants unchanged: only the opener may close, and abandonment
   **commits** rather than stranding. Decision logic in the PURE CORE beside
   `EvaluateEnd` / `ShouldCloseAbandoned` / `ShouldCloseStaleOnActivate` — plain
   integers and bools, no `imgui.h`.

   A new shape rather than reusing `BeginOnActivate`, because a popup's edits are
   made by *foreign widgets we do not submit*. There is no item of ours to observe
   activating. The popup's lifetime is the honest boundary, and we control it.

3. **The popup id is the ownership token.** `Slots::item` parks the popup id
   instead of a widget id. ImGui ids share one space, so the existing ownership
   guard works verbatim.

4. **Abandonment means "no longer open."** Click-away, `Escape`, host-window
   collapse and document close are all reasons a popup vanishes that no widget
   reports. Close keys on `IsPopupOpen(id)` going false; `ScopeGuard` stays the
   backstop for scope exit. Both paths commit.

5. **Lift the canvas-popup rule out of folklore.** Give `ed::Suspend()`/
   `ed::Resume()` a named RAII helper in the widget layer. Not extraction for
   reuse — recording a fact already got wrong in writing, where a comment at the
   `ConstColor` node asserted canvas popups were impossible while three working
   examples sat above it in the same file. `ConstColor` is the one site on both
   gaps at once, which is what puts this in scope.

## Part 2 — The picker

6. **`ColorPicker4` receives sRGB-ENCODED values, not linear.** This is forced by
   how ImGui draws and hit-tests, verified in `imgui_widgets.cpp`:

   - The hue strip and the SV gradient fill use **fixed** display-referred
     constants (`IM_COL32(255,0,0,...)`, `hue_color32` from
     `ColorConvertHSVtoRGB(H,1,1)`), so they look correct whatever we pass.
   - But the **alpha bar tint** (`:350`) and the **side preview** (`:397`) read our
     actual values and render too dark with linear input.
   - Decisively, the **SV cursor position and picking response** derive from HSV of
     our buffer. With linear values the SV square maps linear 0.5-1.0 across half
     its area — display 0.735-1.0 — so most of the picking surface is bright and
     the entire shadow range is crushed into a sliver. Every colour picker operates
     in gamma space for this reason. Honest numbers here would buy an unusable
     picker.

   So the buffer handed to ImGui is `LinearToSrgb(stored)`, and the result is
   decoded back on change. Honesty comes from **labelling**, per Decision 9 — not
   from the buffer's space.

7. **Still exactly ONE `ColorPicker4` call**, `Uint8 | DisplayRGB | DisplayHSV`,
   `DisplayHex` omitted (we draw our own hex rows), `NoSidePreview` (Decision 11).

   One call is a correctness requirement, not economy. `ColorEditRestoreHS` returns
   early unless `ColorEditSavedID == ColorEditCurrentID`, and `ColorEditCurrentID`
   is set only "while inside of the **parent-most** ColorEdit4/ColorPicker4"
   (`imgui_internal.h:2692-2696`). Hue and saturation are undefined at greys and
   blacks and are restored from that slot; splitting the rows into sibling widgets
   gives them a different parent-most id and silently loses hue.

8. **Property rows keep four channel boxes plus a swatch** — a deliberate
   divergence from UE's swatch-only row, because typing a channel without opening a
   popup is worth the width.

   **The row's four boxes show LINEAR floats** — the stored value, with **no
   conversion at all** in the inline path: `ColorEdit4` with
   `Float | NoSmallPreview | NoPicker | NoOptions` straight over storage.
   `NoSmallPreview` because we draw the swatch, so it can be encoded (Decision 10)
   and can carry the popup click.

   Known tension, flagged not buried: `Float` at `%.3f` is what was rejected on
   2026-07-29 for crowding. Four `0.578` boxes need ~40 px more than four `201`s.
   Accepted, judged at the gate; fallback is swatch-only rows, one line per site.

9. **The popup is a dense labelled readout — this is the actual fix for the
   ambiguity.** Visible simultaneously:

   - ImGui's RGB and HSV rows, under a heading naming them **sRGB** (0-255).
   - Our own **Linear** float readout of the stored value.
   - `Hex sRGB` and `Hex Linear`, both labelled, both editable.

   Two labelled hex fields is deliberately **better than UE**, whose single
   mode-switched field still lets a reader see the wrong number because only one is
   on screen. `Hex sRGB` parses through `SrgbToLinear`; `Hex Linear` parses
   straight to linear. A hex from a design tool goes in the sRGB field, which is
   what design tools emit.

   The row shows linear and the popup shows both, named. A user who types `0.5` in
   the row and opens the popup reads `Linear 0.500 / sRGB 0.735` — coherent and
   self-teaching rather than ambiguous.

10. **Swatches are ALWAYS encoded. No toggle.** Every swatch we draw fills with
    `LinearToSrgb(colour)`, never the raw linear value.

    UE's `sRGB Preview` checkbox is **not** pointless — it is UE's answer to this
    same problem, needed because UE authors in linear numbers and must offer some
    way to see true appearance. We do not need the toggle because we always show
    the corrected swatch *and* name both spaces numerically (Decision 9). There is
    no use case for deliberately viewing the un-encoded, known-wrong appearance.

    No ACES mirroring: UE's `SColorBlock` applies sRGB encode only, and a swatch
    shows a colour value, not a rendered scene. Nothing can match a sprite exactly
    anyway — a sprite is `tint x texture` through ACES.

11. **We draw the Old/New pair ourselves**, two stacked swatches as UE has, and
    pass `NoSidePreview`. ImGui's own side preview reads our values and cannot be
    re-encoded from outside, so it would be wrong per Decision 6.

12. **Live commit, no OK/Cancel.** UE's picker is a modal with both; ours is a
    popup editing live, one undo step per popup session. Revert-on-Escape would
    contradict `EditGesture`'s documented invariant that abandonment **commits**,
    which exists because the edits were applied and the user watched them happen.
    Escape and click-away keep the edit and leave exactly one undo step; `Ctrl+Z`
    is the way back.

13. **`hdr` keeps float channels and drops both hex rows.** A `ConstColor` feeds
    raw shader maths and may exceed 1, where a hex is a clamped lie. It also skips
    the sRGB encode of Decision 6 — above 1 the encode is meaningless — accepting
    that the SV response is poor for HDR values, which is inherent and true in UE
    too.

14. **Wheel geometry only, no selection UI.** Tabs and icon buttons are dropped;
    the dense panel is the request and UE ships one geometry. `PickerHueWheel`
    matches today's behaviour.

15. **No persisted preference, and therefore no `ImGuiSettingsHandler`.** Geometry
    selection went with the tabs and the sRGB toggle went with Decision 10, so
    nothing is left to persist. Recorded explicitly so nobody re-adds an ini
    section looking for one. If a preference appears later, the precedent is
    `[ArcaneEditorLayout][MaterialPanel]` registered in `EditorApp::Init` before
    the first `NewFrame`.

16. **Per-channel gradient bars are the cosmetic tier.** UE draws a gradient strip
    under each channel. Pure custom drawing, no interaction. In scope, and the
    first thing to cut if the arc runs long.

## What this reverts, and what survives

| shipped | fate |
|---|---|
| `8f2e7aac` `SrgbToLinear` / `LinearToSrgb` | **SURVIVE, and are used MORE** — the picker buffer encode (D6), both hex fields (D9), every swatch (D10), and now the tonemap's own curve (D1) |
| `8f2e7aac` `ColorDisplayFromLinear` / `ColorLinearFromDisplay` / `ColorField4` | reshaped — same job, but the alpha rule and hdr passthrough move into the new widget |
| `77fabd6b` Inspector single-select 0-255 sRGB boxes | boxes stay, now linear floats with no conversion; swatch becomes ours |
| `77fabd6b` Inspector multi-select 0-255 sRGB encoding | reverts to linear floats, near its pre-arc state. The "two disagreed inside one field" defect stays fixed — both sides are linear now rather than both sRGB |
| `0a571b0e` material param adoption | same shape as the Inspector row |
| `deed9abe` `ConstColor` swatch + `ed::Suspend/Resume` | both **survive**; it opens the new popup and the Suspend/Resume becomes D5's named helper |

The curve work is not wasted. What changed is *where* sRGB is applied, not the
decision to be correct about it.

## Non-goals

- **No saved-colour palette.** UE's "Drag & drop colors here to save" strip is the
  largest part of its picker and least related to this fix. Its own arc.
- **No eyedropper.** Needs screen capture.
- **No OK/Cancel.** D12.
- **No transposed value table.** D7 forbids it; ImGui lays its own rows out.
- **No `SOURCE_IN_LINEAR_SPACE` equivalent in `imgui.hlsl`.** UE has one; we avoid
  needing it by encoding CPU-side at the few call sites that matter. Revisit only
  if linear-authored colour reaches ImGui somewhere we cannot pre-encode.
- **No third geometry.**
- **No decomposition of `ShaderEditorDocument.cpp`.** 5,938 lines carrying the
  material panel, node canvas, pass canvas, param widgets and preview; it produced
  most of this session's catches. Real, separate, recorded so it is not lost.
- **No rework of `EditGesture`'s activation path.** 21 call sites, tested. This
  adds a sibling.

## Verification

**Part 0 (tonemap):** the updated golden test in `TonemapTest.cpp` is the check.
No visual gate, per the user's call. Green suite plus the user's own eye.

**Part 1 (gesture core): unit-tested headlessly**, extending `EditGestureTest`.
The pure core exists so its decision table runs without ImGui, and skipping that
would break the layer's own convention and leave the load-bearing invariant
unobserved — that an abandoned popup gesture commits rather than stranding. A
stranded transaction is not cosmetic: `CommandStack::InTransaction()` gates both the
Inspector's structural affordances and the `Ctrl+Z`/`Ctrl+Y` keybinds, so one
orphan disables Add/Remove Component and undo editor-wide. Cases: open/close
ownership, abandonment commits, click-away and Escape both closing, and a popup
gesture interleaved with an activation gesture on another widget.

A considered deviation from the previous arc's no-tests call, which was made for a
visual feature and still governs Part 2.

**Part 2 (widget): visual gate.**

1. A colour row shows four linear-float boxes plus a swatch; the swatch opens the
   popup and typing in a box still edits without it.
2. **Row width judgement (D8):** four `0.578` boxes are not crowded at the default
   label split and do not clip at a narrow panel width. Decides whether swatch-only
   is needed.
3. **The SV square feels right** — dragging across it gives an even perceptual
   spread, with usable shadow range rather than everything crammed bright. This is
   D6's whole justification; if it feels wrong, D6 is wrong.
4. The alpha bar's tint and the Old/New swatches all look like the colour, not
   darker than it.
5. `sRGB` rows and the `Linear` readout both visible and both correct: `0.5` linear
   reads `0.735`-ish in sRGB, not `0.5`.
6. `Hex sRGB` and `Hex Linear` both visible, both labelled, disagreeing in the
   expected direction.
7. Pasting `C9A0DC` into `Hex sRGB` sets the sprite to that colour and it *looks*
   like that colour.
8. Typing into either hex field works and both agree afterwards.
9. One `Ctrl+Z` undoes one whole popup session, not one step per frame.
10. Editing a channel box in the row is still one undo step per drag — the existing
    activation gesture, not regressed.
11. Opening and closing the popup without editing creates no undo step.
12. `Escape` and click-away both keep the edit and leave exactly one undo step
    (D12 — verify intent, since it differs from UE).
13. Drag value to black and back up — hue is preserved (D7's guard).
14. The `ConstColor` popup opens above the canvas, unclipped, surviving a pan.
15. `hdr` mode shows float channels and no hex rows.

## Risks

- **D6 is the load-bearing UX call.** If encoding the picker buffer causes any
  round-trip drift visible as value creep while dragging, the mitigation is to hold
  the sRGB buffer as the edit-session's authority and decode once per change rather
  than re-encoding from storage each frame.
- **The gesture shape is the load-bearing correctness call.** A wrong popup
  boundary strands a transaction and disables undo editor-wide. Hence tested.
- **`ConstColor` is the hardest site**, on both gaps at once, and D13 also opts it
  out of the encode. Fallback: leave that node on its `DragFloat4` and ship the
  other sites; it keeps working as today.
- **Row width** (D8) may fail its gate item; fallback is swatch-only.
- **Tasks 3 and 4 of the shipped arc were never desk-checked** and are being
  rewritten before anyone confirmed the first version. The gate above must be run
  in full, not partly inherited from this morning's Task 2 pass.
