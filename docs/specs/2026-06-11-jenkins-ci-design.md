# Jenkins CI — Design

**Date:** 2026-06-11
**Status:** APPROVED — design contract for the project's CI environment.
**Context:** Arcane M0 landed the same day (workspace + Core extraction + vendored
stack), tripping the stack spec's trigger: "Engine CI (Windows-first) once the
workspace is stable." The verification matrix (2 workspaces x 6 configurations,
5 C++ test suites, client validation gate, Tools build) is now large enough that
manual runs are the bottleneck, and every Core change compiles in two CRT flavors
(Arcane /MD, Server static) — the most likely silent-regression class going forward.
**Upstream:** `2026-06-10-engine-thirdparty-stack-design.md` (CI trigger, MSVC+g++
commitment), `2026-06-11-engine-architecture-design.md` (workspace layout M0 built).

## Decisions (all confirmed 2026-06-11)

1. **Jenkins** (user mandate), self-hosted. Controller + agents on a dedicated
   mini-PC: **Minisforum X1-255** (Ryzen 7 255 = 8c/16t Zen 4, Radeon 780M,
   32 GB DDR5 dual SO-DIMM, 1 TB NVMe + free M.2 slot, OCuLink) — on order.
2. **Multibranch pipeline** over `https://github.com/T3mps/Aphelyon`, chosen for
   the planned branch model: solo trunk on `main` today; `dev` -> `milestone/*`
   branches with per-milestone deployments when live (game-studio convention).
   The Jenkinsfile is identical under single-job and multibranch operation; the
   topology choice front-loads pre-merge verification (push branch -> CI green ->
   merge) which the user adopts as workflow.
3. **Polling, not webhooks** — branch indexing every ~2 min. The box needs only
   outbound HTTPS; nothing is exposed inbound. (cloudflared tunnel is the noted
   upgrade path if instant triggers are ever wanted.)
4. **Reporting:** GitHub commit status (PAT) + Discord webhook on failure and
   recovery (not per-success) + Jenkins dashboard with JUnit trends. Both
   credentials are user-created, stored only in Jenkins credentials, never in-repo.
5. **AccountTests cadence:** post-merge tier. Trunk-based solo means every `main`
   build is post-merge, so the heavy stage is simply gated to `main` (and later
   `milestone/*`). Branch builds get the fast tier (~15-18 min); `main` builds
   take ~35 min total, with cheapest-failure-first ordering so compile breaks
   report in ~3 min.
6. **GPU:** agent runs as an auto-logon interactive session from day one so
   D3D12/Vulkan devices are visible when M1's device smokes arrive. The 780M
   iGPU covers device-creation/clear/present verification through M4; the
   OCuLink port is the designated upgrade seam for a real-GPU rendering-
   correctness stage (M5+ trigger). No cloud GPU; no WARP needed (local iGPU).
7. **Linux: out-of-scope-but-shaped-for.** Servers and the engine/client will
   eventually build and test on BOTH Windows and Linux (stack spec commitment:
   MSVC + g++, premake vs2026 + gmake2); Windows-first because it is the dev
   environment. CI ships the Linux *lane* now (agent label scheme, container
   agent, gated stage group) but the Linux **port** — gmake2 targets for the
   Server workspace, g++ fixes, Linux libpq — is its own future milestone and
   explicitly not part of this build-out. First real Linux build is expected
   to fail; that is the point of standing the lane up early.
8. **CD half deferred.** Nothing deployable exists (Combat is a stub; engine has
   no shipping artifact). `milestone/*` deploy stages are an empty, named hook.

## Topology

```
Minisforum X1-255 (Windows 11 Pro, always on, outbound HTTPS only)
├── Jenkins controller        Windows service; built-in node 0 executors
├── Agent "win-gpu"           label: windows && gpu
│     inbound agent in an auto-logon interactive session (GPU visibility);
│     1 executor (builds serialize; prevents Docker/port collisions);
│     toolchain: VS2026 Build Tools (or Community), Git, vcpkg, Docker Desktop
└── Agent "linux-1"           label: linux
      Docker container (Linux, via Docker Desktop/WSL2) from ci/linux-agent.Dockerfile
      (gcc, clang, make, premake5 Linux, libpq dev); 1 executor.
      Real Linux userspace, native x86-64 speed — compiles AND runs tests,
      which cross-compilation could not. Own checkout/workspace on a Docker
      volume INSIDE the Linux filesystem (cross-OS /mnt/c file access is ~10x
      slower). Migration seam: the same image runs unchanged on any future
      Linux host. Linux lane is build + headless tests only; Linux GPU work
      (WSL2 paravirt) is explicitly out of scope until real Linux hardware
      has a reason to exist.
```

