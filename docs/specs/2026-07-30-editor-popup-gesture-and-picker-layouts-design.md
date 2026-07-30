# Popup-Scoped Edit Gestures + Selectable Picker Layouts — Design

**Status:** proposed, 2026-07-30
**Supersedes nothing.** Builds directly on the colour-picker unification shipped
earlier today (`8f2e7aac`..`deed9abe`).

## Problem

Two problems, one root cause.

**The surface request:** the colour picker should let the user choose between
ImGui's two picker geometries, with the choice exposed as square icon buttons
stacked vertically at the top-right of the popup rather than hidden in a
right-click menu, and with every numeric representation (RGB, HSV, Hex) visible
at once. The chosen geometry should survive an editor restart.

**The root cause found while scoping it:** `Arcane::Editor::EditGesture` models
exactly ONE edit boundary — *item activated -> item deactivated*. Every catch
that cost us time while building the colour picker was a widget whose boundary is
a different shape. The editor currently has four boundary shapes in play and only
one of them is a named, tested abstraction:

| boundary shape | owner today | status |
|---|---|---|
| item activate -> deactivate | `EditGesture` | named, pure core unit-tested |
| single-shot commit | `StableTextEdit`, `ApplyImmediate` | named, untested |
| canvas-scoped popup | `ed::Suspend()/Resume()`, inline, 3 sites in 1 file | folklore |
| **popup lifetime** | **nothing** | **missing** |

