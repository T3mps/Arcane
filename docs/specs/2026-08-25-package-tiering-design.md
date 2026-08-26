# Package tiering — where the mode/package line falls, and the manifest that expresses it

**Date:** 2026-08-25 (written 2026-08-26)
**Arc:** Plan B (Servitor comparator), Task 13 — the final agent task.
**Amends:** `docs/specs/2026-08-23-agent-verification-offscreen-design.md`, "Tiering"
**Companion research:** `docs/research/2026-08-26-unreal-screenshot-comparison-research.md`

---

## The verdict, first

**Servitor is a mode plus a corpus. It is not a package. Multiplayer is the only
real package on the board, and it is not built.**

This was an open question, recorded as such in the Plan A spec's 2026-08-25
amendment. It is answered here with the Step 1 inventory as evidence, not with
prose. The temptation this document exists to resist was inventing a manifest
format that made Servitor *look* like a package because a Hub sidebar entry said
it was one. That did not happen: Servitor ships no `.arcpkg`, and the reasoning
is recorded in the Hub view instead.

The format is still specified, in full, and validated on paper against
Multiplayer. A rule that has been applied exactly once, to say "no", is a rule
worth having written down — and the next candidate is a genuine package.

---

## Step 1 — inventory: what Servitor actually needs

Taken from the code as built at `eb522fba`, not from any outline. Every row is
marked **ships with the engine** or **per-project / per-repo**.

| # | Thing | Where it is | Ships or authored | Can a doctor install it? |
|---|---|---|---|---|
| 1 | `ImageCompare` — the comparator cascade | `ArcaneClient/src/Arcane/Assets/ImageCompare.{hpp,cpp}` | **ships in every build**, unconditionally | n/a — nothing to install |
| 2 | `ReferenceImages` — resolution, blessing, diff paths | `ArcaneClient/src/Arcane/Host/ReferenceImages.{hpp,cpp}` | **ships in every build** | n/a |
| 3 | `--offscreen`, `--frames`, `--fixed-dt`, `--settle`, `--compare`, `--bless`, `--backend`, `--report`, `--screenshot`, `--dump-layout`, probes | `ArcaneClient/src/Arcane/Host/HostConfig.hpp`; both hosts | **ships in every build** | n/a |
| 4 | `VerifyReport` + its `schemaVersion` — the tier seam | `ArcaneClient/src/Arcane/Host/VerifyReport.{hpp,cpp}` | **ships in every build** | n/a |
| 5 | `References/<name>.png` — shared-level goldens (`editor-ui`, `inprocess-lit-cube`, `runtime-scene`) | `ReferenceProject/Verify/References/` | **per project, authored** | **No** — see "the bootstrap test" below |
| 6 | `References/<backend>/<name>.png` — backend overrides (`vulkan/runtime-scene`) | `ReferenceProject/Verify/References/vulkan/` | **per project, authored** | **No** |
| 7 | `Traps/` — the conformance corpus: 3 should-fail pairs, 1 should-match pair, plus its README | `ReferenceProject/Verify/Traps/` | **per project, authored** (an authored corpus in its own right, distinct from the goldens) | **No** |
| 8 | `Saved/verify-layout.ini` — the offscreen editor layout seed | `ReferenceProject/Saved/verify-layout.ini` (a committed exception inside an otherwise-ignored `Saved/`) | **per project, authored** — and the one thing that makes an offscreen editor capture reproducible across desks | **No** |
| 9 | `Saved/Verify/<name>-<backend>-diff.png` — failure diff artifacts | `ReferenceProject/Saved/Verify/` | **generated, gitignored** | n/a |
| 10 | `scripts/golden-gate.ps1` — the four-lane host orchestration | Arcane repo, `scripts/` | **per repo, not per project** | n/a — it is a checked-in script |
| 11 | The Jenkins `Golden gate` stage + its failure-artifact archiving | Arcane repo, `Jenkinsfile` | **per repo** | n/a |
| 12 | What (10) needs to run: PowerShell 5.1, `ci\msbuild.cmd`, `ThirdParty\premake5\premake5.exe`, a GPU agent with both D3D12 and Vulkan drivers | Arcane repo / CI agent | **already required to build Arcane at all** | these ARE doctor-checkable — but they are the *engine's* build requirements, not Servitor's |
| 13 | External services, daemons, containers, ports, environment variables | — | **none** | — |
| 14 | Known-state: three OWED engine defects (below) | — | — | — |

