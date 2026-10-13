# Arcane public distribution: Hub engine manager, R2 releases, signing, SDK packaging

**Date:** 2026-10-12 · **Base:** `main` 946092fb · **Branch:** `docs/distribution-design`
**Status:** design approved in conversation (decisions, sections 1-9, the Arcane-side review fold-ins, conflicts 1 and 2); written-spec review rounds 1 and 2 folded in (packaged-game writes, logs, premake, notices, manifest expiry, release image, PR packaging check, SAC and local modules; the AVX2 requirement and check, the SAC registry state); approved. Phase 2 plan review folded in (the manifest pair bypasses the cache, write-once history and repair, the notes file, staging rehearsals, a repository_dispatch website rebuild, an expiry watchdog, action pins and `persist-credentials: false`). Plan: `docs/superpowers/plans/2026-10-12-arcane-distribution-phase2-pipeline.md`

## 0. Status and context

This spec takes Arcane from "build it from the repo" to a public, free download:
the Arcane Hub installs, updates and manages several engine versions, each a
signed release served from `downloads.starworks.dev`.

It **supersedes**:
- the Hub launcher spec's v1 non-goal "engine version management, installs,
  downloads, accounts" (`docs/specs/2026-07-26-arcane-hub-launcher-design.md`,
  Non-goals): versions, installs and downloads are now in scope; accounts stay
  out (s11);
- the Hub product review's **Tier 3** (`docs/audits/2026-07-29-arcane-hub-product-review-vs-comparable-launchers.md`):
  download/install, Verify/repair, channels, opt-in Hub self-update and code
  signing are designed here. The review's deliberate non-goals (sign-in,
  store/news, telemetry, forced updates, tray residency) still hold;
- the project-format spec's deferral of a "packaged/installed SDK with a
  multi-version registry" (`docs/specs/2026-07-22-arcane-project-format-design.md`
  §12). Its §10A SDK layout is the starting point; s7 updates it for the
  module system.

It **builds on**:
- engine modularization (`docs/superpowers/specs/2026-10-11-engine-modularization-design.md`):
  `.arcmodule` descriptors, `build/modules.lua`, `arcane_module_consumer`,
  `arcbuild package`. Engine work here (phase 1) starts after its P6/P7 land;
- the local CI pipeline, sub-project A (`docs/superpowers/specs/2026-10-12-local-ci-pipeline-design.md`
  on `ci/local-pipeline`, "plan A" below): `scripts/ci/build.{sh,ps1}`, the
  content-addressed GHCR image, and its `ci-image` job rules;
- roadmap D15 (ship pipeline: "cooking, installers and Dist crash capture",
  per the modularization spec §3) and D26 (GitHub Actions security rules) in
  `docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md`;
- `LICENSING.md`: each version becomes MIT four years after it is first
  publicly distributed, so release dates and version identity are evidence
  (s4, s12).

**What exists today** (verified on `main` 946092fb):
- The Hub is Tauri 2 (`ArcaneHub/`, binary `arcane_hub`, identifier
  `dev.starworks.arcanehub`, 0.1.0). It has no network code. Its NSIS
  installer (`installMode: "currentUser"`) exists only as an artifact of
  `.github/workflows/build-arcane-hub.yml`. Its state is
  `%LOCALAPPDATA%\Arcane\hub\{recents,engines,settings}.archub`
  (`ArcaneHub/src-tauri/src/paths.rs`). Engines are added by path
  (`register_engine`) and probed with `ArcaneEditor.exe --print-engine-info`.
- The probe prints one line of JSON, `{ "engineAbi", "build", "exePath" }`
  (`HostBoot::EngineInfoJson`, `ArcaneClient/src/Arcane/Host/ProjectBoot.hpp`).
  `build` is `BuildInfo()`, a display string such as `Arcane 0.1 (M5) [Dist]`
  (`ArcaneCore/src/Arcane/Base/Engine.cpp`).
- `ArcaneCore/src/Arcane/Version.hpp` has `kVersionMajor = 0`,
  `kVersionMinor = 1` and `VersionString()` = `"Arcane 0.1 (M5)"`: no patch,
  no prerelease, no tags.
- The plugin ABI is `kGamePluginABIVersion = 58` (`ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`);
  modularization bumps it to 59. `.arcproj` requires `engine.abi`
  (`ArcaneCore/src/Arcane/Project/ProjectManifest.cpp`).
- No engine installer, no signing, no release manifest. The Dist editor
  folder is about 43 MB unpacked (`dxcompiler` 18 MB, D3D12 9.6 MB).

Paths below are as on `main`; modularization moves programs and modules under
`Source/<Type>/` with include paths unchanged.

## 1. Goals and non-goals

**Goals.**
1. Anyone can download the Hub from starworks.dev, install an engine from it
   without admin rights, and build a game against that engine's SDK.
2. Every byte the Hub installs is verified against a signed manifest; every
   Windows binary is Authenticode-signed, so Smart App Control lets it run.
3. A project records exactly which engine it uses; nothing upgrades it
   silently.
4. One tag push produces a release: build, sign, package, upload, manifest,
   GitHub Release, website.
5. Release dates are recorded in more than one independent place, because
   each version's MIT change date depends on them.

**Non-goals (v1).** macOS packages and notarization; the Microsoft Store;
Flathub; accounts; telemetry and crash upload; forced updates; nightly builds;
auto-upgrading projects; a Hub-managed C++ toolchain (later, s11); shipped-game
portability below the engine's glibc floor (follow-on, s7.5).

## 2. Decisions

