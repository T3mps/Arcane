# Unreal's screenshot-comparison system — four questions Playwright structurally cannot answer

**Date:** 2026-08-26
**Context:** Plan B (Servitor comparator), Task 13. Banked by the user 2026-08-25
and scoped explicitly to this task.
**Companion:** `docs/specs/2026-08-25-package-tiering-design.md`

---

## Why this note exists, and what it may NOT do

Playwright is the **reference of record** for our comparator (user ruling,
2026-08-25). Its arithmetic was ported bit-for-bit and validated against its own
fixture corpus — 21 pairs, **zero divergence**, both SSIM traps caught
(`ArcaneTests/src/ImageCompareConformanceTest.cpp`). That is the oracle proving
Tasks 1–5, and bit-parity is a Global Constraint.

**Nothing in this note may change comparator arithmetic, constants, or cascade
structure.** Where the research suggests something should change, it is written
down as a finding for a *future* plan and explicitly marked as such. Two findings
below are of that kind (Q1-F1, Q2-F1); neither was acted on.

Playwright is a browser tool. It has never faced a GPU, a driver, an RHI, a
shader model, or a temporal-antialiasing history buffer. Those are exactly the
axes Servitor lives on, so a second body of evidence was worth having — from
someone who has run image tests across platforms and graphics APIs for over a
decade.

**Sourcing boundary — documentation and observable behaviour ONLY, never
source.** Playwright is Apache-2.0, which is why porting it with `NOTICE.md`
attribution is clean. Unreal's source is licensed and is *not* that. Nothing in
this note is read from, quoted from, paraphrased from, or transcribed from
Unreal Engine source. Every claim below cites a public Epic documentation page
by URL. Where public documentation does not answer a question, this note says
so and records what was searched, rather than closing the gap from source. This
matches the standing "no leaked proprietary engine code, ever" rule.

One consequence worth stating plainly: a couple of answers below are
**partial**, because Epic documents the *behaviour* of these systems well and
the *data structures* barely at all. A partial answer honestly bounded is the
correct output here.

---

## Q1 — KNOB STRUCTURE

> Unreal runs `ToleranceAmount` + `MaximumLocalError` + `MaximumGlobalError` —
> three knobs where we have two. We dropped Unity's third deliberately (spec
> amendment 3). Does Unreal's third express something our two cannot, or is it
> the same thing already rejected?

### Answer: it is a genuinely different third knob, and it is NOT the one we rejected.

The three-way structure, from Epic's own descriptions:

| Unreal knob | Documented description (verbatim) | Scope |
|---|---|---|
| `tolerance_amount` | *"For each channel and brightness levels you can control a region where the colors are found to be essentially the same. Generally this is necessary as modern rendering techniques tend to introduce noise constantly to hide aliasing."* | **per pixel** |
| `maximum_local_error` | *"After you've accounted for color tolerance changes, you now need to control for local acceptable error…focusing on a smaller subset of the image to locate hot spots of change."* | **per region** |
| `maximum_global_error` | *"After you've accounted for color tolerance changes, you now need to control for total acceptable error. Which depending on how pixels were colored on triangle edges may be a few percent of the image being outside the tolerance levels."* | **whole image** |

