# Inspector Visual Polish — Design

**Date:** 2026-07-28
**Branch:** continues `arcane-entity-rename` (tip `f0237012`) or a new branch cut
from it — the Inspector code this restyles all lives there, unmerged.
**Reference:** the user's side-by-side screenshots (UE 5.6 Details vs our
Inspector, 2026-07-28) + the vendored UE tree for any behavioural question.

## Goal

Close the visual gap between our Inspector and UE's Details panel. Scope was
user-decided: **layout + color + rhythm** — the two-column grid, axis colors,
muted header bands, in-grid unsupported rows, consistent padding, and removal
of the dead `x` on read-only asset refs. Explicitly OUT of scope: filter chips,
asset thumbnails, Advanced sub-sections, reset arrows (feature was cut), and
any rework of the panel's top area (name header / Add Component placement).

## The deltas this closes (from the screenshots)

| # | Ours today | UE / target |
|---|---|---|
| 1 | Widget first, label trailing — label x-position wanders per row | Label LEFT in one consistent column, value RIGHT, one draggable split shared by every section |
| 2 | Plain numeric drags | X/Y(/Z) axis color bars on vector components (red/green/blue) |
| 3 | Component headers in default bright-blue `ImGuiCol_Header` | Muted full-width dark bands |
| 4 | `Texture Id (unsupported)` as bare mid-list text | Label stays in the label column; value cell renders greyed — dead rows keep the grid rhythm |
| 5 | Read-only `Identity > Id` renders a disabled, meaningless `x` clear button | No clear affordance on read-only refs |
| 6 | Ad-hoc vertical spacing | Consistent row height and padding |

## Section 1 — The two-column grid

**Approach (chosen):** one ImGui table per component section, opened inside the
`CollapsingHeader`, two columns (label, value), `ImGuiTableFlags_Resizable`
with no visible border until hovered (`NoBordersInBodyUntilResize`). UE's
defining trait — ONE split shared by all sections — is achieved by syncing the
label column's width through `InspectorState` (a single float): each table
initializes column 0 from the stored width and writes back the user's resize.
ImGui has no public per-column width getter, so the write-back reads
`GetCurrentTable()->Columns[0].WidthGiven` via `imgui_internal.h` — already an
accepted dependency in this TU. **Plan-time verification item:** confirm the
field spelling and that `BeginTable` honours a per-call init width when the
table's own `.ini` settings would otherwise win (`ImGuiTableFlags_NoSavedSettings`
is expected to be required so OUR state is the only authority).

Rejected alternatives: a single panel-spanning table (collapsing headers cannot
span table columns cleanly); a hand-rolled splitter with manual
`SetNextItemWidth` arithmetic (more custom code than syncing one float).

