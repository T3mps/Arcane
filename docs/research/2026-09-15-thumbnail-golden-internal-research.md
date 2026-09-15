# Thumbnail golden coverage — is "option 4" the right shape? (internal-directives research)

**Date:** 2026-09-15
**Status:** research only — no code, no gate run, no bless
**Question posed:** does an in-process `[gpu][golden][thumbs]` Catch2 case, driving the real
`MaterialPreviewHarvester` offscreen against the staged `ReferenceProject` and `ImageCompare`ing
each harvested PNG against a committed reference under `ReferenceProject/Verify/References/`,
fit this repo's own directives and existing machinery — and what would be wrong with it.

---

## 1. Directives that bind this decision

### 1.1 Renderer evolution will move every lit pixel (binding, 2026-09-14)

`docs/research/2026-09-14-engine-ceiling-deadlock-and-box3d.md` is the binding north star. Its
sequencing (§5) puts, in order: T1 (light grid + GGX + forward/deferred), T5+T6 (exposure,
bloom, NPR/TAA), T3 lite (CSM + cubemap IBL), T4 lite (fog), T2 lite (a baker). Every one of
those changes how a **lit mesh** or a **lit material preview** renders:

> "`mesh.hlsl` is Lambert on purpose ('DELIBERATELY NOT PBR'). T1 is the cutover to GGX, not a
> second half-PBR model beside it." (§2, line 59)

The harvester's mesh-thumbnail branch and its mesh-kind-material sphere branch both go through
the same lit path (`MaterialPreviewHarvester.cpp:1223-1225`, `:1268-1273`, same
`lightDirection`/`lightColor`/`ambient` triple in both branches — "the SAME light/ambient values
... so a material thumbnail and a mesh thumbnail read as one family at 64px"). **T1's GGX
cutover alone will move every thumbnail pixel that touches lighting**, and T5/T6/T3 lite touch
tonemapping/exposure/bloom which the harvester's `NriGraphContext::RenderFrameOffscreen` also
runs through (it is the same graph context class the real viewport uses). Time estimates in §3
("a well-specced architectural arc is 1–3 weeks, often less") say this is not a one-time cost:
T1 alone is "2–5 weeks" (§3 table), and the sequencing ledger names five more arcs after it.

**Implication for option 4:** whatever reference set is committed will need **re-blessing on the
same cadence as the renderer's own T1→T6 arcs**, not once. A design that makes re-bless expensive
per-reference (see §2.2) will accumulate a matching amount of manual toil at each of ~5 renderer
milestones. A thumbnail bless workflow's cost-per-arc is therefore a first-order design
criterion, not a nice-to-have.

### 1.2 Golden discipline: bless SOURCE, restage BOTH hosts, JSON-only verdicts (Arc 2 / gate design)

`docs/specs/2026-09-10-f2c-mesh-import-design.md` §9 states the discipline by name for the one
prior extension of the golden lanes:

> "One re-bless cycle when it lands — **Arc 2 discipline (bless SOURCE, restage BOTH hosts,
> JSON-only verdicts, archive diffs before re-runs)**." (§9)

