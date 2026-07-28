# Inspector Visual Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restyle the Inspector to UE's Details shape — label-left two-column grid with one shared draggable split, axis color bars, muted header bands, in-grid unsupported rows, consistent rhythm.

**Architecture:** All presentation; no decision logic moves. A per-component ImGui table gives the grid; a single `InspectorState` float syncs the split across tables; axis bars are draw-list overlays; bands are scoped style pushes. Every visitor arm converts to the row shape once.

**Tech Stack:** C++23, Dear ImGui (docking, tables, `imgui_internal.h`), the existing `InspectorMeta`/`InspectorFields` pure TUs (unchanged).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-28-inspector-visual-polish-design.md`. Decisions there are locked; its "Invariant consequences" block is the review rubric.
- **Branch:** `arcane-entity-rename`, tip `a284dfbf`. Entry gate baseline: **30256 assertions / 586 cases**, `~[gpu]`, seeds 6 AND 17. **This arc must not move the count** — it is presentation-only; any drift is a defect.
- **Build with the VS 18 toolchain explicitly** (`msbuild` on PATH is VS 2022, fails MSB8020):
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal`
- **Gate from the exe dir:** `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests` then `.\ArcaneTests.exe "~[gpu]" --rng-seed 6` and `--rng-seed 17`.
- **`EditorPanels.cpp` is NOT in the gate.** Exe-timestamp check after every task (`ArcaneEditor.exe` newer than everything under `Arcane/ArcaneEditor/src`); the acceptance for the whole arc is a SCREENSHOT desk pass against the UE reference. Do not invent ImGui tests.
- Exception-free, ASCII-only, UTF-8 no BOM. **Comments explain WHY, never overclaim, and cite vendored IMPLEMENTATION lines** — stale/doc-comment cites are this project's recurring failure class.
- **Nothing in vendored ImGui/Astra/UE may be modified.**
- **Machinery that must survive byte-for-byte in behaviour:** `label` (display) / `rawName` (identifier) split — `rawName` feeds the three transaction-description builders + `AssetKindFilterForFieldName`; per-field `PushID(f.nameHash)` wraps the whole row, nothing between push and pop may `continue`; `BeginGestureIfActivated`/`EndGesture` + ownership guard; `CloseAbandonedGesture`; `stringEditItem` seed latch; `ApplyImmediate` fan-out; mixed-value blanking; the C1 `!readOnly` drag-drop gate; the search filter and category grouping; commit-on-deactivate + equality-guard cancel for strings.
- **Item IDs change ONCE** (labels move to `"##"` form; `BeginTable` pushes an ID — verified, `imgui_tables.cpp:460`). Stable per-frame thereafter, which is all the cross-frame id comparisons need. The one-time transient reset is expected; say so in reports.
- **Verified vendored facts** (re-verify at the step that uses them): `BeginTable` force-inherits `NoSavedSettings` in child windows (`imgui_tables.cpp:297-301`) — pass it explicitly anyway so OUR state is the only width authority; `ImGuiTableColumn::WidthGiven` (`imgui_internal.h:3126`) is the post-layout width; `TableSetupColumn`'s init width applies only at column INIT (`imgui_tables.cpp:1639-1642`), so live cross-table sync requires `ImGui::TableSetColumnWidth` (internal — verify signature and that calling it when the stored and actual widths differ does not fight user drags mid-frame).

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/ArcaneEditor/src/EditorPanels.hpp` (modify) | `InspectorState` gains `float labelColWidth` (0 = uninitialised → default fraction). |
| `Arcane/ArcaneEditor/src/EditorPanels.cpp` (modify) | Everything else: grid helpers, arm conversion, axis bars, bands, rhythm, palette constants. |

---

### Task 1: The two-column grid — every arm converts

**Files:** `EditorPanels.hpp` (state member), `EditorPanels.cpp` (helpers + `ImGuiFieldVisitor::Visit` + `MultiScalarRow` + the unsupported/readonly row treatment + the read-only AssetRef `x` removal).

**Interfaces produced** (anonymous namespace):
- `bool BeginFieldGrid(InspectorState& state)` / `void EndFieldGrid(InspectorState& state)` — wraps `BeginTable("##fields", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_NoBordersInBodyUntilResize)`, label column `WidthFixed` initialised from `state.labelColWidth` (default: 40% of avail on first use), value column `WidthStretch`. Inside: if `state.labelColWidth > 0` and differs from the live `Columns[0].WidthGiven`, apply via `TableSetColumnWidth(0, ...)`; before `EndTable`, read `GetCurrentTable()->Columns[0].WidthGiven` back into state. **Verify the sync protocol against `imgui_tables.cpp` first** — the goal: drag ANY section's split, ALL sections follow, width persists for the session (not the .ini).
- `bool FieldLabelCell(const std::string& label, bool dimmed)` — `TableNextRow` + `TableSetColumnIndex(0)` + (grey-pushed when dimmed) `TextUnformatted(label)`; returns `IsItemHovered(ImGuiHoveredFlags_ForTooltip)` so the caller can OR it into the tooltip decision; then `TableSetColumnIndex(1)` + `SetNextItemWidth(-FLT_MIN)`.

- [ ] **Step 1:** Read `Visit` end-to-end, `MultiScalarRow`, and the tail tooltip block. Verify the vendored facts above plus: `CalcItemWidth` inside a table cell derives from the cell (`MultiScalarRow`'s `PushMultiItemsWidths(count, CalcItemWidth())` must fill the value cell); the `AssetRef` arm's `hovered` optional pattern (the model for OR-ing the label-cell hover in).
- [ ] **Step 2:** Add the helpers + state member. Each category region (the uncategorised drive and each named category's drive) gets its own `BeginFieldGrid`/`EndFieldGrid` pair OUTSIDE the visitor, so headers span full width and the visitor only emits rows. The visitor gains a `labelHovered` per-row bool routed into the tail tooltip (fire when EITHER cell hovers).
- [ ] **Step 3:** Convert every arm: `FieldLabelCell(label, readOnly || kind == ReadOnly)` first; widget labels become `"##" + rawName` (build once per row; the ID-hidden form — visible text now lives in column 0 only). Preserve every non-label widget argument byte-for-byte. `MultiScalarRow` loses its trailing `TextUnformatted` (the label cell replaced it) and its inner `SameLine` label plumbing. The unsupported arm renders greyed `unsupported` in the value cell. The read-only AssetRef arm drops its `x` clear button.
- [ ] **Step 4:** Build; full gate seed 6 (count unchanged 30256/586); exe timestamp; commit `feat(editor): UE-style two-column Inspector grid`.

---

### Task 2: Axis color bars

**Files:** `EditorPanels.cpp`.

- [ ] **Step 1:** Palette constants in the anonymous namespace beside the other inspector helpers (survey first for an existing editor style home; if none, they live here): `kAxisBarColors[3]` — muted UE-ish red/green/blue (start `IM_COL32(196,64,54,255)`, `IM_COL32(96,166,58,255)`, `IM_COL32(58,122,196,255)`; final values are a desk call), bar width 3.0f.
- [ ] **Step 2:** After each vector-component widget (single-select `DragFloat2/3` is ONE widget — per-component bars need the multi-item form; read how `DragFloatN` lays out internally or draw N bars across its item rect using `PushMultiItemsWidths` arithmetic — **decide by reading; if per-component rects are not cheaply recoverable for the combined widget, switch the Vec2/Vec3 single-select arms to N individual `DragFloat`s with `PushMultiItemsWidths`, matching `MultiScalarRow`'s shape, so each component owns an item rect** — flag this in the report either way) and each `MultiScalarRow` box: `GetWindowDrawList()->AddRectFilled` over the frame's left edge (`GetItemRectMin`, +barWidth, `GetItemRectMax.y`), color by component index. Overlay only — no layout shift.
- [ ] **Step 3:** If the Vec arms were split into N drags: the gesture machinery must still see activation/deactivation per component (each drag is its own item — `BeginGestureIfActivated`/`EndGesture` already handle exactly this via the group-forwarding OR individual ids; re-read the ownership-guard comments and confirm which path applies, and that one drag remains one undo step). State the analysis in the report.
- [ ] **Step 4:** Build; gate seed 6 unchanged; exe timestamp; commit `feat(editor): axis color bars on vector fields`.

---

### Task 3: Header bands + rhythm

**Files:** `EditorPanels.cpp`.

- [ ] **Step 1:** Band palette beside the axis palette: muted dark header values for `ImGuiCol_Header/HeaderHovered/HeaderActive` (start ~`IM_COL32(48,48,52,255)` / slightly lighter hover/active; desk call). Push around the Inspector's component `CollapsingHeader`s and category `TreeNodeEx`s ONLY — scoped pairs, counted, popped on every path (the review gate: no style-stack imbalance; remember the filter `continue` sits BEFORE the header).
- [ ] **Step 2:** Rhythm: one `PushStyleVar` region around the panel's grid content — `FramePadding.y` and `ItemSpacing.y` tuned to the UE screenshot's ~4 px vertical rhythm (start `FramePadding.y 3`, `ItemSpacing.y 4`; desk call). Balanced pops.
- [ ] **Step 3:** Build; FULL gate BOTH seeds (arc acceptance: still 30256/586); exe timestamp; commit `feat(editor): muted header bands and row rhythm in the Inspector`.

---

## Self-Review

Spec coverage: §1 grid+rows+tooltips+MultiScalarRow → Task 1; §2 axis bars (+ the DragFloatN fork stated as a decision with its gesture analysis) → Task 2; §3 bands → Task 3; §4 unsupported/readonly/no-x → Task 1; §5 rhythm → Task 3; testing honesty → Global Constraints + desk list. Placeholders: palette/rhythm numbers are deliberate desk calls with stated starting values, not TBDs. Type consistency: `BeginFieldGrid`/`EndFieldGrid`/`FieldLabelCell`/`labelColWidth` used with one spelling throughout.

**The implementer must verify against the codebase rather than trust this plan:** the `TableSetColumnWidth` sync protocol; `CalcItemWidth`-in-cell; the `DragFloatN` per-component rect question; the tail tooltip block's exact shape.

## Desk-Verify (arc acceptance — screenshot vs the UE reference)

1. Labels left, one straight vertical split; drag it in ANY section — all sections follow; persists until the editor closes.
2. Tooltips (raw identifier) fire from BOTH the label cell and the value widget, including on disabled rows.
3. Undo unchanged: drag = one entry with the raw-name description; rename + string edits + multi-select fan-out + Escape cancels all behave exactly as before the restyle; per-component vec drags (if split) still one undo step per drag.
4. Axis bars on Transform position/scale and Sprite Renderer size, single AND multi-select.
5. Header bands muted; no bright-blue headers anywhere in the panel.
6. `Texture Id` / `Tint` / `Shape` rows sit in the grid with greyed values; `Identity > Id` greyed with NO `x`.
7. One-time reset of open/collapsed section state after first launch — expected, not a bug.
8. The panel at ~340 px width (the screenshot's) — no clipped labels, no horizontal scroll.