**Row 14, expanded.** Found by the gate on first real use; owed to a future
plan, deliberately not fixed here. They are inventory input, and they are
material to what the Hub view may honestly claim:

1. `--settle` counts **attempts** while the condition it waits on
   (shader-compiler idle) is denominated in **milliseconds** — at ~3.3 ms per
   attempt a 30-attempt budget burns in ~100 ms. A mode-wide design flaw. The
   **Release editor lane is red on this.**
2. The editor's verify capture includes the Assets panel, which enumerates
   `Saved/Diagnostics/` — a directory the editor itself writes crash and hang
   captures into. The golden image therefore depends on the machine's failure
   history. The **Debug editor lane is red on this**, at exactly 24 px: the
   Assets scrollbar thumb cap.
3. `settleAttemptsUsed` is absent from `VerifyReport` entirely — verified, the
   symbol exists only in `EditorApp`'s private state. A report cannot currently
   distinguish "converged" from "exhausted its attempts."

**Both runtime lanes PASS 0-diff on both backends and both configurations.**

### What the inventory decides

Three tests. Each would have flipped the verdict had it gone the other way;
none did.

**Test A — the doctor test, which is the codebase's own.** `PackagesView.svelte`
defines a package as *"optional capability, and the dependencies each one
needs"*, served by a doctor that *"reports what is missing and installs it"*.
Run every non-shipping row against that definition. Rows 5–8 are **authored
project content** — PNGs and an INI that live inside the project, are versioned
with it, and were produced by it. They are content in exactly the sense a scene
or a texture is content. A doctor cannot install `References/runtime-scene.png`:
there is nowhere to fetch it from. It does not exist until this project makes
it. Rows 10–12 are checked-in scripts and the engine's own build prerequisites.
**Zero rows are things a doctor could report missing and then install.** Had
even one been a tool, a service, a port, an environment variable, or a version
floor, the verdict would be "package". None is.

**Test B — the machine-state test.** A `requires` entry is a statement about the
**machine**, not about the project. Servitor adds **zero** machine requirements
over the engine baseline: it needs a GPU and a D3D12 or Vulkan driver, which
`ArcaneEditor` already needs in order to start at all. Nothing that runs
`--compare` is absent from a machine that can run the editor. Compare
Multiplayer, which adds Docker, a PostgreSQL 16 image carrying `pg_partman`, a
free TCP 5432, and an environment variable. Different kind of thing, and the
difference is not one of degree.

**Test C — the bootstrap test, which is the decisive one.** The only "install"
action Servitor has is `--bless` — a flag **on the mode itself**. The capability
produces its own alleged dependency, using nothing but the engine that already
ships. A package whose sole dependency is manufactured by running the package is
not carrying a dependency; it is carrying **state**. That is the whole finding
in one sentence.

**The honest corollary, which is where "plus CI glue" comes from.** Rows 10–12
*are* optional and *are* dependency-bearing. But they are **per-repo**, not
per-project: they live in the Arcane repo, they orchestrate the Arcane repo's
own CI, and no consuming project installs them. That is CI glue by any
reasonable reading, and calling it a package would require the word to mean
something it does not mean anywhere else in this codebase.

**A fourth signal, from the brief itself.** The task's own proposed home for the
manifest was `ReferenceProject/Verify/servitor.arcpkg` — a *package's* manifest,
filed inside one *project's* content folder. That location is not a slip; it is
the only place such a file could naturally sit, because everything Servitor owns
per-project **is** that project's content. The mismatch between an
engine-scoped Hub surface and a project-scoped manifest path is itself evidence
for the verdict. See "Where manifests live" below.

---

## 1. The rule

> **A *mode* ships in every build and has nothing for a doctor to check.
> A *package* adds optional capability AND carries dependencies a doctor can
> report on and install.**

The second clause is a conjunction and both halves bind. Optional capability
with no dependencies is a mode. Dependencies with no optional capability is a
build prerequisite. Only both together make a package.

**The framing that produced this rule stands, and is kept verbatim from the Plan
A spec:**

> **Playwright is a package. Headless Chrome is a mode of Chrome.**

Nobody installs anything to get `--headless`; it ships in the browser, unified
with the normal path, which is exactly what Chrome 112's unification was *for*.
What you install is the driver, the comparator, the golden corpus, and the
runner that *use* that mode.

