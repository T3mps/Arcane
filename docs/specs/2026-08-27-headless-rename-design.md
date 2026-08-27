# `--offscreen` → `--headless`, and the vocabulary collision that blocked it

**Status:** design, approved in brainstorming 2026-08-27. Supersedes the naming decision in
`docs/specs/2026-08-23-agent-verification-offscreen-design.md` ("Naming"), which chose
`--offscreen` for the reason this spec now resolves rather than accepts.

**Sequenced first, ahead of the four owed engine defects** (see "What this unblocks"). The two
are separate arcs on purpose: this one is a broad mechanical rename and that one is four
semantic fixes, and mixing them would make both diffs unreviewable.

---

## Why

The mode flag is `--offscreen`. The tool the industry actually calibrates against calls this
**headless**, and its documented definition is our mode almost word for word. From Chrome's own
docs, on the unified headless introduced in Chrome 112:

> "Chrome now has unified Headless and headful modes."
>
> "Chrome creates, but doesn't display, any platform windows. All other functions, existing and
> future, are available with no limitations."

*Creates but does not display windows, with everything else fully available* — that is exactly
what `--offscreen` does, and "headless" is what it is called everywhere else. Ours is the
outlier name, and a user or agent reaching for the obvious flag finds nothing.

*(Scope of that citation, stated because the distinction is what made the old name defensible:
the Chrome page confirms window behaviour and unification. It does **not** address GPU or
hardware acceleration either way, so nothing here rests on a claim about headless Chrome's
rendering path. Our mode's device-ful-ness is a fact about our engine, not an inference from
Chrome.)*

**USER DIRECTIVE, 2026-08-27**, after being shown the counter-argument below and reaffirming:
*"I want to change it to headless. it makes much more sense to many people I would imagine,
including me."*

### The counter-argument, and why it loses

The original decision (`2026-08-23` spec, "Naming") declined `--headless` on a real ground:

> The codebase already uses "headless" to mean *device-less*
> (`SceneRenderResolver.hpp:211`: "this one CAN report bound in a headless census"); this mode
> is *windowless but device-ful*.

That was correct, and it is why this spec exists: **the collision is the work.** The flag rename
is trivial on its own; what makes it an arc is that one word currently carries two meanings, and
the flag needs the one the rest of the world uses.

**Where the device-less sense is actually load-bearing** — checked at source, because the
original spec cited only the weaker of the two sites. It is `SceneRenderResolver.hpp:76`, on the
`batcher` field, where the word decides behaviour:

> "The frame batcher registered materials bind into. Null (**a headless host or a test**)
> disables material binding; sprite resolution still works."

`:211` is the *contrast* case, not the decision — it explains why the mesh pair, unlike the
sprite pair, "CAN report bound in a headless census". Both need renaming; only `:76` changes
what the code does.

Resolution: the *engine-internal* sense moves to **`deviceless`**, freeing **`headless`** for the
user-facing mode. The internal comments already gloss "headless" as "device-less" in prose, so
this mostly promotes the gloss to the name.

---

## Decisions

### 1. `--offscreen` is REMOVED, not aliased