**Row shape, every visitor arm:** `TableNextRow`; column 0 = the display-name
label (greyed when the field is `Astra::ReadOnly` or unsupported, matching
UE's disabled-label treatment); column 1 = the widget(s) at full cell width
(`SetNextItemWidth(-FLT_MIN)` is CORRECT here — the old objection, pushing the
trailing label off-panel, dies with trailing labels). Widgets switch their
visible labels to `"##<rawName>"` hidden-id form; the label text lives in
column 0 as its own item.

**Invariant consequences (each must hold; they are the review gates):**

- **ImGui item IDs change once** (labels moved into the ID-hidden form and a
  table ID joins the stack — `BeginTable` pushes its ID; verify in vendored
  source). IDs remain stable frame-to-frame afterwards, which is all the
  cross-frame machinery needs: `gestureItem`, `stringEditItem`, and the
  `EndGesture` ownership guard all compare ids recorded and read under the
  SAME layout. One-time transient-state reset (open sections, cursor
  positions) is acceptable and must be called out in the desk notes.
- **Tooltips:** the raw-identifier tooltip must fire from BOTH cells — the
  label text (now a separate item in column 0) and the value widget. The
  visitor's existing tail block queries the last drawn item; extend it with
  the label-item hover, OR-ed the way the AssetRef arm's `hovered` optional
  already composes. `IsItemHovered(ForTooltip)` semantics unchanged.
- **`label`/`rawName` split unchanged:** `rawName` still feeds transaction
  descriptions and `AssetKindFilterForFieldName`; `label` becomes column-0
  text instead of a widget suffix. Undo descriptions stay byte-identical.
- **`MultiScalarRow`** renders its N boxes inside the value cell; its
  `PushMultiItemsWidths(count, CalcItemWidth())` derives from the cell width.
- **PushID discipline:** the per-field `PushID(f.nameHash)` continues to wrap
  the whole row (both cells). Nothing between a push and its pop may
  `continue`; `TableNextRow` inside the pushed scope is fine.
- **Category sub-headers** (`TreeNodeEx`) and the component `CollapsingHeader`
  stay OUTSIDE the tables; each category's fields get their own table region
  (same synced width), so the split is global but headers span full width.

## Section 2 — Axis color bars

Vec2/Vec3 component boxes (single-select drags AND multi-select text boxes)
get a UE-style colored strip on the left edge of each frame: X red, Y green,
Z blue. Implementation: after each component widget, draw a filled rect via
`GetWindowDrawList()` over the frame's left edge (~3 px, `GetItemRectMin/Max`)
— an overlay, no style-stack surgery, no layout shift. Palette constants live
beside the other inspector style constants (one place; the implementer surveys
where the editor's style setup lives and matches). Muted tones sampled from
the UE screenshot, not neon.

## Section 3 — Header bands

Component `CollapsingHeader` and category `TreeNodeEx` colors move from
theme-default blue to muted dark bands (UE's treatment): push
`ImGuiCol_Header/HeaderHovered/HeaderActive` around the Inspector's headers
only — scoped pushes, not a global theme change, so the Outliner and other
panels are untouched. Values defined next to the axis palette.

## Section 4 — Unsupported and read-only rows

- Unsupported (`FieldKind::ReadOnly` classification): label in column 0
  (greyed), value cell renders greyed text `unsupported` with the field's
  C++ type kept OUT of the cell (the tooltip already carries the raw name).
  No more bare mid-list text.
- `Astra::ReadOnly` fields: label greyed like UE's disabled labels; the value
  widget stays disabled as today.
- Read-only AssetRef rows drop the `x` clear button entirely (it is disabled
  AND meaningless; the C1 drag-drop gate already protects writes).

## Section 5 — Rhythm

One style-var push around the panel's grid regions: consistent `FramePadding`
and `ItemSpacing` (values chosen at desk against the screenshot; start from
UE's apparent ~4 px vertical rhythm). Category indent stays the tree's.

## Testing honesty

This arc is ~entirely `EditorPanels.cpp` — **uncoverable by the gate, by
construction**. The pure module gains nothing testable (the palette is
constants). The full suite must stay green (no assertion drift), the exe
timestamp is the compile evidence, and the acceptance is a SCREENSHOT desk
pass against the UE reference. The one-time ID reset (Section 1) is expected
and must not be filed as a bug. Desk list: split drags and persists per
session; both-cells tooltips; gesture/undo behaviour identical (drag, rename,
string edit, multi-select fan-out, Escape cancels); axis bars on Transform +
Sprite Renderer size; header bands; unsupported rows greyed in-grid; no `x`
on `Identity > Id`.

## Plan-time verification items

1. `BeginTable` ID-stack behaviour and per-call column init vs `.ini` settings
   (`NoSavedSettings`), and `ImGuiTable::Columns[n].WidthGiven` spelling —
   vendored `imgui_tables.cpp` / `imgui_internal.h`.
2. Whether `CalcItemWidth` inside a table cell returns cell-relative width
   (MultiScalarRow's arithmetic depends on it).
3. The tail tooltip block's exact composition with a second (label) item.
4. Where the editor's style constants live, for the palette's home.