The layering itself is sound and was already hardened once: `EditGesture`'s header
records that it exists to own "the two invariants every hand-rolled copy kept
breaking (the 2026-07-26 audit's CRITICAL 1 class)". This design does not rework
that. It adds the one missing member of the family and promotes the one piece of
folklore, then builds the picker as the first consumer.

### Why the popup gap is real and not theoretical

Popup edits land inside undo TODAY only because ImGui *lends* us its popup's
active id. Inside `ColorEdit4`:

```cpp
// When picker is being actively used, use its active id so IsItemActive() will function on ColorEdit4().
if (picker_active_window && g.ActiveId != 0 && g.ActiveIdWindow == picker_active_window)
    g.LastItemData.ID = g.ActiveId;
```

`picker_active_window` is set only when **ImGui itself** opened the picker popup.
`EditGesture::BeginOnActivate` is built on `IsItemActivated()`, so it has been
riding that loan. The moment we open our own popup — which is the only way to put
our own icon buttons inside it — the loan stops, `IsItemActivated()` stops firing
for popup drags, and popup edits fall outside undo entirely.

This also means the `NoPicker` comment retired in `77fabd6b` was **half right**,
and the commit message overstated its wrongness. Its conclusion ("popup edits
would land outside undo") is false for ImGui's built-in popup and true for a
hand-rolled one. This design records that correction.

## Decisions

1. **Add a popup-lifetime boundary to `EditGesture`, as a peer of `ScopeGuard`.**
   The gesture opens when the popup opens and closes on the frame the popup is
   gone. It honours the two existing invariants unchanged: only the opener may
   close, and abandonment **commits** rather than stranding. Decision logic goes
   in the PURE CORE beside `EvaluateEnd` / `ShouldCloseAbandoned` /
   `ShouldCloseStaleOnActivate` — plain integers and bools, no `imgui.h` — so the
   `[editor]` units drive it headlessly exactly as they drive the rest.

   Rationale for a new shape rather than reusing `BeginOnActivate`: a popup's
   edits are made by *foreign widgets we do not submit*. There is no item of ours
   to observe activating or deactivating. The honest boundary is the popup's own
   lifetime, which we do control because we open it.

2. **Popup id, not item id, is the ownership token.** `Slots::item` currently
   holds the id of the widget that opened the gesture. For a popup gesture the
   parked token is the popup's id. The two never collide because ImGui ids are
   drawn from one space, so the existing ownership guard keeps working verbatim
   and needs no new branch.

3. **Abandonment for popups means "the popup is no longer open."** A popup can
   vanish for reasons no widget reports: click-away, `Escape`, the host window
   collapsing, the document closing mid-edit. The close therefore keys on
   `IsPopupOpen(id)` going false, and the existing `ScopeGuard` remains the final
   backstop for scope exit. Both paths commit, per invariant.

4. **Lift the canvas-popup rule out of folklore.** `ed::Suspend()`/`ed::Resume()`
   currently appears in `ShaderEditorDocument.cpp` and nowhere else in the editor.
   Give it a named RAII helper in the widget layer so that a popup opened inside
   the node canvas is a documented capability rather than one file's private
   knowledge. This is not extraction-for-reuse; it is recording a fact that has
   already been got wrong in writing — a comment at the `ConstColor` node asserted
   canvas popups were impossible while three working examples sat above it in the
   same file.

   `ConstColor` is the one site that sits on BOTH gaps at once: a popup-lifetime
   gesture inside the node canvas. It is the reason this decision is in scope.

5. **The picker is composed, but stays ONE `ColorPicker4` call.** Structure:

   - Inline row (Inspector, material panel, graph node): `ColorEdit4` with
     `NoSmallPreview | NoPicker | NoOptions` draws the channel boxes only. We draw
     the swatch ourselves as a `ColorButton` and open our own popup on click.
   - Our popup contains: the icon strip, and exactly one `ColorPicker4` call
     carrying `DisplayRGB | DisplayHSV | DisplayHex` plus the active geometry flag.

   The icon button changes exactly one bit — `PickerHueWheel` <-> `PickerHueBar`.
   Same call, same display flags, same rows. The geometry swaps above; the number
   rows below do not move. This is what makes "leave the text boxes in place"
   cheap instead of expensive: **we never write a numeric widget**, and never
   re-own clamp, edit, or round-trip behaviour.

   Two details pinned so they are not left to interpretation:

   - The popup's `ColorPicker4` carries `NoSidePreview`. ImGui's side preview
     would duplicate the inline swatch sitting immediately behind the popup, and
     it costs width the icon strip has already taken.
   - **Default geometry is the wheel**, matching what ships today
     (`8f2e7aac` forced `PickerHueWheel`) and what was originally asked for. Note
     this differs from ImGui's own default of `PickerHueBar`, so the default must
     be stated explicitly rather than inherited — including on the fallback path
     in Decision 9.

6. **One call is also a correctness requirement, not just economy.**
   `ColorEditRestoreHS` returns early unless `ColorEditSavedID == ColorEditCurrentID`,
   and `ColorEditCurrentID` is documented as being set "while inside of the
   **parent-most** ColorEdit4/ColorPicker4 (because they call each others)"
   (`imgui_internal.h:2692-2696`). Hue and saturation are undefined at greys and
   blacks and are restored from that saved slot. Splitting geometry from the number
   rows into two sibling widgets gives them different parent-most ids, so the
   restore silently stops working and hue is lost when value or saturation reaches
   zero. Keeping one `ColorPicker4` preserves it.

7. **Row order is ImGui's, not the transposed table from the first mockup.**
   `ColorPicker4` lays its own rows out: an RGB row, an HSV row, and a Hex row,
   each a full-width group of channel boxes. It will not emit them transposed
   (channels down the rows, representations across the columns) as the first two
   mockups drew.

   This is a **deliberate departure from the earlier mockup** and the trade is
   explicit: transposing requires `NoInputs` plus three sets of hand-written
   numeric editors, which forfeits both Decision 5's zero-reimplementation and
   Decision 6's hue restore. The result is still tabular — aligned columns of
   channel boxes, one row per representation — and it matches the inline row's
   existing horizontal orientation, which is a consistency gain. Hex is naturally
   its own full-width row because it is one value, which resolves the "Hex cannot
   be a third column" question by construction.

8. **`hdr` mode drops the Hex row and keeps float display.** A `ConstColor` node
   feeds raw shader maths and may legitimately exceed 1, where an sRGB hex string
   has no meaning and would display clamped. `hdr` therefore passes
   `Float | HDR | DisplayRGB | DisplayHSV` and omits `DisplayHex`. Geometry
   selection and persistence behave identically in both modes.

9. **The geometry preference persists through an `ImGuiSettingsHandler` into the
   editor's `imgui.ini`**, following the established precedent rather than
   introducing a config file. ImGui deliberately does not persist
   `g.ColorEditOptions` and exposes no getter for it, so the editor must store its
   own copy.

   - Section: `[ArcaneEditorColorPicker][Picker]`, its own TypeName so the widget
     layer does not have to reach into `ShaderEditorDocument`'s handler.
   - Registered from `EditorApp::Init` alongside
     `ShaderEditorDocument::RegisterLayoutSettings()` and
     `RegisterPlayModeSettings()` — after the ImGui context exists and **before
     the first `NewFrame`**, which is where ImGui reads the ini. A handler
     registered later never sees the saved entry.
   - **Sanitized on load.** The value arrives from a text file a human can edit,
     so anything that is not one of the two known geometries falls back to the
     default. `ReadOpen` returns null for an unknown entry name, which makes stale
     entries inert exactly as the existing handler documents.
   - Scope: one preference for the whole editor, not per-site. A user who prefers
     the bar prefers it everywhere; per-field memory would be surprising and would
     multiply ini entries without being asked for.

10. **The preference is a persisted editor preference, not project data.** It does
    not belong in `.arcproj` or its layered config: it describes how one user likes
    to author, not anything about the project. This matches where pane splits and
    play-mode state already live.

## Non-goals

- **No transposed value table.** Decision 7. Revisit only if someone is willing to
  own three numeric editors and lose hue restore.
- **No change to the sRGB encode/decode.** `ColorField4`'s conversions, the true
  IEC piecewise curve, and alpha-never-encoded all stand exactly as shipped today.
  This design changes the popup's chrome and the gesture boundary, not the colour
  maths.
- **No rework of `EditGesture`'s existing activation path.** 21 call sites depend
  on it and it is tested. This adds a sibling; it does not refactor.
- **No decomposition of `ShaderEditorDocument.cpp`.** It is 5,938 lines and holds
  the material panel, node canvas, pass canvas, param widgets and preview, and it
  produced most of this session's catches. That is a real and separate concern;
  recording it here so it is not lost, explicitly out of scope.
- **No third geometry.** ImGui offers exactly two (`PickerHueBar`,
  `PickerHueWheel`). We are not writing our own.
- **No per-field or per-document geometry memory.** Decision 9.

## Verification

**The two halves get different treatment on purpose, and the split is the point.**

**Gesture core — unit-tested, headlessly.** `EditGesture`'s pure core exists
precisely so its decision table can be driven without ImGui, and `EditGestureTest`
already does that for the three existing predicates. Adding a boundary shape
without extending that suite would break the layer's own convention and would
leave the invariant that matters least observable — that an abandoned popup
gesture commits rather than stranding. A stranded transaction is not cosmetic:
`CommandStack::InTransaction()` gates both the Inspector's structural affordances
and the Ctrl+Z / Ctrl+Y keybinds, so one orphan disables Add/Remove Component and
undo editor-wide. Tests cover: open/close ownership, abandonment commits,
click-away and Escape both closing, and a popup gesture interleaved with an
activation gesture on another widget.

This is a deliberate deviation from the previous arc's "no unit tests" call. That
call was made for a *visual* feature and remains right for the widget. The gesture
core is not visual — it is a decision table with an existing headless harness.

**Widget — the user's visual gate.** Consistent with the earlier arc:

1. Both icon buttons appear top-right, stacked, and the active one is lit.
2. Clicking the other button swaps the geometry and **the number rows do not move
   or flicker**.
3. RGB, HSV and Hex are all visible and all editable; editing any one updates the
   others and the swatch.
4. One `Ctrl+Z` undoes one whole picker session, not one step per frame.
5. Opening the popup and closing it without editing creates no undo step.
6. Drag value to black, then back up — hue is preserved (Decision 6's guard).
7. The `ConstColor` node's popup opens above the canvas, unclipped, and survives a
   canvas pan while open.
8. `hdr` mode shows float RGB and HSV and no Hex row.
9. Choose the bar, restart the editor — the bar is still selected.
10. Corrupt the ini entry by hand, restart — the editor falls back to the default
    and does not crash.

Item 10 is worth running rather than assuming; it is the one that exercises the
sanitize path, and hand-editing the ini is the documented reason that path exists.

## Risks

- **The gesture shape is the load-bearing part.** If the popup boundary is wrong,
  the failure mode is a stranded transaction that disables undo editor-wide until
  another gesture absorbs it. This is why it is tested rather than eyeballed.
- **`ConstColor` is the hardest site**, being both gaps at once. If the canvas
  popup fights `ed::Suspend`/`Resume` in a way the three existing sites did not,
  the fallback is to leave that node on its `DragFloat4` alone and ship the other
  three sites — the node keeps working exactly as it does today.
- **Widening the popup for the icon strip narrows the SV picking area** unless the
  popup grows, because ImGui derives `sv_picker_size` by subtracting the side bars
  from the total width. The strip is ~26 px. Decision: widen the popup rather than
  shrink the picking area.
