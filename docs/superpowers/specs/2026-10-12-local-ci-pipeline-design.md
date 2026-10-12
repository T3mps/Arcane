# Local CI pipeline: one Linux image, shared CI scripts, one local command

**Date:** 2026-10-12 · **Base:** `main` 946092fb · **Branch:** `ci/local-pipeline`
**Status:** design approved in conversation (sections 1-3); awaiting written-spec review

This is sub-project **A** of the move off Jenkins (user decision 2026-10-05:
one CI system, GitHub Actions). The decomposition, agreed 2026-10-12:

| # | Sub-project | Repo |
|---|---|---|
| **A** | **This spec.** Linux build image + shared CI scripts + one local command | Arcane |
| B | Self-hosted Windows GPU runner on the desk; Arcane's Jenkinsfile retired after side-by-side parity | Arcane |
| C | Distroless (chiseled Ubuntu 24.04) server-runtime images, ArcaneServer first | Arcane |
| D | Aphelyon Server Linux port + per-service runtime images; Aphelyon CI on Actions | Aphelyon |

Order A -> B -> C -> D. The engine modularization arc (`feat/modularization`,
P6 parked uncommitted) resumes after B and runs its wholesale test on this
pipeline.

## 0. Intent

**What the user asked for:** a formal local build/test pipeline without
Jenkins, in favor of GitHub Actions -- not ad hoc commands.

**Success:**
1. One documented command at the desk runs the same build + test sequence the
   hosted CI runs, for Windows (native) and Linux (container), and reports
   one pass/fail table.
2. The hosted Linux legs run inside the same repo-defined image the local
   command uses, so "green locally" and "green in CI" mean the same thing.
3. The CI build steps live in scripts, not in YAML: `ci.yml` and the local
   command are two callers of one sequence.

**Decisions (user, 2026-10-12):**
- Local mode: BOTH a pre-push local run (no GitHub round trip) AND, in B, the
  same scripts executed by a self-hosted runner. Scripts-first; runners are callers.
- Linux environment: a repo-defined Docker image (Ubuntu 24.04, CI's exact
  toolchain), used locally AND by the hosted Linux legs (`container:`).
- Default local run: fast (windows-msvc Debug + linux-gcc-14 Debug), with
  `-Leg` / `-Config` selection and `-Full`.
- `.wslconfig` raised to 24 GB / 16 processors (the desk has 64 GB / 20
  threads; the 8 GB / 10 cap thrashed a Debug link at -j6).
- Image delivery: content-addressed tags, built on demand by CI.
- Server runtime images: full images (C, D) -- out of this spec.

**Constraints carried in:**
- `docs/ci.md` is the Starworks CI standard shared with Astra and Manifold2D:
  one workflow, one build and one test entry point per shell, departures
  listed under Deviations.
- Roadmap D26 security rules for Actions (self-hosted specifics are B's; the
  token, pinning and fork rules apply here): least-privilege `GITHUB_TOKEN`,
  third-party actions pinned to commit SHAs, nothing secret reachable from a
  fork PR.
- Server OS decision (2026-10-10): Ubuntu 24.04 LTS, GCC 14 / Clang 19 -- the
  image is that toolchain.

## 1. Components

### 1.1 `ci/linux/Dockerfile`, target `build`

Base `ubuntu:24.04`, pinned by digest. Baked in:
- CI's current apt set: `g++-14`, `clang-19`, `make`, `cmake`, `ninja-build`,
  `pkg-config`, `libvulkan-dev`, `mesa-vulkan-drivers` (lavapipe), the SDL3
  X11/Wayland build deps, `libasound2-dev`, `libpulse-dev`, `libudev-dev`,
  `libdbus-1-dev`, `xvfb`, `xauth`, plus the GPU-lane extras (`weston`,
  `xdotool`, `imagemagick`, `x11-utils`, `python3`) and `ccache`, `rsync`, `git`.
