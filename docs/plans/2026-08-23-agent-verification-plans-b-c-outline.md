# Agent Verification -- Plans B and C -- OUTLINE ONLY

> **STATUS: PROVISIONAL. NOTHING HERE IS DECIDED.**
>
> These are rough outlines, written before Plan A was implemented, and kept
> only so a later session inherits concrete thinking instead of a blank page.
> **Plan A's implementation is expected to change them** -- the "What Plan A
> will teach us" sections below name exactly which parts are most likely to
> move. Do NOT execute from this document. Promote a section to a real plan
> (via `superpowers:writing-plans`, with its own spec -> plan coverage diff)
> only after Plan A's desk checkpoint has landed and this outline has been
> re-read against what was actually learned.

Spec: `docs/specs/2026-08-23-agent-verification-offscreen-design.md`
Plan A: `docs/plans/2026-08-23-agent-verification-offscreen-hosts.md`

---

# Plan B (outline) -- Image comparison and reference images

**Goal:** Turn Plan A's manual "do these two PNGs look the same" desk check into
an automated, perceptually-sound gate that can run on every build.

**Spec requirements owned:** R21 (cascade), R22 (knobs), R23 (size mismatch as a
named error), R24 (reference hierarchy), R25 (blessing), R26 (automated parity).

## >>> The licensing fact that shapes this whole plan <<<

Playwright's `packages/utils/image_tools/compare.ts` carries an **Apache License
2.0** header, copyright Microsoft. **A C++ port is legally clean with
attribution** -- which goes in the repo's existing `NOTICE.md`.

That converts B's central question from "can we?" to "should we port or write
fresh?" A port is faster and inherits tuned constants; a fresh implementation
avoids a second-hand understanding of code we will have to debug. **Undecided.**
Whoever writes the real plan should read `compare.ts`, `colorUtils.ts`,
`imageChannel.ts` and `stats.ts` in full first -- this outline only saw
`compare.ts` and `comparators.ts`.

## Rough task shape

1. **`colorUtils` -- RGB -> Lab, and `colorDeltaE94`.** Pure math, trivially
   unit-testable against published dE94 pairs. The constant `1.0` is the
   just-noticeable-difference and is **derived, not tuned** -- do not "improve"
   it by fitting to our hardware.
2. **`ImageChannel` -- per-channel planes with padding.** Playwright pads with
   alternating magenta/green (`[255,0,255]` / `[0,255,0]`) so border windows
   never see a uniform field that would read as flood fill. Padding size is
   `max(VARIANCE_WINDOW_RADIUS, SSIM_WINDOW_RADIUS)` = 15.
3. **`FastStats` -- integral images for windowed variance and SSIM.** The
   performance-critical piece; naive per-pixel windows over 31x31 will be far
   too slow at 1280x720. This is the task most likely to be underestimated.
4. **The cascade itself.** Exact -> dE94 <= 1.0 -> 3x3 flood-fill variance
   (`var1 == 0 || var2 == 0` means it cannot be antialiasing) -> SSIM over 31x31
   at `>= 0.99`. Returns a differing-pixel count.
5. **The knobs.** Per-pixel tolerance and aggregate (`maxDiffPixels`,
   `maxDiffPixelRatio`, **default 0**) are independent. Unity adds a third,
   `AverageCorrectnessThreshold`, which catches uniform slight-drift that a
   per-pixel test passes and a pixel count never sees -- **probably worth
   having, not yet decided.**
6. **Diff image output.** Red = real difference, yellow = classified as
   antialiasing, grey = unchanged (blended toward white at 0.1). The diff image
   is the artifact that makes a failure diagnosable, so it is not optional.
7. **Size mismatch as a separate named error.** Pad both to the max, report the
   mismatch **alongside** the pixel count rather than aborting. Never silently
   rescale.
8. **Should-fail fixtures.** A comparator tested only on pairs it correctly
   passes is untested in the direction that matters. Playwright keeps a
   `julia-ssim-trap` case precisely because SSIM false-passes. **We need our own
   trap corpus**, and generating it is real work -- likely the second-most
   underestimated task here.
9. **Reference-image hierarchy.** Unity's `ColorSpace/Platform/GraphicsAPI`
   shape, adapted: an image lives at the most general level that is still
   correct, and resolution walks up. Needed because `mesh.hlsl`'s `#if SPIRV`
   split means D3D12 and Vulkan sometimes must differ and sometimes must not.
10. **Blessing.** A documented, one-command way to accept a new reference,
    writing to the level the image resolved from. **Without this the gate gets
    disabled the first week an intentional visual change lands.**
11. **Wire it to the hosts.** Either a `--compare <ref>` flag on the host, or a
    separate `arccompare` tool that consumes Plan A's captures. **Undecided** --
    see below.
12. **The parity gate as an automated test**, replacing Plan A Task 14's manual
    eyeball: offscreen vs windowed, both backends.
13. Desk checkpoint.

## What Plan A will teach us

- **Whether cross-format parity is even a problem in practice.** Plan A Task 14
  compares `format=9` against `format=11` by eye. If they turn out visually
  identical, the cascade's stage 2 may be doing all the work and stages 3-4 may
  be over-engineering for our case. **If they differ visibly, B gets more
  important and possibly larger.**
- **Where comparison belongs.** If Plan A's `VerifyReport` grows naturally into
  the right home, `--compare` is a flag; if captures are more useful as files
  post-processed by tooling, a separate binary is better. Task 11 above should
  not be decided before A ships.
- **What the real per-pixel deltas are.** The threshold must be derived from a
  measured delta between the two paths. That measurement does not exist yet --
  Plan A produces the first capture pairs that make it possible.
- **Whether `--settle` (Plan A Task 10) already removes most nondeterminism.**
  If it does, the aggregate knob can be tight; if not, B inherits a flakiness
  problem A could not solve.

