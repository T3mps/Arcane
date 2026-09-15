# F2c Plan 2 (runtime + editor) — closeout notes

Task 13 of `docs/plans/2026-09-10-f2c-mesh-import-plan2-runtime-editor.md`.
All 12 tasks (plus one unplanned fix task, 12a) are committed at HEAD
`7f82ddb1` when this closeout was written. This document is the
end-to-end verification record and the handoff to whoever picks up the
standing debts below — there is no Plan 3 queued.

## Commit list

**Plan 1** (`docs/plans/2026-09-10-f2c-mesh-import-plan1-pipeline.md`):
`a21e19ef..9b0ad0f5` — 16 implementation tasks (`2bb0c5ef..4a968173`), the
whole-branch final-review fix wave, and the closeout doc + its correction
(`e39bbd53`, `9b0ad0f5`). Recorded in
`docs/plans/2026-09-10-f2c-mesh-import-plan1-closeout.md`.

**Plan 2** (this plan): `29490876~1..7f82ddb1`:

| Task | Commit | Subject |
|---|---|---|
| T1 | `29490876` | feat(render): the mesh residency byte budget and its frame-boundary LRU |
| T2 | `83eb99c9` | feat(render): NriMeshBufferCache -- resident mesh geometry with a byte-budget LRU |
| T3 | `eb38caee` | feat(render): the vehicle owns the mesh buffer cache and evicts at frame boundaries |
| T4 | `8f671a81` | refactor(render): mesh geometry draws from the resident cache; the ring path retires |
| T5 | `631f1809` | feat(render): per-section mesh draws with per-slot materials |
| T6 | `4c93612e` | feat(engine): mesh cook completion invalidates geometry and residency |
| T7 | `93ab79ce` | feat(engine): a slot reassignment re-resolves materials without re-uploading geometry |
| T8 | `587db573` | feat(editor): Model kind chrome -- graph hue, rail row, and the companion fold |
| T9 | `f61092d1` | feat(editor): the preview harvester renders mesh assets framed by their bounds (amended once for a degenerate-box fix) |
| T10 | `bcd557c5` | feat(editor): mesh thumbnails -- request, prime, invalidate |
| T11 | `d07eab34` | test(reference): a multi-section imported prop joins the golden scene |
| T12a | `2970ebfc`, `3dcdd3a1` | fix(editor): headless --settle also waits for the cook queue and the thumbnail harvester to go idle; fix(editor): wire meshArtifactFor/cookPending onto the preview harvester so imported-mesh thumbnails resolve |
| T12 | `7f82ddb1` | test(reference): re-bless both golden lanes for the imported prop |

T12a was not in the plan text — it was added mid-arc by the controller to
fix a real defect the golden fixture exposed (see the two RCAs below).
Nothing in Plan 2 is pushed.

## Two things the plan did not anticipate

