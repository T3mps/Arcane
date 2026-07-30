# UE-Style Colour Picker + Popup-Scoped Edit Gestures — Design

**Status:** proposed, 2026-07-30
**Supersedes:** `2026-07-30-editor-popup-gesture-and-picker-layouts-design.md`
(tabbed geometry selection). Its gesture-hardening half survives here unchanged;
its picker half is replaced after reading UE's actual `SColorPicker`.
**Revises shipped work:** partially reverts `77fabd6b`, `0a571b0e`, `deed9abe`.

## Problem

Colour authoring in the editor has two defects, and they turned out to have the
same shape: **nothing on screen ever says which colour space a number is in.**

That ambiguity has already cost real time today. A curve-sanity check written to
catch a wrong transfer function had `128 -> 186` as its own expected output —
computed with the `pow(1/2.2)` display gamma the work explicitly forbade — and it
propagated into a spec and into a user-facing gate list before arithmetic caught
it. The true answer is 188. A widget that named its spaces would have made that
error unwriteable.

The same ambiguity is why "your existing tint authored as 128 now reads 188" reads
as a bug rather than a relabel: the number changed and nothing on screen explained
which space either number was in.

**The fix is labelling and dual representation, not a different curve.** The curve
shipped this morning is correct and is verified against the hardware sRGB decode
(`SrgbToLinear(128/255) = 0.215861`).

### The second problem: one gesture boundary where four are needed

`Arcane::Editor::EditGesture` models exactly ONE edit boundary — *item activated ->
item deactivated*. The editor has four boundary shapes in play and only that one is
named and tested:

| boundary shape | owner today | status |
|---|---|---|
| item activate -> deactivate | `EditGesture` | named, pure core unit-tested |
| single-shot commit | `StableTextEdit`, `ApplyImmediate` | named, untested |
| canvas-scoped popup | `ed::Suspend()/Resume()`, inline, 3 sites in 1 file | folklore |
| **popup lifetime** | **nothing** | **missing** |

Popup edits land inside undo TODAY only because ImGui *lends* us its popup's active
id. Inside `ColorEdit4`:

```cpp
// When picker is being actively used, use its active id so IsItemActive() will function on ColorEdit4().
if (picker_active_window && g.ActiveId != 0 && g.ActiveIdWindow == picker_active_window)
    g.LastItemData.ID = g.ActiveId;
```

`picker_active_window` is set only when **ImGui itself** opened the popup. A dense
custom panel means we open our own, the loan stops, and popup edits fall outside
undo entirely. So the hardening is not optional for this design — it is a
precondition.

This also means the `NoPicker` comment retired in `77fabd6b` was **half right**,
and that commit message overstated its wrongness. Its conclusion is false for
ImGui's built-in popup and true for a hand-rolled one.

## What UE actually does — verified, not recalled

Read from the vendored UE 5.6 source, per the standing directive to defer editor
UX questions to Unreal:

- **`sRGB Preview` is a preview-only toggle.** Tooltip verbatim: *"When enabled,
  the preview swatch uses sRGB encoding to correct the colors for display."* It
  does not alter stored values or the numeric fields.
- **The R/G/B/A fields are LINEAR floats**, each with a gradient bar beneath.
- **There is exactly ONE hex field** — a dropdown plus textbox
  (`SColorPicker.cpp:378-413`) whose *label* changes between `Hex sRGB` and
  `Hex Linear` per `EColorPickerHexMode`. It does not show both at once.
- **Preferences persist in an ini**:
  `GConfig->GetBool("ColorPickerUI", "bSRGBEnabled", ..., GEditorPerProjectIni)`.
- **The Details-panel property row is a swatch only** — `CreateColorWidget` builds
  an `SColorBlock` (`ColorStructCustomization.cpp:134`). No numbers in the row.
  That is how UE avoids the row-crowding that caused the 2026-07-29 rejection of
  `ImGuiColorEditFlags_Float` in our Inspector row.

Arithmetic cross-check confirming UE's curve is ours: the screenshot's
`R 0.57758` with `Hex Linear 931B12FF` gives `0x93 = 147`, `147/255 = 0.576`; and
`LinearToSrgb(0.57758) * 255 = 200.0 = 0xC8`, matching `Hex sRGB C85B4BFF`.

## Decisions

### Gesture hardening (carried over from the superseded spec, unchanged)