## Pipeline (one Jenkinsfile, repo root)

Branch-class tiers via `when` gates; platform groups via agent labels:

| Stage group | Agent | Branches | Content |
|---|---|---|---|
| Windows build | win-gpu | all | generate (Server, Arcane, Tools) -> msbuild: Server Debug+Release, Arcane Debug+Release+Dist, Tools Debug |
| Windows fast tests | win-gpu | all | CommonTests, AuthTests, CombatTests, ArcaneTests (Debug + Release) with JUnit XML output |
| Client gate | win-gpu | all | lovec parse-check + the 3 headless harnesses (threading/render/world) |
| AccountTests | win-gpu | `main`, `milestone/*` | ephemeral CI Postgres (below), ~17 min |
| Linux build+test | linux | all | **gated OFF by a single pipeline flag** until the Linux port milestone lands; activates gmake2 -> make -> same Catch2 suites |
| Deploy | — | `milestone/*` | empty named hook (CD deferred) |

Stage order is cheapest-failure-first. Multibranch hygiene configured day one:
orphaned-item cleanup, workspace deletion on branch removal (per-branch
workspaces are multi-GB in this repo), build retention ~30 per branch.

Future audit stages noted for later addition: protocol.json byte-parity
(Client/data vs Server/data), WARP fallback if CI ever moves to GPU-less
hardware, eGPU-over-OCuLink rendering correctness (M5+).

## Ephemeral CI database

AccountTests requires Postgres 16 + pg_partman — i.e. the repo's custom
`Server/Dockerfile.postgres` image; native Windows Postgres cannot provide
pg_partman, so Docker Desktop on the agent is required, not optional.

- Dedicated compose project `aphelyon_ci`, **port 5433**, throwaway named volume.
- Per heavy-stage run: up -> apply `schema.sql` + `seed.sql` -> run suite ->
  `docker compose -p aphelyon_ci down -v` (scoped to the CI project ONLY).
- The CI database is created and destroyed every run **by design**. It is never
  the dev database (which lives on the dev box, not the CI box); the standing
  never-destroy-the-dev-DB rule is satisfied by construction, and the `-v` is
  scoped by project name as defense in depth.
- Code touch required: AccountTests' connection string is currently the compiled
  default (`localhost:5432`). The suite gains an environment-variable override
  (e.g. `APHELYON_TEST_DB_URL`) so CI points it at 5433. Exact mechanism decided
  in the implementation plan; dev-box behavior unchanged when the variable is unset.

## What lives where

In-repo (all reviewable, all portable):
- `Jenkinsfile` — the pipeline above.
- `ci/provision-agent.ps1` — idempotent Windows agent provisioning (JDK, Git,
  VS Build Tools, vcpkg + repo triplets, Docker Desktop note, auto-logon agent
  setup), runnable on any future replacement box.
- `ci/linux-agent.Dockerfile` — the Linux agent image.
- `ci/README.md` — controller install steps, credential checklist (GitHub PAT
  with repo:status scope; Discord webhook URL), plugin list, multibranch job
  configuration values.

On-box only: Jenkins home, the two credentials, agent auto-logon configuration.

## Acceptance criteria

1. Push to `main` -> GitHub status pending -> green in ~35 min, all stages incl.
   AccountTests against the ephemeral DB; the dev DB is untouched (it is not on
   the machine).
2. Push a feature branch -> fast tier only (~15-18 min), own GitHub status.
3. A deliberately broken commit -> red status + Discord notification; the fix ->
   green + recovery notification.
4. The `linux-1` agent connects and passes a hello-world stage (toolchain image
   proven) while real Linux stages remain gated.
5. JUnit trends visible per branch in the dashboard; workspace/orphan cleanup
   policies verified by deleting a test branch.
6. Two consecutive `main` builds leave no stray containers, volumes, or port
   bindings (idempotent heavy stage).

## Out of scope (each its own future effort)

- **Linux port milestone**: Server/engine gmake2 targets, g++ compilation fixes,
  Linux libpq, then flipping the pipeline's Linux flag. Satisfies the spirit of
  Astra hardening item 2 at the project level.
- CD/deployments (activates with `milestone/*` branches and a deployable artifact).
- protocol.json parity audit and further audit stages.
- Rendering-correctness CI (eGPU via OCuLink, image diffs) — M5+.
- Webhook triggering via cloudflared (only if polling latency ever annoys).