The Servitor verdict is where that analogy stops being decorative. Our comparator
moved **engine-side** during Plan B — the settle predicate and the comparison are
two halves of one mechanism, so the comparator lives inside the wait loop and
therefore in-process. That single move took most of what "the Playwright half"
would have contained and put it in the browser, so to speak. What is left on the
package side of the line is a corpus of images, and images are content.

**Three sharpening tests, in the order they should be applied.** These are
generalized from the Servitor case above and are the operative form of the rule:

1. **The doctor test.** Name a check a doctor could run for this candidate that
   returns *missing*, and name what it would then install. If you cannot name
   both halves, it is not a package.
2. **The machine-state test.** Does it add any requirement on the *machine* over
   the engine baseline? Project content never does.
3. **The bootstrap test.** Can the capability produce its own dependency by
   being run? If so, that dependency is state, not a dependency.

### Two consequences, carried forward and still binding

1. **Nothing in the engine mode is optional, installable, or conditionally
   compiled.** No feature macro, no premake option, no `#if !defined(ARCANE_DIST)`
   around the offscreen path — verification must work in Release. A mode only
   some builds have re-creates the divergence the Chrome argument exists to
   prevent, one layer down.
2. **The report JSON is the interface between the tiers**, so it is a contract:
   it carries a `schemaVersion`, in the same spirit as `.arcproj`'s
   `formatVersion` and the engine-identity `abi`. That the seam is a *file
   format* rather than a C++ API is deliberate — it is the most package-friendly
   boundary available, and it lets any future package be written in any language
   without linking the engine.

---

## 2. The manifest format — `*.arcpkg`

JSON, with a `formatVersion`, matching `.arcproj`'s own convention.