1. **Add a popup-lifetime boundary to `EditGesture`, as a peer of `ScopeGuard`.**
   Opens when the popup opens, closes on the frame the popup is gone. Honours the
   two existing invariants unchanged: only the opener may close, and abandonment
   **commits** rather than stranding. Decision logic goes in the PURE CORE beside
   `EvaluateEnd` / `ShouldCloseAbandoned` / `ShouldCloseStaleOnActivate` — plain
   integers and bools, no `imgui.h` — so the `[editor]` units drive it headlessly.

   A new shape rather than reusing `BeginOnActivate`, because a popup's edits are
   made by *foreign widgets we do not submit*. There is no item of ours to observe
   activating. The honest boundary is the popup's lifetime, which we control
   because we open it.

2. **The popup id is the ownership token.** `Slots::item` parks the popup's id
   instead of a widget's. ImGui ids come from one space, so the existing ownership
   guard works verbatim with no new branch.

3. **Abandonment for popups means "no longer open."** Click-away, `Escape`, host
   window collapse, and document close are all reasons a popup vanishes that no
   widget reports. The close keys on `IsPopupOpen(id)` going false; `ScopeGuard`
   remains the backstop for scope exit. Both paths commit.

4. **Lift the canvas-popup rule out of folklore.** Give `ed::Suspend()`/
   `ed::Resume()` a named RAII helper in the widget layer. Not extraction for
   reuse — recording a fact already got wrong in writing, where a comment at the
   `ConstColor` node asserted canvas popups were impossible while three working
   examples sat above it in the same file. `ConstColor` is the one site on BOTH
   gaps at once, which is what puts this in scope.

### The picker

5. **Property rows keep four channel boxes plus a swatch — a deliberate divergence
   from UE.** UE's row is a swatch only; ours keeps in-row numeric editing, because
   typing a channel without opening a popup is worth the width. Clicking the
   swatch opens the dense popup.

   **The four boxes show LINEAR floats, and this is not negotiable within this
   design.** If the row displayed 0-255 sRGB while the popup displayed linear
   floats, one widget would present the same stored value in two unlabelled spaces
   — precisely the defect this arc exists to remove, reintroduced at a smaller
   scale. Row and popup must agree.

   A welcome consequence: the inline path gets *simpler* than what shipped today.
   Storage is linear and display is linear, so there is **no conversion at all** in
   the row — `ColorEdit4` with `Float | NoSmallPreview | NoPicker | NoOptions` over
   the stored value directly. `NoSmallPreview` because we draw the swatch ourselves,
   so it can honour Decision 9's preview toggle and carry the popup's click.

   **Known tension, flagged rather than buried:** `ImGuiColorEditFlags_Float` at
   `%.3f` is what was rejected on 2026-07-29 for crowding the row. Four boxes of
   `0.578` need roughly 40 px more than four of `201`. This design accepts that,
   and the visual gate below is where it gets judged. If it reads as too tight in
   practice, the fallback is UE's actual answer — swatch-only rows — which is a
   one-line change at each of the three sites and removes no other decision here.

6. **The popup shows LINEAR floats, and never relabels anything.** Stored values
   are linear; the channel rows display them raw. A tint stored as `0.502` reads
   `0.502` forever. **The entire relabel class disappears** — there is no
   `128 -> 188` to explain, because no displayed number changes when this ships.

7. **Still ONE `ColorPicker4` call**, carrying `Float | DisplayRGB | DisplayHSV`
   plus a geometry flag, with `DisplayHex` **omitted** (we draw our own hex rows,
   below — a third, unlabelled ImGui hex would reintroduce the ambiguity this
   design exists to remove).

   One call is a correctness requirement, not economy. `ColorEditRestoreHS`
   returns early unless `ColorEditSavedID == ColorEditCurrentID`, and
   `ColorEditCurrentID` is set only "while inside of the **parent-most**
   ColorEdit4/ColorPicker4" (`imgui_internal.h:2692-2696`). Hue and saturation are
   undefined at greys and blacks and are restored from that slot. Splitting the
   channel rows out into sibling widgets gives them a different parent-most id, so
   hue is silently lost whenever value or saturation reaches zero.

   Consequence accepted: ImGui formats colour floats as `%.3f`, so channels read
   `0.578` where UE reads `0.577580`. Matching UE's six decimals would require
   hand-written numeric fields, forfeiting the guarantee above. Three decimals is
   the trade.

8. **Both hex representations, always visible, each labelled.** Two fields we
   draw: `Hex sRGB` and `Hex Linear`. This is the actual fix for the ambiguity —
   and it is deliberately **better than UE**, whose single mode-switched field
   still lets a reader see the wrong number because only one is on screen. Two
   labelled fields cannot be misread.

   Both are editable. `Hex sRGB` parses through `SrgbToLinear`; `Hex Linear`
   parses straight to linear. Pasting a hex from a design tool goes in the sRGB
   field, which is what a design tool emits.