Source: [unreal.AutomationScreenshotOptions, UE 5.4 Python API](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AutomationScreenshotOptions?application_version=5.4).
The prose docs put it the same way: MaximumLocalError *"focuses on a smaller
subset of the image. These chunks will be compared to the local error in an
attempt to … locate hot spots of change"*
([Screenshot Comparison Tool, UE 5.8](https://dev.epicgames.com/documentation/en-us/unreal-engine/screenshot-comparison-tool-in-unreal-engine)).

Ours, for comparison (`ArcaneClient/src/Arcane/Assets/ImageCompare.hpp`):

| Our knob | Scope |
|---|---|
| `CompareOptions::maxColorDeltaE94` (default 1.0, the JND) | **per pixel** |
| `ImageCompareOptions::maxDiffPixels` | **whole image** |

So the mapping is: `tolerance_amount` ↔ `maxColorDeltaE94`,
`maximum_global_error` ↔ `maxDiffPixels`, and `maximum_local_error` ↔ **nothing
we have**.

### It is not Unity's rejected knob

Unity's third knob is `AverageCorrectnessThreshold` — an *average* bound. We
dropped it (spec amendment 3) because it catches **uniform slight drift**: a
whole image nudged fractionally, which a per-pixel threshold passes and a
differing-pixel count never sees. Our measured distributions were p50=0, p90=0
on both backends, so there was no such drift to catch.

Unreal's `MaximumLocalError` catches the **opposite** failure: not uniform drift
but a **spatially concentrated** difference — a small, dense, badly-wrong region
that is diluted below the whole-image budget. The two knobs are dual, not
duplicate. Rejecting one says nothing about the other, and the spec amendment
that dropped Unity's third did not rule on this one.

### Does the hole exist for us today? No — because our budget is zero.

The gap `MaximumLocalError` closes only opens when the aggregate budget is
**non-zero**. Worked example at ReferenceProject's 1280×720: a single 32×32
block rendering completely wrong is 1024 px, or **0.111%** of the frame. Under a
0.5% global budget that passes. Under a `maxDiffPixels` of **0** — which is what
the per-build gate actually runs, and what all four lanes are measured at
(0-diff, both backends, both configs, Task 12) — nothing can hide under it,
because there is no budget to hide under. At zero, a local-error knob is
strictly redundant.

Two nuances that keep this honest rather than smug:

1. Our cascade already contains **local** tests — stage 3 is a 3×3 flood fill
   and stage 4 is SSIM over a 31×31 window (`ImageCompare.hpp`). But those are
   per-pixel *classifiers* (they decide whether one pixel counts as a
   difference), not an aggregate *budget over a region*. Having local structure
   awareness is not the same as having a local error bound, and it would be
   wrong to claim stage 4 covers this.
2. The place we *do* run a non-zero budget is the cross-backend question — the
   measured 121-pixel D3D12↔Vulkan delta that decides shared-vs-split
   references. Those 121 pixels are ImGui **text glyphs**, i.e. exactly the
   spatially clustered kind of difference `MaximumLocalError` was built to
   notice. Today that comparison is an authoring decision made at a desk, not a
   gate with a threshold, so nothing is at risk.

### FINDING Q1-F1 — for a FUTURE plan, NOT acted on

> The moment any lane runs a **non-zero** `maxDiffPixels`, the absence of a
> local-error bound becomes a real hole: a dense, localized, genuinely-wrong
> region can pass a whole-image budget. The obvious first candidate is a
> tolerant cross-backend lane over shared references. If that lane is ever
> built, a per-region bound should be specified with it, and Unreal is the
> prior art for its shape.

Not acted on, per HARD BOUNDARY 1. No comparator arithmetic, constant, or
cascade stage was touched by this task.

---

## Q2 — REFERENCE VARIANT AXES

> Task 7 chose the minimum: shared vs backend. Unreal keys on more (platform,
> RHI, quality level). The useful question is not WHAT they key on but **WHAT
> PRESSURE MADE THEM ADD EACH AXIS** — that predicts which axis we need next.

### Answer: partially answered. The axes are documented; the *pressure* is documented for two of them and inferable for a third, and one sub-claim in the question could not be confirmed at all.

**What is documented.** Unreal buckets ground-truth screenshots by
**`Platform_RHI_ShaderModel`**, and the same page states that *"further
refinement is needed"* beyond that bucketing
([Screenshot Comparison Tool, UE 5.8](https://dev.epicgames.com/documentation/en-us/unreal-engine/screenshot-comparison-tool-in-unreal-engine);
also the [4.27 page](https://dev.epicgames.com/documentation/en-us/unreal-engine/screenshot-comparison-tool?application_version=4.27),
which additionally names the on-disk home as the project's
*"Saved > Automation > Comparisons folder"*).

That is three axes where we have one:

| Unreal axis | Our equivalent |
|---|---|
| Platform | **none** — we are Windows-only today |
| RHI | `Verify/References/<backend>/` (D3D12, Vulkan) — the one axis Task 7 built |
| Shader Model | **none** |

**The resolution rule is the same shape as ours.** Unreal does not require an
exact key match; when several ground truths exist *"the system will always pick
the ground truth screenshot matching closest based on the metadata"*
(4.27 and 5.8 pages, verbatim). Ours walks up from most specific
(`References/<backend>/<name>.png`) to most general
(`References/<name>.png`), landing at `ReferenceLevel::Backend` or
`ReferenceLevel::Shared` (`ArcaneClient/src/Arcane/Host/ReferenceImages.hpp`).
**Two independent engines converged on "closest match, with a general fallback"
rather than "one image per full key."** That is the meaningful confirmation of
Task 7's design, and it is stronger evidence than the axis list.

### The pressure behind each axis

**RHI — confirmed, and it is the pressure we already felt.** Epic exposes RHI
in *two* independent places, which is what makes it convincing rather than
incidental. It is a bucketing key for ground truths, **and** it is a first-class
field in the test-exclusion mechanism:
`+ExcludeTest=(Test="…",Reason="…",RHIs=("Vulkan","DirectX 11"),Warn=False)`
([Configure Automation Tests, UE 5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/configure-automation-tests-in-unreal-engine)).
An engine only builds a per-RHI *skip* list after discovering that some tests
are legitimately correct on one graphics API and legitimately wrong on another.
That is precisely our `mesh.hlsl` `#if SPIRV` split and our measured 121-pixel
ImGui text delta. Same pressure, same axis, arrived at independently.

**Shader Model — inferable, not documented.** Epic does not state why it is in
the key. The mechanism is not mysterious: one RHI compiles the same material to
different shader permutations at different feature levels
([ERHIFeatureLevel::Type](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/ERHIFeatureLevel__Type)),
and different permutations can legitimately produce different pixels. Marked
inferred, not cited.

**"Add As Alternative" — the pressure axis Epic reached for when the key was not
enough.** This is the most useful finding in Q2, because it is a *documented
escape hatch from keying altogether*. Epic: *"Occasionally, two images can both
be correct… it adds another ground truth version of the image, and when
comparing images, the system will always pick the ground truth screenshot
matching closest based on the metadata. This option will be grayed out if the
screenshot is from an identical device"* (4.27 and 5.8 pages, verbatim). The
last clause is the tell: alternatives are keyed on **device**, an axis finer
than Platform_RHI_ShaderModel and one Epic clearly did *not* want in the folder
structure. They hit "the key does not separate these two correct images" and
solved it by allowing **multiple ground truths at one key**, disambiguated by
richer metadata at compare time, rather than by adding another directory level.

### FINDING Q2-F1 — the axis we most likely need next, for a FUTURE plan

> Not platform, and not shader model. **Configuration** — Debug vs Release vs
> Dist. Both are currently forced onto the *same* reference tree, and Task 12's
> gate runs both configs against one set of images. That works today only
> because it is measured to work (0-diff on all four lanes). Nothing in the
> hierarchy could express it if it stopped working; `ResolveReference` takes
> `name` and `backend` and nothing else.
>
> Unreal's answer suggests the cheap move if that day comes is **not** a third
> directory level. It is Epic's "Add As Alternative" shape: allow more than one
> blessed image at a key and pick the closest by richer metadata — the report
> JSON already carries the metadata such a choice would need. Recorded as a
> design option, not a recommendation, and not acted on.

### What could NOT be answered

**"Quality level" as a reference-keying axis is UNCONFIRMED.** The question
assumed Unreal keys on it. Public documentation names
`Platform_RHI_ShaderModel` and does not name quality level or scalability
settings as part of the bucketing key. Searches run:
`Unreal Engine screenshot functional test "quality level" scalability ground truth image comparison different quality settings`;
`Unreal Engine automation screenshot metadata "Platform" "RHI" "Feature Level" ground truth folder path fallback closest match`;
`unreal.AutomationScreenshotMetadata python api properties name platform rhi feature_level device_model quality_level`.
The Python API surfaces `AutomationScreenshotOptions` (the capture *options*
struct) but no `AutomationScreenshotMetadata` page; a UE 5.2 docs mirror
returned HTTP 403. The complete metadata field list appears to exist only in
engine headers, which HARD BOUNDARY 2 forbids reading. **Recorded as
unanswerable from public sources.** The `AutomationScreenshotOptions` docs do
carry a `resolution` field — *"The desired resolution of the screenshot, if none
is provided, it will use the default for the platform setup in the automation
settings"* — which shows resolution is a capture *input* they pin rather than an
axis they key on; that is the same choice we make by fixing 1280×720.

---

## Q3 — THE APPROVAL WORKFLOW

> The spec's own warning is that a gate nobody can cheaply bless gets switched
> off in the first week. Unreal has a decade of evidence on what actually causes
> teams to disable image tests.

### Answer: fully answered, and it validates `--bless` while exposing one thing we lack.

Epic's workflow, verbatim from the [4.27 page](https://dev.epicgames.com/documentation/en-us/unreal-engine/screenshot-comparison-tool?application_version=4.27):

- **Add** — *"Adds the screenshot to the ground truth folder and adds it to a
  pending change list in your Source Control."*
- **Replace** — *"Deletes all examples of ground truth data and replaces them
  with the newest screenshot as the new ground truth data."*
- **Add As Alternative** — *"Occasionally, two images can both be correct…"*

And the bootstrap case: *"The first time you run a screenshot automation test, a
warning in the Message Log and the Screenshot Browser will indicate that you
need to add the newly taken screenshot as your Ground Truth image"* (5.8 page).
Failures *"automatically show up in the Screenshot Browser tab for comparison"*,
where a slider blends between Ground Truth, Difference, and Incoming.

Five things this confirms, and one it does not:

1. **Blessing must be one action, not a procedure.** Epic's is a button. Ours is
   `--bless` on the same command line that just failed. Both are one step.
   Confirmed.
2. **The bootstrap case gets a *warning*, not an error.** A first run with no
   reference is a normal state to be resolved, not a build break. Ours behaves
   the same way — `ReferenceLevel::None` resolves rather than throwing, and
   `--bless` disables the compare conjunct for the whole run precisely so a
   first bless can converge with nothing on disk (`HostConfig.hpp:134-138`).
   Confirmed, and arrived at independently.
3. **Blessing is a source-control operation and Epic makes that explicit** —
   "adds it to a pending change list." An approved image is a reviewable diff,
   not a mutation of a shared server. Ours writes into the project tree under
   git. Confirmed.
4. **Diff review is visual and side-by-side.** Epic ships a blend slider. We
   write a diff PNG to `Saved/Verify/<name>-<backend>-diff.png`
   (`ReferenceImages.hpp`), red for a real difference and yellow for a pixel
   classified as antialiasing, and Jenkins archives it on failure only. Weaker
   than a slider, but it is the same artifact serving the same purpose.
5. **Epic ships a bulk hammer, and it is dangerous.** "Replace" *deletes all
   examples of ground truth data*. That is the affordance a frustrated team
   reaches for at 6pm, and it silently destroys every hard-won alternative for
   that image. We have no equivalent, and **that is a feature, not a gap** —
   `--bless` writes exactly one image, at the level the reference resolved from.

**What we lack: a triage surface.** Epic's Screenshot Browser shows every
failing comparison from a run at once, with its diff, ready to adjudicate. Ours
reports one comparison per host invocation, and the aggregate view is
`golden-gate.ps1`'s console output plus archived PNGs. For four lanes that is
fine. It would not be fine at forty. Recorded as an observation; not a finding
that needs acting on at this corpus size.

---

## Q4 — NONDETERMINISM POLICY

> How they classify a test as inherently flaky vs broken — the condition our
> settle loop exists to make unnecessary.

### Answer: fully answered, and the answer is a genuine philosophical split from ours.

Unreal attacks nondeterminism from **two** directions. We use only the first,
and deliberately so.

**Direction 1 — suppress the noise at capture time.** `AutomationScreenshotOptions`
is largely a determinism kit
([UE 5.4 Python API](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AutomationScreenshotOptions?application_version=5.4)):

- `delay` — *"The delay before we take the screenshot (measured in seconds).
  Both this delay and the frame delay must be met before the screenshot is
  taken."*
- `frame_delay` — *"…measured in number of frames. Both this frame delay and the
  time delay must be met…"*
- `disable_noisy_rendering_features` — *"Disables Anti-Aliasing, Motion Blur,
  Screen Space Reflections, Eye Adaptation, Tonemapper and Contact Shadows,
  because those features contribute a lot to the noise in the final rendered
  image."*
- `disable_tonemapping` — *"Disables Eye Adaptation and sets Tonemapper to fixed
  gamma curve. Should generally be on unless testing tone mapping or other
  post-processing results."*
- `override_time_to` — *"Overrides World Time, Real Time to the value provided.
  Sets Delta Time to 0."*
- `ignore_anti_aliasing`, `ignore_colors` — comparison-side noise suppression.

Epic is explicit that the default tolerance exists *because* of residual noise:
*"we default to low, because generally there's some constant variability in
every pixel's color introduced by TxAA."*

**Two observations that matter more than the list.**

First, **`delay` + `frame_delay` is a conjunction — "both … must be met" — and
they are denominated in different units, one in seconds and one in frames.**
Epic evidently found that neither unit alone was sufficient. That is directly
relevant to us: OWED defect 1 in this arc is that `--settle` counts **attempts**
while the condition it waits on (shader-compiler idle) is denominated in
**milliseconds**, so ~3.3 ms/attempt burns a 30-attempt budget in ~100 ms.
Unreal hit the same class of problem and resolved it by requiring *both* a time
bound and a frame bound. **Independent corroboration that the fix for defect 1
is a conjunction of a time budget and an attempt budget, not a bigger attempt
count.** Recorded here for whichever plan owns that defect; not acted on.

Second, **`override_time_to` sets delta time to zero** — Epic *freezes time* for
the capture. Our settle loop freezes the render/sim advance once the `--frames`
budget is spent, for the same reason: un-frozen frames never converge on
anything (`HostConfig.hpp:96-115`). Same trick, arrived at independently.

**Direction 2 — declare the test out of scope, in config, with a stated reason.
This is the part we do not have.** Epic ships a first-class exclusion list:

```
+ExcludeTest=(Test="<test or section name>",Reason="<a reason>",Warn=False)
+ExcludeTest=(Test="<test or section name>",Reason="<a reason>",RHIs=("Vulkan","DirectX 11"),Warn=False)
```

([Configure Automation Tests, UE 5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/configure-automation-tests-in-unreal-engine)).
Excluded tests report as **"Skipped"** with the reason attached as an info
event, `Test` accepts a partial name to exclude a whole subtree, and the Test
Automation window carries an **Exclude button** so it can be done from the
editor.

Read this as design evidence rather than a feature list. Epic's answer to
"flaky vs broken" is **not a heuristic and not a retry count**. It is:

- a **human decision**, made once and written down;
- with a **mandatory `Reason` field** — the exclusion is self-documenting;
- **scoped to the axis that actually broke**, via `RHIs=(…)`, so excluding a
  test on Vulkan leaves it enforced on D3D12;
- **loud rather than silent** — the test reports *Skipped with a reason*, not
  *Passed*, so an excluded test stays visible in every report;
- with a `Warn` toggle controlling how noisy that visibility is. *(What
  `Warn=False` versus `Warn=True` actually does is **not** specified on that
  page; recorded as undocumented rather than guessed.)*

**The contrast with ours is the real finding.** Epic makes flakiness
*declarable*: they accept that some image tests will be inherently unstable and
build a governed path for saying so. We took the other road — the settle loop
exists so that a test is either deterministic or it is a bug, and there is no
third state. That is a stronger position and it is currently earning its keep:
both runtime lanes measure 0-diff on both backends and both configs.

But it is also **unfalsifiable in the direction that matters**. When a lane goes
red for a reason nobody can fix today, we have exactly two moves: fix it, or
delete it from `golden-gate.ps1`. Deletion is silent, carries no reason, and is
indistinguishable in the script from "this lane never existed." That is the
mechanism by which a gate quietly stops covering things — the failure mode the
spec's own "switched off in the first week" warning names.

We have three such lanes-in-waiting right now: the two editor lanes are red for
OWED defects 1 and 2, and defect 3 (`settleAttemptsUsed` absent from
`VerifyReport`) is the missing instrument that would let a report distinguish
"converged" from "ran out of attempts."

### FINDING Q4-F1 — for a FUTURE plan, NOT acted on

> If a lane must be taken out of the gate before its defect is fixed, it should
> be **excluded with a stated reason and reported as skipped**, never deleted
> from `golden-gate.ps1`. Epic's shape is the right one: a reason string is
> mandatory, the exclusion is scoped to the axis that broke (for us: backend,
> configuration, host), and the excluded lane still appears in the output.
> Whichever plan owns the two editor-lane defects should decide whether it
> needs this; it is not needed while the lanes are simply red and known.

---

## Summary — the four questions in one line each

| Q | Answer | Acted on? |
|---|---|---|
| 1. Knob structure | Unreal's third knob is a **per-region** bound, dual to Unity's average-correctness knob and NOT the one we rejected. It closes a hole that only exists when the aggregate budget is non-zero; ours is **zero**, so it is redundant today. | No — Finding Q1-F1 for a future plan |
| 2. Reference variant axes | Axes are `Platform_RHI_ShaderModel`, and Epic says *"further refinement is needed"*. **RHI is confirmed to come from the same pressure we felt.** The resolution rule — closest match with a general fallback — **independently matches Task 7's**. "Quality level" as a keying axis is **unconfirmed from public docs**. Our likeliest next axis is **configuration**, and Epic's "Add As Alternative" is the cheaper shape than another directory level. | No — Finding Q2-F1 for a future plan |
| 3. Approval workflow | One-click bless, first-run is a *warning* not an error, approval is a source-control diff, visual side-by-side review. **All four match what we built, independently.** Their bulk "Replace" (deletes all ground truth) is a hazard we correctly lack. We lack a multi-failure triage surface; fine at four lanes. | No change needed |
| 4. Nondeterminism policy | Two directions: suppress noise at capture (we do this), **and declare a test out of scope in config with a mandatory reason, scoped by RHI, reported as Skipped** (we do not). Their `delay`+`frame_delay` conjunction — *both* a time bound and a frame bound — **independently corroborates the fix shape for OWED defect 1**. | No — Finding Q4-F1 for a future plan |

## Sources

- [Screenshot Comparison Tool in Unreal Engine — UE 5.8](https://dev.epicgames.com/documentation/en-us/unreal-engine/screenshot-comparison-tool-in-unreal-engine)
- [Screenshot Comparison Tool — UE 4.27](https://dev.epicgames.com/documentation/en-us/unreal-engine/screenshot-comparison-tool?application_version=4.27)
- [unreal.AutomationScreenshotOptions — UE 5.4 Python API](https://dev.epicgames.com/documentation/en-us/unreal-engine/python-api/class/AutomationScreenshotOptions?application_version=5.4)
- [Configure Automation Tests in Unreal Engine — UE 5.7](https://dev.epicgames.com/documentation/en-us/unreal-engine/configure-automation-tests-in-unreal-engine)
- [ERHIFeatureLevel::Type — UE 5.7 API reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/RHI/ERHIFeatureLevel__Type)