- SDL3 from its pinned release via `scripts/build-sdl3.sh`, installed to
  `/usr/local` (pkg-config `sdl3` finds it; premake's discovery is unchanged).
- premake 5.0.0-beta8 (`scripts/fetch-premake.sh`) and DXC
  (`scripts/fetch-dxc-linux.sh`), SHA-256 checked, installed under
  `/opt/arcane-tools/` and exposed through `ARCANE_TOOLS_DIR` -- `build.sh`
  links them into the checkout's `ThirdParty/` locations the build expects.
- The pinned Vulkan validation layer (`scripts/build-vvl-linux.sh`, tag +
  commit pinned) for the GPU lane.
- A non-root user `arcane` (uid 1000) as the default user.

The image holds tools only -- never the Arcane source.

### 1.2 `scripts/ci/image-tag.sh`

Prints `build-<hash>`: the first 16 hex digits of SHA-256 over the
Dockerfile and every file it copies or runs (`scripts/build-sdl3.sh`,
`scripts/fetch-premake.sh`, `scripts/fetch-dxc-linux.sh`,
`scripts/build-vvl-linux.sh`, `scripts/sha256-check.sh`), in a fixed order,
with line endings normalized to LF. The one definition of "which image": CI
and the local command both call it. Full image name:
`ghcr.io/starworksdev/arcane-ci:<tag>`.

### 1.3 `scripts/ci/build.sh` and `scripts/ci/build.ps1`

`build.{sh,ps1} <Debug|Release|Dist> [-j N]` -- today's inline `ci.yml` build
steps, unchanged in order:
1. generate (`GenerateProjects.bat` / `premake5 --cc=<gcc|clang> gmake`, the
   compiler from `CC`);
2. engine libraries + tools;
3. ReferenceGame through `arcbuild build --project ReferenceProject`;
4. hosts + ArcaneTests (Windows: the solution again, so hosts restage the module).

Logs to `logs/build-*.log` (the names `ci.yml`'s error summary greps). Exit
code = the first failing step's. The test entry points stay
`scripts/run-tests.sh` / `scripts/run-tests.ps1`, unchanged.

### 1.4 `scripts/ci/local.ps1`

The desk's one command.

```
scripts\ci\local.ps1                         # windows-msvc Debug + linux-gcc-14 Debug
scripts\ci\local.ps1 -Leg linux-clang-19 -Config Release
scripts\ci\local.ps1 -Full                   # windows-msvc + linux-gcc-14 + linux-clang-19, x Debug/Release/Dist
         [-Seeds 1|2] [-Jobs N] [-NoPull]
```

- **Windows legs:** `build.ps1 <cfg>`, then `run-tests.ps1 <cfg> --rng-seed <seed>`, natively.
- **Linux legs:** resolve the tag (`image-tag.sh` via Docker), `docker pull`,
  else `docker build --target build -t <image> ci/linux` (`-NoPull` forces
  the local build). Then `docker run --rm --init --shm-size=2g` with:
  - the checkout mounted read-only at `/src`;
  - a named volume `arcane-src-<leg>` at `/work` (per leg: gcc and clang
    outputs never mix);
  - a named volume `arcane-ccache` at `/ccache` (`CCACHE_DIR`), compiler
    launch through ccache;
  - `CC`/`CXX` for the leg.
  Inside: `rsync -a --delete` `/src` -> `/work` excluding `bin/`, `bin-int/`,
  `.vs/`, generated project/make files, `Packages/`, `Intermediate/`,
  `logs/`; then `build.sh <cfg>` and `run-tests.sh <cfg> --rng-seed <seed>`
  under `xvfb-run` (as CI does); then copy `logs/` and `test-results/` out to
  the host's `logs/local/<leg>-<cfg>/`.
- **Seeds:** one by default (printed); `-Seeds 2` runs CI's pair.
- **Jobs:** default = the CPUs Docker reports (Linux) / `/m` (MSBuild).
- **Uncommitted changes are included** (it is a pre-push check).
- **macOS:** not runnable locally; the hosted leg only.

### 1.5 `scripts/ci/setup-desk.ps1`

One-time desk setup, idempotent:
- `.wslconfig` -> `memory=24GB`, `processors=16` (keeping other keys); warns
  that `wsl --shutdown` applies it (and stops running distros/Docker).
- checks Docker Desktop is installed and running (WSL2 backend);
- checks Visual Studio + MSBuild via vswhere.
- No GHCR login is needed to pull (the image is public, s2.4).

### 1.6 `.github/workflows/ci.yml`

- New first job **`ci-image`** (`ubuntu-24.04`): runs `image-tag.sh`;
  `docker manifest inspect` the tag; when missing on a same-repo event,
  `docker buildx build --target build --push`; when missing on a fork PR,
  fails naming the tag ("a maintainer must build it from a branch"). Output:
  `tag`. Permissions: `contents: read`, `packages: write` -- on this job only.
- **Linux legs:** `needs: ci-image`, `container: { image:
  ghcr.io/starworksdev/arcane-ci:${{ needs.ci-image.outputs.tag }}, options:
  --init --shm-size=2g }`. The apt / SDL3 / premake / DXC / validation-layer
  steps and their caches are removed; the steps become `scripts/ci/build.sh`,
  the two-seed `run-tests.sh`, the GPU lane and the windowed smoke (still on
  the gcc Debug leg).
- **Windows / macOS legs:** unchanged runners and tool fetches; the inline
  build step becomes `scripts/ci/build.ps1` / `scripts/ci/build.sh`.
- Workflow-level `permissions: contents: read` stays the default.

## 2. Flows and failure handling

**CI run.** `ci-image` (seconds when the tag exists; one ~20 min build when
it does not) -> Linux legs in the container; Windows/macOS legs in parallel
as today. The image is **public** in GHCR (Arcane is public and the image
holds no secrets), so hosted and local pulls need no credentials. (Aphelyon's
images in D are private.)

**Local run.** Preflight (Docker up; `.wslconfig` limits present or a
slowness warning; VS found) -- a failed check names the fix and stops before
building. Every requested leg runs even if one fails (`fail-fast: false`
semantics). The summary table: leg, config, build, tests passed/failed, seed,
duration, log path; exit code non-zero if any leg failed.

**Failures.**
- Build failure: the summary shows the first compiler/linker errors (the grep
  `ci.yml` uses).
- Container OOM: exit 137 -> the summary names it and suggests `-Jobs`.
- Ctrl-C: `--init` + `--rm` stop the container cleanly; the volumes persist
  for the next incremental run.
- Fork PR + new image tag: `ci-image` fails with the reason (s1.6).

## 3. Testing / acceptance

1. **Image reproducibility:** `image-tag.sh` twice on the same inputs gives
   the same tag; a content edit to any input file changes it; a CRLF-only
   change does not. A small shell test, run in `ci-image`, checks all three.
2. **Parity:** the containerized Linux legs report the same ArcaneTests counts
   (cases / assertions) as the last green pre-container run of the same base
   commit, for gcc and clang, all three configs; the GPU lane and the windowed
   smoke pass in the container. Windows/macOS counts unchanged.
3. **Local = CI:** `local.ps1 -Leg linux-gcc-14 -Config Debug -Seeds 2` on the
   branch tip reports the same pass/fail counts as that CI leg; `local.ps1
   -Leg windows-msvc` passes `~[gpu]`.
4. **Red is visible:** a deliberately broken commit on a throwaway branch
   (never merged) turns the local summary and the CI run red with the error
   shown.
5. **Security review:** `packages: write` appears only on `ci-image`; every
   `uses:` is pinned to a commit SHA; no secret is referenced by any job a fork
   PR can trigger.

## 4. Docs

- `docs/ci.md` (shared standard): new **Image** (Dockerfile, tag scheme, GHCR,
  public visibility) and **Local runs** (`local.ps1`, `setup-desk.ps1`)
  sections; Toolchains/Build driver point at `scripts/ci/`; **Deviations**
  records that Arcane's Linux legs run in a container (a pattern Astra and
  Manifold2D may adopt later).
- `README.md` / `CLAUDE.md`: one line each -- run CI locally with
  `scripts\ci\local.ps1`.

## 5. Rollout

Branch `ci/local-pipeline` (worktree `D:/dev/starworks/Arcane-ci`) off
`main`. It reaches `main` through a PR so the hosted legs prove it, including
the first image build. Pushing and opening the PR need the user's go-ahead.
Then B gets its own brainstorm; then the modularization arc resumes (its CI
edits rebase onto `scripts/ci/`).

## 6. Out of scope

- ccache in hosted CI (local only for now).
- Windows or macOS containers.
- A self-hosted Linux runner (B decides whether one is wanted).
- Server-runtime / service images (C, D).
- Retiring Jenkins (B).