**Hard break. No deprecated alias.** (User decision; the alternative — following this repo's own
`--nri-graph` precedent of "DEPRECATED, accepted and ignored… kept so existing scripts and saved
launch args do not fail to boot" — was offered and declined.)

**This is safe, and it was verified rather than assumed.** The CLI refuses unregistered
arguments *before* creating a window or device:

```
> ArcaneRuntime.exe --project ReferenceProject --frames 1 --this-flag-does-not-exist
error: unknown argument '--this-flag-does-not-exist'
<usage>
exit 2
```

That verification is load-bearing. Had the parser *ignored* unknown arguments, removing
`--offscreen` would have silently downgraded every gate invocation to a **windowed** run — a
wrong capture, on the machine carrying the driver hazard, with no error. A hard break is only
defensible because the failure is loud.

**Consequence: every caller must be updated in the same commit as the removal.** There is no
grace period. The complete caller set is enumerated in "Scope" below.

### 2. The device-less sense becomes `deviceless`

Only the sense that *collides* is renamed — the one meaning "no GPU device at all", whose
load-bearing site is the resolver's material-binding decision at **`SceneRenderResolver.hpp:76`**
(with `:211` as its contrast case; see "The counter-argument" above).

**Uses of "headless" that already mean "no window / no UI" are left alone**: they are compatible
with the new flag meaning, and rewriting them would be churn. **This requires an audit, not a
blind sweep** — see "Method".

**Scope, measured 2026-08-27 during planning — the earlier figure in this spec was wrong.**
An earlier revision said "395 code uses". That counted `ThirdParty/`, where the Vulkan headers
alone carry ~119. The real figures:

| | |
|---|---|
| `headless` in non-ThirdParty code | **228** lines |
| …co-occurring with device/backend/GPU vocabulary (**the colliding sense**) | **112** |
| …remainder (no-window sense, **left alone**) | 116 |
| files containing **both** `--offscreen` and `headless` | **8** |
| identifiers (not comments) carrying the word | **`PumpHeadless()`** (2 sites), 2 `TEST_CASE` name strings, 1 filename |

So the colliding sense is **not** a minority to be teased out — it is roughly half, and it is the
dominant sense in engine code (NRI, audio, `Runtime`, `GpuInstrumentation`,
`DeviceCreationVulkan`). **USER DECISION 2026-08-27: rename all ~112 of them**, across ~40
files, so the word means exactly one thing everywhere — chosen over confining the rename to the
8 files where both meanings coexist.

The work is overwhelmingly **comment text**: the only code-shaped changes are `PumpHeadless()`
→ `PumpDeviceless()` and two test-case name strings.

**The 8 co-occurring files are the highest-risk review surface** — they are where a line's sense
is genuinely ambiguous, because both meanings are live in the same file:
`HostConfig.hpp`, `NriGraphContext.{cpp,hpp}`, `EditorApp.cpp`, `ArcaneEditor/src/main.cpp`,
`RuntimeApp.cpp`, `RuntimeFrame.cpp`, `HostConfigTest.cpp`.

### 3. The CLI/config surface renames; the render-technique layer does not

- **Renamed:** the flag (`--headless`), and `HostConfig::offscreen` → `HostConfig::headless`.
  `HostConfig` *is* the parsed command line; its field names should match what a user types.
- **NOT renamed:** `OffscreenVehicle`, `CreateOffscreen()`, `IsOffscreen()`, `[offscreen]` log
  tags, and the rest of the internal "offscreen" vocabulary — **1400 tracked mentions in total,
  803 of them in code** (excluding this document), of which only the **144 live flag sites** are
  in scope here.

The line is **mode versus technique**. `--headless` names the mode a user asks for; "offscreen"
names how it is achieved — rendering to an offscreen target with no swapchain. Renaming the
render layer would be a far larger change (803 code mentions) that nothing in the directive asks
for, and "offscreen" is the *more* precise word for what that layer does.

*Reviewer's note: this is the decision most worth challenging on spec review. The alternative —
rename everything to `headless` for total consistency — is coherent, just much larger.*

### 4. Historical documents are NOT rewritten

`docs/` holds 588 of the 1010 "headless" mentions, across 148 files, overwhelmingly retired plans
and specs. **They stay as written.** They describe things accurately as of their date, and
editing them makes them worse records — the same ruling already applied to the pre-extraction
setup-wizard plans earlier in this project's history.

Only **live operational** documents are updated: the close-out doc, this spec's own siblings
where they give runnable commands, and anything a reader would copy a command out of.

`ThirdParty/` is untouched.

---

## Scope

Measured 2026-08-27 via `git grep` on tracked files, **excluding this document** — which
necessarily names both flags, and whose own mentions would otherwise inflate every figure below
each time it is edited. (That is not hypothetical: an earlier revision of this table reported a
count that had gone stale inside the document stating it.)

| Surface | Size | Action |
|---|---|---|
| `--offscreen`, **live** sites | **144** | rename to `--headless` |
| `--offscreen`, **historical** docs (`docs/plans/*`, the 08-23 and 08-25 specs, the rulings record) | **70** | **leave** (decision 4) |
| `headless` in NON-ThirdParty code | **228** (112 device-sense, 116 no-window) | **audit**, rename the 112 device-sense to `deviceless` |
| `headless` in `docs/` | 588 across 148 files | **leave** (historical), except live operational docs |
| `headless` in `ThirdParty/` | ~167 (mostly Vulkan headers) | **leave** |
| `offscreen` everywhere (all senses) | **1400** tracked, **803** in code | of which only the 144 live flag sites change; the rest **stays** (decision 3) |

### Callers that MUST land in the removal commit

Because there is no alias, each of these breaks the moment `--offscreen` stops parsing:

Enumerated from `git grep -c -- "--offscreen"` rather than recalled. **The `Jenkinsfile` is NOT
on this list** — it contains no `--offscreen` at all; it invokes `golden-gate.ps1`, which builds
the arguments. An earlier draft of this spec listed it, which is exactly the kind of assumed
caller that turns a "hard break lands atomically" claim into a broken tree.

- `scripts/golden-gate.ps1` (1) — `--offscreen` in every lane's `$exeArgs`
- `scripts/desk-verify-servitor.ps1` (1) — the bless invocation
- `ArcaneTests` — `HostConfigTest.cpp` (**46**, by far the densest single file),
  `GoldenImageTest.cpp` (1), `VerifyReportTest.cpp` (1)
- `ArcaneClient` — `HostConfig.cpp` (10), `HostConfig.hpp` (3), `VerifyReport.{cpp,hpp}` (1 each),
  `NriGraphContext.{cpp,hpp}` (1 each), `OffscreenVehicle.{cpp,hpp}` (1 each)
- `ArcaneRuntime` — `RuntimeFrame.cpp` (11), `RuntimeApp.cpp` (11), `RuntimeApp.hpp` (5),
  `RuntimeFrame.hpp` (5), `main.cpp` (2)
- `ArcaneEditor` — `EditorApp.cpp` (12), `main.cpp` (8), `EditorAppFrame.cpp` (6),
  `EditorApp.hpp` (4)
- **`ReferenceProject/Saved/verify-layout.ini` (3)** — the *committed* layout seed. Its comments
  carry a runnable regeneration command. Easy to miss because it is data, not code.
- **`ReferenceProject/.gitignore` (1)** — a comment explaining the seed
- **`ReferenceProject/Verify/Traps/README.md` (4)** — trap-corpus regeneration commands
- **`ArcaneHub/src/lib/views/PackagesView.svelte` (1)** — prose describing the mode
- `docs/2026-08-26-servitor-closeout-and-desk-verify.md` (2) — carries runnable commands
- **ArcaneHub saved launch args** — `RecentProject.args` (`state.rs:45`), split at launch and
  appended to the host's argv (`launch.rs:217`, `:240`). These live in the user's Hub data, not
  the repo. **They cannot be fixed by a commit**, and a project with `--offscreen` saved will
  fail to launch with a loud parse error. Called out so it is a known consequence, not a
  surprise.

  **ArcaneHub's Rust is NOT a code caller** — it contains no `--offscreen` anywhere. Verified,
  because an earlier draft assumed otherwise.

---

## Method

1. **Audit before renaming.** Classify all **228** non-ThirdParty code uses of "headless" into
   *device-less sense* (rename, ~112) and *no-window sense* (leave, ~116). A blind `sed` would
   rename both and destroy the distinction this arc exists to create. The discriminator: does the
   sentence mean **no GPU device** (rename) or **no window / no UI** (leave)? The 8 files holding
   both meanings need line-by-line judgment; the rest are usually uniform per file.
2. **Flag rename and internal rename are separate commits.** One is a user-visible contract
   change; the other is internal vocabulary. Reviewing them together hides both.
3. **The removal and its callers are ONE commit.** A commit where `--offscreen` is gone but
   `golden-gate.ps1` still passes it is a broken tree.

---

## Verification

- `ArcaneTests.exe ~[gpu]` from its own directory. Baseline to beat: **52203 assertions / 1254
  cases**, 0 warnings across Debug/Release/Dist.
- **A negative test that the removal actually happened**: `--offscreen` must now produce
  `error: unknown argument '--offscreen'` and exit 2. Without this the rename could half-land —
  `--headless` added, `--offscreen` never removed — and every existing caller would keep working,
  hiding the fact that nothing was finished.
- `scripts\golden-gate.ps1 -Configuration Debug -AdvisoryLanes ArcaneEditor` must still reach its
  lanes and produce `golden-gate-summary.json` with `gatePassed: true`. **The two editor lanes
  remain KNOWN-RED** — this arc does not fix them and must not appear to.
- The desk-verify script must still complete phases A–C.

**Do not re-bless anything in this arc.** The rename changes no pixels. If a reference appears to
need blessing, that is a bug in the rename, not a new golden.

---

## Found while verifying this spec — a live Hub bug, OUT OF SCOPE

Not part of this arc. Recorded here because it was found by checking whether ArcaneHub is a
caller, and because it is the same class of drift this rename exists to end.

**`ArcaneHub`'s `golden_run()` keys on vocabulary the engine does not have.**
`launch.rs:104` is `extra.iter().any(|a| a.starts_with("--golden-"))`, but **no `--golden-*`
flag exists anywhere in ArcaneClient, ArcaneRuntime, ArcaneEditor or ArcaneTests.** The engine's
actual golden vocabulary is `--offscreen`, `--settle`, `--compare`, `--report`. `--golden-*` is
retired vocabulary from a pre-Plan-A design.

Consequence: the guard at `launch.rs:335`, `Some(3) if !golden`, **always** matches. A post-boot
exit 3 — a real golden capture/compare failure — is reported to the user as *"that project is
already open in another editor."* The correct arm at `:338` (*"a golden capture or compare
failed"*) is **unreachable**.

Its tests do not catch this because they assert on the same retired vocabulary
(`golden_run_sees_golden_vocabulary_only`, `launch.rs:585`): they prove the *function* matches
`--golden-capture`, and say nothing about whether any reachable invocation produces it. **A test
that cannot fail.**

The fix is to key on the real vocabulary, and its test must assert against flags the engine
actually registers. Belongs with the four owed defects, not here.

## Non-goals

- Fixing the four owed engine defects. Separate arc, sequenced next.
- Renaming the `Offscreen*` render-technique API (decision 3).
- Rewriting historical docs (decision 4).
- Any change to comparator arithmetic, the reference hierarchy, or `-AdvisoryLanes`.

---

## What this unblocks

The next arc fixes the four owed engine defects, and its design is already agreed:

1. **`--settle` unit mismatch** → add `--settle-timeout <ms>`; bail only when **both** bounds are
   spent (`attemptsUsed >= attempts AND elapsed >= timeout`). Corroborated by Unreal's
   `delay` + `frame_delay` ("both … must be met"), and note that **neither reference bounds by
   attempts alone** — Playwright bounds by time, Unreal by both.
2. **Editor capture depends on `Saved/Diagnostics/`** → give `Project::Open` an **options
   struct** (`mountDiagnostics`, default true), threaded from `HostConfig`. Chosen over an
   editor-local fix because the mount is created in the engine and a symptom fixed one layer up
   comes back. The struct is intended to absorb future per-open settings.
3. **`settleAttemptsUsed` absent from `VerifyReport`** → `schemaVersion` 2→3, honest count
   (the editor stops via a separate flag instead of forcing the counter to its budget), **plus a
   `settleBailReason`**. Modelled on Unreal, where a non-pass state carries a mandatory `Reason`
   and stays visible in the report. With a timeout added there are now two distinct exhaustion
   modes, so a bare count is ambiguous.
4. **`--fixed-dt nan`** → the same range/finite refusal already applied to
   `--max-diff-pixel-ratio`.

That arc also re-blesses `editor-ui` — legitimate once its input is deterministic, unlike before
— and then deletes `-AdvisoryLanes` and its Jenkinsfile argument. **The switch was built to be
its own tracking item; leaving it after its condition clears is the stale-marker failure this
project has now hit four times in one checklist.**