## Known risks

- **`FastStats` performance.** Integral images over three channels at 1280x720,
  twice, per comparison. If this is slow the gate will not run per-build, which
  defeats it.
- **The trap corpus is a research task, not a coding task.** Budget it as such.
- **Do not let the comparator's elegance drive scope.** The deliverable is a
  gate that catches a missing mesh or a wrong normal matrix. That is a coarse
  signal; the cascade exists to suppress false positives, not to be a
  perceptual-science project.

---

# Plan C (outline) -- Identity, the script tier, actionability, and trace

**Goal:** An agent drives the real editor by stable identity, with actions that
wait on measured conditions rather than sleeps, and produces a trace that
localises a failure to a step without a re-run.

**Spec requirements owned:** R9 (identity addressing), R10 (`###` discipline
extension), R11 (script steps), R12 (SDL injection), R13 (actionability), R15
(trace).

## Rough task shape

**Phase 1 -- identity (can start before the script tier)**

1. **Audit which widgets a verification script actually needs**, then assign
   stable `###` ids to them. Document windows are already done --
   `"###meshdoc_" + m_data.id.ToString()` (`MeshDocument.cpp:98`), plus
   `spritedoc_` / `matdoc_` / `crashdoc_`. Most in-panel widgets are plain
   labels. **This is mechanical and improves the editor's identity hygiene
   whether or not an agent ever runs**, so it is worth doing even if C stalls.
2. **Entity addressing by `Identity.id`** (`Components.hpp:274-282`, a real
   `Guid`, serialized alongside the name). **Strict resolution**: zero matches
   and two matches are distinct, separately-reported failures.
3. **Probe output carries both `name` and `id`** so an agent bootstraps from a
   readable query and then addresses durably. Plan A Task 9 may already have
   established the shape -- check before duplicating it.

**Phase 2 -- the script tier**

4. **Script file format and parser.** JSON step list. Steps: `key`, `click`,
   `move`, `screenshot`, `probe`. **No `wait-frames`** -- it was deliberately
   deleted from the spec; a step that needs to wait names the condition.
5. **`SDL_PushEvent` injection**, upstream of the real backend so the agent
   drives the identical input path a human does.

**Phase 3 -- actionability**

6. **Stable** -- the target's screen-space bounds unchanged across two
   consecutive rendered frames. Playwright's definition, measured rather than
   slept.
7. **Hit-target** -- before a click at (x,y), probe (x,y) via
   `FrameDesc::pickPixel` and confirm the entity there is the intended target.
   **We already own this primitive**; Playwright had to inject script into the
   page for it.
8. **Per-action check sets, plus `force`.** `click` needs all checks; a key
   press needs no hit-target test and demanding one would fail keyboard steps
   for no reason.

**Phase 4 -- trace**

9. **`--trace <path>`**: per step, the step record, a capture **before and
   after**, probe results at that point, and log output during it. The
   before/after pairing is the load-bearing part -- an end-state screenshot
   shows a wrong result, a before/after pair shows which step made it wrong.
10. **The engine log belongs in the trace.** The `[nri-graph]` / census /
    resolver lines are where this engine explains itself; the census line alone
    resolves a whole class of "why is nothing on screen."
11. **Crash and hang reports belong in the trace.** Diagnostics already
    auto-capture to `<exe dir>/diagnostics`. Carrying the report means an agent
    is not left to discover a directory nobody told it about -- the exact
    failure mode this arc found in its own evidence base, where six weeks of
    driver faults sat in a log nobody was reading.
12. **Round-trip test**: a deliberately failing script produces a trace whose
    before/after pair localises the failing step **without a re-run**. This is
    the only test of the trace that means anything.
13. Desk checkpoint.

## What Plan A will teach us

- **Whether `SDL_PushEvent` actually reaches ImGui cleanly with a hidden
  window.** Plan A proves the hidden window services the event pump; it does
  **not** prove injected events land the way real ones do. If they diverge, the
  fallback is injecting into `ImGuiIO` directly -- which was the original
  Section 1 proposal and carries its own divergence risk. **This is C's single
  biggest unknown.**
- **Whether the editor's full-frame capture (Plan A Task 11) is good enough to
  verify UI.** If the composited capture works well, C's trace is mostly
  plumbing. If it is awkward, C inherits that problem.
- **How much of the report shape A already fixes.** C should extend A's
  `VerifyReport`, not build a parallel artifact.
- **Whether pinned layout (A Task 11) makes widget positions stable enough**
  that hit-target checks are meaningful. If layout still shifts between runs,
  actionability gets harder.

## Known risks

- **`imgui_test_engine` is the road not taken.** The user chose to borrow the
  idea rather than vendor it, because its licence is not MIT ("free for
  individuals, educational, open-source and small businesses; paid for larger
  businesses"). The consequence: **we do not get its "find its way" behaviour**
  -- scrolling to an off-screen item, un-collapsing a header, CTRL+Tabbing to a
  window. If scripts start failing because a target is scrolled out of view,
  that is the gap, and revisiting the vendoring decision is a legitimate
  response rather than a failure of this plan.
- **Scope creep toward a general UI test framework.** C's deliverable is agents
  verifying their own work. It is not a replacement for the desk pass and not a
  product feature.
- **Phase 1 is independently valuable and should not be held hostage** to the
  script tier. If C stalls, the `###` discipline and entity addressing still
  leave the editor better than they found it.

---

## Sequencing note

B and C are independent of each other and both depend only on A. If a choice is
needed after A lands: **B if the parity check at A's desk checkpoint looked
shaky** (it means visual regressions are real and going undetected), **C if it
looked clean** (parity is fine, so the next bottleneck is that agents still
cannot drive the editor).
