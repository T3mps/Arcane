# Shader Graph Upgrade Implementation Plan (Pin Literals + Library Batch 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Inline per-pin constant literals on unwired graph inputs + 11 new library nodes, so graphs stop drowning in Const nodes and `Exp`-class math leaves Custom islands.

**Architecture:** All engine work sits on two existing seams: `GraphNode` (+ append-only serialization) and codegen's `argOr` unconnected-input lambda; the node batch is append-only enum/table/switch growth. Editor work is a per-pin widget on the canvas riding the existing graph-undo idiom.

**Tech Stack:** C++23, nlohmann::json, Catch2; Dear ImGui + imgui-node-editor (editor task only).

## Global Constraints

- **Spec:** `docs/superpowers/specs/2026-07-28-shader-graph-upgrade-design.md` — decisions locked (Approach A literals; the 11-node list; SSA emission and `lineNodeIds` untouched; non-goals stand).
- **Branch:** the current working branch at execution time (record the base SHA in the ledger at arc start). Entry gate at plan time: **30304/595** `~[gpu]` seed 6 — counts MOVE (new tests); account every delta exactly. Seed 6 per task, BOTH seeds 6+17 on the final task.
- **Build with VS 18 msbuild** (PATH msbuild fails MSB8020): `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /v:minimal` from `Arcane/`. **Gate from the exe dir** (`Arcane\bin\Debug-windows-x86_64-md\ArcaneTests`), never the workspace root.
- **APPEND-ONLY enum contract** (`MaterialGraph.hpp:72-73`): new `GraphNodeType` values go after `Comment`; tokens are serialized identity; the `static_assert` in `GraphNodeInfo` (`MaterialGraph.cpp` table) must be updated by construction, never suppressed.
- **Byte-identity tripwire:** a graph with NO literals must emit byte-identical snippets to today (pin it with a test). Unwired + no literal = the existing neutral default path.
- **Editor surfaces (ShaderEditorDocument) are NOT gate-compiled as canvas behavior** — widget UX is desk-verified; but note `ShaderEditorDocument.cpp` IS in the ArcaneTests file list (`premake5.lua:528-600`), so it must COMPILE clean and its pure helpers stay testable.
- Exception-free, ASCII, UTF-8 no BOM; comments explain WHY and cite implementation lines actually read; never modify vendored code.
- `logo_showcase.arcmat` lives in the SIBLING repo (`D:/dev/starworks/Aphelyon/Content/`) — edit the file there; never commit in that repo (user's call).

## Verified facts (re-verify at the step that uses them)

- `argOr` unconnected-input seam: `MaterialGraph.cpp:563-571`; neutral default is a width-1 string `Adapt`-ed to target (`:197-205`). Non-zero neutrals exist (clamp max, pow exponent, TilingOffset `v.uv`).
- Dynamic-width rule (literals must not perturb): `MaterialGraph.cpp:542-556` — min of CONNECTED non-scalar dynamic inputs, else 1.
- `inputLink` map (`:305`) resolves wires; `GraphNode` struct at `MaterialGraph.hpp:155-185`; Custom pins are per-node data (`:178-184`).
- Serialization: `GraphToJson`/`GraphFromJson` (`MaterialGraph.cpp:1010+`); per-node extras are optional keys (`value`, `param`, `mask`, `slot`, `custom`, `comment`); unknown node TYPES are refused, unknown KEYS are ignored — `pinDefaults` rides the ignored-key tolerance for old engines.
- `kNodeInfos` table + pin arrays: `MaterialGraph.cpp:18-100`; fixed-out-1 with dynamic input precedent: `SimpleNoise`. Static assert: `GraphNodeInfo` (`:~103-108`).
- The self-heal path regenerates BOTH snippets for graph-only files (`MaterialAsset.cpp:233-251`, fixed @02c4d82f) — the re-authored showcase can stay graph-only.
- Existing graph tests: `Tests/src/MaterialGraphTest.cpp` (tag `[material]`; temp-dir convention as in `MaterialAssetTest.cpp:19-27`).

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Arcane/src/Arcane/Material/MaterialGraph.hpp` (modify) | `GraphPinLiteral` + `GraphNode::pinLiterals` + lookup helper; 11 enum values. |
| `Arcane/Arcane/src/Arcane/Material/MaterialGraph.cpp` (modify) | Serialization, `argOr` literal check, 11 table rows + emission cases. |
| `Arcane/ArcaneEditor/src/ShaderEditorDocument.cpp` (modify) | Pin widgets, undo wiring, palette (if hand-listed). |
| `Arcane/Tests/src/MaterialGraphTest.cpp` (modify) | Literal + batch-2 + byte-identity tests. |
| `D:/dev/starworks/Aphelyon/Content/logo_showcase.arcmat` (modify) | Re-authored acceptance content. |

---

### Task 1: Engine pin literals

**Files:** `MaterialGraph.hpp` (data), `MaterialGraph.cpp` (serialization + argOr), `MaterialGraphTest.cpp` (tests).

**Interfaces produced:**
```cpp
struct GraphPinLiteral { std::uint32_t pin = 0; float v[4] = {0,0,0,0}; };
// on GraphNode:
std::vector<GraphPinLiteral> pinLiterals;                  // user-set only; sparse
const GraphPinLiteral* FindPinLiteral(std::uint32_t pin) const;   // null when absent
```

- [ ] **Step 1:** Re-read `argOr` + the width-resolution block (`MaterialGraph.cpp:530-576`) and the per-node serialization switch (`:1029-1092`, `:1140-1200`). Decide the Custom-pin question at the seam per the spec (v1 exclusion is acceptable — if excluded, `FindPinLiteral` is simply never consulted for Custom nodes because their `argOr` path differs; document whichever falls out).
- [ ] **Step 2 (RED):** Tests in `MaterialGraphTest.cpp`:

```cpp
TEST_CASE("pin literal feeds an unwired input and beats the neutral default", "[material][graph]")
{
    // Graph: const_float(2) -> mul.a ; mul.b UNWIRED with literal 3.0 -> output
    // Expect the emitted mul statement to contain "* 3" (literal inline, no extra line).
    // Second graph: clamp.x wired, clamp.max UNWIRED with literal 0.5 -> emission
    // contains "0.5" where the neutral "1" default would have been.
}
TEST_CASE("pin literal on a dynamic pin stays scalar and does not pin width", "[material][graph]")
{
    // add.a wired to const_float2, add.b literal 1.5 -> resolved width 2, "(1.5).xx" splat.
}
TEST_CASE("graph without literals emits byte-identical snippets", "[material][graph]")
{
    // Build a representative graph (sample + mul + output), run GenerateGraphSnippet
    // twice: once from a graph object round-tripped through JSON WITHOUT pinDefaults.
    // CHECK the snippet strings are IDENTICAL to a golden generated pre-change?
    // No golden files in this suite: instead assert the two runs match each other AND
    // assert the emitted text contains the existing neutral "0.0" path for one
    // deliberately unwired pin (pins today's behavior surviving).
}
TEST_CASE("pinDefaults serialization round-trip and tolerance", "[material][graph]")
{
    // Round-trip: literal on fixed float2 pin (array) + dynamic pin (scalar number).
    // Tolerance: hand-JSON with value as number AND as array both load; unknown pin
    // index warns + drops; absent field -> empty pinLiterals.
}
```
(Write real graphs with the existing test helpers; the comments above are the required assertions, not placeholders to skip.)

- [ ] **Step 3:** Implement. Serialization writes `"pinDefaults": [{"pin":N,"value":X}]` only when non-empty (scalar number for 1-lane, array otherwise); reader tolerant per the spec. `argOr` becomes: unconnected -> literal (formatted like Const emission at the literal's lane count, then `Adapt` to target) else neutral default. Width loop untouched.
- [ ] **Step 4:** Build (VS 18), `[material]` green, full gate seed 6, exact delta accounting. Commit: `feat(arcane): inline pin literals on unwired graph inputs`.

---

### Task 2: Library growth batch 2

**Files:** `MaterialGraph.hpp` (enum), `MaterialGraph.cpp` (table + cases), `MaterialGraphTest.cpp`.

- [ ] **Step 1 (RED):** Emission tests, one per node minimum, plus the width-sensitive cases:

```cpp
// exp/negate/floor/ceil/round/sign/normalize: unary dynamic -- wire const_float2,
//   expect intrinsic over width 2 ("exp(", "-(", "floor(" ...).
// length/distance/dot: dynamic in, out width 1 -- wire float2 inputs, expect the
//   scalar local ("float _nX = length(...)"), and downstream splat when consumed
//   by a float2 consumer.
// panner: unwired uv -> "v.uv + Time * ..."; wired speed literal via Task 1 -> the
//   flagship combo test: panner with speed LITERAL (0.3, 0.0) and unwired uv emits
//   "v.uv + Time * float2(0.3, 0)"-shaped text (exact formatting per Const emission).
// serialization: every new token round-trips through GraphToJson/FromJson.
```

- [ ] **Step 2:** Implement: 11 enum values after `Comment`; 11 `kNodeInfos` rows (tokens per the spec table; `Length`/`Distance`/`Dot` use a fixed `kOut1`, `Panner` gets `kPannerIn = { {"uv",2}, {"speed",2} }` + `kOut2`); emission cases in the codegen switch (`negate` parenthesizes: `-(x)`; `panner` uses `argOr(uvPin, 2, "v.uv")` — the TilingOffset pattern). Update the static_assert by construction.
- [ ] **Step 3:** Build, `[material]` green, full gate seed 6, delta accounting. Commit: `feat(arcane): shader graph library batch 2 -- 11 nodes`.

---

### Task 3: Editor widgets + palette + acceptance content

**Files:** `ShaderEditorDocument.cpp`; `D:/dev/starworks/Aphelyon/Content/logo_showcase.arcmat`.

- [ ] **Step 1:** Plan-time verify items 1-2 from the spec at the code: find the node-body draw site (where input pins render), the graph-undo op idiom (how existing node/param edits create undo entries and coalesce drag gestures), and the palette source (info-table-driven vs hand list). Follow what is actually there; report the idiom used.
- [ ] **Step 2:** Implement the pin widget: on an UNWIRED input pin, a compact width-matched `DragFloat`/`DragFloat2`/`DragFloat4` (scalar for dynamic pins) bound to the node's literal (creating the entry on first edit, removing it is NOT v1 — absent-until-touched only); hidden while wired. Edits create/coalesce undo entries via the existing idiom. If the palette is a hand list, add the 11 nodes.
- [ ] **Step 3:** Re-author `logo_showcase.arcmat` (graph-only, same Guid): literals replace all eight Const nodes; `Exp` chain replaces the SheenFalloff Custom island; `Negate` replaces the −1; `Panner`/literals simplify the sweep; halo island stays. Target ≤ ~30 nodes. Validate by loading in the editor (self-heal generates snippets; canvas must show no error badges).
- [ ] **Step 4:** Build, exe-timestamp check (`ArcaneEditor.exe` newer than editor src), FULL gate BOTH seeds with final delta accounting vs 30304/595. Commit engine-repo changes: `feat(editor): inline pin literal widgets and batch-2 palette`. (Aphelyon file: leave uncommitted, user's repo.)
- [ ] **Step 5:** Desk list for the arc: widget editing + undo/redo of a literal drag; wire-then-unwire restores the value; the re-authored showcase renders identically to the 48-node version; palette shows all 11; error badges still node-accurate on a deliberately broken Custom island.

---

## Self-Review

**Spec coverage:** §1 -> Task 1 (data/serialization/argOr/precedence/byte-identity); §2 -> Task 2 (all 11, width rules, Panner neutral uv); §3 -> Task 3 Steps 1-2 (widgets/undo/palette); §4 -> tests in Tasks 1-2 + Task 3 Step 3 acceptance content + Step 5 desk list. Non-goals respected (no subgraphs, no params-panel changes, SSA kept).
**Placeholders:** test blocks state required assertions with real construction left to the implementer against existing helpers — the assertions and expected emission fragments are specified; no TBDs.
**Type consistency:** `GraphPinLiteral`/`pinLiterals`/`FindPinLiteral` (Task 1) are the names Task 3's widget binds; `kPannerIn`/`kOut1`/`kOut2` follow the existing pin-array naming.
