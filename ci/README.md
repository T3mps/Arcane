# CI operations (Arcane)

The controller, agents, plugins and shared credentials live on the same
box as Aphelyon's pipeline. That checklist is
`D:\dev\starworks\Aphelyon\ci\README.md` (controller install, `win-gpu`,
`linux-1`, `github-pat`, `discord-webhook`). **This file is only the
engine job** — extraction Phase 3
(`docs/specs/2026-08-11-arcane-repo-extraction-design.md`): a second
multibranch pipeline over `github.com/T3mps/Arcane`.

The `Jenkinsfile` at the repo root is the pipeline: Debug+Release+Dist,
unfiltered `ArcaneTests` (including `[gpu]`) on `windows && gpu`,
`arccook --check`, both-config golden-gate, self-test on `main` /
`milestone/*`, ReferenceProject SDK build, `--frames 180` GPU-verify.

GitHub Actions (`.github/workflows/ci.yml`) is the hosted `~[gpu]` lane
on `windows-latest`. It is not a substitute for this job: no GPU, no
golden-gate, no `--frames`.

## The job

New Item -> **Multibranch Pipeline** -> name `Arcane`.

**Preferred bring-up (no new PAT):** Branch Sources -> **Git** (not
GitHub):

- Project Repository: `https://github.com/T3mps/Arcane.git`
- Credentials: **- none -** (the repo is public; `ls-remote` is enough)
- Behaviors: Discover branches. Do **not** add Discover pull requests.
- Build Configuration: by Jenkinsfile, path `Jenkinsfile`
- Scan Repository Triggers: periodically, **2 minutes**
- Orphaned Item Strategy: discard old items, max 30 days / 30 items

GitHub commit statuses will not publish on this shape. That is
acceptable until a T3mps-scoped PAT exists (below). Discord
failure/recovery still fires via the existing `discord-webhook`
credential.

**GitHub Branch Source (statuses):** the live `github-pat` credential is
a fine-grained PAT whose resource owner is the `StarworksDev` org. It
cannot see `T3mps/Arcane` even after StarworksBuilder is a collaborator.
To turn statuses on:

1. `StarworksBuilder` already has a pending **write** invite on
   `T3mps/Arcane` (sent 2026-08-11, extraction day — never accepted).
   Sign in as that account and accept
   https://github.com/T3mps/Arcane/invitations so a later PAT can see
   the repo.
2. Create a **second** fine-grained PAT, resource owner **T3mps**,
   repository `T3mps/Arcane`, **Contents: read** + **Commit statuses:
   read/write**. No Pull-requests permission (same 403 trap as Aphelyon).
3. Jenkins -> Credentials -> Global: ID `github-pat-arcane`, Kind
   Username with password, username `StarworksBuilder`, password = the
   PAT.
4. Recreate (or edit) the job with Branch Sources -> GitHub, owner
   `T3mps`, repository `Arcane`, credentials `github-pat-arcane`,
   Discover branches only.

## Script Console (creates the Git job)

Manage Jenkins -> Script Console, paste `ci/create-arcane-job.groovy`,
Run. It is a no-op if `Arcane` already exists. Then **Scan Multibranch
Pipeline Now** on the job if indexing did not start itself.

## Unfiltered baselines

The Tests stage runs the suite **unfiltered** and checks
`-Invocation unfiltered`. `scripts/automation-baselines.json` has no
those entries on purpose: a `~[gpu]` baseline would absorb ~62000 GPU
assertions as slack. After the first green Jenkins run, copy the Debug
and Release case/assertion counts from the JUnit/JSON reports into
`unfiltered` entries and commit them. Until then the stage prints
`NO BASELINE -- not checked` and still fails on a red suite.

## Agent notes

Same `windows && gpu` interactive session as Aphelyon. `VCPKG_ROOT`
must point at the shared tree (`D:\dev\_shared\tools\vcpkg` on this
desk). The Provision stage self-heals SDL3 via
`scripts\setup-vcpkg-deps.bat` when
`x64-windows-static-md\lib\SDL3-static.lib` is missing.
