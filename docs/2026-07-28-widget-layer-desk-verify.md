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
      NOTE: if the document is literally CLOSED mid-drag, the destructor-pushed step is
      inert by design (the `weak_ptr` anchor dies with the document), so "Ctrl+Z
      restores" is unobservable there -- the restorable case is **abandonment with the
      document still open** (sibling caveat to item 10's redo-clears note; see item 11).
- [ ] **3.** Sprite doc: drag ppu, Ctrl+Z restores; Save-dirty still works (new).
- [ ] **4.** Field-grid label-width drag still syncs across grids and never fights imgui.ini.
- [ ] **5.** Intra-group tab .x -> .y: the KNOWN loss reproduces identically (not worse).
- [ ] **6.** AssetRef drop on a ReadOnly field still refused; clear button still gated.

---

## Plan additions (7-9) + this sweep's additions (10-11)

- [ ] **7.** Shader editor rename sites (pass name, comment title, param/texture name,
      swizzle mask) commit once on Enter/click-away, and **Escape REVERTS** -- type into
      one, press Escape, the old text comes back and nothing is committed (no undo step,
      no dirty flag).
      CHANGED, post-merge and user-sanctioned (rider R1, 2026-07-29). It previously
      committed the typed text on Escape too: `StableTextEdit` compared against the
      PREVIOUS frame's typed snapshot, and ImGui writes its revert text into the caller's
      buffer only on the deactivation frame. The commit test now reads that post-widget
      buffer, so an escaped edit compares equal to the live value and commits nothing
      (`EditorWidgets.cpp`, `StableTextEdit`; the imgui citation chain is in the comment
      there). All four rename sites inherit this from the one widget -- there is no
      per-site Escape handling. Earlier task-6/8 reports describing Escape-commits are
      historical and were true when written.
      TWO PRE-EXISTING LIMITS, unchanged by R1 -- check them, but neither is a new
      finding:
      (a) **64-byte cap.** The shared buffer is 64 bytes, so a name of 63+ characters
      seeds truncated, and Escape reverts to that truncation rather than to the live
      value -- so touching such a field commits the shortened name however the edit
      ends. Equally true of Enter/click-away. Only worth confirming it behaves the
      same as before.
      (b) **Click straight from one text edit into another.** All four sites share one
      `m_textEdit` buffer keyed by one `activeKey`. If the edit you click INTO is
      submitted earlier in the frame than the one you leave, it claims the shared key
      while going active (`EditorWidgets.cpp:478-483`) and the edit you left then finds
      the key already taken and returns without committing (`:484-485`) -- its typed
      text is dropped. The other direction commits normally. Verified by reading, not
      guessed; pre-existing and untouched by R1. Press Enter before moving on.
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
- [ ] **11.** The in-document abandonment path `ScopeGuard` owns (EditGesture.hpp:76-85)
      is reachable, not theoretical -- this is spec section 5.1's primary repro, and
      it is currently UNEXERCISED by items 2/9/10 (all three close or destroy the
      document; this one keeps it open). In the shader editor: ctrl+click a drag
      widget into its temp text entry, then collapse the component/section (or send
      its tab to the background) so the widget stops being submitted while the
      document stays OPEN. The parked gesture must close with **ONE undo step**
      containing the live-applied edit, and **Ctrl+Z must restore it**.

---

## Sign-off

- [ ] All of the above pass and it *feels right*.

If so, the **Arcane::Editor widget-layer arc is DONE.** Remaining action is entirely the
user's call (branch `arcane-editor-widget-layer`, merge/push per the team-branching
convention).