```json
{
  "formatVersion": 1,
  "name": "Multiplayer",
  "version": "0.1.0",
  "description": "Persistent player state and matchmaking for a project that wants them.",
  "provides": [
    "Account service (player state, gacha, quests)",
    "Auth service (login, registration, session tokens)",
    "Combat service (match lobby, realtime gameplay)"
  ],
  "requires": [
    { "kind": "tool", "id": "docker", "minVersion": "4.0", "remedy": "..." }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `formatVersion` | yes | Integer. `1` today. |
| `name` | yes | The package's identity. What a project's `packages: []` lists. |
| `version` | yes | The package's own version, semver text. Distinct from `formatVersion`. |
| `description` | yes | One sentence. What the Hub shows. |
| `provides` | yes | What the project gets. Prose strings, deliberately — this is the one field a human reads to decide whether they want it, and it is not machine-actionable. |
| `requires` | yes | An array of **checkable** entries. May be empty — but a package with an empty `requires` fails the rule and is a mode. |

### `requires` entries must be checkable, not prose

Every entry is an object with a `kind`, a `remedy`, and the fields its kind
needs. **`kind` is a closed set**; a manifest naming an unknown kind is
malformed, not "checked loosely". Every entry carries a **`remedy`** — a
human-readable sentence saying how to satisfy it — because a doctor that reports
*missing* without saying *what to do* is a worse experience than a setup
document.

| `kind` | Fields | Checks | Precedent |
|---|---|---|---|
| `tool` | `id`, optional `minVersion`, optional `probe` | An executable is present and, if `minVersion` is given, new enough | Aphelyon's doctor checks Visual Studio (via vswhere, with version), `docker`, `git`, `vcpkg` |
| `service` | `id`, optional `port`, optional `host` | A service is reachable, or a port is free to bind | Aphelyon's doctor checks port 5432 |
| `tree` | `path`, optional `mustContain` | A file or directory exists relative to a named root | Aphelyon's doctor checks `vcpkg-triplets\x64-windows-static.cmake` |
| `env` | `id`, optional `pattern` | An environment variable is set, and optionally matches a shape | Aphelyon's `APHELYON_INTERNAL_SECRET` (Release refuses to start without it) |
| `package` | `id`, optional `minVersion` | Another `.arcpkg` is present and satisfied | none yet — reserved, and marked as unexercised |

Two rules about the set itself:

- **`env` was added by the Multiplayer paper validation, not by Servitor.** See
  §5. That is the point of the exercise, and it is recorded rather than smoothed
  over.
- **`kind: "package"` is reserved and unexercised.** It is named so the format
  has a place to put transitive dependencies, and flagged so nobody mistakes an
  untested field for a validated one.

### What a check returns

Fixed, and identical for every `kind` — this is what makes the set closed useful:

| Result | Meaning |
|---|---|
| `pass` | Present and satisfies every constraint. |
| `warn` | Present, usable, but not as specified — an optional entry absent, or a soft version floor unmet. Does not block. |
| `fail` | Absent, or present and unusable. Blocks. |

plus, always, a **human-readable message** and the entry's **`remedy`**.

This three-state shape is not invented: it is exactly what
`Gacha/scripts/doctor.bat` emits today —
`@@WIZ doctor item=<name> status=<pass|warn|fail> msg="<text>"` — across its
eight checked items. A two-state pass/fail was considered and rejected on that
evidence: three of that doctor's real checks are genuinely `warn`, and
collapsing them to `fail` would block setup on things that do not block setup.

---

## 3. Where manifests live, and how they are discovered

The brief left this open. It is resolved here, and the resolution is part of the
evidence for the verdict.

**A package's manifest belongs with the package, not inside a consuming
project.** Two separate things were being conflated:

| | A package's **manifest** | A project's **usage declaration** |
|---|---|---|
| What | `<name>.arcpkg` — describes the package: what it provides, what it requires | `packages: []` in `.arcproj` — names which packages this project uses |
| Scope | **engine-scoped**, one per package, shared by every project | **project-scoped**, one per project |
| Owned by | the package | the project |
| Contains | the full manifest | names only (optionally a version floor) — never a copy of the manifest |

**Specified location and discovery mechanism** (specified, **not built** — see
§6):

- A package installs to `<engine>/Packages/<name>/`, beside the existing
  `Binaries/` and `ThirdParty/` in an engine tree.
- Its manifest is `<engine>/Packages/<name>/<name>.arcpkg`.
- **Discovery is `<engine>/Packages/*/*.arcpkg`** — one glob, one level deep,
  no recursion and no index file to fall out of sync with the directory.
- The Hub is **engine-scoped** (Packages is a sidebar entry, not a project
  surface), so it enumerates against the selected engine. A project's
  `packages: []` is then a *lookup* into that set, and a name with no manifest
  is reported as such rather than silently ignored.

**Why the brief's location was wrong, and why that matters.** The brief put
`servitor.arcpkg` at `ReferenceProject/Verify/servitor.arcpkg` — inside a
project, inside that project's content. That is the wrong scope for a document
describing a package. And the reason it looked natural is the finding: **for
Servitor, the only per-project things that exist ARE that project's content.**
The manifest wanted to live in the content folder because that is where all of
Servitor's per-project surface already lives. A package's manifest never wants
to live there. Corroborates the verdict; recorded rather than quietly fixed.

---

## 4. The `.arcproj` declaration — and why it is a different mechanism from `plugins`

`packages: []` sits beside the existing `plugins: []`:

```json
{
  "formatVersion": 1,
  "name": "ReferenceProject",
  "engine": { "abi": 17 },
  "gameModule": "ReferenceGame.dll",
  "plugins": [],
  "packages": [],
  "bootScene": "…",
  "guid": "…"
}
```

**These are different mechanisms, and conflating them would be a mistake.**

| | `plugins` | `packages` |
|---|---|---|
| What it names | in-process DLLs loaded into the host | optional capability with machine dependencies |
| Who parses it | **the engine** — `ProjectManifest::FromJson` reads it into `std::vector<PluginRef>` | **nobody in the engine.** Tooling reads the `.arcproj` JSON directly |
| Gate | the ABI gate — a cross-build mismatch is refused | a doctor, when one exists |
| Failure mode | the host refuses to load the project | the project runs; the capability is unavailable |

### The engine deliberately does NOT parse `packages`

This is a decision, not an omission. `ProjectManifest` has no `packages` member
and will not grow one.

**The reasoning.** Consequence 1 of the rule (§1) forbids the engine tier
growing optional-capability machinery. A `packages` member on `ProjectManifest`
is precisely that: it would put a tooling-tier concept inside the type every
host loads on every boot, and the first question after adding it would be "what
does the engine *do* when a declared package is missing?" — a question with no
good answer, because the engine's correct behaviour is to not care. Packages are
a tooling-tier concept. The Hub reads the JSON.

**This is safe, and it is verified rather than assumed.** `ProjectManifest::FromJson`
reads only known keys and ignores unknown ones rather than rejecting them, and
`RewriteManifest` (`ArcaneClient/src/Arcane/Project/Project.cpp:37-62`) is a
**read-modify-write over `nlohmann::ordered_json`**, not a reconstruction — so a
field the engine never parses survives every rewrite, in place, in order. That
covers all three rewrite paths: `Project::Open`'s guid self-heal,
`SetBootScene`, and `RestampEngineAbi`.

It is covered by a **running test**, not only by reading:
`ArcaneTests/src/ProjectManifestTest.cpp` writes a manifest carrying an unknown
top-level `zzzFuture` key, opens it (triggering the guid self-heal), calls
`SetBootScene`, re-reads from disk, and asserts both that `zzzFuture` still holds
its value **and that the full key order is unchanged** including the appended
`guid`. `RestampEngineAbi` gets the same treatment with an unknown key nested
inside `engine{}`. `packages` is an unknown top-level key of exactly that class.

### `formatVersion` is NOT bumped, and stays at 1

The condition for bumping was "bump if the field is **required** rather than
optional". It is optional and unparsed:

- absence means the same as `[]` — no packages;
- no reader requires it, because the engine has no reader for it at all;
- a manifest without it is not out of date, and must never be reported as such.

`Project::Create`, which mints new manifests, is **not** modified: a new project
declares no packages, and an absent field already says exactly that. Adding it
to the minting path would put an inert field in every project ever created in
exchange for nothing.

### Why `ReferenceProject` carries an empty one anyway

`ReferenceProject.arcproj` gains `"packages": []`. Empty, and truthfully so —
ReferenceProject uses zero packages, exactly as it uses zero plugins and has
carried an empty `plugins: []` all along. It is the declaration slot made
concrete, so this spec's format has a live instance rather than only an example,
and so the preservation guarantee above has something real to hold. It claims
nothing about Servitor: **there is no `servitor.arcpkg`, and Servitor's name
does not appear in that array.**

---

## 5. The doctor contract

**Arcane has no doctor today, and this task does not write one.** What follows
is the contract a doctor must satisfy when one is built.

### The correction this section exists to make

The Plan A spec's 2026-08-25 amendment stated that the doctor would drive *"the
existing `scripts/setup.ps1` orchestrator"*, and `PackagesView.svelte`'s comment
said the same. **That was factually wrong, and it is corrected here.** Arcane's
`scripts/` contains `check-faults.ps1`, `gen_icons_lucide.py`, `generate.bat`,
`golden-gate.ps1`, `launch.bat`, `launch.ps1`, `setup-vcpkg-deps.bat`, and
`sync-astra.ps1`. **There is no `setup.ps1` in this repository.**

`setup.ps1` lives in a **different repository** — the Aphelyon/Gacha repo at
`D:\dev\starworks\Gacha\scripts\setup.ps1` — where it is the headless
orchestrator behind that repo's `Setup.exe`. It is a **precedent and reference
implementation**, explicitly located elsewhere. **It is not a dependency of
Arcane and must not become one.**

The discipline the original claim was reaching for still binds, and is honoured
here by **building nothing**: when Arcane grows a doctor, the Hub drives that
one orchestrator rather than growing a second one beside it.

### What the precedent actually demonstrates

Worth naming precisely, because it is what the `requires` kinds in §2 were
derived from rather than invented. `Gacha/scripts/doctor.bat` checks eight items
across four shapes — a **tool** (Visual Studio via vswhere with its version,
`docker`, `git`, `vcpkg`), a **file tree** (the overlay triplet), a **port**
(5432), and a parameterized port check — and emits per item:

```
@@WIZ doctor item=<name> status=<pass|warn|fail> msg="<text>"
```

`setup.ps1` sequences it (`-DoctorOnly`, `-SkipDoctor`, `-NonInteractive`,
`-DryRun`), sets `_APH_WIZ=1` to switch the doctor into marker-emitting mode,
and a Tauri GUI renders the stream. Four shapes, three states, one line per
check. §2's five kinds are that set plus `env` (§5's finding) and a reserved
`package`.

### The contract

Any Arcane doctor implementation must:

1. **Enumerate** — discover manifests per §3 (`<engine>/Packages/*/*.arcpkg`)
   and resolve a project's `packages: []` names against them, reporting a named
   package with no manifest as an error rather than skipping it.
2. **Check every `requires` entry**, dispatching on its `kind` from §2's closed
   set. An **unknown `kind` is a hard error on the manifest**, never a silently
   skipped entry — a check that cannot run must never read as a check that
   passed.
3. **Return the three-state result** — `pass` / `warn` / `fail` — with a
   human-readable message and the entry's `remedy`, per §2.
4. **Report before it acts.** Checking and installing are separate phases, and
   the report is available without any install being attempted (the `-DoctorOnly`
   shape).
5. **Never install silently.** Installation is a distinct, explicitly invoked
   action.
6. **Be drivable headlessly**, so CI and a GUI consume the same run rather than
   duplicating logic. The `@@WIZ`-marker-plus-plain-log shape is the working
   precedent.
7. **Be the only orchestrator.** The Hub drives it; the Hub does not
   reimplement it.

Until all seven hold, the Hub view says plainly that checking is not built. It
says exactly that today.

---

## 6. Paper validation against Multiplayer

Required by the rule that a manifest which can only describe the package it was
extracted from has not been formalized. **Not built** — this is the manifest
Multiplayer *would* have, derived from the Aphelyon Server as it actually exists.

Multiplayer is not hypothetical. It is `Auth`/`Account`/`Combat` plus
PostgreSQL 16 in Docker, in the Gacha repo, and its dependency shape is the
**inverse** of Servitor's: almost nothing authored, almost everything installed.

```json
{
  "formatVersion": 1,
  "name": "Multiplayer",
  "version": "0.1.0",
  "description": "Persistent player state and matchmaking for a project that wants them.",
  "provides": [
    "Auth service — login, registration, 64-char session tokens (TCP 7777)",
    "Account service — player state, gacha, quests, collection (TCP 7771)",
    "Combat service — match lobby and realtime gameplay (TCP 7772, UDP 7778)",
    "Event-sourced player persistence on PostgreSQL"
  ],
  "requires": [
    {
      "kind": "tool",
      "id": "docker",
      "minVersion": "4.0",
      "remedy": "Install Docker Desktop 4.x or newer and start it."
    },
    {
      "kind": "tool",
      "id": "msbuild",
      "remedy": "Install Visual Studio 2026 with \"Desktop development with C++\"."
    },
    {
      "kind": "tool",
      "id": "vcpkg",
      "remedy": "Clone vcpkg and set VCPKG_ROOT; then run Server/scripts/setup-vcpkg-deps.bat."
    },
    {
      "kind": "service",
      "id": "postgres",
      "port": 5432,
      "host": "127.0.0.1",
      "remedy": "Run Server/scripts/db-setup.bat. If a native PostgreSQL already holds 5432, stop it or remap the host port in docker-compose.override.yml."
    },
    {
      "kind": "tree",
      "path": "Server/Account/schema.sql",
      "remedy": "Re-pull the repository — the canonical schema is checked in."
    },
    {
      "kind": "tree",
      "path": "vcpkg-triplets/x64-windows-static.cmake",
      "remedy": "Re-pull the repository — the v143 overlay triplet is checked in."
    },
    {
      "kind": "env",
      "id": "APHELYON_INTERNAL_SECRET",
      "remedy": "Set APHELYON_INTERNAL_SECRET to a shared non-empty string before launching all three services. Release builds refuse to start without it; Debug falls back to a documented dev secret."
    }
  ]
}
```

### What the exercise found — and it found something

**`kind: "env"` did not exist before this step.** Servitor never exercised an
environment-variable dependency, because Servitor has no dependencies at all. It
was added *because* expressing Multiplayer needed it, which is exactly what the
paper validation is for. `APHELYON_INTERNAL_SECRET` is a real, hard requirement:
a Release Auth build **refuses to start** without it
(`Server/Auth/src/main.cpp:115-119`). A manifest that could not say so would be
describing a package that does not start.

**Two things it did NOT force, worth recording as bounded gaps rather than
overspecifying now:**

1. **Container image identity.** `docker` as a `tool` says a container runtime
   is present. It does not say the image `aphelyon/postgres:16` — a custom build
   adding `pg_partman` over stock `postgres:16` — has been built. Stock
   `postgres:16` does **not** ship `pg_partman`, and pointing compose at it makes
   schema apply fail with `schema "partman" does not exist`. The `service` check
   above catches "Postgres is not reachable" but not "Postgres is reachable and
   is the wrong image". **A future `kind: "image"` (id + tag + a capability
   probe) is the shape this wants.** Not added, because inventing a kind on one
   unbuilt example is how a format gets a field nobody needed.
2. **Ordering.** Several entries are sequential in reality — vcpkg deps before
   the build, `db-setup` before the services. `requires` is a **set**, not a
   plan; ordering belongs to the orchestrator, which is exactly how
   `setup.ps1`'s `Step` sequencing works today. Deliberate: a manifest that
   encoded a build order would be a build system.

### And the counter-check, which is the whole point

Run the **same** manifest format at Servitor, honestly, and `requires` comes out
**empty** — every row of the Step 1 inventory is either engine-shipped or
project content. Per §2, a package with an empty `requires` fails the rule.

**The format did not have to be bent to reach that answer, and it was not.** It
produced "no" for Servitor and a seven-entry manifest for Multiplayer, from the
same five kinds. That is the evidence that the format describes something real
rather than describing the one thing it was extracted from.

---

## 7. What this task deliberately did NOT build

Recorded so a later reader does not mistake absence for oversight:

- **No `servitor.arcpkg`.** Servitor is a mode plus a corpus. Shipping a
  manifest for it would be the exact failure this task existed to avoid.
- **No package registry, and no Tauri command to discover manifests.** The Hub
  has 27 commands, none package-shaped; a registry over zero entries is
  machinery pretending to be a feature. Discovery is *specified* (§3) so the
  work is defined the day a real package exists.
- **No doctor.** §5 is a contract, not an implementation. Arcane has no
  `setup.ps1` and does not gain one.
- **No `packages` member on `ProjectManifest`, and no `formatVersion` bump.** §4.
- **No install buttons in the Hub, disabled or otherwise.** The placeholder's
  own reasoning — a disabled "Install" is a worse lie than a sentence — does not
  expire, and it did not expire here either, since no registry arrived to
  pressure it.
- **No change to comparator arithmetic, constants, or cascade structure.**
  Playwright is the reference of record; Task 6's conformance corpus (21 pairs,
  zero divergence) is the oracle. The Unreal research produced three findings
  banked for future plans and acted on none of them.
- **No fix to the three OWED engine defects.** Owed to a future plan; recorded
  as row 14 of the inventory.

---

## Appendix — the Unreal research

Banked by the user 2026-08-25 and scoped to this task: four questions Playwright
**structurally cannot answer**, because it is a browser tool that has never
faced a GPU, a driver, or an RHI. Answered from public Epic documentation only —
never from Unreal source, which is licensed.

Full answers, citations, and the boundary statement:
**`docs/research/2026-08-26-unreal-screenshot-comparison-research.md`**

Headline results:

1. **Knob structure.** Unreal's third knob (`MaximumLocalError`, a **per-region**
   bound) is **not** the one we rejected — Unity's `AverageCorrectnessThreshold`
   catches uniform drift, Unreal's catches spatial concentration. They are dual.
   It closes a hole that exists only when the aggregate budget is non-zero; ours
   is **zero**, so it is redundant today. Finding banked for a future plan.
2. **Reference variant axes.** `Platform_RHI_ShaderModel`, with Epic's own
   *"further refinement is needed"*. **RHI comes from the same pressure we felt**
   — it is a bucketing key *and* a test-exclusion axis. Their resolution rule,
   *"the ground truth screenshot matching closest based on the metadata"*,
   **independently matches Task 7's** walk-up hierarchy. "Quality level" as a
   keying axis is **unconfirmed from public docs** and recorded as such. Our
   likeliest next axis is **configuration**, not platform.
3. **Approval workflow.** One-click bless; first run is a **warning, not an
   error**; approval lands as a source-control pending change; review is visual
   and side-by-side. **All four match what we built, independently.** Their bulk
   "Replace" — *"Deletes all examples of ground truth data"* — is a hazard we
   correctly lack.
4. **Nondeterminism policy.** Two directions: suppress noise at capture (we do
   this), **and declare a test out of scope in config with a mandatory `Reason`,
   scoped by RHI, reported as *Skipped***  (we do not). Their `delay` +
   `frame_delay` conjunction — *both* a time bound and a frame bound must be met
   — **independently corroborates the fix shape for OWED defect 1**, whose whole
   problem is an attempt budget standing in for a time budget.