9. **`sRGB Preview` checkbox, matching UE's meaning exactly** — it affects only
   swatches, never the numbers. When ticked, swatches fill with
   `LinearToSrgb(colour)`; unticked, with the raw linear value.

   This is the visual counterpart to Decision 8's numeric fix. ImGui draws
   post-tonemap into the backbuffer, so a raw linear fill appears darker than the
   colour actually reads in the viewport; gamma-encoding it is much closer.
   Deliberately *not* claiming an exact match — the viewport also goes through ACES
   tonemapping, so "closer" is the honest claim, not "identical".

   The toggle governs the popup's Old/New swatches and the inline row swatch alike,
   so one setting cannot make two swatches of one colour disagree.

10. **We draw the Old/New pair ourselves**, two stacked swatches as UE has, and
    pass `NoSidePreview` to `ColorPicker4`. Reason: ImGui's own side preview is
    drawn inside `ColorPicker4` and cannot be re-encoded, so Decision 9's toggle
    would have nothing to act on. Two `ColorButton`s are trivial and give the
    toggle a target.

11. **Live commit, no OK/Cancel.** UE's picker is a modal with explicit OK and
    Cancel; ours is a popup that edits live and lands one undo step per popup
    session. A revert-on-Escape would directly contradict `EditGesture`'s
    documented invariant that abandonment **commits**, which exists because the
    edits were already applied and the user watched them happen. Escape and
    click-away therefore keep the edit and leave exactly one undo step; `Ctrl+Z`
    is the way back.

12. **`hdr` collapses to almost nothing.** With linear floats in the channel rows,
    `hdr` no longer changes how channels display. It governs only two things: the
    ImGui `HDR` flag, and hiding both hex rows — a hex of a value above 1 is a
    clamped lie. Geometry and persistence behave identically.

13. **Per-channel gradient bars are the cosmetic tier, included but cuttable.**
    UE draws a gradient strip under each of R/G/B/A/H/S/V showing that channel's
    axis. They are pure custom drawing over ImGui's rows and carry no interaction.
    In scope, and the first thing to cut if the arc runs long.

14. **Geometry: wheel only, no selection UI.** Tabs and icon buttons are dropped —
    the dense panel is the request, and UE ships one geometry. `PickerHueWheel`
    matches both today's behaviour and UE's wheel. This removes the persistence of
    a *geometry* preference; the only persisted preference is Decision 9's toggle.

15. **`sRGB Preview` persists through an `ImGuiSettingsHandler` into the editor's
    `imgui.ini`**, following the established precedent rather than adding a config
    file, and mirroring UE's own choice to keep it in an editor ini.
    - Section `[ArcaneEditorColorPicker][Picker]`, its own TypeName so the widget
      layer never reaches into `ShaderEditorDocument`'s handler.
    - Registered from `EditorApp::Init` beside
      `ShaderEditorDocument::RegisterLayoutSettings()` and
      `RegisterPlayModeSettings()` — after the context exists and **before the
      first `NewFrame`**, which is where ImGui reads the ini. A handler registered
      later never sees the saved entry.
    - Sanitized on load; the file is human-editable. `ReadOpen` returns null for an
      unknown entry name, which makes stale entries inert as the existing handler
      documents. Default when absent or malformed: **ticked** (gamma-corrected
      preview is the one that matches the viewport).
    - One editor-wide preference, not per-site.

16. **It is an editor preference, not project data.** It describes how one user
    likes to author, so it does not belong in `.arcproj` or its layered config.
    Same placement as pane splits and play-mode state.

## What this reverts, and what survives

| shipped today | fate |
|---|---|
| `8f2e7aac` `SrgbToLinear` / `LinearToSrgb` | **SURVIVES, still load-bearing** — now drives the `Hex sRGB` field and Decision 9's preview instead of the channel boxes |
| `8f2e7aac` `ColorDisplayFromLinear` / `ColorLinearFromDisplay` / `ColorField4` | replaced — the sRGB-display-for-channels premise is gone |
| `77fabd6b` Inspector single-select 0-255 sRGB boxes | boxes STAY (Decision 5) but display linear floats; the encode/decode drops out, and the swatch becomes ours so it can open the popup and honour the preview toggle |
| `77fabd6b` Inspector multi-select 0-255 sRGB encoding | reverted to linear floats, i.e. close to its pre-arc state. The "two disagreed inside one field" defect it fixed stays fixed — both sides are now linear rather than both sRGB |
| `0a571b0e` material param `ColorField4` adoption | same shape as the Inspector row: linear float boxes + our swatch |
| `deed9abe` `ConstColor` node swatch + `ed::Suspend/Resume` | swatch and Suspend/Resume **survive**; it opens the new popup, and the Suspend/Resume becomes Decision 4's named helper |