`scripts/golden-gate.ps1`'s header spells out what that costs in practice for the *existing*
two-slot reference hierarchy (shared `editor-ui.png` + backend-split `runtime-scene.png`): a
5-step manual bless procedure (`golden-gate.ps1:94-135`) — rebuild the single-slot
`ReferenceProject/Binaries/`, apply the mutation to **both staged trees**, run each host **from
its own exe directory** with `--project ReferenceProject --bless`, and never touch `Verify/`
during ordinary staging (`golden-gate.ps1:120-124`: "Verify/ is deliberately never restaged by
the staging loop"). This procedure does not scale linearly with the number of references without
either (a) a loop over N images per bless, or (b) a purpose-built bless mode. Any per-thumbnail
reference set inherits this cost unless it gets its own, cheaper bless path.

### 1.3 Introspection direction: observe the real host from outside, not a parallel harness

`docs/specs/2026-09-03-host-witness-harness-design.md` (Arc B, LANDED) is the concrete answer to
"how does this repo want to observe editor/runtime product from outside": it explicitly rejected
building a second harness and instead put the grader **in ArcaneTests**, spawning the **real
staged host executable** (never linking it):

> "**Arc B builds the grader layer only**... **The grader lives in ArcaneTests** as Catch2 cases
> tagged `[gpu][witness]`... 'ArcaneTests links neither host' stays true: it spawns them, which
> is Gauntlet's own relationship to its target." (§2)

Its own precedent for what *not* to build: "**Any generic cascade engine** — the precedence
lives in assert order, not machinery" and "**Any scenario-config file format** — three scenarios
get three cases, not a framework" (§9). The `--probe census` / `--report` surface
(`docs/specs/2026-08-23-agent-verification-offscreen-design.md`) is the same family: a real
headless host answering structured queries about its own live state, machine-readable, not
scraped from a log. **The consistent thread across both docs: when the question is "does the
CLI/boot/settle/report path work", spawn the real host; when the question is "does the render
path itself produce the right pixels", an in-process Catch2 case against the real graph context
is the accepted, cheaper instrument** — that is precisely `GoldenImageTest.cpp`'s own "honest
split" (§3 below) and `MeshThumbnailHarvestTest.cpp`'s existing shape.

### 1.4 Editor-shell volatility — do not overload `editor-ui.png`

Two independent facts argue against growing `editor-ui.png` coverage as the fix: (a) F4 is
explicitly scoped as future editor-authoring work touching the viewport/gizmo/camera
(`docs/research/2026-08-21-3d-foundations-assessment.md` §"F4 — editor authoring for 3D", lines
284-294: 3D gizmo, `EditorCamera` orbit/fly, 3D picking — all viewport-shell changes), and (b)
the closeout's own debt record shows the shell is *already* too crowded for this purpose: Debt 12
in `docs/plans/2026-09-10-f2c-mesh-import-plan2-closeout.md` records that
`golden_prop.arcmesh`'s Browser row is scrolled out of the 1280×720 `editor-ui.png` capture
today, and the final-review report calls this "not merely a coverage gap, it is *why C1
shipped*" (`final-review-report.md:472`). Rejected alternative (1) in the prompt — scroll the
Browser so mesh rows appear in `editor-ui.png` — fights the same crowding problem that already
lost once, and F4's viewport churn means panel real estate keeps moving.

### 1.5 Homogenized rendering — thumbnails ride the same path as the viewport (not a directive violation, a design fact)

The homogenization mandate (`docs/specs/2026-05-27-rendering-homogenization-design.md`, and
repeated across a dozen specs as `feedback_homogenized_rendering`) is "one canonical render
path, no bespoke chains". `MaterialPreviewHarvester` complies: it is "one LAZY 64×64 offscreen
`NriGraphContext`... exactly what the shader editor's preview does"
(`MaterialPreviewHarvester.hpp:9-22`), the *same* `NriGraphContext`/`RenderFrameOffscreen`/
`MeshNode` class family the viewport and `GoldenImageTest.cpp`'s lit-cube case use. This is why
§1.1's renderer-evolution argument applies to thumbnails at all: they are not a separate toy
renderer that could dodge T1's GGX cutover, they are downstream of the same graph.

---

## 2. The existing machinery, exactly

### 2.1 The gate lane model (`scripts/golden-gate.ps1`)

- **Four fixed lanes, no framework**: `ArcaneRuntime --backend {dx12,vulkan} --compare
  runtime-scene`, `ArcaneEditor --backend {dx12,vulkan} --compare editor-ui`
  (`golden-gate.ps1:11-15`). Not a config file, not N lanes — four, named, hardcoded.
- **`resolvedLevel: shared|backend`**: `editor-ui` is a **shared** reference slot (one
  `editor-ui.png` serves both backends); `runtime-scene` is **backend-split**
  (`Verify/References/runtime-scene.png` shared/fallback + `Verify/References/vulkan/
  runtime-scene.png` backend-specific override) — confirmed both in the bless procedure
  (`golden-gate.ps1:112-114`: "editor-ui is a SHARED slot ... runtime-scene is backend-split")
  and in every closeout's reported JSON (`resolvedLevel=shared (expected backend)` for
  `dx12/runtime-scene`, i.e. `PassedOnFallback`; `resolvedLevel=backend` for
  `vulkan/runtime-scene`). This IS the mechanism for "one reference serves both backends" the
  prompt asked about — it exists today, per-slot, not per-lane.
- **The verdict is `exitReason` out of the report JSON, never the raw process exit code alone**
  (`golden-gate.ps1:17-31`) — a documented exit-code collision table, not an assumption.
