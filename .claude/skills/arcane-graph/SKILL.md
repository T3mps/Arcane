---
name: arcane-graph
description: Use when working out how Arcane files, symbols, subsystems or design docs relate before changing or explaining them, for impact analysis of an interface change, "what uses X", "which spec governs Y", or tracing how two subsystems connect.
---

# Querying the Arcane knowledge graph

## Overview

`graphify-out\graph.json` is a knowledge graph of the engine: the code structure of every
first-party module plus Astra, Manifold2D and Mosaic, merged with concepts, rulings and
cross-references from `docs\` (specs, plans, research, audits). Grep finds files that NAME a
symbol; the graph also finds the specs, plans and rulings that govern it and the subsystems it
bridges. Use it as the map, then confirm the specific file and line in source.

## Quick reference (repo root)

| Question | Command |
|---|---|
| **Impact analysis: every dependent file plus the governing specs/plans** | `python .claude/skills/arcane-graph/neighbors.py ViewTransform` (`--docs-only`, `--exact`) |
| Broad context around a symbol or concept | `graphify query "ViewTransform" --budget 2000` |
| Trace one path in depth | `graphify query "ViewTransform" --dfs --budget 2000` |
| How two things connect | `graphify path "ViewTransform" "GpuScene"` |
| What a node is and its neighbours | `graphify explain "PhysicsWorld"` |
| Refresh after code or docs changed | `/graphify . --update` |

`graphify` may not be on PATH; it is installed as a uv tool (`uv tool dir` -> `graphifyy\Scripts\graphify.exe`).

## Rules

- **Graph plus grep, never either alone.** For impact analysis run `neighbors.py` AND
  `grep -rl <Symbol> --include=*.cpp --include=*.hpp`, then union them. Measured on `ViewTransform`:
  the graph surfaced files and governing docs grep missed, and grep caught test files the graph
  missed (tests that use the type without an extracted reference edge).
- **One symbol, several nodes.** The code node from the AST pass and the concept nodes that docs
  mint for it are separate; `neighbors.py` unions them by label, `--exact` keeps only exact labels
  (usually just the code node, so it drops the docs).
- **Query with exact symbol or concept names, not sentences.** Plain words are matched as node
  names: "what depends on ViewTransform" also seeds an unrelated `DependsOn` node.
- **Raise `--budget`.** The default truncates hard and says so; the answer is often in the cut part.
- **Treat INFERRED `calls` edges on generic names (`readback`, `local`, `align`) as name
  collisions** until the source confirms them. Substring matching also collides on prefixes:
  `SetView` matches `SetViewMode` and unrelated Astra nodes.
- **Doc nodes come from header, ruling and heading level reads of long plans**, not their task
  code. Absence from the graph is not proof of absence from a doc; grep `docs\` to confirm.
- **Check freshness.** If the area changed since the last build (compare `graphify-out\manifest.json`
  with `git log`), update first.

## If `graphify-out\` does not exist

The graph is gitignored; each machine builds its own with `/graphify .`. Scope is fixed by the
root `.graphifyignore` (first-party code and docs plus the three Starworks libraries under
`ThirdParty\`; no other vendored code, no images). Do not change that scope to make a build smaller.
