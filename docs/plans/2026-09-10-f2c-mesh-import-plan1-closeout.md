# F2c Plan 1 (mesh import pipeline) — closeout notes

Task 16 of `docs/plans/2026-09-10-f2c-mesh-import-plan1-pipeline.md`. All 15
implementation tasks are committed at HEAD `4a968173` when this closeout was
written. This document is the end-to-end verification record and the
handoff to Plan 2 (`docs/plans/2026-09-10-f2c-mesh-import-plan2-runtime-editor.md`).

Controller amendments bound this closeout: **ABI is 25** (not the brief's
original 24 — an Astra sync consumed 24 before this arc started, Task 11
bumped 24→25), and the **expected case total is 1611** (baseline 1535 + 76,
not the brief's original +70 — six review-driven hardening tests were added
during fix rounds beyond the plan's per-task estimates). Both are confirmed
below by direct measurement, not recollection.

## Step 1 — End-to-end, headless

A scratch copy of `ReferenceProject` was made outside the repo tree (under
the session scratchpad, not `%TEMP%` directly, but equally outside any repo
working copy). `Content/meshes/multi.glb`, `nested.gltf`, and `nested.bin`
were copied in from `ArcaneTests/data/gltf/`. Two `.meta` sidecars were
minted by hand — `multi.glb.meta` and `nested.gltf.meta`, each
`{"guid": "<fresh-uuid>", "version": 1}`, the same shape as
`ReferenceProject/Content/textures/uv_marker.png.meta`. `nested.bin` gets no
`.meta` of its own — confirmed against `CookSession.cpp`'s `EnumerateSources`
(only files matching the mesh extension set `{.gltf, .glb}` are scanned for a
sidecar; an external buffer is discovered through the `.gltf`'s own `uri`
field via `ReadExternalBuffers`, never registered as its own source).

Command: `bin/Debug-windows-x86_64-md/arccook/arccook.exe --project <scratch> --verbose`

**Verdict 1 — first cook, `multi.glb`:**
```
OK   .../Content/meshes/multi.glb -- cooked
```

**Verdict 2 — first cook, `nested.gltf`:**
```
OK   .../Content/meshes/nested.gltf -- cooked
```
(A pre-existing `uv_marker.png` in the copied tree also reported `up to
date` in the same run; irrelevant to the mesh e2e.) Full-run summary:
`arccook: cooked=2 upToDate=1 failed=0`, **exit 0**.

**Verdict 3 — `--check` on the clean cooked tree:**
```
arccook --check: clean
```
**exit 0.**

Then one byte of `nested.bin` was flipped (offset 0: `0x00` → `0x01`).

**Verdict 4 — `--check` after the edit:**
```
arccook --check: stale
```
**exit 2.** This is §5.4's cook-key claim ("for `.gltf` with external
buffers, source bytes covers the `.gltf` file PLUS every referenced external
`.bin`") observed directly at the CLI: editing only the `.bin`, never the
`.gltf` itself, still flips the cook key.

Only geometry artifacts were produced — no companion `.arcmesh` was minted,
because the scratch project was never opened by the editor. This is the
`§5.5` division of labor working as designed ("A headless `arccook` run on
a never-opened project cooks geometry artifacts only — correct, not a gap"),
not a defect.

The real, tracked `ReferenceProject/` was never touched:
`git status ReferenceProject/` stayed clean (`nothing to commit, working
tree clean`) throughout.

## Step 2 — Zero-legacy sweep

`grep -rniw` for each symbol across `ArcaneClient ArcaneEditor
ArcaneAssetPipeline ArcaneTests arccook scripts`:

- **`EnumerateTextureSources`** — 6 hits, all in comments narrating the
  generalization ("generalized from `EnumerateTextureSources`'s own
  hardcoded `.png` check", "the one extension `CookSession.cpp`'s
  `EnumerateTextureSources`..."). No live call site remains — the function
  itself no longer exists under that name (renamed/generalized in T8).
- **`SetImporterForTesting`** — 5 hits:
  - `ArcaneEditor/src/Project/CookQueue.cpp:34` and `.hpp:74` — the actual
    `CookQueue::SetImporterForTesting` method. **Allowed per controller
    amendment 3**: this is the editor wrapper's own public name, kept on
    purpose (Task 8 renamed the underlying `CookSession` seam but
    deliberately left the wrapper's name alone).
  - `ArcaneAssetPipeline/src/.../CookSession.hpp:150` — a comment noting the
    pipeline-side rename ("renamed from `SetImporterForTesting` now that
    there are...").
  - `ArcaneTests/src/CookQueueTest.cpp:248,265` — a test exercising the
    allowed `CookQueue` wrapper.
- **`DiscoverUnknownTextureSources`** — 2 hits, both comments narrating the
  generalization (`ArcaneEditor/src/Project/ContentDiscovery.hpp:32`,
  `ArcaneTests/src/ContentDiscoveryTest.cpp:26`).
- **`EnumerateContentPngFiles`** — 3 hits, all comments narrating the same
  generalization.
- **`MeshAssetData::material`** — **0 hits** as a literal string. The
  concept it names no longer exists as a struct member: F2c s4.4 retired
  the F2a scalar `material` field into `MeshAssetData::slots` (a
  `std::vector<MeshSlot>`, each carrying its own `.material` Guid). The
  scalar survives only as a **JSON key**, "material", read by the
  deliberately-kept legacy arm at `ArcaneClient/src/Arcane/Mesh/MeshAsset.cpp:202-206`
  (`else if (doc.contains("material") ...)`), documented at
  `MeshAsset.cpp:178-186` and exercised by `ArcaneTests/src/MeshAssetTest.cpp:190,225`.
  This matches controller amendment 3 exactly.

**Verdict: PASS.** Every hit outside the allowed set is a historical/
explanatory comment; every live hit is the intentionally-kept `CookQueue`
wrapper or the intentionally-kept legacy JSON-key read arm.

## Step 3 — `CGLTF_IMPLEMENTATION` sweep

`grep -rn "CGLTF_IMPLEMENTATION" --include=*.cpp --include=*.hpp .` (repo
root):

```
./ArcaneAssetPipeline/src/Arcane/AssetPipeline/CgltfImpl.cpp:8:// CGLTF_IMPLEMENTATION is defined here and NOWHERE ELSE. Grep-verifiable:
./ArcaneAssetPipeline/src/Arcane/AssetPipeline/CgltfImpl.cpp:9://   grep -rn "CGLTF_IMPLEMENTATION" --include=*.cpp --include=*.hpp .
./ArcaneAssetPipeline/src/Arcane/AssetPipeline/CgltfImpl.cpp:12:#define CGLTF_IMPLEMENTATION
./ArcaneTests/src/VendorSmokeTest.cpp:131:// and that static lib is linked into this exe -- defining CGLTF_IMPLEMENTATION here
```

Exactly one **active** `#define CGLTF_IMPLEMENTATION` in the whole
`.cpp`/`.hpp` surface: `CgltfImpl.cpp:12`. The other three hits are
comments (two in `CgltfImpl.cpp` itself explaining/self-verifying the rule,
one in `VendorSmokeTest.cpp` explaining why the TU does *not* redefine it).

**One methodology note, not a defect:** the literal command run unscoped
from repo root also picks up 3 incidental hits inside `.example/` — a
`.gitignore`d (`.gitignore:69`) external reference dump (NRI samples +
Unreal Engine source, per project memory, not Arcane's own tree) that
happens to vendor its own, unrelated `cgltf` copy (`NRIFramework/Source/Utils.cpp`
and two `UnrealEngine-release/.../MaterialX/.../CgltfLoader.cpp` occurrences,
the latter immediately `#undef`ing it again). `ThirdParty/cgltf/cgltf.h`
itself was **not** picked up by the same command — it's an `.h`, not
`.hpp`, so the brief's own `--include` filters exclude it by construction;
inspecting it directly shows its one `#define CGLTF_IMPLEMENTATION` is
inside an `__INTELLISENSE__`/`__JETBRAINS_IDE__` guard (line 919-922), the
standard trick vendored single-header libraries use to make IDE tooling see
full definitions without affecting real builds.

**Verdict: PASS** — exactly `CgltfImpl.cpp` defines it, once `.example/`
(gitignored reference material, not Arcane code) is set aside.

## Step 4 — Placement-rule sweep

Premake `ArcaneClient` project block (`premake5.lua:322-437`): no line
inside that range names `cgltf` or `meshoptimizer` (`grep` returns nothing).
Its `links{}` list is exactly
`{ "ArcaneCore", "NRI", "msdfgen", "freetype", "imgui", "enkiTS", "Manifold2D" }`
— no `IncludeDir.cgltf`/`IncludeDir.meshoptimizer` reference, no linked
library either.

`grep -rn "cgltf\|meshoptimizer" ArcaneClient/src` is **not** literally
empty (14 hits) — a discrepancy from the brief's literal expected output,
investigated rather than waved off. Every hit is a **comment**, in
`ArcaneClient/src/Arcane/Assets/ArtifactReader.cpp`/`.hpp`,
`Render/MeshBuilder.hpp`, and `Render/Nri/nodes/MeshNode.hpp`. The
`ArtifactReader.cpp`/`.hpp` hits document that the client's URI-decoder
(`DecodeUriPercentEscapes`) is a **byte-for-byte independent
reimplementation** of `cgltf_decode_uri`/`cgltf_unhex`
(`ThirdParty/cgltf/cgltf.h`), per F2b's deliberate-reimplementation rule
(spec §5.2: "ArcaneClient gets its own independent reader mirror") — naming
the function it mirrors is how the parity claim stays checkable, not a
dependency. Confirmed independently: `grep -rn "#include.*cgltf\|#include.*meshopt"
ArcaneClient/src` returns **nothing**, and `ArcaneClient`'s `links{}` (above)
carries neither library. The substance of the 08-21 rule — `cgltf`/
`meshoptimizer` are never *consumed* by `ArcaneClient` — holds. The brief's
"is empty" wording predates the documentation comments T4 legitimately
added; the rule itself is intact.

**Verdict: PASS** (placement rule holds; the literal grep substring is
non-empty for a benign, verified reason — documented above rather than
silently reported as a clean empty match).

## Step 5 — Full suites, Debug and Release

Both configs rebuilt from a clean invocation before running (foreground,
no background jobs, per controller amendment 6):

- `msbuild Arcane.slnx /p:Configuration=Debug /m` — **Build succeeded, 0
  warnings, 0 errors** (already up to date; 3.57 s).
- `msbuild Arcane.slnx /p:Configuration=Release /m` — **Build succeeded, 0
  warnings, 0 errors** (full rebuild; 2 min 32 s).

The arc-start baseline (`docs/plans/.../plan1-pipeline.md:25`) is **55464
assertions / 1535 cases**, measured with the `~[gpu]` filter (a
baseline-comparability convention, not a hazard gate — see project memory
`project_machine_gpu_driver_crash_env.md`). All suite runs below were run
from each config's own `ArcaneTests` exe directory.

### Baseline-comparable runs (`"~[gpu]"`, order=rand)

| Config | Seed | Result |
|---|---|---|
| Debug | `789358831` | **All tests passed (56000 assertions in 1611 test cases)** |
| Release | `2649618695` | **All tests passed (56000 assertions in 1611 test cases)** |

1611 − 1535 = **+76**, matching the controller's revised, binding
attribution exactly (derived from the run, not recalled):

| Task | Delta | Task | Delta | Task | Delta |
|---|---|---|---|---|---|
| T1 | +4 | T6 | +6 | T11 | +6 |
| T2 | +3 | T7 | +9 | T12 | +3 |
| T3 | +7 | T8 | +6 | T13 | +7 |
| T4 | +3 | T9 | +4 | T14 | +4 |
| T5 | +5 | T10 | +5 | T15 | +4 |

Sum: 4+3+7+3+5+6+9+6+4+5+6+3+7+4+4 = **76**. ✓ Six of those cases are the
review-driven hardening tests beyond the plan's original +70 estimate,
corroborated against the commit list between the pre-arc baseline
(`a21e19ef`) and HEAD: T3's `21732b7b` (header/array-count-mismatch
refusal, "missing-section refusal"), T5's `eecdc4cc` (length-prefix
source bytes — the "source/buffer-list boundary collision" fix), T7's
`673ffcb1` (scene-unreachable-mesh refusal), T11's `5083307e`
(percent-decode external buffer uri), and T13's two review commits
`e115dbef`/`18c83780` (byte-compare no-overwrite/A4-suffix reconciliation +
chain-walk/dot-only-stem fixes — the "identical-bytes dedup" pair) each
carry one or more of the six named hardening cases. **No task's count was
found to be wrong** — the derived total matches the controller's binding
table exactly; no further reconciliation needed.

### Full runs (no filter, `[gpu]`-tagged cases included)

| Config | Seed | Result |
|---|---|---|
| Debug | `3730666080` | **All tests passed (118274 assertions in 1644 test cases)** |
| Release | `119130017` | test cases: 1644 \| 1642 passed \| **2 failed**; assertions: 118245 \| 118243 passed \| **2 failed** |

The 33-case gap between 1611 (`~[gpu]`) and 1644 (unfiltered) is the
`[gpu]`-tagged case population; both configs enumerate the identical 1644
total, confirming the two runs see the same test corpus.

**The 2 Release failures**, both in `ArcaneTests/src/WitnessScenariosTest.cpp`,
tagged `[witness][gpu]`:

- `W1: settle spends BOTH bounds when the compare conjunct cannot pass`
  (`WitnessScenariosTest.cpp:134`) — `REQUIRE_FALSE(GradeProcessFacts(run).has_value())`
  failed; `exit 1, wall 1070 ms`.
- `W3: a reference missing at EVERY level reports the ordered search space`
  (`WitnessScenariosTest.cpp:203`) — same assertion, `exit 1, wall 1068 ms`.

Both scenarios spawn a real staged `ArcaneRuntime.exe --project ReferenceProject`
subprocess and grade its report. The kept witness artifacts' captured
stderr for both failures reads identically:

```
[error] plugin: initial load failed
[error] ArcaneRuntime: failed to load game module 'ReferenceProject\Binaries\ReferenceGame.dll'
[error] BootSequence: fatal stage 'plugin_load' failed
```

This is **the known environmental note, not an arc regression**: Task 11's
report (`.superpowers/sdd/2026-09-10-f2c-mesh-import-plan1-pipeline/task-11-report.md:201-217`)
documents the identical `plugin: initial load failed` /
`failed to load the game module` failure at `ArcaneEditor.exe --project ReferenceProject`,
investigated and reproduced **byte-for-byte** at pre-Task-11 HEAD (`01ad0da7`,
ABI 24 throughout, via a `git stash` round-trip) — ruling out an ABI
mismatch from Task 11's own work and confirming it as "a pre-existing,
environmental condition on this machine, unrelated to this task." This
closeout's Release run reproduces the same condition again, now surfaced
automatically through the `[gpu]`-tagged Witness suite rather than only a
manual desk-check. The Debug full run (same session, same machine, run
immediately after the Release build) did **not** hit it — both `W1` and
`W3` ran to completion successfully in Debug (7.208 s and 1.970 s
respectively, vs. Release's ~1.07 s immediate-exit failures) — consistent
with Task 11's framing of an intermittent, machine-local, plugin-load
condition rather than anything deterministic in either configuration's
build correctness. Per the "Rebuild Game Module arc" memory item (already
marked DESK-VERIFY-OWED), this is corroborating evidence for whoever next
touches `PluginHost`/`Module` loading — not something this closeout task
fixes (verification-only per its brief).

**Step 5 verdict:** the baseline-comparable suites are 100% green in both
configs at exactly the expected 1611 cases / 56000 assertions. The known,
pre-documented `ReferenceGame.dll` plugin-load environmental condition
reproduces in the Release `[gpu]` Witness suite; this is not a new defect
and requires no code change from this task.

## Step 6 — Handoff to Plan 2

Plan 2 (`docs/plans/2026-09-10-f2c-mesh-import-plan2-runtime-editor.md`)
begins at this commit (`4a968173`; the closeout commit lands one after).

- **ABI is now 25.** `ReferenceProject.arcproj`'s `engine.abi` reads `25`
  (Task 11 bumped it 24→25; the arc-start Astra sync had already consumed
  ABI 24 before Task 1). `ReferenceProject.slnx` is rebuilt against it.
- **Gacha's Game-module rebuild debt is one deeper** (recorded, not
  actioned this task). Gacha's `Game/Aphelyon.arcproj` last restamped
  against Arcane ABI 21 (`chore(game): restamp Aphelyon.arcproj to engine
  ABI 21`, Gacha commit `5923da65`); Arcane has since moved 21→...→25
  across this arc and the ones before it. Someone owes Gacha a Game module
  rebuild + restamp cycle before its next build against this SDK checkout.
- **`CollectMeshInstances` still emits one instance per entity**, resolving
  through `slots[0]` only (`ArcaneClient/src/Arcane/Scene/MeshSubmissionSystem.hpp:81-88,144-148`).
  A multi-slot imported mesh's sections beyond the first are invisible on
  the draw side until **Plan 2 Task 5**, which makes this per-section.
- **`AssetKind::Model` has no graph hue** — confirmed at
  `ArcaneEditor/src/Panels/AssetGraphPanel.cpp:327-335`: the hue switch
  covers `Texture`/`Material`/`Mesh`/`Sprite`/`Scene` and falls to
  `default: break` (the theme's neutral fallback) for `Model` — **and no
  browser fold** yet. Both land in **Plan 2 Task 8**.
- **Imported meshes resolve to `MeshData` on the CPU but nothing uploads
  them** — residency (spec §7.2, `NriMeshBufferCache`) is explicitly out of
  this plan's scope (spec coverage table: "§3 R2 (residency) — Plan 2").
  **Plan 2 Tasks 1-5** cover CPU-resolution-to-GPU-upload.
- **Render-side mesh invalidation in `SceneRenderResolver`** (spec §7.3) is
  likewise unbuilt here — **Plan 2 Task 6**.
- **The known `ReferenceGame.dll` "plugin: initial load failed" condition**
  (Step 5 above) is environmental to this machine, first traced by Task 11
  and reproduced again by this closeout's Release `[gpu]` suite run. Not an
  arc regression; not actioned by this task.

## Step 7 — Files changed

- `docs/plans/2026-09-10-f2c-mesh-import-plan1-closeout.md` (this file) —
  new.

No product code changed. `ReferenceProject/` (tracked) was never touched —
the Step 1 e2e ran entirely against a scratch copy outside the repo tree.