- **Bless has no CLI flag on the PS1 script at all** (`... no --bless` in its own header,
  line 11) — blessing is a host-exe flag (`--bless`) run by hand against the **staged** tree,
  never invoked by the script itself.
- **`-SelfTest`**: the ONE mode that mutates the tracked SOURCE tree, in a `try/finally` that
  restores unconditionally, and it also **pins the wire contract** (schema constants must agree
  across `VerifyReport.hpp`, `automation-vocabulary.txt`, and this script — "the first live test
  of Arc A's anti-drift pin"). It is the proof-of-capability-to-fail instrument this repo
  requires of every gate ("A gate never observed failing is not a gate").
- **Tolerance**: the gate reads `compare.diffCount` and `compare.maxLocalDifference` off the
  report; every closeout run cited above passed at `diffCount=0 maxLocalDifference=0.0` — **zero
  budget is the operating norm for the shipped lanes**, not a relaxed threshold.
- **No notion of "many small references in one lane."** Each lane compares exactly one PNG
  (`editor-ui.png` / `runtime-scene.png`). A per-thumbnail reference set is a new shape this
  script does not have and was not asked to grow (§9 of its own out-of-scope list: "Any
  generic cascade engine", "Any scenario-config file format").
- **A "lane" could in principle be a host launched with `--report` emitting per-thumbnail
  facts** — the host-witness precedent (§1.3) is exactly "spawn the real host, assert on its
  report" — but no existing CLI surface asks the host to harvest+diff N named thumbnails and
  report per-asset pass/fail; `--compare` compares exactly one named reference per invocation
  today (`golden-gate.ps1`'s four fixed `--compare <name>` calls).

### 2.2 The in-process golden's bless workflow — and its exact gap

`ArcaneTests/src/GoldenImageTest.cpp`'s `[gpu][golden]` case (D3D12 only, "deliberately" — one
backend, because "whether the two backends agree pixel-for-pixel is the reference HIERARCHY's
own question... answered at the host level", `GoldenImageTest.cpp:118-124`):

- Builds its own hand-authored scene (procedural `BuildCube`, no asset load) so it never needs
  anything staged under `Content/` — deliberately, because `ArcaneTests`' postbuild only
  `{COPYDIR}`s `ReferenceProject/Verify/`, not `Content/` (`:92-96`).
- Compares at **budget 0** (`CompareImages(expected, actual)` default) against
  `ReferenceProject/Verify/References/inprocess-lit-cube.png`.
- **This case never writes to `referencePath` itself** (`:232-235`): "A missing reference is a
  REFUSAL, not a silent re-bless." **There is no `--bless`-equivalent switch, env var, or test
  mode anywhere in this file or its harness.** The only re-bless mechanism found: `git log
  --follow` on `inprocess-lit-cube.png` shows exactly **one commit ever touched it**
  (`e84b67ae`, 2026-08-26, the case's own introduction) — it was hand-authored/copied in at
  creation and has never been re-blessed since. **This is the gap option 4 must not blindly
  copy**: the template it would follow has no re-bless discipline at all, only a "write it once
  by hand" precedent. A failure produces a diff artifact
  (`ReferenceProject/Saved/Verify/inprocess-lit-cube-diff.png`) a human is expected to eyeball
  and then manually overwrite the committed PNG with — there is no scripted loop.

### 2.3 The harvester's injection seams and the existing two-mesh test's exact shape

`MaterialPreviewHarvester::Services` (`MaterialPreviewHarvester.hpp:120-159`) is a plain struct
of callables — `chromeGraph` (returns `NriGraphContext*`), `hostConfig`, `compiler`/`sources`
(only needed for material/sprite subjects, not mesh), `resolveAsset`, `pixelSupply`,
`thumbnailDir`, `meshArtifactFor`/`cookPending` (only for *imported* mesh assets, not
primitives). **No EditorApp, no Project, no headless editor boot is required.**
`ArcaneTests/src/MeshThumbnailHarvestTest.cpp`'s existing `[gpu][thumbs]` case proves this
directly: it builds `Arcane::OffscreenVehicle::Create(cfg, 256, 128)` (a bare offscreen chrome
context, `ArcaneClient/src/Arcane/Host/OffscreenVehicle.hpp`), writes two `.arcmesh` fixtures to
a scratch temp dir with `Arcane::SaveMeshAsset`, and constructs `MaterialPreviewHarvester`
directly with hand-filled `Services` (`resolveAsset` mapping guids to those files, `thumbnailDir`
pointing at a scratch dir) — **no compiler, no source provider, no project**
(`MeshThumbnailHarvestTest.cpp:130-146`). It drives the harvester with `RequestMesh` +
`h.Pump(clock)` in a loop (`Drain`, `:99-107`) until `PendingCount()==0`, then reads the written
PNG off disk. **This is the exact vehicle option 4 would need — it already exists, is already
`[gpu]`-tagged, and is already proven to work without booting `ArcaneRuntime`/`ArcaneEditor` at
all.** The gap is only in what the test *asserts*: today it is a self-referential control/subject
byte-equality check (proving no cross-contamination between two harvests through one cache), not
an `ImageCompare` against a committed reference PNG.

### 2.4 CI flow

`Jenkinsfile`'s `windows && gpu` agent runs `[gpu]` Catch2 (including `[gpu][thumbs]` and
`[gpu][golden]`) inside the **unfiltered** "Tests (incl [gpu])" stage (lines 67-97) — one backend
implicitly (whatever `ArcaneTests.exe` is compiled/linked against; the lit-cube case gates itself
to D3D12 via `ARC_REQUIRE_BACKEND`). The **separate** "Golden gate" stage (lines 99-154) runs
`golden-gate.ps1` twice (Release then Debug, order load-bearing for the single-slot
`ReferenceProject/Binaries/`), which is what actually launches both real hosts on both backends.
"Golden gate self-test" runs on `main`/`milestone/*` only. **A new committed reference PNG set
flows through CI exactly like the two existing ones**: committed under
`ReferenceProject/Verify/References/`, staged into each exe's directory by the existing
`{COPYDIR}` postbuild (no premake change needed, per `GoldenImageTest.cpp:229-231`), read at test
run time — nothing new is needed on the CI side for an in-process case; a new PS1-driven lane
would need explicit new stage code.

---

## 3. Option 4 assessed against each directive

**(a) Catch2 in-process vs gate lane vs both.** The introspection direction (§1.3) draws the line
at *what question is being asked*, not at "in-process bad, out-of-process good": boot/settle/
CLI/report questions go to the real host (`golden-gate.ps1`, `[gpu][witness]`); render-path
questions go to an in-process case against the real graph context (`[gpu][golden]`,
`[gpu][thumbs]`). Thumbnail correctness — "does this asset's committed offscreen render still
match" — is a render-path question, and §2.3 shows the harvester needs no host boot to answer it.
**An in-process Catch2 case is the correctly-scoped instrument; it does not need to be a gate
lane**, and per §2.4 it also does not naturally run on both backends the way a gate lane does (a
Catch2 case is compiled into one `ArcaneTests.exe` per build, same D3D12-only precedent
`GoldenImageTest.cpp` set and justified). If per-backend divergence in *thumbnails specifically*
ever needs proving, that question belongs at the host level exactly as `runtime-scene`/
`editor-ui`'s own hierarchy already does it — not duplicated inside this new case.

**(b) How re-bless works when Deadlock moves every reference.** This is option 4's real design
debt. Copying `GoldenImageTest.cpp` literally means copying its **zero** re-bless mechanism
(§2.2) — acceptable for one hand-authored fixture blessed once in 2026-08-26, not acceptable for
a set that must move at every T1→T6 renderer arc (§1.1). The spec must add what
`GoldenImageTest.cpp` never needed: either (i) a `--bless`-style env/CLI switch on the Catch2
case itself (e.g. `ARCANE_THUMBS_BLESS=1`, checked once at the top of the test, writing
`actual` over `referencePath` instead of comparing, mirroring the host's own `--bless` semantics
so the discipline stays recognizable) or (ii) a tiny standalone bless tool that runs the same
harvester Services wiring and writes PNGs. Given §1.2's Arc-2 discipline (bless SOURCE, JSON-only
verdicts — no ambient magic), an explicit opt-in switch that still requires a human to review the
diff and re-run is more consistent with this repo's stance than a fire-and-forget auto-bless.

**(c) Reference organization.** One image per lane is the *gate's* convention, not a Catch2
convention — nothing stops a single test file iterating N named subjects and comparing each
against its own file, since it never needs a lane per se. `ReferenceProject/Verify/References/
thumbs/<asset-name>.png` (a subdirectory, mirroring the existing flat-vs-`vulkan/`-subdir split
precedent for `runtime-scene`) keeps thumbnails visually and physically distinct from the two
existing host-level references, and lets a re-bless script glob one directory. Per-subject-kind
naming (`thumbs/mesh-cube.png`, `thumbs/material-lit.png`) mirrors the harvester's own `Subject`
tag (mesh vs material vs sprite vs fullscreen) and would let a future spec add coverage per
`Subject` without renaming existing files.

**(d) Tolerance.** The lit-cube precedent runs at the library default — **budget 0**
(`CompareImages(expected, actual)`, no `ImageCompareOptions` passed at all,
`GoldenImageTest.cpp:248`) — and every host-level gate lane in every closeout has also passed at
`diffCount=0 maxLocalDifference=0.0`. There is no example anywhere in this codebase of a
*passing* golden case with a nonzero budget; `maxDiffPixels`/`maxDiffPixelRatio` exist in
`ImageCompareOptions` (`ImageCompare.hpp:213-214`) but the comment there is explicit that
"if neither is set the budget is ZERO" and raising it is discouraged ("Raising it to make a
flaky test pass is the wrong lever", `ImageCompareOptions`'s sibling `maxColorDeltaE94` comment,
`:165-167`). **Option 4 should default to budget 0**, consistent with everything else in this
repo, and treat any need for a nonzero budget as a signal that determinism (not tolerance) is
the actual bug — same standing lesson `feedback_default_values_are_not_measurements` and the W1
scenario's own "a bound not separated from the thing it exists to exclude is not a bound" apply
here by analogy.

**(e) What must be injected, and whether it's already there.** §2.3 answers this concretely:
`Services{chromeGraph, hostConfig, backend, resolveAsset, thumbnailDir}` plus, for imported
(non-primitive) mesh assets, `meshArtifactFor`/`cookPending`; an `OffscreenVehicle` for the
chrome graph; scratch `.arcmesh`/`.arcmat` fixtures written via `SaveMeshAsset`/whatever the
material-asset equivalent is. **All of this already exists and is already exercised by
`MeshThumbnailHarvestTest.cpp`** — option 4 is additive (new assertions + a bless mode + new
reference files), not a new harness. One gap: that existing test only drives `Subject::Mesh`
via `RequestMesh`; a full thumbnail-golden case set would also want `Subject::Material` (sprite/
fullscreen/mesh-kind-material) coverage, which needs `compiler`/`sources` wired too (the harness
supports it — `Services::compiler`/`sources` are already fields — but no existing `[gpu][thumbs]`
test exercises that path against a committed reference; `GoldenImageTest.cpp`'s lit-cube case
is the nearest analog for "a real compiled/lit result compared to a PNG").

---

## 4. What would be wrong with option 4 — the honest case against

1. **It inherits a template with a known, undocumented bless gap** (§2.2) — copying
   `GoldenImageTest.cpp`'s shape means copying a comparator that has never once been re-blessed
   in this repo's history, against a renderer that is about to enter its most pixel-churning
   period ever (§1.1's five queued arcs). If the spec does not explicitly solve re-bless up
   front, this becomes the "declared knob nobody has watched apply" pattern the host-witness
   arc's own founding lesson exists to catch (§1.3's spirit, applied to the wrong layer).
2. **It is a second, parallel comparator surface to the one that already exists and is already
   proven** (`[gpu][thumbs]`'s control/subject check). Adding `[golden]` semantics next to it
   risks the two drifting — one asserting "no cross-contamination", one asserting "matches a
   fixed picture" — over the same harvester, doubling maintenance for two overlapping but
   distinct guarantees. A cleaner shape might fold ImageCompare assertions *into*
   `MeshThumbnailHarvestTest.cpp` (or its sibling for materials) rather than opening a third
   file, so there is one home for "the harvester renders the right pixels for the right guid".
3. **It answers a narrower question than the debt that motivated it.** Debt 8 in the F2c
   closeout is about the **golden scene** having only one imported mesh, so the **host-level**
   lanes (`editor-ui`/`runtime-scene`) never exercise a second imported asset end-to-end (drop →
   cook → resolve → render → thumbnail, in the real editor, with the real Browser UI, the real
   cook queue, the real `--settle` barrier). An in-process thumbnail-only golden proves the
   **harvester's** pixels are right in isolation; it proves nothing about `PollAssetWatch`
   staleness bookkeeping, the cook-completion → `InvalidateMesh` wiring (T12a's own "never
   desk-exercised live" caveat), or the Browser row rendering at all. Option 4 and "close debt 8
   properly" are answering two different, both-legitimate questions — the F2c review's own
   recommendation #5 ("add a second imported mesh to the golden scene ... one imported asset is
   exactly one too few") is still not closed by option 4 alone.
4. **Per-reference bless multiplies the exact toil `golden-gate.ps1`'s header already documents
   as expensive for TWO images.** A thumbnail set of even 6-10 subjects (one per `Subject` kind ×
   a couple of asset shapes) means 6-10 diff artifacts to eyeball and 6-10 files to overwrite by
   hand at every renderer milestone, unless the bless-mode design in §3(b) actually ships as part
   of the same spec — if it is deferred "for later", option 4 ships a maintenance trap on day one.
5. **64px is a small canvas for a strict SSIM/dE94 cascade.** `Compare`'s antialiasing rule uses
   a 31×31 SSIM window (`ImageCompare.hpp:177`) — at a 64×64 thumbnail that window covers roughly
   a quarter of the whole image per pixel judged, which may behave differently (more permissive
   or more brittle, untested either way in this codebase) than at the 160×96 lit-cube or
   1280×720 host resolutions where the existing budget-0 precedent was actually measured. This is
   an open empirical question, not a resolved one — see Open Questions.

---

## 5. Recommended shape

Extend the **existing** `[gpu][thumbs]` machinery rather than opening a parallel `[gpu][golden]`
file: add ImageCompare-at-budget-0 assertions to (or beside) `MeshThumbnailHarvestTest.cpp`
comparing each harvested PNG against a committed file under a new
`ReferenceProject/Verify/References/thumbs/<subject>-<name>.png` tree (one file per `Subject`
kind × representative asset, mirroring the harvester's own dispatch tag, not a generic N-image
framework); ship a re-bless mode alongside it in the same spec — the cheapest shape consistent
with this repo's Arc-2 discipline is a single environment-variable gate (checked once, at the
top of the relevant `TEST_CASE`s) that writes the harvested bytes over the reference path instead
of comparing, so re-blessing all thumbnail references after a renderer arc is "set the var, run
the filter `[thumbs]` once, unset the var, `git diff` and eyeball, commit" — never a per-image
manual host launch. Do not build this as a `golden-gate.ps1` lane: it answers a render-path
question the in-process instrument already owns, it cannot cheaply prove backend parity the way
host lanes exist to prove, and CI already runs `[gpu]` unfiltered on the one GPU agent regardless.
Keep it explicitly out of scope for closing Debt 8 — that debt still wants a second imported mesh
(or a Browser-scroll fix) in the actual golden scene so the *end-to-end* editor path is proven,
which this in-process addition does not touch.

---

## 6. Open questions

- **Does the 31×31 SSIM / 10×10-block antialiasing cascade behave sanely at 64×64?** No existing
  test exercises `Compare`/`CompareImages` at this resolution; worth a throwaway probe before
  committing to budget-0 at 64px specifically (vs., e.g., harvesting at 64px but comparing a
  larger intermediate, if the harvester ever exposes one — it currently does not).
- **Should the bless switch be a compile-time-visible env var, or a Catch2 CLI tag/section**
  (e.g. a case tagged `[thumbs][bless]` excluded from normal runs)? The host's own `--bless` is a
  CLI flag; an env var breaks that symmetry slightly. This is a naming/ergonomics call the user
  or a dedicated spec should make, not one this research can settle from directives alone.
- **How many `Subject` × asset-shape combinations are "enough"?** The F2c review's "one fixture
  is one too few" lesson (§4.3) argues for at least two per `Subject` kind that can collide
  (mesh, mesh-kind-material) to catch the C1 class of guid-residency bug in the *thumbnail*
  reference set too — but this is a coverage-vs-bless-cost tradeoff only the spec author can size
  against how often thumbnail-adjacent bugs have actually occurred (this research did not find a
  second historical incident beyond C1/C2/C3, which were all caught by the existing device-less/
  control-subject tests, not by a golden image).
- **Does the parked cvar arc want a say in thumbnail size/light constants before they get
  baked into N committed references?** `kThumbSize`, `kThumbTime`, and the mesh light/ambient
  triple are all `constexpr` today (`MaterialPreviewHarvester.cpp:43,54,1223-1225`); if any of
  them becomes a cvar later, every committed thumbnail reference re-blesses again on that arc
  too, which is worth flagging to whoever owns that parked arc's trigger list.