**(a) Task 12a — the settle barrier and the harvester wiring.** The
editor's headless `--settle` now ALSO waits for the cook queue and the
thumbnail harvester to go idle, not just the shader compiler
(`ArcaneClient/src/Arcane/Host/SettleBound.hpp`, new
`ArcaneTests/src/SettleBoundTest.cpp`, `[settle]` 8 cases / 22 assertions,
+2 `~[gpu]` cases beyond the plan's own table). `EditorApp.cpp` now wires
`Services::meshArtifactFor`/`cookPending` onto the preview harvester
(`EditorApp.cpp:867-897`, mirroring `SceneRenderResolver.cpp:126-143`) —
Task 10 had left these two fields added-but-unwired, calling it Task 10's
own job in its report, and its reviewer independently judged that
correctly out of scope; the gap survived because Task 10's desk check
exercised `reference_cube.arcmesh`, a Cube PRIMITIVE that needs no cooked
artifact, never an Imported mesh. `.gitignore` also gained
`ReferenceProject/Saved/`, which was previously uncovered.

**(b) The re-bless took three rounds, not the plan's target of one.** The
staged `ReferenceProject` trees under `ArcaneRuntime` and `ArcaneEditor`
(both configs) carried a stale `Source/GameApi.hpp`, deleted from SOURCE
by Arcane commit `a5d77e30` (ReferenceGame's move onto
`ARCANE_GAME_MODULE`) but never removed from the staged copies because the
postbuild `{COPYDIR}` step (`premake5.lua:591`/`:682`) is additive-only.
`Project::Open` mounts `Source/` unconditionally and `AssetRegistry`
registers every file it finds there with no kind filter, so the stray
`.hpp` counted as a 16th asset in the editor's Browser health line ("All
16" vs the correct "All 15"), moving the editor-lane golden pixels by a
one-digit caption + badge (721 px, stable) that no amount of re-blessing
under the additive-staging trap could ever converge. Full detail in
`task12-rca-report.md` (round 1: a TOCTOU race in the settle loop, fixed
by 12a but insufficient alone) and `task12-rca2-report.md` (round 2: the
staged-`Source/`-tree phantom asset, the actual root cause). The durable
fix — making the postbuild mirror deletions for `Source/` and `Content/`
subtrees, not just add to them — is **owed, not done**; it is an
engine-build-staging change whose blast radius must be scoped carefully
(a naive recursive mirror would also wipe staged `Intermediate/` cooked
artifacts and `Saved/`, which are legitimately staged-only).

## Step 1 — Zero-legacy sweep

`git grep -n -w` (then re-run case-insensitively; the two came back
identical, so the case-sensitive pass had no blind spot) for each retired
ring-path symbol, path-scoped to `ArcaneClient ArcaneEditor ArcaneRuntime
ArcaneTests scripts` (`docs/`, `.superpowers/`, `ThirdParty/`, `bin/`, and
`Intermediate/` are excluded by construction — none of those five symbols
live outside the five searched trees, and `bin/`/`Intermediate/` are build
output under those trees, explicitly filtered):

- **`kInitialUploadSlots`** — 1 hit, `ArcaneClient/src/Arcane/Render/Nri/nodes/MeshNode.cpp:230`, a **comment** ("a frame carrying more than kInitialUploadSlots distinct..."), narrating history. No live symbol.
- **`m_warnedRingOverflow`** — 0 hits, both cases.
- **`uploadFor`** — 0 hits, both cases.
- **`MeshNode::Upload`** — 0 hits, both cases.
- **`MeshInstance::mesh` as a POINTER** — 6 hits, all **comments or a static_assert**, and every one of them documents the CURRENT (Guid) shape, not the retired pointer:
  - `MeshNode.cpp:936` — comment ("an empty slot is not an error -- see MeshInstance::mesh")
  - `Scene/Components.hpp:161` — comment
  - `Scene/MeshSubmissionSystem.hpp:116` — comment ("MeshInstance::mesh is the component's Guid (F2c s7.2)")
  - `Scene/SceneResources.hpp:161` — comment
  - `Project/MaterialPreviewHarvester.cpp:1037` — comment
  - `ArcaneTests/src/MeshNodeTest.cpp:173` — `static_assert(std::is_same_v<decltype(Arcane::MeshInstance::mesh), Arcane::Guid>)`, i.e. the sweep's own proof the field is a `Guid`, not a pointer.

**Verdict: PASS.** Every hit outside a comment/static_assert narrating the current shape is zero; the ring path for mesh geometry is retired, and this sweep is what proves it (per the brief's own framing) rather than recollection.

## Step 2 — Ring-usage sanity

`grep -n "ring.Allocate" ArcaneClient/src` is **not** empty — 6 hits, all
in the 2D/UI batch paths, confirming the ring is alive and still doing its
narrow job:

```
ArcaneClient/src/Arcane/ImGui/ImGuiNri.cpp:790-791            (ImGui vertex/index alloc)
ArcaneClient/src/Arcane/Render/Nri/nodes/Batch2DNode.cpp:1349-1350   (2D sprite batch vertex/index alloc)
ArcaneClient/src/Arcane/Render/Nri/nodes/PickOutlineNodes.cpp:338-339 (pick/outline vertex/index alloc)
```

**Verdict: PASS.** "Retired for mesh geometry" is the narrow claim Step 1
proves; this step proves it was never widened by accident to "retired,
full stop."

## Step 3 — Budget-constant sanity

`grep -rn "kMeshResidencyBudgetBytes"` across the five trees:

- **Definition:** `ArcaneClient/src/Arcane/Render/Nri/MeshResidencyBudget.hpp:37` — `inline constexpr std::uint64_t kMeshResidencyBudgetBytes = 512ull * 1024ull * 1024ull;`
- **Eviction call:** `ArcaneClient/src/Arcane/Render/Nri/NriMeshBufferCache.cpp:272` — `const std::vector<Guid> drop = SelectEvictions(live, budget, frameCounter);`, where `budget` defaults to `kMeshResidencyBudgetBytes` at `NriMeshBufferCache.hpp:113`.
- **Test:** `ArcaneTests/src/MeshResidencyBudgetTest.cpp:84` — `CHECK(kMeshResidencyBudgetBytes == 536870912ull);`

`grep -rn "512ull\|536870912\|512 \* 1024\|512\*1024"` across the same
five trees returns exactly those same two hits (the definition and the
test's literal-equality check) — **no second literal 512 anywhere.**

**Verdict: PASS.** One home for the number, matching the brief and
leaving exactly one place for the parked cvar arc to move it from.

## Step 4 — Full suites, Debug and Release

Both configs rebuilt from a clean invocation, foreground, in this
session:

- `msbuild Arcane.slnx /p:Configuration=Debug /m` — exit 0, 0 errors (incremental; the previously-built HEAD binary was reconfirmed rather than trusted).
- `msbuild Arcane.slnx /p:Configuration=Release /m` — **Build succeeded, 0 Warning(s), 0 Error(s)**, full rebuild, 3 min 16 s.

All runs below were executed from each config's own `ArcaneTests` exe
directory, `--order rand`, in the foreground.

| Config | Filter | Seed | Result |
|---|---|---|---|
| Debug | `~[gpu]` | `3033912028` | **1747 cases (1743 passed, 4 skipped), 57190/57190 assertions** |
| Debug | unfiltered | `2874931107` | **1790 cases (1786 passed, 4 skipped), 119559/119559 assertions** |
| Release | `~[gpu]` | `2048139651` | **1747 cases (1743 passed, 4 skipped), 57190/57190 assertions** |
| Release | unfiltered | `3854477921` | **1790 cases (1786 passed, 4 skipped), 119559/119559 assertions** |

**Debug and Release agree exactly on both filters** (matching Plan 1's
own precedent that the two configurations must agree). The 4 skips in
every run are the same pre-existing `IdeLaunchTest.cpp` desk-only probes
("ARCANE_IDE_DESK not set"), unrelated to this plan. The Release
unfiltered run (which includes the `[gpu][witness]` cases that spawn real
`ArcaneRuntime`/`ArcaneEditor` subprocesses against `ReferenceProject`)
was run immediately after Step 5's Release golden-gate pass had already
rebuilt the single-slot `ReferenceProject/Binaries/ReferenceGame.dll` for
Release — avoiding Plan 1's own documented CRT-mismatch trap
(`ucrtbased.dll` vs `ucrtbase`-family imports) by construction rather than
by accident. Confirmed directly: before any Release rebuild, the
in-tree `ReferenceProject/Binaries/ReferenceGame.dll` still imported
`ucrtbased.dll` (Debug CRT, left over from Task 12/12a's Debug-only bless
work) — this session never ran the Release suite against a mismatched DLL.

### Static-vs-live delta reconciliation

The brief's own printed baseline (55464/1535) and Plan 1's final baseline
(1620 cases / 56118 assertions, `~[gpu]`) both predate unrelated arcs
between Plan 1's close and Plan 2's start (the ledger has no
Plan-2-Task-1 baseline number for exactly this reason) — so the delta is
derived **statically**, from the diff, not from either absolute number.

`git diff 29490876~1..HEAD -- ArcaneTests/src`, counting `TEST_CASE(`
lines (no `TEMPLATE_TEST_CASE(` was added or removed anywhere in the
range): **41 net additions, 0 removals.** Reading each added case's tag
from the diff's own context (every added `TEST_CASE` carries its tag on
the same or the immediately following line, entirely inside the added
hunk):

- **9 are `[gpu]`-tagged**: 3 `[gpu][meshnode]` pixel cases (T4/T5 — "a
  cube drawn from the resident cache", "an instance whose mesh is not
  resident is SKIPPED", "two sections of one mesh draw with distinct
  base colours") + 6 `[gpu][meshcache]` pixel cases (T2/T3 — upload/evict/
  release/refuse/vehicle-ownership/resize-does-not-release).
- **32 are non-`[gpu]`** (`~[gpu]`): the remaining editor/render/mesh/
  settle cases.

Per-task attribution, from `git diff <task-base>..<task-head> --
ArcaneTests/src` for each commit range in turn (T9's range spans its
amend `f61092d1`; T12's range is `3dcdd3a1..7f82ddb1`, the bless-only
commit):

| Task | Total added | of which `[gpu]` | Plan's own table |
|---|---|---|---|
| T1 | 6 | 0 | +6 ✓ |
| T2 | 8 | 4 | +4/+4 ✓ |
| T3 | 2 | 2 | +0/+2 ✓ |
| T4 | 3 | 2 | +1/+2 ✓ |
| T5 | 5 | 1 | +4/+1 ✓ |
| T6 | 3 | 0 | +3 ✓ |
| T7 | 2 | 0 | +2 ✓ |
| T8 | 4 | 0 | +4 ✓ |
| T9 | 4 | 0 | +4 ✓ |
| T10 | 1 | 0 | +1 ✓ |
| T11 | 1 | 0 | +1 ✓ |
| T12a | 2 | 0 | (unplanned; ledger's own corrected figure) |
| T12 | 0 | 0 | (bless-only, no test changes) |

**Sum: 6+8+2+3+5+3+2+4+4+1+1+2+0 = 41 = 32 `~[gpu]` + 9 `[gpu]`.** This
matches the brief's expected **+30/+9 (T1-T11) plus 12a's own corrected
+2 = +32/+9** exactly. **No task's stated delta was found to be wrong.**
(The ledger's *original*, uncorrected 12a entry claimed "+6 vs Task 11's
1741" — the ledger itself already superseded that with a controller
derivation, "12a is +2 `~[gpu]` cases", after its reviewer caught the
implementer comparing 12a's TOTAL against Task 11's PASSED count rather
than passed-to-passed; this session's independent diff-count confirms the
corrected +2, not the original +6.)

Cross-checked against the live run: this session measured **1747** total
cases (`~[gpu]`, both configs). Working backward from the per-task table,
the case count immediately before Task 9 (i.e. after T1-T8) is
`1747 - (4+1+1+2) = 1739` — matching the ledger's own Task 9 report
verbatim ("1743 cases (1739 passed, 4 pre-existing skips)"). The chain
`1739 -> 1743 (T9, +4) -> 1744 (T10, +1) -> 1745 (T11, +1) -> 1747 (12a,
+2)` is internally consistent with every task's own reported total, and
Task 12 (bless-only) added zero, landing this session's live count
exactly on 1747. Both independent methods (whole-range diff count, and
the chained per-task ledger totals) agree.

## Step 5 — `golden-gate.ps1`, Release then Debug

**Release stray sweep (before running the gate).** The Release stager
trees had never been swept — Task 12's sweeps only ever touched the
Debug-configuration staged trees. Confirmed present and removed:

```
bin/Release-windows-x86_64-md/ArcaneRuntime/ReferenceProject/Source/GameApi.hpp   (removed)
bin/Release-windows-x86_64-md/ArcaneEditor/ReferenceProject/Source/GameApi.hpp    (removed)
```

`ArcaneTests`'s own staged tree has no `Source/` directory at all (matches
the ledger's Task 12 note). `Content/` trees for both Release-staged
hosts were byte-for-byte diffed against SOURCE and came back empty (no
strays there). No pre-existing diff PNGs were present anywhere under
`bin/` to preserve before the run.

**Release gate** (`powershell scripts\golden-gate.ps1 -Configuration
Release`): rebuilt `ReferenceProject.slnx` for Release as its own step
one (the single-slot precondition, honoured), restaged
`ReferenceGame.dll` + `Content/` + `Intermediate/Artifacts` beside both
hosts, then ran all four lanes:

| Combo | Verdict | Detail (verbatim from the JSON) |
|---|---|---|
| ArcaneRuntime/dx12/runtime-scene | PassedOnFallback | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared (expected backend)` |
| ArcaneRuntime/vulkan/runtime-scene | Passed | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=backend` |
| ArcaneEditor/dx12/editor-ui | Passed | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared` |
| ArcaneEditor/vulkan/editor-ui | Passed | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared` |

`gatePassed: true`. Saved verbatim as `task13-release-golden-gate-summary.json`.

**Debug gate, re-run as the final word**
(`powershell scripts\golden-gate.ps1 -Configuration Debug`): rebuilt
`ReferenceProject.slnx` for Debug (flipping the single slot back to Debug
CRT — the state a desk pass expects), restaged, ran all four lanes:

| Combo | Verdict | Detail (verbatim from the JSON) |
|---|---|---|
| ArcaneRuntime/dx12/runtime-scene | PassedOnFallback | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared (expected backend)` |
| ArcaneRuntime/vulkan/runtime-scene | Passed | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=backend` |
| ArcaneEditor/dx12/editor-ui | Passed | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared` |
| ArcaneEditor/vulkan/editor-ui | Passed | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared` |

`gatePassed: true`. Saved verbatim as `task13-debug-golden-gate-summary.json`.

No diff PNGs were produced by either run (both gates passed cleanly), and
`git status --short ReferenceProject/` stayed empty throughout — neither
gate run wrote a bless; both simply verified. Both Debug- and
Release-staged `Source/` trees were re-checked after the Debug run and
remain free of `GameApi.hpp`.

## Step 6 — Performance observation (recorded, not asserted)

**The prop's byte count**, computed from `golden_prop`'s actual cooked
mesh artifact (`ReferenceProject/Content/meshes/golden_prop.glb` ->
artifact `1c12fa58bc5a0766.arcart`, confirmed by matching its header's
`sourceGuid` hi/lo, `0x87d2729a374a4d19` / `0xaec590526617a36a`, against
`golden_prop.arcmesh`'s `importedSource`, `87d2729a-374a-4d19-aec5-
90526617a36a`):

- `vertexCount = 8`, `indexCount = 18`, `sectionCount = 3` (two sections
  on the "Metal" slot, one on "Paint" — matching the commit body's
  description of the fixture).
- `vertexBytes = 8 * sizeof(MeshArtifactVertex=32) = 256` — matches the
  artifact's own `VertexData` section size (256) exactly. **Format-pinned**:
  `ArtifactFormat.hpp:206` has `static_assert(sizeof(MeshArtifactVertex)
  == 32, ...)`, so this term cannot silently drift on another toolchain.
- `indexBytes = 18 * 4 = 72` — matches the artifact's own `IndexData`
  section size (72) exactly. Also format-pinned (`indexWidth == 4` is
  asserted on every read, per `ArtifactFormat.hpp`'s own reader rules).
- `sectionBytes` (`NriMeshBufferCache::SectionBytes`, client-side): `3 *
  sizeof(MeshSection) + len("Metal")+len("Metal")+len("Paint")`. Client
  `MeshSection` (`Render/MeshBuilder.hpp`) is `std::string name` + three
  `uint32_t`, with **no checked-in `static_assert` on its size anywhere
  in the codebase** (unlike `MeshArtifactVertex` above). Measured by
  compiling a byte-identical struct out-of-repo, in the session
  scratchpad, with this desk's own MSVC toolset (`cl /std:c++20`, VS
  2026 18.8.1, x64): `sizeof(MeshSection) == 48` on this machine's STL.
  **This one input is soft**: `std::string`'s layout is an
  MSVC-STL-ABI detail, not a format guarantee, so a different compiler
  or STL version could measure a different `sizeof(MeshSection)` without
  the artifact format itself changing at all. `sectionBytes = 3*48 + 15
  = 159` — this figure, and everything derived from it below, inherits
  that softness.
- `MeshResidencyBytes(256, 72, 159) = 2*(256+72) + 159 = 656 + 159 =
  815 bytes` total resident cost for this one mesh — CPU copy kept +
  GPU vertex/index buffers (a verbatim mirror of the CPU copy, both
  format-pinned) + the CPU-only section-table overhead (the one
  soft term, `sectionBytes = 159` of the 815). **815 B is therefore not
  itself format-pinned** — reproducible on this exact desk/toolchain, but
  not guaranteed byte-identical on another one, unlike `vertexBytes`/
  `indexBytes` which are.

Against the 512 MiB (536,870,912-byte) budget, `golden_prop` alone is
~0.00015% of it either way — nowhere near eviction pressure, consistent
with `MeshResidencyBudgetTest.cpp`'s "nothing is evicted while under
budget" case; the softness above does not change that conclusion by any
margin that matters.

**`ResidentBytes()` after a few seconds of running — no instrument
exists, stated plainly rather than invented.** `grep -rn "ResidentBytes"
ArcaneClient/src ArcaneEditor/src ArcaneRuntime/src ArcaneTests/src`
finds exactly: the accessor's own definition and declaration
(`NriMeshBufferCache.cpp:111`, `.hpp:128`), one internal call that only
fires a one-time WARN when resident bytes exceed budget
(`NriMeshBufferCache.cpp:283-288` — never true here), and one test-level
call (`NriMeshBufferCacheTest.cpp:179`) that checks it against a
hand-built fixture with no device or live host involved. Neither
`ArcaneEditor` nor `ArcaneRuntime` logs, prints, or surfaces
`ResidentBytes()` anywhere in a live run — there is no `--perf` log line,
no `[nri-graph]` diagnostic, no overlay panel. **No live measurement is
recorded here because none exists to take**, per the brief's own
instruction not to invent one.

**The ring-vs-resident reasoning §12's "per-frame ring re-upload cliff"
was raised about**, recorded rather than measured: before Plan 2, the
ring path re-uploaded `vertexBytes + indexBytes` (328 bytes for
`golden_prop`) to the transient `NriUploadRing` on **every frame** the
mesh was visible, for every visible mesh instance — a cost that scales
with frame count and instance count with no ceiling. After Plan 2, that
328-byte transfer happens **once**, at first resolve; every subsequent
frame the mesh stays resident (and keeps being drawn, protecting it from
eviction per `SelectEvictions`'s "never evict an entry drawn this frame"
rule) costs zero re-upload bytes. This is the shape change the plan's
Task 1-5 chain exists to produce; there is deliberately no perf TEST
pinning a frame-time number, because a threshold measured on this desk is
not a fact about any other machine (brief's own stated rule, and Plan
2's self-review record says the same under "Known intentional gaps").

## Step 7 — Standing debts (handed off, not actioned here)

1. **The additive-staging fix is owed, not done.** The postbuild
   `{COPYDIR}` staging step (`premake5.lua:591`/`:682`) copies `Source/`
   and `Content/` into each host's `bin/<Config>/<Host>/ReferenceProject/`
   tree additively — it never mirrors a deletion made in SOURCE. This
   plan swept the symptom twice (Task 12's Debug sweep, this task's
   Release sweep) rather than fix the mechanism, because the durable fix
   has real blast radius: a naive recursive mirror (rsync-style delete-
   what's-missing) would also wipe staged `Intermediate/` cooked
   artifacts and `Saved/` (both legitimately staged-only, never present
   in SOURCE), so the fix needs to scope its mirroring to exactly
   `Source/` and `Content/`, nothing broader. Recorded in
   `task12-rca2-report.md`.
2. **Every parked Minor from the ledger, still open:**
   - Task 9: `kMinRadius = 0.5f`'s floor in `FrameMeshBounds` is
     unconditional (not a near-zero guard like `MeshDocument.cpp:291-293`'s
     own precedent), so a legitimate imported mesh under ~1 m gets the
     same loose framing as a truly-empty one. No golden impact today.
   - Task 10: `PollAssetWatch`'s Mesh branch mirrors only the Material
     branch's mtime bookkeeping, not its richer machinery (log line,
     `MarkAllDirty`, activity feed); and `PrimeFromDisk`'s
     extension-sniffing silently defaults an unrecognized extension to
     `Subject::Material` (currently unreachable) rather than warning.
   - Task 11: `WriteAutoScreenshot()` (the Hub cover thumbnail) is
     skipped by the bare `SaveSceneFile` path used to author the golden
     scene — outside `Content/`, no test/gate/scene effect.
   - Task 12a: the settle barrier **narrows, not eliminates**, the
     TOCTOU it targets (the cook worker flips `m_running` false after
     pushing its result under the lock; a flip between this frame's
     `Pump()` and the settle read is still theoretically possible — the
     window shrank from "an async cook" to "one frame's draw-to-present
     tail"). Also, `StartOneMesh`'s `PendingCook` branch newly reaches a
     `Fail()` call via 12a's Fix B, inferred-correct from code
     (re-armed by `OnCookCompleted`->`InvalidateMesh`) but never
     desk-exercised live (the harvest succeeded on its first attempt
     every time it was tried).
   - Task 12: `golden_prop.arcmesh`'s Browser row is scrolled out of the
     1280x720 `editor-ui.png` capture, so the real thumbnail claim rests
     on the host's `[thumbs] harvested a 64px preview` log line, not on
     anything visible by eye in the blessed image.
3. **The per-slot material list UI is parked to F4/post-F2c** — a model
   is imported, never authored, so `RailKindCreatable` for `Model` stays
   `false` by explicit design (Task 8), and there is no in-editor way to
   reassign a slot's material beyond what already exists on `MeshDocument`.
4. **`kMeshResidencyBudgetBytes` stays a compile-time constant**, awaiting
   the parked cvar arc's own trigger discipline (Step 3 above confirms
   it currently has exactly one home, ready to be moved when that arc
   lands).
5. **The Gacha ABI line is CURRENT, not owed, as of this checkout.**
   `grep -n "kGamePluginABIVersion" ArcaneClient/src/Arcane/Plugin/PluginABI.hpp`
   -> `inline constexpr uint32_t kGamePluginABIVersion = 29;`. `git log
   29490876~1..HEAD -- ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` is
   **empty** — this plan bumped nothing (confirming the plan's own Global
   Constraints intent). Gacha's `D:\dev\starworks\Gacha\Game\
   Aphelyon.arcproj` reads `"engine": { "abi": 29 }` — **already matching**,
   via Gacha's own later, independent work (its commit `23ea4d3a`,
   "restamp Aphelyon.arcproj to engine ABI 29"), not anything this plan
   did. `ReferenceProject.arcproj` agrees at 29 as well. Nothing is owed
   here today; the debt returns only if Arcane's ABI moves again before
   Gacha's next rebuild.
6. **The plan file's own Global Constraints are stale text, not a defect
   to fix**: `docs/plans/2026-09-10-f2c-mesh-import-plan2-runtime-editor.md`
   says `kSceneJsonVersion` 4 and "ABI stays 24" — current source has
   `kSceneJsonVersion = 5` (`ArcaneClient/src/Arcane/Serialization/
   SceneSerializer.hpp:83`, already noted as stale by Task 11's own
   ledger entry) and ABI 29 (above). The plan's own text predates later
   arcs; its governing constants were never meant to be edited after
   the fact, and this closeout is where the correction is recorded.
7. **`sizeof(MeshSection)` has no checked-in `static_assert`, unlike its
   sibling `MeshArtifactVertex` (`ArtifactFormat.hpp:206`).** Step 6's
   815 B resident-cost figure for `golden_prop` depends on a `48` this
   session measured with an out-of-repo scratch probe on this desk's
   MSVC STL, not on anything the codebase pins — a different compiler or
   STL version could measure a different `sizeof(MeshSection)` without
   the on-disk mesh format changing at all. Owed: either add a
   `static_assert(sizeof(MeshSection) == 48, ...)` next to the struct
   (`Render/MeshBuilder.hpp`) so the number becomes a checked fact like
   its vertex-stride sibling, or drop the CPU-only section-table term
   from any future byte-cost reporting and report only the two
   format-pinned terms (`vertexBytes`, `indexBytes`).

## Desk observations, sourced

- **Tasks 9, 10, 11, 12** — their own reports in the task workspace:
  `.superpowers/sdd/2026-09-10-f2c-mesh-import-plan2-runtime-editor/
  task-9-report.md`, `task-10-report.md`, `task-11-report.md`,
  `task-12-report.md` (plus `task-12a-report.md` for the unplanned fix
  task, and `task12-rca-report.md` / `task12-rca2-report.md` for the two
  root-cause investigations).
- **Tasks 6 and 8** predate this ledger (no per-task report files exist
  for them); their desk notes are their own commit bodies, the only
  surviving record:
  - `git show -s 4c93612e` (T6): "Viewport graph only -- document-preview
    NriGraphContext instances are not invalidated on cook (F2b gap; F2c
    s7.3 re-accepts). UniqueImportedCompanion is the mapping. `~[gpu]`
    1729/57067 (+3). `[mesh][host]` 9/139. **Desk check of re-export
    viewport heal still wants a human.**"
  - `git show -s 587db573` (T8): "Companion `.arcmesh` folds under its
    Model (Texture || Model whitelist). Graph accent #5b7fb0 extends
    s11.3 next to Mesh. Rail Create stays off -- a model is imported,
    never authored. `~[gpu]` 1735/57132 (+4). `[editor]` 373/4312." T8's
    body carries no explicit "wants a human" line beyond its own suite
    numbers — its commit is the whole of its desk record.

## Files changed by this task

- `docs/plans/2026-09-10-f2c-mesh-import-plan2-closeout.md` (this file) — new.

This task changed no product code — Steps 1-3 are read-only sweeps, Step
4 rebuilt and re-ran existing suites, Step 5 ran `golden-gate.ps1` (which
rebuilds `ReferenceProject.slnx` and restages content, never touching
tracked SOURCE) and deleted two staged-only stray files under `bin/`
(never tracked by git), and Step 6 is a read-only computation. `git
status --short` shows only this new file plus the four pre-existing,
plan-external untracked paths (`ArcaneAssetPipeline/ArcaneAs.25D4CEF5/`,
`ArcaneEditor/ArcaneEditor/`, `arcbuild/arcbuild/`, `out.txt`) that this
task did not create and must never stage.

**Push only after the desk pass.**

---

# Addendum — 2026-09-14: final-review fix wave (C1-C3, I1-I4)

The whole-branch review over `a04a1e0e..1e1a673b`
(`.superpowers/sdd/2026-09-10-f2c-mesh-import-plan2-runtime-editor/final-review-report.md`)
returned **NOT ready (→ With fixes)**: 3 Critical, 4 Important, ~11 Minor. One
fix wave landed all seven Critical and Important findings plus the cheap
Minors, in four commits on top of `1e1a673b`. No bless, no ABI change, no push.

## What each finding was, and where it was fixed

| # | Finding, in one line | Fix commit | Test |
|---|---|---|---|
| **C1** | `MaterialPreviewHarvester` rendered every mesh asset under one session-fixed synthetic `'MESH'` guid, so harvest #2 HIT harvest #1's resident buffers — mesh A's geometry drawn with mesh B's per-section index ranges (an OOB index read whenever B had more indices), persisted to `Saved/Thumbnails/<B>.png` | `762d5dcc` | NEW `ArcaneTests/src/MeshThumbnailHarvestTest.cpp`, `[gpu][thumbs]` |
| **C2** | `MeshDocument`'s live preview froze on the first geometry it ever built — `RebuildPreviewMesh` never invalidated the fixed `'PRVW'` guid, so all four rebuild paths redrew the old shape | `762d5dcc` | `MeshDocumentTest.cpp`, device-less, through the new `PreviewGeometryInvalidations()` instrument |
| **C3** | Task 7's residency-keep comparison read `{source, importedSource}` only, so a `rings`/`segments`/`subdivisions`/`capsuleLengthRatio` edit took the KEEP arm and drew the new index count against the old index buffer | `7b04b3bc` | `SceneRenderResolverTest.cpp`, two new `[mesh][host]` cases |
| **I1** | `Upload` wrote `CreateCommittedBuffer`'s out-param straight onto the entry's live handle, and the cold-CPU re-upload arm retried a failed upload every frame — one leaked `nri::Buffer` per frame, and the documented memoization never happened | `f84d2205` | `NriMeshBufferCacheTest.cpp`, new device-less "refused ONCE and never retried" |
| **I2** | Eviction freed only the GPU half; the kept CPU copy stopped being counted, so `ResidentBytes()` reported a number the process was not honouring | `f84d2205` | `NriMeshBufferCacheTest.cpp`, new device-less eviction case, plus the existing `[gpu]` case's expectation flipped |
| **I3** | The 512 MiB budget is per-vehicle, not process-wide, and that was recorded nowhere | `f84d2205` | comment only (`MeshResidencyBudget.hpp`) |
| **I4** | `MeshCache::Query` answered `PendingCook` for a guid nobody ever `Request`ed — quiet, retried forever, never reported | `f84d2205` | precondition comment + latched WARN, backed by a `requested` set; no test asserts the WARN (this suite has no logger seam), the precondition is pinned by reading, not by assertion |

## The I2 ruling, recorded

**Eviction ERASES the entry, CPU copy included** — plan Task 2 Step 6, not Task
2 Step 1's shipped `[gpu]` expectation. The plan contradicted itself on this
point and the implementer picked Step 1; the controller ruled for Step 6.

Reasoning: the spec's binding invariant is ONE COMBINED, HONEST byte budget. A
kept-but-uncounted CPU copy makes `ResidentBytes()` a lie, and a
kept-and-counted one can never be shed by an LRU that only frees GPU. The
spec's "CPU copy kept for re-upload after eviction, no disk read" is already
satisfied one layer up: the supply is `SceneRenderResolver`'s in-memory
`MeshTable`, so a re-upload after eviction is a table lookup, not an artifact
read. The `[gpu]` test's "assert the supply was NOT asked again" therefore
FLIPS to `asks == 4`; `MeshResidencyBudget.hpp` and `NriMeshBufferCache.hpp`'s
banners now say where the surviving CPU copy actually lives, and that
`ResidentBytes()` is exactly the resident CPU+GPU bytes. `Release` keeps its
existing semantics (buries everything, empties).

Cost if wrong: one extra `MeshTable` lookup plus one upload on the first draw
after an eviction.

## Minors taken in this wave

- `MeshNode.cpp`/`.hpp`: the stale `kInitialUploadSlots` / "See Upload's own
  comment" prose is gone; `m_residents` carries ONE correct description; the
  table is cleared at the end of `Record` as well as the start of `Prepare`
  (via RAII, so every exit path), removing the dangling-borrow window an
  `InvalidateMeshGeometry` between `Record(N)` and `Prepare(N+1)` opened.
- `MeshNode::Prepare` takes a NULLABLE cache and resolves the pipeline
  unconditionally; only the residency loop is gated (`AddMeshNode`'s call site,
  which lives in `MeshNode.cpp`, not `NriGraphContext.cpp` as the review's
  cross-reference said).
- `SceneRenderResolver.cpp`'s `InvalidateMeshArtifact`: the "GPU FIRST"
  rationale was factually wrong — both calls are synchronous inside one
  function — and now says so.
- `NriGraphContext.cpp`: states that eviction runs only after a successful
  `Execute`, and why that is accepted (behaviour unchanged).
- `MaterialPreviewHarvester.cpp`: `PrimeFromDisk`'s extension sniff `ARC_WARN`s
  (latched) instead of silently defaulting an unresolvable/unknown extension to
  `Subject::Material`; material `Invalidate` and `InvalidateMesh` now erase
  `ready` by id AND subject symmetrically.
- `RuntimeFrame.cpp`: the inline settle predicate carries a one-line
  back-reference to `SettleBound.hpp`'s `SettleConverged` (no behaviour change).
- `NriGraphPixelTest.cpp`: `SupplyOne`/`SupplyTwo` capture their guids by value.

## Post-fix evidence

**Suites, Debug, foreground, from the exe dir**

| Filter | Seed | Result |
|---|---|---|
| `~[gpu]` | `19810145` | **1752 cases (1748 passed, 4 skipped), 57260/57260 assertions** |
| `[gpu]` | `2680713561` | **44 cases, 62383/62383 assertions** |
| `[gpu][golden]` | `2211835299` | **1 case, 14/14 assertions — still matches, not re-blessed** |

Delta against this closeout's own Step 4 Debug baselines (1747 cases / 57190
assertions `~[gpu]`; 1790 / 119559 unfiltered, i.e. 43 `[gpu]` cases / 62369
`[gpu]` assertions), attributed in full:

- `~[gpu]` **+5 cases, +70 assertions** — 2 in `NriMeshBufferCacheTest.cpp`
  (eviction erases; zero-size refused once), 2 in `SceneRenderResolverTest.cpp`
  (topology drops residency; every generator parameter counts), 1 in
  `MeshDocumentTest.cpp` (every preview rebuild invalidates).
- `[gpu]` **+1 case, +14 assertions** — 12 from the new
  `MeshThumbnailHarvestTest.cpp` case, 2 from the two `ResidentBytes()` checks
  added to the flipped eviction case.

**Golden gate, Debug, NO `--bless` anywhere.** The staged `Source/` and
`Content/` trees under both hosts were swept against SOURCE first (file-list
diff, both hosts: identical — no strays; the staging step is additive, and a
stale `Source/GameApi.hpp` was the last one it stranded). `gatePassed: true`,
four lanes, every one `diffCount=0`:

| Combo | Verdict | Detail (verbatim from the JSON) |
|---|---|---|
| `ArcaneRuntime/dx12/runtime-scene` | `PassedOnFallback` | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared (expected backend)` |
| `ArcaneRuntime/vulkan/runtime-scene` | `Passed` | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=backend` |
| `ArcaneEditor/dx12/editor-ui` | `Passed` | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared` |
| `ArcaneEditor/vulkan/editor-ui` | `Passed` | `exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared` |

JSON kept at
`.superpowers/sdd/2026-09-10-f2c-mesh-import-plan2-runtime-editor/fixwave-golden-gate-summary.json`
(`schemaVersion: 3`, `selfTest: false`, `refusalReason: ""`). No pixel moved.

**Constraints held.** `PluginABI.hpp` untouched across the wave; `out.txt` and
the three plan-external untracked directories never staged; every commit staged
by explicit path.

## Standing debts gained by this wave

8. **A second imported mesh in the golden scene — DECLINED, not forgotten.**
   The review recommended adding one (or scrolling the Browser so
   `golden_prop.arcmesh`'s row lands inside `editor-ui.png`), since one
   imported asset is exactly one too few for the golden lanes to catch C1's
   whole bug class. Declined for this branch because a second fixture forces a
   third bless cycle against the plan's one-cycle goal; C1's two-mesh
   `[gpu][thumbs]` harvester test is the net instead. Owed: revisit when the
   golden scene is next re-blessed for an unrelated reason — the marginal cost
   is zero then.
9. **`main.arcscene`'s missing trailing newline is a SERIALIZER fix.** The
   re-save dropped the file's final newline, so every future editor save of any
   scene produces a "\ No newline at end of file" diff. Ruled out of scope for
   this wave (a serializer change with its own blast radius, and the fixture
   must not be hand-edited around it). Owed: one character in the scene writer,
   then a re-save of the fixture on the next bless cycle.
10. **`MeshCache`'s `requested` set is an accounting cost.** I4's latched WARN
    has to know whether a guid was ever `Request`ed, which `table`/`failed`
    cannot say, so `MeshCache` now keeps one `Guid` per referenced mesh for the
    project's lifetime (cleared by `Clear()`). Trivial today; if the mesh count
    per project ever makes it non-trivial, the honest alternative is the
    `NotRequested` state the ruling declined, once `MeshResolveState`'s three
    consumers can absorb a fourth value safely.
11. **`MeshDocument::PreviewGeometryInvalidations()` is an instrument with one
    reader.** It exists because C2's invalidate is unobservable without a
    device; it counts the drop the document ISSUES, not one the vehicle
    confirms, so it proves "which paths invalidate", not "the vehicle dropped
    it". The two facts sit on adjacent lines in `RebuildPreviewMesh`. Owed:
    nothing, unless a device-bearing document test ever becomes cheap, at which
    point the stronger assertion should replace this one.

## Standing debts from the fix wave's scoped re-review (parked by the controller)

The re-review of `1e1a673b..a8547d71` verdicted every finding ADDRESSED with no new
Critical/Important breakage, and left four low-severity residuals. The SDD process
allows one fix wave; these are parked here, with a ruling each, rather than fixed
by a second wave. None changes behaviour a desk pass could observe except 14's one
log line.

12. **`NriMeshBufferCache::Resident::uploadRefused` is write-only.** I1 added it to
    distinguish "refused" from "evicted"; I2's ruling then made eviction ERASE the
    entry, so the only consumer that would have read it is gone. Harmless (the
    memoized-null entry still refuses per frame without re-asking), but a flag with
    no reader is a comment pretending to be code. Owed: either delete it or make
    `ResidentCount()`/a diagnostic read it.
13. **The real I1 leak path has no test.** The new `[render]` case exercises the
    zero-size refusal, which returns BEFORE `Upload`; the create-into-locals /
    publish-on-full-success rewrite is correct by inspection (the re-review traced
    all three `abandon()` arms), not by evidence. Owed: a `[gpu][meshcache]` case
    that forces the SECOND `CreateCommittedBuffer` to fail (an over-limit index
    buffer, or an injectable failure seam) and asserts `ResidentCount()==0`,
    no NRI validation error, and no re-ask on the next frame. This is the only
    residual with correctness exposure: a future `Upload` edit could regress the
    leak unnoticed until this lands.
14. **`MeshCache::Clear()` resets `requested` but not `warnedNeverRequested`.** On
    a project switch, one spurious "queried a never-requested mesh" WARN can fire
    for the new project's first pre-`Request` query. One line, cosmetic. Owed:
    clear the latch beside the set.
15. **`SceneResources.hpp:18`'s include comment cites `std::tie`**, which
    `GeometryIdentity()` deliberately does not use (it returns a value type so the
    caller can compare across an erase). Comment-only. Owed: fix the comment.

Reminder carried from the re-review, addressed to the desk pass: **debt 8 is the one
with teeth** — ReferenceProject holds exactly one imported mesh, one too few for the
golden lanes to catch the C1 class again. The "revisit at the next re-bless" trigger
must not lapse.