| Topic | Decision |
|---|---|
| Audience | Public and free. The Hub installs, updates and manages multiple engine versions. |
| What an engine download contains | Full development: the editor, runtime and server, plus the packaged SDK (s7). A Hub-managed C++ toolchain comes in a later phase. |
| Hosting | Cloudflare R2 behind `downloads.starworks.dev`. The manifest lives in R2 and is written by Arcane's CI (approach A). |
| Channels | Stable, plus an opt-in preview channel. No nightly builds. |
| Integrity | Each file's SHA-256 is listed in a manifest signed with minisign (Ed25519). The Hub verifies everything. |
| Windows signing | Azure Artifact Signing (formerly Trusted Signing), Basic SKU, about $10 a month (US individual). It signs the engine, the SDK tools and the Hub installer. CI authenticates by OIDC federation, never a stored secret. |
| Distribution | Direct downloads from starworks.dev only. No Microsoft Store. |
| Platforms | Windows and Linux. macOS isn't offered; Mac users build from source. |
| Linux floor | Ubuntu 24.04 / glibc 2.39 for the engine and Hub downloads, one toolchain (plan A's image). Games packaged with the v1 SDK inherit it; a lower shipped-game floor is a follow-on (s7.5). |
| Engine build | The Dist engine, with debug info split into a separate symbols archive (s7.3). Game modules build in two configs, Development and Release, against it (s7.2). |
| Project pinning | `.arcproj` records the exact version, the build and the channel. Upgrades are always explicit; "follow patches" is opt-in. |
| Install scope | "Just me" by default (no admin). "All users" is an option (Program Files, UAC). |
| Website | `/arcane/download` and `/arcane/releases`, both generated from the same manifest. |
| Spec home | This file, in the Arcane repo; the website repo carries a pointer. |

### 2.1 Install locations

| | Just me (default) | All users (UAC) |
|---|---|---|
| Hub | `%LOCALAPPDATA%\Arcane Hub\` (as today) | `C:\Program Files\Arcane Hub\` |
| Engines | `%LOCALAPPDATA%\Arcane\Engines\<version>\` | `C:\Program Files\Arcane\Engines\<version>\` |
| Symbols | `%LOCALAPPDATA%\Arcane\Symbols\<version>\` | same (always per user) |
| Data | `%LOCALAPPDATA%\Arcane\`, always per user | same |

Linux: engines in `~/.local/share/arcane/engines/<version>/`; symbols in
`$XDG_CACHE_HOME/arcane/symbols/<version>/` (default `~/.cache`); Hub data in
the XDG state and cache folders.

A **Choose location...** setting installs engines somewhere else, such as
another drive. The Hub's data never moves.

Note: the Hub's "as today" path is `%LOCALAPPDATA%\Arcane Hub\` (the NSIS
per-user default; `ArcaneHub/src-tauri/src/lib.rs` and the `assoc.rs` test
agree). The comment at `paths.rs:8` saying `%LOCALAPPDATA%\Programs\Arcane Hub\`
is stale. All-users Hub installs need the NSIS `installMode` changed from
`"currentUser"` to `"both"` (phase 4).

## 3. Components (design section 1)

**The Arcane repo:**
- **Engine packaging.** A Dist build is packed into a versioned archive (.zip
  on Windows, .tar.gz on Linux), plus a symbols archive from the same build
  (s7.3).
- **One release workflow** in GitHub Actions, triggered by a tag. It builds
  through `scripts/ci/build.{sh,ps1}` (plan A §1.3), signs, packages, uploads
  to R2, regenerates and signs `releases.json`, creates a GitHub Release, and
  tells the website to rebuild (a `repository_dispatch`).
- **The Hub** gains:
  - an engine manager (list, install, verify, remove);
  - a release client that reads and verifies the manifest;
  - project pinning and upgrades;
  - Tauri self-update;
  - install-scope settings.

**Cloudflare:**
- An R2 bucket `arcane-downloads`, served at `downloads.starworks.dev` (a
  custom domain on the starworks.dev zone; never the rate-limited `r2.dev`
  URL). It holds:
  - `releases.json` and its signature `releases.json.minisig`;
  - the Hub's update feed, `hub/latest.json`;
  - the immutable archives under `engine/<version>/...` and `hub/<version>/...`;
  - `history/<serial>.json` and `.minisig`: every manifest ever published,
    written first and write-once, so a half-finished update can always be
    repaired from a signed copy (and a fourth record of release dates);
  - `staging/`: a separate manifest for rehearsals, signed with a committed
    test key no Hub trusts.
- Cache Rules: archives (including `.AppImage`) get a long edge cache
  lifetime; the manifest pair (`.json`, `.minisig`) bypasses the cache, so the
  edge can never serve a manifest beside a stale signature (s12).

**The website repo** (`D:/dev/web/Starworks`, Pages project `starworksdev`):
- `/arcane/download` and `/arcane/releases`, built from the manifest at build
  time.
- A workflow that rebuilds and deploys the site on a `repository_dispatch`
  (`arcane-release`) from Arcane's release job.

**Credentials** live only in a protected GitHub environment (`release`) in
the Arcane repo, with required reviewers:
- Artifact Signing: no stored secret. `azure/login` exchanges the job's
  GitHub OIDC token through a federated credential whose subject is the
  `release` environment; the environment holds only the client, tenant and
  subscription IDs;
- the minisign key and the Tauri updater key, each with an offline backup;
- an R2 API token scoped to Object Read & Write on the `arcane-downloads`
  bucket only;
- a StarworksDev GitHub App's ID and private key, for the cross-repo
  dispatch (never a personal token).

**Unchanged:** Arcane's existing CI (`ci.yml`) keeps building and testing all
three platforms, macOS included.

## 4. Release pipeline (design section 2)

**Versions**
- Semver tags: `v0.3.0` for stable, `v0.3.0-preview.N` for previews. CI checks
  the tag against `Version.hpp`, which gains a patch number and a prerelease
  string (phase 1).
- The probe gains `version` (the semver string) and `commit` (the full git
  SHA, passed in by CI through an environment variable, since plan A's build
  scripts never call git, plan A §1.3). The existing `engineAbi`, `build` and
  `exePath` keys stay as they are, so today's Hub keeps parsing it.
- The ABI version comes from the probe's `engineAbi`.
- The Hub has its own `hub-v*` tags, so the Hub and the engine release
  independently.

**Runners.** `windows-2025` and `ubuntu-24.04`, the images `ci.yml` uses
today. The Linux build runs in plan A's content-addressed image
(`ghcr.io/starworksdev/arcane-ci@sha256:<digest>`), so a release is built by
exactly the toolchain CI tests.

The release workflow never builds that image. It computes the tag with
`scripts/ci/image-tag.sh`, requires `ghcr.io/starworksdev/arcane-ci:<tag>` to
be published already (by `ci.yml`'s `ci-image` job on `main`), and pins the
digest it reads from that tag. If the tag isn't published, the run fails
closed. The release workflow holds `packages: read` only; `packages: write`
stays with `ci-image` alone (plan A, `docs/ci.md`).

**Workflow rules** (plan A's `ci-image` rules and roadmap D26):
- Triggered only by a tag push or a manual dispatch from `main`. Nothing in it
  is reachable from a pull request: no `pull_request` or `pull_request_target`
  trigger touches the `release` environment, and fork code never runs with its
  credentials.
- Every `uses:` is pinned to a commit SHA, at the releases plan A pins (no
  Node 20 `v4` actions), and every checkout sets `persist-credentials: false`.
- `permissions:` is read-only by default; `id-token: write` (for OIDC) and
  `contents: write` (for the GitHub Release) are granted only to the jobs that
  need them.
- Publishing jobs run in the `release` environment and wait for the owner's
  one-click approval.

**Engine job**
1. `scripts/ci/build.{sh,ps1} Dist` on `windows-2025` and in the image on
   `ubuntu-24.04`, with debug info on (s7.3).
2. Split symbols (s7.3). Check that every binary's PDB GUID + age (Windows)
   or GNU build-id (Linux) matches its debug file. A mismatch stops the run.
3. Run `--print-engine-info` and the test suite against the built binaries.
4. Sign every Windows `.exe` and `.dll` with Artifact Signing, then run
   `--print-engine-info` again on the signed binaries. It is cheap, and it
   proves signing didn't break loading.
5. Package, then record each file's size and SHA-256. The archive's LICENSE
   carries that version's BSL change date; `THIRD_PARTY_NOTICES` is
   generated (s7.6).
6. Upload to R2 at `engine/<v>/arcane-<v>-<os>-<arch>.<ext>` and
   `engine/<v>/arcane-<v>-<os>-<arch>-symbols.<ext>`, refusing to overwrite
   anything already there. Uploads use the S3 API (aws-cli or rclone), not
   Wrangler (s12).
7. Update `releases.json`, bump its serial, set its expiry, sign it, and
   upload it.
8. Create a GitHub Release with the notes and no files attached; it serves as
   a permanent date record. Notes come from `docs/releases/<version>.md`,
   which must exist at the tag, and feed both the GitHub Release and the
   manifest's `notes`. Then send the website its `repository_dispatch`.

**Hub job**
- Builds the NSIS installer on `windows-2025`, and an AppImage plus a .deb on
  `ubuntu-24.04`.
- Signs the Windows installer and the Hub's binaries with Artifact Signing.
- Signs the Tauri update files with the Tauri updater key.
- Uploads everything to `hub/<v>/` and updates `hub/latest.json`.

**Channels and yanking**
- A tag containing `-preview` publishes to the preview channel.
- Yanking is a small manual workflow. It marks the release yanked and re-signs
  `releases.json`. The archives stay, so projects pinned to that version can
  still install it.

**Re-signing.** A scheduled job re-signs `releases.json` monthly, even when
nothing changed (s5). It runs in the `release` environment, so it waits for
the owner's approval like any publish. GitHub silently disables a public
repository's schedules after 60 days without activity, so a watchdog outside
GitHub (the Starworks Ops bot) checks `expires` daily and alerts when fewer
than 30 days remain.

**Safety**
- The approval gate above.
- Pull requests check packaging on `ci.yml`'s existing Dist legs instead of
  rebuilding Dist in a separate workflow (s10).
- The release workflow has a dry-run mode for a manual dispatch: it builds and
  packages without uploading anything (test key, no `release` environment).
- A rehearsal mode (from `main`) signs for real and publishes a fixture to
  `staging/` with the committed test key. A test checks that the production
  key list rejects manifests signed with it.

## 5. Manifest and trust (design section 3)

```json
{
  "schema": 1, "serial": 42,
  "generated_at": "...", "expires": "...",
  "base_url": "https://downloads.starworks.dev/",
  "channels": { "stable": "0.3.1", "preview": "0.4.0-preview.2" },
  "hub": { "latest": "0.2.0", "min_supported": "0.2.0", "artifacts": [] },
  "releases": [{
    "version": "0.3.1", "channel": "stable", "released_at": "...",
    "build": "<full git commit SHA>", "abi": 59,
    "license": "BUSL-1.1", "change_date": "2030-11-02",
    "min_hub_version": "0.2.0",
    "yanked": false, "yank_reason": null, "security": false,
    "notes_url": "https://starworks.dev/arcane/releases#0.3.1",
    "notes": "<markdown from docs/releases/0.3.1.md>",
    "artifacts": [
      { "os": "windows", "arch": "x86_64", "kind": "engine",
        "path": "engine/0.3.1/arcane-0.3.1-windows-x86_64.zip",
        "size": 0, "sha256": "..." },
      { "os": "windows", "arch": "x86_64", "kind": "symbols",
        "path": "engine/0.3.1/arcane-0.3.1-windows-x86_64-symbols.zip",
        "size": 0, "sha256": "..." }
    ]
  }]
}
```

`build` is the release's full commit SHA (the probe's `commit`), not the
probe's display string `build`. `change_date` is four years after
`released_at`, per `LICENSE`; for a stable version that had previews, four
years after its first preview's `released_at` (s12, pending the legal
review).

How the Hub checks it:
1. **Signature.** Verify the minisign signature against a public key built
   into the Hub. Reject the manifest if it fails.
2. **No rollback.** Remember the highest serial seen, and reject any manifest
   with a lower one. This blocks a downgrade by replaying an old, validly
   signed manifest.
3. **No stale lists.** Reject a manifest past its `expires`, which is set
   about **90 days** after signing. CI re-signs it every 30 days even when
   nothing changed (s4, Re-signing), so two missed approvals in a row are
   still survivable. When offline, installed engines keep working.
4. **Every download.** Check its SHA-256 against this verified manifest before
   anything else touches it (s6).
5. **Old Hubs.** If `min_supported` or `min_hub_version` is newer than the
   Hub, prompt to update the Hub.

**Keys.** The minisign key (for the manifest) and the Tauri key (for Hub
updates) are separate. The Hub embeds a *list* of trusted manifest keys, so a
key can be rotated by shipping the new one before retiring the old.

**The Hub's own updates.** Tauri's updater verifies each update artifact's
minisign signature, but its feed (`hub/latest.json`) is not itself signed.
The Hub requires `tauri-plugin-updater` **2.13.3 or later**: 2.12.0 removed
`allowDowngrades` from the webview-callable `check` command and made it a
plugin-config setting (default `false`), and 2.13.3 moved to `minisign-verify`
0.3, which rejects non-canonical Ed25519 signatures. The Hub sets
`allowDowngrades: false` and supplies no custom version comparator. Sources:
the [updater plugin docs](https://v2.tauri.app/plugin/updater/) and the
[updater changelog](https://github.com/tauri-apps/plugins-workspace/blob/v2/plugins/updater/CHANGELOG.md)
(2.12.0, 2.13.3).

**Website.** The site reads the same file. Yanked releases are shown struck
through, with the reason.

## 6. The Hub (design section 4)

**The Engines view**
- Lists each installed engine with its version, channel, ABI, scope, location
  and size, with buttons for Open folder, Verify and Remove.
- **Install engine...** lists stable versions, plus previews if you've opted
  in, with dates, notes and "security" or "yanked" badges. Yanked versions are
  hidden unless a project needs one.
- Dev builds are handled as today: added by path (`register_engine`), probed,
  labelled "Dev build". Removing one only unregisters it; the files stay.

**The machine check.** An x64 CPU without AVX2 would crash the engine with
an illegal instruction instead of showing an error. So the Hub checks first,
before an install and before every launch: AVX2 and FMA, including OS support
for the AVX state (Rust's `is_x86_feature_detected!`, which reads CPUID and
XGETBV). The Hub itself is built for the plain x86-64 baseline, so the check
always runs. On a machine that fails, it says plainly "Arcane needs a CPU with
AVX2 (Intel Haswell / AMD Excavator or newer)". The download page states the
same requirement (s8).

A check inside the editor is not v1. The editor exe and the engine DLLs are
compiled with `/arch:AVX2`, so AVX2 instructions can run before any
`main()`-time check: static initializers run as each DLL loads. A reliable
editor-side check needs a small entry stub compiled without AVX2 that checks
before loading the engine; it's listed under Later (s11).

**Installing.** The order is fixed, and nothing from the download runs before
its hash is proven:
1. Download the archive to a temp folder (resumable).
2. **Verify its SHA-256 against the signed manifest** (s5). On a mismatch,
   nothing is extracted and nothing is executed.
3. Extract to `<version>.partial`.
4. Only now run `--print-engine-info` from `.partial`, and check that its
   `version`, `commit` and `engineAbi` match the manifest entry.
5. Rename `.partial` to the final folder.

An interrupted install leaves nothing half-done: a leftover `.partial` is
deleted on the next start.
- **Just me** needs no admin.
- **All users** uses a signed helper behind one UAC prompt. If you decline it,
  the Hub says so clearly and offers "Install for just me instead".
- **Verify** re-checks every file against the archive's own file list
  (`manifest.txt`) and offers to repair.
- **Symbols** are an optional per-version download into the symbol cache
  (s2.1), offered from the Engines view and from a crash report. They are
  verified the same way.

**Projects**
- `.arcproj`'s `engine` block gains `version`, `build`, `channel` and
  `followPatches` beside the required `abi`, which the host's ABI gate keeps
  reading. Older projects that only record an ABI are still read, and get
  migrated the next time you upgrade them.
- Opening a project whose version is missing offers **Install 0.3.1** or
  **Use another installed version...**. The second warns if the ABI differs.
- **Upgrade project...** is always explicit. It shows the release notes and an
  ABI warning.
- **Follow patches**, if a project opts in, means the newest `0.3.x` with the
  same ABI.
- The Hub always shows which engine a project uses and why.
- Release notes are shown as escaped text or sanitized Markdown, never raw
  HTML: the webview sits in an app that can launch processes, so the signed
  manifest is not the only defense.
- The Hub sets `ARCANE_SDK` to the pinned engine's `sdk/` folder before
  launching the editor or a build. Outside the Hub (a terminal, an IDE), you
  set it yourself: each engine in the Engines view has **Copy environment**,
  which copies the line for your shell (`set ARCANE_SDK=...` or
  `export ARCANE_SDK=...`), and the download page's docs show the same.

**Updates and settings**
- The Hub checks for updates at launch and then every few hours. A new engine
  release shows up as a quiet badge.
- The Hub's own update goes through Tauri: "Restart to update", never forced,
  never a downgrade.
- **Settings:** default install scope, install location, show previews, check
  automatically, and **Remove all engines**.
- No telemetry.

## 7. Engine packaging and the SDK (design section 5)

### 7.1 Archive layout

```
arcane-0.3.1/
  Binaries/     <Config>-<os>-<arch>-md/ (and -mono/): editor, runtime, server,
                crash reporter, arcbuild, arccook, engine module libraries,
                runtime deps (D3D12/, DXC; Vulkan loader on Linux)
  data/         built-in assets
  sdk/          the installed SDK (below)
  LICENSE       BSL 1.1 + this version's change date
  THIRD_PARTY_NOTICES   every redistributed dependency's licence (s7.6)
  manifest.txt  every file + SHA-256 (Verify)
  engine.json   { version, build, abi, channel }
```

`engine.json` must equal what the probe reports (`version`, `commit` as
`build`, `engineAbi` as `abi`), checked in CI. The ReferenceProject is not
included.

**Folder names (the modularization layout).** Engine binaries live in
`Binaries/<Config>-<os>-<arch>-md|mono/` (formerly `bin/`) and build
intermediates in `Intermediate/Build/` (formerly `bin-int/`; the Hub's cargo
target is `Intermediate/Build/hub-cargo`). Every SDK path in the manifest, the
installer, the Hub's engine manager and the docs uses `Binaries/`, for example
`%ARCANE_SDK%\Binaries\Release-windows-x86_64-md\arcbuild\arcbuild.exe` and
`$ARCANE_SDK/Binaries/Release-linux-x86_64-md/ArcaneCore/libArcaneCore.so`.
On Linux and macOS, scripts resolve the folder with
`scripts/arcane-bin-dir.sh <Config>` rather than hardcoding it. Unchanged:
vendored `ThirdParty/*` libraries keep their own `bin/`, a game package's
internal `Intermediate/Package/<cfg>/bin`, and the packaged-game layout
(`Packages/<Proj>-<os>-<cfg>/<Proj>.exe` with
`<Proj>/Binaries/Win64|Linux|Mac/`).

**The SDK after modularization** (`sdk/`, what `build/arcane.lua` reads as
`ARCANE_SDK`):
- every `.arcmodule` descriptor of the engine's modules and vendor modules,
  at the paths `build/modules.lua` expects;
- `build/arcane.lua` and `build/modules.lua` (the descriptor reader);
- the libraries `arcbuild package` links: each module's and vendor library's
  static library for the monolithic layout, `ArcaneRuntimeHost`, and a
  prebuilt package launcher (`ArcanePackageLauncher`; today
  `arcane_package_launcher` compiles `Launcher.cpp` from the SDK's source at
  package time, which an installed SDK without sources cannot do); plus the
  import libraries for the modular layout the editor loads;
- public headers, **generated from the module graph**: a module's headers
  ship when the module is public, and a vendor's headers ship only when a
  public module depends on it publicly. Private dependencies' headers are not
  shipped. This replaces the hand-kept "list of public headers";
- the header-only dependencies a public header reaches (`arcane_module_consumer`
  names glm, Astra, enkiTS, Manifold2D and Mosaic today);
- **premake 5.0.0-beta8**, because `arcbuild` generates through premake. It
  goes where `Arcane::Toolchain` looks for its bundled copy,
  `sdk/ThirdParty/premake5/premake5[.exe]` (`Toolchain.hpp:65` and
  `Toolchain.cpp:247` on `feat/modularization`). On Windows that is the
  vendored `ThirdParty/premake5/premake5.exe`; on Linux it is the CI image's
  copy (`/opt/arcane-tools`, plan A `docs/ci.md`, "Build driver"). Without
  it, `arcbuild` falls back to a `premake5` on `PATH`, which an engine
  download can't assume.

Size: the static libraries dominate; expect **100+ MB** unpacked for the SDK
on top of the ~43 MB runtime. Phase 1 measures the real figure. The per-file
figures go in the manifest, so the Hub's disk-space check uses real numbers.

`arcane.lua` gains an "installed SDK" mode (it finds libraries under
`sdk/lib/` instead of `Binaries/<cfg>-<sys>-<arch>-md/` and `-mono/`). Developer
checkouts keep working as they do now.

### 7.2 Game module configs: Development and Release

An installed engine is one Dist build, so a game module built against it may
not change the ABI. `arcane_game_module` and `arcane_module_consumer` offer two
configs against an installed SDK:

| Config | ABI macros (identical to the engine's Dist) | Codegen |
|---|---|---|
| Development | `NDEBUG`, `ARC_BUILD_DIST`, release CRT (`runtime "Release"`, `/MD`), so `_ITERATOR_DEBUG_LEVEL` is 0 | only `/Od /Zi` (`-O0 -g`) |
| Release | the same | `optimize "speed"`, debug info on |

- The flavor contract `arcane_module_consumer` already enforces (per-config
  runtime, `ARC_BUILD_*`, `NDEBUG`, `/utf-8`, `/arch:AVX2`, so inline header
  layouts agree with the DLLs') is the one place this is enforced. Against an
  installed SDK it refuses, at generate time with a named error, any
  ABI-affecting change: a Debug config, the debug CRT, `_ITERATOR_DEBUG_LEVEL`
  other than 0, a missing `NDEBUG`/`ARC_BUILD_DIST`, or, on Linux,
  `_GLIBCXX_DEBUG` (libstdc++'s debug mode changes container layouts).
  `_GLIBCXX_ASSERTIONS` is allowed: it adds checks without changing any
  layout.
- `ARC_ASSERT` follows `NDEBUG` (`MOSAIC_ASSERT`, `ThirdParty/Mosaic/include/Mosaic/Assert.hpp`),
  so it is off in both configs. Game code gets **`ARC_GAME_ASSERT`**, keyed to
  the module's own config: active in Development, compiled out in Release.
  It never changes a type's layout.
- Developer checkouts keep Debug/Release/Dist as today.

### 7.3 Symbols

Dist gets debug info, split off:
- **Windows:** `/Zi`, linker `/DEBUG /OPT:REF,ICF` (with `/DEBUG` the MSVC
  linker otherwise defaults to `/OPT:NOREF`). Every Dist filter in
  `premake5.lua` sets `symbols "off"` today: on `feat/modularization`
  (b81d73da) the module block at line 350 and the hosts' one-line filter at
  478, among others (559, 639, 692, 870, 953, 1084, 1214); on `main` the same
  filters sit at other lines. ArcaneTests' Dist block already turns symbols on.
- **Linux:** `-g`, then `objcopy --only-keep-debug`, `strip --strip-debug`
  (`strip`), and `objcopy --add-gnu-debuglink`.
- Both archives come from the same build. CI checks that each binary's PDB
  GUID + age or build-id matches its debug file (s4, step 2).

**The symbol cache.** The Hub installs a symbols archive into
`%LOCALAPPDATA%\Arcane\Symbols\<version>\` (Linux: the XDG cache, s2.1). In
phase 1 the crash reporter adds the running engine's cache folder to its
symbol path: `DefaultSymbolPath` in `ArcaneCrashReporter/src/Symbolizer.cpp`
(DbgEng, `IDebugSymbols3`), which today searches the dump's module folders,
the reporter's own folder and, outside Dist only, `_NT_SYMBOL_PATH`. Because a
Dist reporter ignores `_NT_SYMBOL_PATH` on purpose (R113), the cache must be
added explicitly. The reporter has no Linux symbolizer today; on Linux the
cache serves `gdb` (`debug-file-directory`) until it does.

### 7.4 Read-only install folder

An audit moves every write the engine makes out of its install folder. Known
targets (verified on `main`):
- **`imgui.ini`**: ImGui's default `IniFilename` is a relative `imgui.ini`
  until the editor retargets it, and the editor reads a legacy `imgui.ini` as
  a layout seed (`ArcaneEditor/src/App/EditorApp.cpp:1809`). Session layouts
  already go to `<UserRoot>/Editor/Layouts/Session/` (`EditorUserDir`,
  `Paths.cpp`), which is per user.
- **`diagnostics/`**: with no project, `Paths::DiagnosticsDir` resolves to
  `<engineDir>/diagnostics` (`ArcaneCore/src/Arcane/Platform/Paths.cpp:180`),
  and the reporter's fallback report folder is `<exeDir>/diagnostics`
  (`ArcaneCore/src/Arcane/Base/Diagnostics.cpp:2661`).
- **`Logs/`** beside the exe: with no configured log folder, the log goes to
  the report folder's *parent* plus `Logs` (`Diagnostics.cpp:2319`), so it
  lands in a sibling `<exeDir>/Logs/`, next to `<exeDir>/diagnostics/`, not
  inside it. Observed on the P6 Windows package boot as
  `Binaries/Win64/Logs/ReferenceProject.log`. It is its own target.
- **Packaged games** under `<Project>/Binaries/<Platform>/` (`Win64`, `Linux`,
  `Mac`; `build/arcane.lua` on `feat/modularization`). Here the project's
  `Saved/` is inside the install folder: the P6 Windows package boot wrote
  `ReferenceProject/Saved/Logs/` and `Saved/Diagnostics/` inside `Packages/...`,
  and a Program Files install would do the same.

Where each write goes:
- **The editor** (and the runtime run against a project): the project's
  `Saved/` or the per-user data folder (`%LOCALAPPDATA%\Arcane\...`, the XDG
  state folder on Linux). The project is a user-owned folder, so its
  `Saved/` is writable.
- **A packaged game, in every config:** a per-user folder, never the
  project's `Saved/`. Dist packages already resolve one: `GameUserDir` is
  `%LOCALAPPDATA%\<Company>\<Game>` (`$XDG_CONFIG_HOME/<Company>/<Game>` on
  Linux) and Dist diagnostics go below it (`Paths.cpp`, `ResolveGameUserDir`;
  settings spec S7, "the Dist user directory"). A non-Dist package resolves
  `Saved/` and diagnostics through the project folder instead, and that is
  the case to fix. Unreal does the same: a packaged game's `Saved/` is
  `%LOCALAPPDATA%\<Project>\Saved`.

**The test.** CI runs the editor, the runtime, and a **Release and a Dist**
packaged game from a read-only copy, and fails on any write. Both package
configs are covered because Dist and non-Dist resolve paths differently.

### 7.5 Linux

- Rendering uses Vulkan; the archive ships the Vulkan loader. A
  Vulkan-capable driver is required.
- **The download floor is Ubuntu 24.04 / glibc 2.39** for the engine, its
  SDK and the Hub: one toolchain, plan A's image.
- **The shipped-game floor is a separate concept.** A game packaged with the
  v1 SDK links the SDK's static libraries and so inherits glibc 2.39. v1
  release notes state: **"Games packaged with this SDK require glibc 2.39+."**
- **Follow-on, not in v1: packaged-game portability.** A second build of the
  SDK's static libraries in the Steam Linux Runtime sniper SDK image
  (`registry.gitlab.steamos.cloud/steamrt/sniper/sdk`, glibc 2.31), produced
  through plan A's content-addressed images, so packaged games run on older
  systems and in the Steam runtime.

### 7.6 Third-party notices

The engine archive and the SDK redistribute third-party code, including
SDL3, ImGui, FreeType, msdfgen, NRI, enkiTS, spdlog, nlohmann, glm, DXC, the
D3D12 Agility SDK and, on Linux, the Vulkan loader. Each has its `LICENSE` in
`ThirdParty/<name>/`, indexed by `ThirdParty/README.md`. Two of them come with
conditions:
- **FreeType** (FTL) requires an acknowledgment in the documentation.
- **The Agility SDK and DXC** are under Microsoft's redistribution terms.

What ships:
- **`THIRD_PARTY_NOTICES`** at the archive root, generated at packaging time
  from the vendor modules in the module graph plus the runtime dependencies
  in `Binaries/`. Each entry has the name, the version and the licence text
  verbatim. A dependency that isn't redistributed (Catch2, rapidcheck, the
  Playwright fixtures) is left out.
- **Packaged games** carry the notices for whatever they link.
  `arcbuild package` stages a `THIRD_PARTY_NOTICES` beside the game, built
  from the same data for that game's modules.
- The download page and the Hub's About link to the notices.

The notice list goes to the legal review (s12).

## 8. Website (design section 6)

**`/arcane/download`**
- Hub download buttons for the visitor's OS (Windows or Linux), with each
  file's size and SHA-256 and how to check it.
- System requirements, on both platforms an **x64 CPU with AVX2 and FMA**
  (the engine's min-spec, `/arch:AVX2` and `-mavx2 -mfma` in
  `build/arcane.lua`; roughly Intel Haswell / AMD Excavator and later):
  - Windows: Windows 10 22H2 or Windows 11, 64-bit, with a Direct3D 12 GPU
    and driver;
  - Linux: Ubuntu 24.04 (glibc 2.39) or newer, with a Vulkan driver;
  - the build tools needed for game development.
- macOS: "build from source on GitHub".
- The current stable and preview versions.
- A licence line: BSL 1.1, with each version becoming MIT four years after it
  is first published.

**`/arcane/releases`**
- Every release with its channel, date, ABI, notes and badges. Yanked releases
  are struck through, with the reason.
- Each version has its own anchor, e.g. `#0.3.1`.
- Notes come from `docs/releases/<version>.md` at the tag; CI copies them into
  both the GitHub Release and `releases.json`.

**How it's built**
- A `+page.server.ts` loader fetches `releases.json` at build time, verifies
  its signature, and prerenders.
- If the manifest is unreachable, the build uses a committed fallback copy.
- If the signature is bad, the build fails.

**Links**
- A Download button on `/arcane`, and a Download link in the Technology menu.
- The page gets its own share card.
- No visitor tracking.

## 9. Failure handling (design section 7)

| Failure | What the Hub does |
|---|---|
| Offline, or the manifest can't be fetched | Installed engines work. Install and update show "can't reach downloads.starworks.dev" with Retry. The cached manifest is shown with its age. |
| Bad signature, lower serial or expired manifest | Rejects it and keeps the cached copy. No installs from an unverified list. |
| Hub too old | "Needs Arcane Hub 0.2+", with an Update Hub button. |
| Download interrupted | Resumes. After 3 failures, shows an error and Retry. |
| SHA mismatch | Deletes the file and retries once. If that fails, shows an error with a report link. Nothing is extracted or run. |
| Not enough disk space | Checked before downloading, using the sizes in the manifest. |
| Probe fails, or its version, commit or ABI doesn't match the manifest | Deletes `.partial`. |
| UAC declined | Clear message, with "Install for just me instead" and Try again. |
| CPU without AVX2/FMA | Install and launch are disabled, with a plain message naming the requirement (s6). |
| Smart App Control on, or in evaluation mode | A note before the first build that game development needs it off (s12). |
| The pinned version was yanked | The project opens, with a banner giving the reason and offering an upgrade. |
| The pinned version is unknown | Offers adding an engine by path, or picking another version. |
| Removing an engine an editor is running from | Blocked. The Hub finds running editors through each project's `Saved/editor.lock` (`{pid, creation time}`, `ArcaneHub/src-tauri/src/editorlock.rs`) and its own spawn map, and compares each live editor's executable path with the engine folder; the lock itself does not name an engine. |
| Installed SDK and an ABI-affecting module config | Generation fails with a named error (s7.2). |

**CI fails closed.** A tag that doesn't match `Version.hpp`, an unpublished
Linux image tag, a failing test, a symbol-match failure, a signing failure, a
probe that fails on the signed binaries, an object that already exists, a
failing read-only audit, or a manifest that won't verify all stop the run
before `releases.json` is touched. Any archives already uploaded stay
unreferenced, and a cleanup step lists them.

**Website.** An unreachable manifest means the fallback is used. A bad
signature means the build fails.

## 10. Testing (design section 8)

- **Hub (Rust and vitest)**
  - Golden manifests: valid, bad signature, lower serial, expired, unknown
    fields, `min_hub_version`, rotated key.
  - The installer against a local server: full install, resume, SHA mismatch
    (asserting the probe never ran), failure mid-extract, probe mismatch
    rollback, Verify.
  - Pinning: exact, follow patches, yanked, unknown, migration of ABI-only
    projects, the "why this engine" text.
  - Engine removal blocked by a live editor.
  - The machine check and the SAC note, with the CPU features and the
    registry value injected: AVX2 without FMA, no OS AVX support, SAC states
    0, 1 and 2.
  - UI states of the Engines view.
- **Engine**
  - Archive layout, `manifest.txt` completeness, `engine.json` matching the
    probe, the LICENSE, `THIRD_PARTY_NOTICES` covering every redistributed
    dependency, premake present in the SDK.
  - Symbol match: PDB GUID + age / build-id per binary.
  - SDK smoke test: build the ReferenceProject game module, in Development and
    Release, against an *extracted archive*, then `arcbuild package` it, on
    Windows and Linux.
  - The installed SDK refuses a Debug config, a debug CRT and
    `_GLIBCXX_DEBUG`.
  - Read-only install test: the editor, the runtime, and a Release and a
    Dist package (s7.4).
- **Release workflow**
  - On pull requests, a packaging step on `ci.yml`'s existing Dist legs
    (Windows and Linux), reusing the build they already make: package,
    `manifest.txt`, the symbol split and match, `engine.json` against the
    probe. No signing, no upload, no `release` environment. Dist is never
    rebuilt for this.
  - Unit tests for the manifest generator, the refusal to overwrite, the yank
    workflow and the re-sign workflow (including the 90-day expiry).
- **Website**
  - Loader tests: signature, fallback, bad signature.
  - Playwright: download buttons per OS, the macOS note, release anchors, the
    existing link test.
- **First real release, by hand**
  - A clean Windows 11 VM with Smart App Control on, and Ubuntu 24.04.
  - Install the Hub; install an engine in both scopes; open a project; build a
    module; fetch symbols; update the Hub; remove everything.
  - The specific risk this pass checks: whether Smart App Control lets the
    signed editor load the developer's own unsigned, freshly built game
    module (s12).

## 11. Phases (design section 9)

Engine work starts after modularization lands (its P6 and P7). Phase 2 needs
no engine changes and may be prepared in parallel with it.

1. **Engine release-readiness** (after modularization P6/P7). Real semver in
   `Version.hpp`; the probe's `version` and `commit`; the read-only audit
   (including per-user folders for packaged games in every config) and its
   test; public headers generated from the module graph; the installed-SDK
   mode in `arcane.lua`, with the Development/Release configs, `ARC_GAME_ASSERT`
   and the refusal; premake in the SDK; Dist debug info, the symbol split and
   the reporter's symbol cache path; `THIRD_PARTY_NOTICES` and its staging by
   `arcbuild package`; the packaging script and its self-test, and the
   packaging step on `ci.yml`'s Dist legs. Result: a standalone engine
   archive and its symbols archive.
2. **Pipeline and hosting** (may run in parallel with modularization). R2 and
   `downloads.starworks.dev` with the Cache Rules; Artifact Signing and its
   OIDC federation; the keys and their backups; the scoped R2 token; the
   release workflow scaffolding (dry run, then publish behind approval), with
   the Linux image resolved, never built; the manifest generator, the yank
   workflow, the monthly re-sign behind approval, history and repair, and the
   GitHub Release; the website's `repository_dispatch` rebuild; the expiry
   watchdog in the Starworks Ops bot; the legal review (s12). Result: pushing `v0.2.0` publishes a release (once phase 1
   has landed).
3. **The Hub as engine manager.** Manifest trust; the Engines view; install
   scopes and the UAC helper; pinning, upgrades and follow patches; symbols;
   the AVX2 machine check and the SAC note; Remove all. Result: the Hub
   installs released engines.
4. **Hub distribution (Windows).** The signed NSIS installer with both scopes;
   Tauri self-update (`tauri-plugin-updater` 2.13.3+, downgrades blocked); the
   Hub's release workflow. Result: the Hub updates itself.
5. **The Linux Hub port.** The Hub is Windows-shaped today, and each of these
   needs a Linux counterpart: the AppUserModelID taskbar grouping
   (`spawn::APP_USER_MODEL_ID`, `dev.starworks.arcane`, shared with the
   editor), the tray (`tray.rs`), the `.arcproj` file association (`assoc.rs`,
   HKCU), and the NSIS-specific install and uninstall code. Plus the AppImage
   and .deb, desktop entries and MIME registration. Result: the Hub installs
   and updates on Ubuntu 24.04.
6. **Website and launch.** `/arcane/download`, `/arcane/releases`, the
   Download buttons, and the loader with its fallback. Result: public launch,
   then the manual VM pass.

**Later, listed but not designed yet**
- The Hub sets up the C++ toolchain (Build Tools, via a `.vsconfig`).
- Packaged-game portability (s7.5).
- A sample-project download.
- An AVX2 check in the editor itself, through an entry stub compiled without
  AVX2 (s6).
- Delta updates.
- Optional modules.

**Out of scope**
- macOS packages and notarization.
- The Microsoft Store.
- Flathub.
- Accounts.
- Telemetry and crash upload.
- Forced updates.
- Nightly builds.
- Auto-upgrading projects.

## 12. Risks and open items

- **Smart App Control and signing.** Windows 11 Smart App Control checks every
  `.exe` and `.dll` it loads, including Hub-downloaded files that carry no
  mark of the web, so an unsigned engine binary is blocked. Every Windows
  binary in an archive is signed, and the first release is tested by hand
  with Smart App Control on. A signing outage blocks releases (CI fails
  closed) rather than shipping unsigned files.
- **Smart App Control and locally built game modules.** The core workflow has
  the signed editor load the developer's own game module DLL, which is
  unsigned and newly built, so it has no cloud reputation either. Field
  reports show SAC blocking freshly built and unsigned native binaries,
  including DLLs loaded by signed hosts, with no "run anyway". Microsoft
  doesn't document how SAC treats a locally compiled DLL, so this is
  unverified. The manual VM pass (s10) checks it directly.
  **Fallback, decided now:** if SAC blocks local modules, the docs and the
  Hub say that game development needs Smart App Control turned off on that
  machine, as other native development tools do. Players are unaffected:
  a shipped game is signed by its developer, or not, and that's between the
  developer and SAC. The Hub reads SAC's state from
  `HKLM\SYSTEM\CurrentControlSet\Control\CI\Policy\VerifiedAndReputablePolicyState`
  (0 off, 1 on, 2 evaluation mode) and shows the note before the first build
  instead of after a confusing failure. It warns for both 1 and 2, because
  evaluation mode can turn itself on later.
- **Legal review before the first public release.** No review is recorded
  yet. It covers:
  - Arcane's BSL 1.1 parameters and change dates;
  - whether a preview (such as `0.4.0-preview.2`) counts as the "first public
    distribution" that starts `0.4.0`'s four-year clock. Previews are public
    downloads, so the safe reading is yes;
  - the third-party notices (s7.6), with FreeType's acknowledgment and
    Microsoft's terms for the Agility SDK and DXC.
- **A lost key strands installed Hubs.** A Hub trusts only the manifest keys
  it embeds and the Tauri key it was built with. Lose the minisign key and
  installed Hubs reject every new manifest; lose the Tauri key and they can
  never self-update again. Both keys have offline backups; rotation ships the
  new key in a Hub release before the old one retires.
- **R2 cache rules.** Cloudflare caches by extension: `.zip`, `.gz` and `.exe`
  are cached by default, `.json` and `.AppImage` are not. Explicit Cache Rules
  give the archives (and `.AppImage`) a long TTL; the manifest pair (`.json`,
  `.minisig`) bypasses the cache, since a cached manifest beside a fresh
  signature (or the reverse) would fail every Hub's check. On the Free plan files over 512 MB are not cached; they are still
  served from R2.
- **aws-cli checksums with R2.** aws-cli 2.23 and later calculate request
  checksums by default (`WHEN_SUPPORTED`), which R2 does not fully support.
  The release workflow sets `AWS_REQUEST_CHECKSUM_CALCULATION=WHEN_REQUIRED`
  (and `AWS_RESPONSE_CHECKSUM_VALIDATION=WHEN_REQUIRED`), and pins the
  aws-cli version.
- **Wrangler's 315 MB cap.** `wrangler r2 object put` cannot upload objects
  above 315 MB, and the SDK plus symbols archives can exceed it. CI uploads
  through the S3 API only.
- **BSL date evidence.** Each version's change date runs from its first public
  distribution, so the date must be provable. Three independent records back
  `released_at`: the git tag, the GitHub Release timestamp, and the R2 upload
  time. `released_at` is **never rewritten**, not by the yank workflow, not by
  a 30-day re-sign, not by a fix to the notes. Until the legal review answers
  the preview question, `change_date` for a stable version is computed from its
  **first preview's** `released_at` when it had one. If that reading is wrong,
  the result is an earlier MIT date, which errs toward the user.
- **The modularization dependency.** Phase 1 cannot start until P6/P7 land;
  if the module layout changes in its wholesale test, the SDK contents
  follow it (s7.1 is stated in terms of "every descriptor" and the module graph
  for that reason).

## 13. Sources

Research from 2026-10-12.
- Cloudflare R2: [pricing](https://developers.cloudflare.com/r2/pricing/),
  [public buckets and custom domains](https://developers.cloudflare.com/r2/buckets/public-buckets/),
  [limits](https://developers.cloudflare.com/r2/platform/limits/),
  [aws-cli with R2](https://developers.cloudflare.com/r2/examples/aws/aws-cli/),
  [S3 API compatibility](https://developers.cloudflare.com/r2/api/s3/api/),
  [Wrangler R2 commands](https://developers.cloudflare.com/workers/wrangler/commands/r2/).
- Cloudflare cache: [default cache behavior](https://developers.cloudflare.com/cache/concepts/default-cache-behavior/),
  [Cache Rules](https://developers.cloudflare.com/cache/how-to/cache-rules/).
- Cloudflare Pages: [limits](https://developers.cloudflare.com/pages/platform/limits/),
  [deploy hooks](https://developers.cloudflare.com/pages/configuration/deploy-hooks/).
- AWS: [data integrity protections, `AWS_REQUEST_CHECKSUM_CALCULATION`](https://docs.aws.amazon.com/sdkref/latest/guide/feature-dataintegrity.html).
- Tauri: [updater plugin](https://v2.tauri.app/plugin/updater/),
  [updater changelog](https://github.com/tauri-apps/plugins-workspace/blob/v2/plugins/updater/CHANGELOG.md) (2.12.0, 2.13.3),
  [Windows installer](https://v2.tauri.app/distribute/windows-installer/).
- minisign: [jedisct1.github.io/minisign](https://jedisct1.github.io/minisign/).
- Microsoft: [Artifact Signing](https://learn.microsoft.com/en-us/azure/artifact-signing/overview),
  [Smart App Control](https://support.microsoft.com/en-us/topic/what-is-smart-app-control-285ea03d-fa88-4d56-882e-6698afdb7003),
  [Q&A: signed binaries still blocked on reputation](https://learn.microsoft.com/en-us/answers/questions/5791210/application-signed-and-blocked-by-smart-app-contro);
  field reports of SAC blocking freshly built or unsigned native binaries:
  [rebuilt executables](https://blog.devgenius.io/smart-app-control-may-blocked-your-app-the-fix-was-to-stop-rebuilding-it-5676f69ec3bc),
  [an unsigned DLL in a signed tool](https://abp.io/support/questions/10365/ABP-Studio-Blocked-by-Windows-Smart-App-Control-and-Cannot-Launch-on-Windows-11),
  [an unsigned native addon](https://discuss.ai.google.dev/t/bug-smart-app-control-blocks-vscode-policy-watcher-node-part-of-this-app-was-blocked-windows-11-ide-2-5-5/178683),
  [Azure login with OpenID Connect](https://learn.microsoft.com/en-us/azure/developer/github/connect-from-azure-openid-connect),
  [`azure/login`](https://github.com/Azure/login).
- GitHub: [security hardening for Actions](https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions),
  [runner images](https://github.com/actions/runner-images).
- Steam Linux Runtime: [sniper SDK](https://gitlab.steamos.cloud/steamrt/sniper/sdk).
- Business Source License 1.1: [mariadb.com/bsl11](https://mariadb.com/bsl11/); Arcane's
  parameters in `LICENSE` and `LICENSING.md`.
- Launcher precedents (Unity Hub, Epic, Godot, rustup, Python install manager):
  summarised in the brainstorm's research B; the design adopts their per-user
  default, signed manifest with per-file hashes, Verify/Repair, explicit
  Locate for dev builds and an exact project pin.