The curve work is not wasted. What reverts is the decision about *where sRGB is
shown*, not the decision to be correct about it.

## Non-goals

- **No saved-colour palette.** UE's "Drag & drop colors here to save" strip is the
  largest piece of its picker and the least related to this fix. Its own arc.
- **No eyedropper.** Needs screen capture; separate concern.
- **No OK/Cancel.** Decision 11.
- **No transposed value table.** Decision 7 forbids it; ImGui lays its own rows out.
- **No third geometry.** ImGui has two; we ship the wheel.
- **No change to the transfer curve.** It is verified correct.
- **No decomposition of `ShaderEditorDocument.cpp`.** 5,938 lines holding the
  material panel, node canvas, pass canvas, param widgets and preview; it produced
  most of this session's catches. Real, separate, recorded here so it is not lost.
- **No rework of `EditGesture`'s activation path.** 21 call sites depend on it and
  it is tested. This adds a sibling.

## Verification

**Split deliberately, and the split is the point.**

**Gesture core — unit-tested headlessly.** `EditGesture`'s pure core exists so its
decision table can be driven without ImGui, and `EditGestureTest` already does that
for three predicates. Adding a boundary shape without extending that suite would
break the layer's own convention and leave the invariant that matters least
observable — that an abandoned popup gesture commits rather than stranding. A
stranded transaction is not cosmetic: `CommandStack::InTransaction()` gates both the
Inspector's structural affordances and the `Ctrl+Z` / `Ctrl+Y` keybinds, so one
orphan disables Add/Remove Component and undo editor-wide. Cases: open/close
ownership, abandonment commits, click-away and Escape both closing, and a popup
gesture interleaved with an activation gesture on another widget.

This is a considered deviation from the previous arc's no-unit-tests call. That call
was made for a visual feature and still governs the widget below. The gesture core
is a decision table with an existing headless harness, not a visual.

**Widget — visual gate.**

1. A colour property row shows four channel boxes plus a swatch; clicking the
   swatch opens the dense popup, and typing in a box still edits without it.
2. **Row width judgement (Decision 5):** with four `0.578`-width boxes, the row is
   not crowded at the default label-column split, and the boxes do not clip at a
   narrow panel width. This is the item that decides whether swatch-only is needed.
3. Channel boxes and popup channel rows show **the same linear floats** — no
   unlabelled space change anywhere in the widget.
4. Channel rows show linear floats. **No existing colour's displayed number
   changes when this ships** — the strongest signal the relabel class is gone.
5. `Hex sRGB` and `Hex Linear` are both visible, both labelled, and disagree with
   each other in the expected direction (sRGB brighter for mid-tones).
6. Pasting `C9A0DC` into `Hex sRGB` sets the sprite to that colour and it *looks*
   like that colour.
7. Typing into `Hex Linear` and into `Hex sRGB` both work and agree afterwards.
8. Ticking `sRGB Preview` changes only swatches; every number stays put.
9. With it ticked, the swatch reads much closer to the sprite in the viewport than
   unticked.
10. One `Ctrl+Z` undoes one whole popup session, not one step per frame.
11. Editing a channel box in the row (no popup) is still one undo step per drag —
    the existing activation gesture, unchanged and not regressed by the new shape.
12. Opening and closing the popup without editing creates no undo step.
13. `Escape` and click-away both keep the edit and leave exactly one undo step
    (Decision 11 — verify the intended behaviour, since it differs from UE).
14. Drag value to black and back up — hue is preserved (Decision 7's guard).
15. The `ConstColor` popup opens above the canvas, unclipped, and survives a pan.
16. `hdr` mode shows float channels and **no** hex rows.
17. Untick the preview, restart the editor — still unticked.
18. Corrupt the ini entry by hand, restart — falls back to ticked, no crash.

## Risks

- **The gesture shape is load-bearing.** A wrong popup boundary strands a
  transaction and disables undo editor-wide until another gesture absorbs it. Hence
  tested, not eyeballed.
- **`ConstColor` is the hardest site**, on both gaps at once. If the canvas popup
  fights `Suspend`/`Resume` in a way the three existing sites did not, fall back to
  leaving that node on its `DragFloat4` and ship the other sites; the node keeps
  working as it does today.
- **Row width is the open question.** Keeping four float boxes revisits the
  2026-07-29 crowding rejection (Decision 5). It is accepted deliberately and
  judged at the gate; the fallback is swatch-only rows, one line per site. No
  capability is removed either way — the popup always has the full set.
- **Reverting shipped call sites means re-verifying them.** Tasks 3 and 4's desk
  checks were never walked; those sites are being rewritten before anyone confirmed
  the first version. Net saving, but the gate list above must be run in full rather
  than assumed from this morning's Task 2 pass.
