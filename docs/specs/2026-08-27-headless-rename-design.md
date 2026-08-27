# `--offscreen` → `--headless`, and the vocabulary collision that blocked it

**Status:** design, approved in brainstorming 2026-08-27. Supersedes the naming decision in
`docs/specs/2026-08-23-agent-verification-offscreen-design.md` ("Naming"), which chose
`--offscreen` for the reason this spec now resolves rather than accepts.

**Sequenced first, ahead of the four owed engine defects** (see "What this unblocks"). The two
are separate arcs on purpose: this one is a broad mechanical rename and that one is four
semantic fixes, and mixing them would make both diffs unreviewable.

---

## Why

The mode flag is `--offscreen`. Every comparable tool calls this **headless**: Chrome's
`--headless`, and Playwright's headless mode, both mean *windowless but GPU-capable* — exactly
what our flag does. Ours is the outlier name, and a user or agent reaching for the obvious flag
finds nothing.

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
load-bearing site is the resolver's material-binding decision (`SceneRenderResolver.hpp:211`).

**Uses of "headless" that already mean "no window / no UI" are left alone**: they are compatible
with the new flag meaning, and rewriting them would be churn. **This requires an audit, not a
blind sweep** — see "Method".

### 3. The CLI/config surface renames; the render-technique layer does not

- **Renamed:** the flag (`--headless`), and `HostConfig::offscreen` → `HostConfig::headless`.
  `HostConfig` *is* the parsed command line; its field names should match what a user types.
- **NOT renamed:** `OffscreenVehicle`, `CreateOffscreen()`, `IsOffscreen()`, `[offscreen]` log
  tags, and the rest of the internal "offscreen" vocabulary — **1400 tracked mentions in total,
  803 of them in code**, of which only the **214** flag mentions are in scope here.

The line is **mode versus technique**. `--headless` names the mode a user asks for; "offscreen"
names how it is achieved — rendering to an offscreen target with no swapchain. Chrome draws the
same line: the flag is `--headless`, the machinery underneath is not. Renaming the render layer
would be a far larger change that nothing in the directive asks for.

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

Measured 2026-08-27 via `git grep` on tracked files.

| Surface | Size | Action |
|---|---|---|
| `--offscreen` flag mentions | **214** across **32** files | rename to `--headless` |
| `headless` in code (`.cpp`/`.hpp`) | **395** | **audit**, rename only the device-less sense |
| `headless` in `docs/` | 588 across 148 files | **leave** (historical), except live operational docs |
| `headless` in `ThirdParty/` | 16 | **leave** |
| `offscreen` everywhere (all senses) | **1400** tracked, **803** in code | of which only the 214 flag mentions change; the rest **stays** (decision 3) |

### Callers that MUST land in the removal commit

Because there is no alias, each of these breaks the moment `--offscreen` stops parsing:

- `scripts/golden-gate.ps1` — builds `--offscreen` into every lane's `$exeArgs`
- `scripts/desk-verify-servitor.ps1` — the bless invocation
- `Jenkinsfile` — the Golden gate stage
- `ArcaneTests` — `HostConfigTest.cpp`, `GoldenImageTest.cpp`, `VerifyReportTest.cpp`
- both hosts' `main.cpp` and their refusal messages
- `docs/2026-08-26-servitor-closeout-and-desk-verify.md` — carries runnable commands
- **ArcaneHub saved launch args** — per-project strings in Hub state, `set_project_args`. These
  live in the user's Hub data, not the repo. **They cannot be fixed by a commit**, and a project
  with `--offscreen` saved will fail to launch with a loud parse error. Called out here so it is
  a known consequence rather than a surprise.

---

## Method

1. **Audit before renaming.** Classify all 395 code uses of "headless" into *device-less sense*
   (rename) and *no-window sense* (leave). A blind `sed` would rename both and destroy the
   distinction this arc exists to create.
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
