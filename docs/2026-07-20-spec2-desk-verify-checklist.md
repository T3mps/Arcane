# SPEC #2 Desk-Verify Checklist — Unified Undo/Redo + Transform Gizmo

One Grimoire session covers both halves: the gizmo and the Inspector edits share the same
`CommandStack`, so a gizmo drag followed by an Inspector edit, then Ctrl+Z, exercises the
whole feature. Both arcs are **code-complete + fully reviewed**; this is the only gate left
(it needs the desk — the GPU-driver crash hazard blocks it headless).

Tick each box. Anything that misbehaves → note it and stop; that's a real finding, not a nit.

## Setup

- [ ] Build is current: `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /t:Grimoire /p:Configuration=Debug /m /nologo /v:minimal`
- [ ] Launch `Arcane\bin\Debug-windows-x86_64-md\Grimoire\Grimoire.exe`, in **Edit mode** (not Play).
- [ ] Have a scene with at least one selectable entity, **including one with a physics body** (so you can confirm the body follows). Keep the **Console panel** visible the whole time.

---

## Part A — Undo foundation (Inspector edits)

- [ ] **A1.** Select an entity → drag a `LocalTransform` field (e.g. `position.x`) in the Inspector → the value changes live.
- [ ] **A2.** **Ctrl+Z** → the field snaps back to its pre-edit value in **one** step. On the physics entity, the **body follows** the revert (SPEC #1 polling reconcile = free PostEditUndo).
- [ ] **A3.** **Ctrl+Y** and **Ctrl+Shift+Z** both re-apply the edit. (Ctrl+Shift+Z must NOT also fire Undo.)
- [ ] **A4.** Toolbar **Undo/Redo** buttons enable/disable per availability; hovering shows the label tooltip (e.g. "Edit LocalTransform.position").
- [ ] **A5.** Edit two different components on one entity (e.g. `LocalTransform`, then `SpriteRenderer`) → Ctrl+Z steps back through **each** as its own step.
- [ ] **A6.** Edit → Undo → make a *new* edit → the redo stack is cleared (Ctrl+Y after does nothing).
- [ ] **A7.** Enter **Play**, drag a field during Play (transient — discarded on Stop), then **Stop**: your **pre-Play Edit history survives** — Ctrl+Z still reverts the edits you made *before* Play, and the play-time edit left no undo entry. *(Changed 2026-07-20: Play no longer wipes history; it persists across Play/Stop.)*

---

## Part B — Transform gizmo (viewport)

- [ ] **B1.** Select an entity → a gizmo appears at it, drawn **on top** of the scene, aligned with the sprite.
- [ ] **B2.** **W / E / R** switch Translate / Rotate / Scale; the toolbar **T / R / S** radios mirror the current mode, and their tooltips name the key ("Translate (W)", "Rotate (E)", "Scale (R)").
- [ ] **B3.** Zoom in and out → the gizmo stays a **constant screen size** AND stays aligned with the entity at every zoom.
- [ ] **B4.** Drag the **X axis**, **Y axis**, **center**, and (in Rotate) the **ring** → the transform updates live; the **physics body follows**.
- [ ] **B5.** **Ctrl+Z reverts the whole drag in one step**; **Ctrl+Y** re-applies it. (A drag = exactly one undo entry.)
- [ ] **B6.** Click a handle and release **without moving** → no undo entry is created (no-move self-drops).
- [ ] **B7.** Hold **Ctrl** while dragging → snaps: translate to a 0.5 grid, rotate to 15°, scale to 0.1 steps.
- [ ] **B8.** Rotate the entity, then toggle **Global/Local**: in Local the **translate axes rotate with the entity**; in Global they stay world-aligned. **Scale axes stay local** in both.
- [ ] **B9.** Hover a handle → it **highlights** (brightens); the correct handle is picked (center wins over axes on overlap).
- [ ] **B10.** Start a drag, then move the cursor **off the viewport edge** while still holding the button → the drag **keeps tracking** and **commits on release** (does NOT abort/strand the value). *(This is the reviewed T4 fix.)*
- [ ] **B11.** Clicking a handle does **not** change the selection; clicking empty space **deselects**.
- [ ] **B12.** Toggle the **physics-debug overlay** → the gizmo still renders **on top** of it. *(This is the final SetLayer fix.)*
- [ ] **B13.** Enter **Play** → the gizmo **disappears** (no handles during Play). *(Edit history now persists across Play — see A7.)*

---

## Part C — Integration + cleanliness

- [ ] **C1.** Interleave a gizmo drag and an Inspector edit on the same entity → Ctrl+Z steps back through each independently, in order, one step each (shared stack behaves as one history).
- [ ] **C2.** Throughout the whole session, the **Console stays clean** of NVRHI / Vulkan-validation errors (no red validation lines / VUID spam). Any validation noise is a failure.

---

## Sign-off

- [ ] All of the above pass and it *feels right*.

If so, **SPEC #2 (unified undo/redo + transform gizmo) is DONE.** Remaining action is entirely yours:
push local `main` (9 commits ahead of `origin/main`: spec + plan + the 7 gizmo commits; the undo
commits `2bd66442`/`64be2f78` are already on origin).

Non-blocking polish deferred by the final Opus review (fine to leave): `AxisDir` None→X contract
hardening, latching the drag entity, a redundant `ToViewportLocal` call, ESC-to-abort-drag.
