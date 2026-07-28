# Shader Graph Upgrade: Inline Pin Literals + Library Batch 2 — Design

**Date:** 2026-07-28
**Motivation:** the graph-authored `logo_showcase` shimmer needed 48 nodes; eight
were bare Float constants feeding one pin each, and a `Custom` island existed
only because the library lacks `Exp`. The compiled-HLSL shape (SSA
one-statement-per-node) is intentional and stays (node-accurate error badges
via `lineNodeIds`; DXC folds it for free) — the fix is authoring ergonomics.
**User-locked scope (approved in sections):** Approach A (true per-pin
literals) + all four node packs (11 nodes).

## Section 1 — Inline pin literals (engine)

Unity SG / UE parity: an UNWIRED input pin can carry a user-set constant.

- **Data:** `GraphNode` gains a small per-node store of pin literals
  (`pin index -> float[4]`, lane count = the pin's declared width; DYNAMIC
  (width-0) pins store a SCALAR). Only user-set literals are stored; absent =
  today's neutral default. The value SURVIVES wiring (ignored while a link
  exists, restored on unwire) — SG behavior.
- **Serialization:** append-only node field `"pinDefaults"` (array of
  `{"pin": N, "value": <number | [floats]>}`); reader tolerant of both value
  shapes and of unknown pins (dropped with a warn, file kept intact —
  the GraphFromJson refusal style stays reserved for structural damage).
- **Codegen:** one seam — `argOr` (`MaterialGraph.cpp:563-571`) checks the
  node's literal for the pin BEFORE the neutral-default string. Literals
  therefore override non-zero neutrals (Clamp max, Power exponent,
  TilingOffset/Panner's `v.uv`) with exactly a wire's precedence. Emission is
  inline into the consumer's statement: no new line, no `lineNodeIds` change.
- **Width rule untouched:** literals never participate in dynamic-width
  resolution (`MaterialGraph.cpp:542-556`) — scalars splat and fixed-width
  literals sit on fixed-width pins. Unwired + no literal = byte-identical
  codegen to today.

## Section 2 — Library growth batch 2 (11 nodes)

APPEND-ONLY after `Comment` (enum values index `kNodeInfos`; tokens are the
serialized identity; the static_assert pins table coverage). All lowercase
snake tokens.

| Node | Token | Pins in | Out | Emission |
|---|---|---|---|---|
| Exp | `exp` | x (dyn) | dyn | `exp(x)` |
| Negate | `negate` | x (dyn) | dyn | `-x` (parenthesized) |
| Floor / Ceil / Round / Sign | `floor` `ceil` `round` `sign` | x (dyn) | dyn | intrinsic |
| Normalize | `normalize` | x (dyn) | dyn | `normalize(x)` |
| Length | `length` | x (dyn) | **1** | `length(x)` (`SimpleNoise` fixed-out precedent) |
| Distance | `distance` | a, b (dyn) | **1** | `distance(a, b)` |
| Dot | `dot` | a, b (dyn) | **1** | `dot(a, b)` |
| Panner | `panner` | uv (2, neutral `v.uv` — the TilingOffset pattern), speed (2) | 2 | `uv + Time * speed` |

Panner's `speed` is the flagship inline-literal pin. Scalar-out nodes adapt
their dynamic inputs to the resolved width first (both pins of Distance/Dot
at the same width), then collapse to width 1.

## Section 3 — Editor (shader graph canvas)

- Unwired input pins render a compact width-matched drag widget beside the
  pin label (1/2/4 drags; scalar on dynamic pins). Wiring hides the widget;
  unwiring restores the stored value. No params-panel changes (literals are
  per-node, not params).
- Edits ride the shader editor's EXISTING graph-undo gesture idiom
  (plan-time verify: the exact op pattern, drag-gesture coalescing, and that
  widget interaction does not fight imgui-node-editor's canvas drag).
- The node palette/search picks up the 11 nodes automatically if it iterates
  the info table (plan-time verify; extend by hand if it is a hand list).

## Section 4 — Validation + testing honesty

- **Gate-coverable (CPU):** per-node emission tests for all 11 (including
  width behavior: scalar-out collapse, Panner's neutral uv); literal
  precedence tests (literal beats neutral default incl. a non-zero neutral;
  wire beats literal; no literal = today's emission byte-identical);
  serialization round-trip + tolerance (number and array value shapes,
  unknown pin index warns and drops, absent field = no literals).
- **Acceptance content:** re-author `logo_showcase.arcmat` (Aphelyon repo)
  with the new toolkit — SheenFalloff Custom island becomes pure nodes via
  `Exp`; `Negate` removes the −1 Const; literals remove all eight Const
  nodes; `Panner`/literal cleanup on the sweep chain. Target: ≤ ~30 nodes,
  with the halo loop as the single remaining Custom island (loops are
  fundamentally island-shaped in every engine's graph).
- **Desk-only:** widget interaction/undo/canvas feel; the re-authored
  material rendering identically to the 48-node version (same math).

## Non-goals

- No compound/macro node system, no node grouping/subgraphs.
- No Custom-island removal for loops (the halo stays an island by design).
- No params-panel or instance-override changes (literals are not params).
- No changes to SSA emission or `lineNodeIds` (intentional design, kept).

## Plan-time verification items

1. ShaderEditorDocument: the graph-edit undo idiom (op type, gesture
   coalescing) and the node-body draw site where pin widgets go.
2. The node palette source (table-driven vs hand list).
3. `GraphNodeInputPin`/pin-count helpers' handling of Custom nodes (whose
   pins are per-node data) — pin literals on Custom pins should work or be
   explicitly excluded (decide at the code seam; exclusion is acceptable v1).
4. Exact neutral-default strings currently passed at each `argOr` call site
   (the literal must adapt from ITS width the same way `Adapt` handles the
   default's width-1 path, `MaterialGraph.cpp:197-205,563-571`).
