# Widget Layer Desk-Verify Checklist — 2026-07-28

Arc: Arcane::Editor widget layer (`EditGesture` + `EditorWidgets` + `InspectorView`),
spec `docs/superpowers/specs/2026-07-28-arcane-editor-widget-layer-design.md`, plan
`docs/superpowers/plans/2026-07-28-arcane-editor-widget-layer.md`. Tasks 1-7 are
code-complete and gate-green; this checklist is the arc's acceptance, per repo
convention (the harness cannot run it — GPU-driver crash hazard under the virtual
display — this is the user's desk pass, at the desk, interactively).

Tick each box. Anything that misbehaves -> note it and stop; that's a real finding,
not a nit.

## Setup

- [ ] Build is current: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /t:ArcaneEditor /p:Configuration=Debug /m /nologo /v:minimal`
- [ ] Launch `Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe`, in **Edit mode** (not Play).
- [ ] Have a scene open with at least one selectable entity (a multi-selectable pair for
      item 1), a shader/material asset open in the shader editor, and a `.arcsprite` asset
      open in the sprite doc. Keep the **Console panel** visible the whole time (validation
      noise is a failure).

---

## Spec section 6 -- carried verbatim

- [ ] **1.** Multi-select `position.x` type-then-gizmo-drag = one undo step (the CRITICAL repro).
- [ ] **2.** Shader editor: drag a param, close the window mid-drag, Ctrl+Z restores (new).
- [ ] **3.** Sprite doc: drag ppu, Ctrl+Z restores; Save-dirty still works (new).
- [ ] **4.** Field-grid label-width drag still syncs across grids and never fights imgui.ini.
- [ ] **5.** Intra-group tab .x -> .y: the KNOWN loss reproduces identically (not worse).
- [ ] **6.** AssetRef drop on a ReadOnly field still refused; clear button still gated.

---

## Plan additions (7-9) + this sweep's addition (10)

- [ ] **7.** Shader editor rename sites (pass name, comment title, param/texture name,
      swizzle mask) still commit once on Enter/click-away; Escape ALSO commits the typed
      text rather than reverting it -- this is **pre-existing behavior**, verified twice
      this arc by direct source reading (`StableTextEdit`, `EditorWidgets.cpp`, has no
      Escape-specific branch; ImGui's own Escape-revert semantics are not consulted by
      this commit-on-deactivate pattern). Confirm it still holds; if a later review
      changes this, this item updates to match.
- [ ] **8.** Sprite doc: undo of a ppu drag updates the viewport via the invalidate hook
      (drag ppu, Ctrl+Z, the sprite's on-screen size/geometry updates immediately --
      not just the Inspector-style field value).
- [ ] **9.** Close a document mid-drag via a project switch (`CloseAll`): no stranded
      transaction -- Ctrl+Z still works everywhere afterwards (amendment 1's teardown
      close, `EditGesture::ClosePending` in the document destructor, is what this
      exercises).
- [ ] **10.** Sprite doc: drag ppu, then destroy the document mid-gesture (close the tab
      while the drag's transaction is still parked, i.e. release the mouse button outside
      the window or otherwise abandon the drag right as you close it) -- no stranded
      transaction; Ctrl+Z works everywhere afterwards; note that **the redo stack clears**
      (the accepted cost of `ClosePending`'s commit-not-cancel rule -- a non-empty
      transaction close pushes a step, and pushing a step clears redo).

---

## Sign-off

- [ ] All of the above pass and it *feels right*.

If so, the **Arcane::Editor widget-layer arc is DONE.** Remaining action is entirely the
user's call (branch `arcane-editor-widget-layer`, merge/push per the team-branching
convention).
