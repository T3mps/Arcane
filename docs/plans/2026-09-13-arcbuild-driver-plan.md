# arcbuild — the game-project build driver — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One executable, `arcbuild.exe`, that generates / builds / rebuilds / cleans / probes a game project against an Arcane SDK — carrying the incremental rule (spec §4.3) so a wizard-made component costs one compile and a link — and make the editor's `ModuleBuild`, the Gacha scripts and the Gacha Jenkinsfile all call it instead of carrying their own premake+msbuild copy.

**Architecture:** Three units. (1) `Arcane::Toolchain` in **ArcaneCore** — the one home for "where are premake / msbuild / devenv on this machine" and "which workspace file does this root carry" (`DiscoverSolution`), consumed by the driver and by the editor's `IdeLaunch`. (2) The driver's **pure core** `arcbuild/src/Driver.{hpp,cpp}` — command/flag parsing over `Arcane::Cli`, the `--sdk`/`ARCANE_SDK` precedence, the §4.3 decision table as a pure function, composition of every child command line, exit-code mapping — source-compiled into ArcaneTests exactly as the editor's pure TUs are. (3) `arcbuild/src/main.cpp` — the only place that touches a process or a PE file: loads the manifest through `Project::ResolveManifestFile` + `ProjectManifest::LoadFile`, probes the slot with `Module::ScanFileCrtFlavor` (never a second scanner), runs each child through `_wpopen` and re-emits its lines with a `[premake]` / `[msbuild]` prefix. The editor's `ModuleBuild` shrinks to: resolve `arcbuild.exe` beside the editor (else `../arcbuild/`), compose `( "<arcbuild>" <cmd> --project … --config … --sdk … ) 2>&1`, and stream it through the unchanged `Runner` / `RunCapture`.

**Tech Stack:** C++23, `Arcane::Cli` (ArcaneCore), `Arcane::Module::ScanFileCrtFlavor` + `Arcane::Project`/`ProjectManifest` (ArcaneClient, `ARCANE_API`), premake5 (vs2026 action) + MSBuild (VS 18), Catch2, PowerShell 5.1 (Gacha `scripts/setup.ps1`), Jenkins declarative pipeline.

**Spec:** `docs/specs/2026-09-13-arcbuild-driver-design.md` (committed `05d8d63d`). §3 CLI, §4.1 resolution, §4.2 generate, §4.3 the incremental rule, §4.4 clean, §5 consumers, §7 testing, §8 rollout. The memory `project_arcane_arcbuild_driver_arc` carries the user rulings the spec folds in (premake stays; v1 = game projects only; auto-build-after-Create is a future opt-in setting — **not wired here**).

## Global Constraints

- **No ABI bump, no Gacha restamp.** Nothing under `ArcaneClient/src` changes (the driver only *calls* `Module::ScanFileCrtFlavor`, `Project::ResolveManifestFile`, `ProjectManifest::LoadFile` — all already `ARCANE_API`). ArcaneCore gains a new TU (`Arcane/Build/Toolchain.cpp`) — a static lib no game module includes; `kGamePluginABIVersion` stays 28. Task 7 proves it: `git diff --stat <plan-start>..HEAD -- ArcaneClient ThirdParty/Astra` is empty.
- **Build ritual (bash tool):** msbuild = `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m` — dash forms (`/p:` is path-mangled by Git Bash); `-t:<project>` does NOT work on `.slnx`, build the whole solution. Regenerate with `cmd /c ".\GenerateProjects.bat"` (PowerShell tool) whenever `premake5.lua` changes or a NEW `.cpp` is added anywhere the file list is explicit (ArcaneTests lists engine/editor TUs explicitly; its own `src/**` is globbed but the generated vcxproj is a snapshot — regenerate on every new file). Tests run **FROM** `bin/<cfg>-windows-x86_64-md/ArcaneTests/` with filters like `"[build]"`. An interrupted msbuild leaves a corrupt `.obj` (LNK1136) — delete it under `bin-int/` and rebuild. An engine rebuild ⇒ rebuild `ReferenceProject.slnx` (`-t:Rebuild`) before any host launch or the plugin refuses to load. The desk's running editor is usually the **Release** build — a Release `ArcaneEditor.exe` link fails (LNK1104) while it is open; report it, do not fight it.
- **Output hygiene:** `msbuild … -v:m > log; grep -E 'error|Warning\(s\)|Error\(s\)'`; `ArcaneTests.exe "<filter>" | grep -E 'seeded|test cases|All tests passed|FAILED'`; never `cat` a suite log.
- **TDD, every task:** the failing test is written and RUN RED before the production change it drives; the step lists the expected failure text. Where the only honest RED is "the exe does not exist yet" (Task 3), that is the RED.
- **Spawn rule:** unit tests (`[build]`, `[editor]`) never create a process. Process-touching paths are reached only by the opt-in `[build-desk]` cases (SKIP unless `ARCANE_BUILD_DESK=<abs project dir>`), the same shape as `[ide-desk]`.
- **Baseline:** `scripts/automation-baselines.json` is booked at Task 7 from **measured** `~[gpu]` runs (Debug + Release), never from recalled numbers. Every case this plan adds is device-less (`[build]`, `[editor]`, `[build-desk]`-SKIP); none is `[gpu]`.
- **Golden lanes:** untouched by construction (nothing renders differently), but Task 7 runs `golden-gate.ps1` in **both** configs anyway — 4/4 each.
- **Git:** Arcane `main` is 36 ahead of origin, Gacha 1 ahead — **do not push**. One commit per task (Task 6 commits in the Gacha repo). Trailers on every commit:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
  Never stage `out.txt`, `ArcaneEditor/ArcaneEditor/`, `ArcaneAssetPipeline/ArcaneAs.*/` (untracked junk that is not ours) in Arcane, nor `Game/Source/TestComponent.*` / `Game/Content/scenes/test.arcscene` (the user's in-flight edits) in Gacha.
- **Ledger:** `.superpowers/sdd/2026-09-13-arcbuild-driver/progress.md` — `Task N: complete (sha)` lines, every ruling inline, measured counts per task.

---

## Plan-time rulings (spec amendments, each with its reason)

| # | Ruling | Why |
|---|---|---|
| R1 | **`VsWhere` / `ResolveMsBuild` / `ResolvePremake` / `DiscoverSolution` move to ArcaneCore as `Arcane::Toolchain` (`ArcaneCore/src/Arcane/Build/Toolchain.{hpp,cpp}`), plus a new `ResolveDevenv`.** The spec says the resolution "leaves the editor" for the driver; it does not say where. | `IdeLaunch::ResolveDevenv` still needs the vswhere probe after ModuleBuild loses it, and `OpenInIde` still needs `DiscoverSolution` to know which `.slnx` to hand devenv. One unit both exes link ("one probe, two questions" — ModuleBuild.hpp's own comment) beats a copy in each. ArcaneCore is game-agnostic and already hosts `Arcane::Cli` for the same reason; the TU is std + `_WIN32`-guarded `_wpopen`, harmless in the Server workspace's static-CRT compile. |
| R2 | **The driver runs premake and msbuild as two separate `_wpopen` children**, not one `&&` chain. | §3 requires per-child line prefixes (`[premake]` / `[msbuild]`) and "the first failing child's" exit code; a single chain cannot attribute either. Each child keeps the `( … ) 2>&1` shape so its stderr folds in. |
| R3 | **The msbuild step does not `cd`; the solution path is absolute.** `main.cpp` absolutises the project root (`std::filesystem::absolute`) before anything is composed. | `arcbuild build --project Game` from a CI workspace is a relative path; premake needs `cd /d "<root>"` (it reads `./premake5.lua`), msbuild does not, and a relative `.slnx` after a `cd` would be wrong. |
| R4 | **`probe` exits 0 on the two "plain build" rows (absent / match) and 3 on the two "/t:Rebuild" rows (mismatch / unreadable).** | §5.3 uses `probe` as CI's nothing-stale check right after a same-config build: 0 = the slot holds what that build should have left. 3 is chosen because 2 is the driver-refusal code (§3) and 1 is what premake/msbuild return. |
| R5 | **`--quiet` suppresses the driver's own informational `[arcbuild]` lines only.** Child lines and `[arcbuild] error:` lines always print. | §3 lists the flag without defining it; this is the smallest meaning that is not a no-op. |
| R6 | **Every driver line goes to stdout, flushed per line.** | The editor reads the driver through a pipe; a block-buffered stdout would deliver everything at exit instead of streaming. Refusals are spelled `[arcbuild] error: …` so the editor's severity lane catches them (Task 5 extends its rule with `"] error:"` / `"] Error:"` — premake's own `Error:` lines now arrive prefixed). |
| R7 | **A content-only project (empty `gameModule`) probes as `Absent`.** | Nothing to be wrong about; `build` still runs premake+msbuild (the solution may have nothing to build, which msbuild reports itself). |
| R8 | **"No msbuild" is not a driver refusal in v1.** `ResolveMsBuild` keeps the bare `"msbuild"` PATH fallback; a missing msbuild surfaces as cmd's 9009 passed through. | Same as today's ModuleBuild; detecting it would need a probe spawn per run. §3's "no msbuild ⇒ 2" is recorded as a follow-up, not silently dropped. |
| R9 | **The editor keeps `Configuration()` / `ExeDir()` / `SdkRootFromExeDir()` / `RunCapture` / `Runner`; loses `DiscoverSolution`, `ComposeInputs`, `ComposeRebuildCommands`, `ComposeGenerateCommand`, `ResolvePremake`, `VsWhere`, `ResolveMsBuild`, `SetSdkEnv`.** Gains `DriverCandidates`, `ResolveDriver`, `DriverInputs`, `ComposeDriverCommand`. | Spec §5.1 verbatim, made concrete. `SetSdkEnv` goes because the editor now passes `--sdk` and the driver sets the variable for its own children. |
| R10 | **The product proof is CLI-driven.** The editor spawns exactly the line `ComposeDriverCommand` produces (pinned by an `[editor]` test), so running that line by hand on the Gacha Game after touching the wizard-made `TestComponent.cpp` and counting msbuild's compile/link lines IS the Console evidence. The click itself stays desk-verify (no host automation flag drives a menu item). | Honest instrument; no scope creep into a `--rebuild-module` automation flag. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `ArcaneCore/src/Arcane/Build/Toolchain.hpp` / `.cpp` | Create (Task 1) | `DiscoverSolution`, `ResolvePremake`, `VsWhere`, `ResolveMsBuild`, `ResolveDevenv` — moved from ModuleBuild, plus devenv. |
| `ArcaneTests/src/ToolchainTest.cpp` | Create (Task 1) | `[build]` — discovery + premake resolution over temp dirs. |
| `arcbuild/src/Driver.hpp` / `.cpp` | Create (Task 2) | The pure core: `Command`, `Request`, `MakeCli`/`RequestFromCli`, `ResolveSdk`, `SlotState`/`ClassifySlot`/`Decide`/`ProbeExitCode`, `Layout`/`SlotPath`/`SolutionPath`, `ComposeGenerate`/`ComposeMsBuild`/`CleanTargets`, `ExitFromChild`. |
| `ArcaneTests/src/BuildDriverTest.cpp` | Create (Task 2) | `[build]` — the whole core; Task 3 appends the `[build-desk]` cases. |
| `premake5.lua` | Modify (Task 2: ArcaneTests `files` + `includedirs`; Task 3: `project "arcbuild"`; Task 5: ModuleBuild entry comment) | Test-exe file list; the new ConsoleApp beside `arccook`. |
| `arcbuild/src/main.cpp` | Create (Task 3) | Manifest load, SDK env, resolution, the slot probe, spawn-and-stream, exit codes. |
| `ArcaneEditor/src/Project/ModuleBuild.hpp` / `.cpp` | Rewrite (Task 5) | Per R9. |
| `ArcaneEditor/src/Project/IdeLaunch.cpp` | Modify (Task 5) | `ResolveDevenv` → `Arcane::Toolchain::ResolveDevenv()`. |
| `ArcaneEditor/src/App/EditorAppProject.cpp` | Modify (Task 5) | `StartModuleRebuild`, `OpenInIde` (DiscoverSolution), `RegenerateSolution`, `PollModuleBuild` (severity rule). |
| `ArcaneEditor/src/App/EditorApp.hpp` | Modify (Task 5, comments) | The Build section describes the spawn. |
| `ArcaneTests/src/ModuleBuildTest.cpp` | Rewrite (Task 5) | `[editor]` — candidates + the pinned driver line + `Configuration()`. |
| `Gacha: Game/premake5.lua` | Modify (Task 6, header comment) | Points at `arcbuild`. |
| `Gacha: scripts/setup.ps1` | Modify (Task 6) | `Find-ArcBuild`; `game-generate` and the Game half of `build` call it. |
| `Gacha: Jenkinsfile` | Modify (Task 6) | `Game (vs ARCANE_SDK)` stage: build Debug, build Release, probe Release. |
| `Gacha: CLAUDE.md` | Modify (Task 6) | The Game build snippet points at `arcbuild`. |
| `scripts/automation-baselines.json` | Modify (Task 7) | Book the measured rise. |
| `docs/specs/2026-09-13-arcbuild-driver-design.md` | Modify (Task 7, status line + R1/R4 pointers) | Status: implemented; §4.1 and §3 gain one-line pointers to R1 and R4. |
| `docs/plans/2026-09-13-arcbuild-driver-plan.md` | Modify (Task 7) | `## Closeout`. |

---

### Task 1: `Arcane::Toolchain` in ArcaneCore

**Files:**
- Create: `ArcaneCore/src/Arcane/Build/Toolchain.hpp`
- Create: `ArcaneCore/src/Arcane/Build/Toolchain.cpp`
- Create: `ArcaneTests/src/ToolchainTest.cpp`

**Interfaces:**
- Consumes: nothing new. The bodies are `ArcaneEditor/src/Project/ModuleBuild.cpp:59-78` (DiscoverSolution), `:169-177` (ResolvePremake), `:179-219` (VsWhere), `:221-228` (ResolveMsBuild) — moved, not rewritten. ModuleBuild keeps its copies until Task 5 deletes them (four tasks of duplication, by the spec's rollout order — driver first).
- Produces (used by Tasks 3 and 5):
  ```cpp
  namespace Arcane::Toolchain {
      std::filesystem::path DiscoverSolution(const std::filesystem::path& projectRoot);
      std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot);
      std::filesystem::path VsWhere(const std::string& arguments);
      std::filesystem::path ResolveMsBuild();
      std::filesystem::path ResolveDevenv();
  }
  ```

- [ ] **Step 1: Write the failing test** — create `ArcaneTests/src/ToolchainTest.cpp`:

```cpp
// Arcane::Toolchain's PURE-ENOUGH halves ([build]): workspace-file discovery
// and the bundled-premake lookup, both over a real temp directory. The
// vswhere-backed lookups (ResolveMsBuild / ResolveDevenv / VsWhere) spawn
// vswhere.exe and are desk-verify territory -- the same "no spawn test"
// split ModuleBuild and RuntimeLaunch draw.

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Build/Toolchain.hpp>

namespace
{
    namespace fs = std::filesystem;

    // Unique temp dir per SECTION run; removed by the guard so a failing
    // assertion cannot strand files for the next run to trip on.
    struct TempDir
    {
        fs::path path;
        explicit TempDir(const char* tag)
        {
            path = fs::temp_directory_path() /
                   (std::string("arcane_toolchain_") + tag + "_" +
                    std::to_string(static_cast<unsigned>(
                        std::hash<const void*>{}(this))));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void Touch(const fs::path& p)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p.string()) << "x";
    }
}

TEST_CASE("Toolchain::DiscoverSolution prefers .slnx over .sln, first lexicographic within a bucket",
          "[build]")
{
    TempDir dir("discover");
    Touch(dir.path / "Zeta.sln");
    Touch(dir.path / "beta.slnx");
    Touch(dir.path / "Alpha.slnx");
    Touch(dir.path / "notes.txt");

    CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Alpha.slnx");
}

TEST_CASE("Toolchain::DiscoverSolution falls back to .sln, and to empty when neither exists",
          "[build]")
{
    TempDir dir("fallback");
    SECTION("only a .sln")
    {
        Touch(dir.path / "Game.sln");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Game.sln");
    }
    SECTION("a hand-generated upper-case extension still counts")
    {
        Touch(dir.path / "Game.SLNX");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).filename() == "Game.SLNX");
    }
    SECTION("neither")
    {
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).empty());
    }
    SECTION("a directory that does not exist")
    {
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path / "missing").empty());
    }
    SECTION("non-recursive: a vendor solution one level down is not ours")
    {
        Touch(dir.path / "ThirdParty" / "vendor.slnx");
        CHECK(Arcane::Toolchain::DiscoverSolution(dir.path).empty());
    }
}

TEST_CASE("Toolchain::ResolvePremake returns the SDK's bundled premake, else the PATH name", "[build]")
{
    TempDir sdk("premake");
    SECTION("bundled copy present")
    {
        Touch(sdk.path / "ThirdParty" / "premake5" / "premake5.exe");
        const fs::path found = Arcane::Toolchain::ResolvePremake(sdk.path);
        CHECK(found.filename() == "premake5.exe");
        CHECK(found.is_absolute());
        // lexically_normal'd: no "." / ".." elements survive.
        CHECK(found == found.lexically_normal());
    }
    SECTION("bundled copy absent -> bare name for cmd's own PATH resolution")
    {
        CHECK(Arcane::Toolchain::ResolvePremake(sdk.path) == fs::path("premake5"));
    }
}
```

- [ ] **Step 2: Regenerate and build; verify RED**

Run (PowerShell): `cd D:\dev\starworks\Arcane; cmd /c ".\GenerateProjects.bat"`
Run (bash): `cd /d/dev/starworks/Arcane && MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=Debug -m -nologo -v:m > /tmp/t1red.log 2>&1; grep -E "error C1083|Error\(s\)" /tmp/t1red.log | head`
Expected: `ToolchainTest.cpp(…): fatal error C1083: Cannot open include file: 'Arcane/Build/Toolchain.hpp'` — RED.

- [ ] **Step 3: Write the header** — create `ArcaneCore/src/Arcane/Build/Toolchain.hpp`:

```cpp
#pragma once

// Arcane::Toolchain -- where the build tools live on THIS machine, and which
// generated workspace file a project root carries. The ONE home for the
// vswhere probe and the premake / msbuild / devenv lookups: the editor's
// ModuleBuild carried them until the arcbuild driver arrived (spec
// docs/specs/2026-09-13-arcbuild-driver-design.md, s4.1 -- plan ruling R1),
// and both arcbuild.exe (premake + msbuild) and the editor's IdeLaunch
// (devenv) now consume this, so there is exactly one answer to "which Visual
// Studio". Presentation-free, std + Win32 only; ArcaneCore is also compiled
// into the Server workspace, where nothing here is called.
//
// DiscoverSolution and ResolvePremake are pure enough to unit-test against a
// temp directory ([build], ToolchainTest.cpp). VsWhere / ResolveMsBuild /
// ResolveDevenv spawn vswhere.exe and are desk-verify territory.

#include <filesystem>
#include <string>

namespace Arcane::Toolchain
{
    // The generated workspace file a build drives: the first *.slnx in
    // `projectRoot` (lexicographic, for determinism), else the first *.sln,
    // else empty. Extension compare is ASCII case-insensitive (a hand-
    // generated "Game.SLNX" is still the workspace file). Non-recursive on
    // purpose -- the committed convention puts the premake workspace file in
    // the project root (Aphelyon.slnx beside Aphelyon.arcproj), and a
    // recursive scan would find ThirdParty/vendor solutions that are not
    // ours to build.
    std::filesystem::path DiscoverSolution(const std::filesystem::path& projectRoot);

    // The engine's bundled premake: <sdkRoot>/ThirdParty/premake5/premake5.exe
    // (the repo layout build/arcane.lua documents), lexically normalised,
    // falling back to bare "premake5" (PATH) when the bundled copy is not
    // there -- a packaged SDK may ship it elsewhere, and cmd's own resolution
    // is the honest fallback.
    std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot);

    // The one VS-install-aware query Microsoft documents: run
    // %ProgramFiles(x86)%/Microsoft Visual Studio/Installer/vswhere.exe with
    // `arguments` and return the FIRST line it prints (a path), or empty when
    // vswhere is absent or found nothing. Shared by the two lookups below --
    // one probe, two questions. Windows-only; always empty elsewhere.
    std::filesystem::path VsWhere(const std::string& arguments);

    // MSBuild via VsWhere:
    //   vswhere -latest -requires Microsoft.Component.MSBuild
    //           -find MSBuild\**\Bin\MSBuild.exe
    // falling back to bare "msbuild" (PATH -- a Developer Command Prompt).
    std::filesystem::path ResolveMsBuild();

    // devenv.exe via VsWhere: vswhere -latest -find Common7\IDE\devenv.exe.
    // Empty when no Visual Studio install is found (the editor greys its
    // Open Visual Studio item on that).
    std::filesystem::path ResolveDevenv();
}
```

- [ ] **Step 4: Write the implementation** — create `ArcaneCore/src/Arcane/Build/Toolchain.cpp` (the four bodies are ModuleBuild.cpp's, verbatim, plus the devenv one-liner):

```cpp
#include <Arcane/Build/Toolchain.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Arcane::Toolchain
{
    namespace
    {
        // ASCII-lowercased extension: a hand-generated "Game.SLNX" is still
        // the workspace file.
        std::string LowerExt(const std::filesystem::path& p)
        {
            std::string ext = p.extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return ext;
        }

#ifdef _WIN32
        // UTF-8 -> UTF-16 for the _wpopen boundary (install paths may be
        // non-ANSI).
        std::wstring Widen(const std::string& utf8)
        {
            if (utf8.empty())
                return {};
            const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                                static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(n), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                  wide.data(), n);
            return wide;
        }
#endif
    }

    std::filesystem::path DiscoverSolution(const std::filesystem::path& projectRoot)
    {
        std::vector<std::filesystem::path> slnx, sln;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(projectRoot, ec))
        {
            if (!entry.is_regular_file(ec))
                continue;
            const std::string ext = LowerExt(entry.path());
            if (ext == ".slnx")     slnx.push_back(entry.path());
            else if (ext == ".sln") sln.push_back(entry.path());
        }
        // Lexicographic within each bucket: directory_iterator order is
        // unspecified, and "first" must mean the same file every build.
        std::sort(slnx.begin(), slnx.end());
        std::sort(sln.begin(), sln.end());
        if (!slnx.empty()) return slnx.front();
        if (!sln.empty())  return sln.front();
        return {};
    }

    std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot)
    {
        const std::filesystem::path bundled =
            sdkRoot / "ThirdParty" / "premake5" / "premake5.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(bundled, ec))
            return bundled.lexically_normal();
        return "premake5";   // PATH fallback (cmd resolves it)
    }

    std::filesystem::path VsWhere(const std::string& arguments)
    {
#ifdef _WIN32
        // vswhere is the one install-location contract VS actually documents:
        // it always lives under %ProgramFiles(x86)%/Microsoft Visual Studio/
        // Installer once any VS >= 15.2 is present.
        const char* pf86 = std::getenv("ProgramFiles(x86)");
        if (pf86)
        {
            const std::filesystem::path vswhere =
                std::filesystem::path(pf86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
            std::error_code ec;
            if (std::filesystem::is_regular_file(vswhere, ec))
            {
                std::string query = "\"";
                query += vswhere.string();
                query += "\" ";
                query += arguments;
                // A quoted exe at the head of a bare _popen line loses its
                // quotes to cmd's outer-quote stripping; the standard dodge is
                // one extra wrapping pair.
                query = "\"" + query + "\"";
                if (FILE* pipe = ::_wpopen(Widen(query).c_str(), L"r"))
                {
                    char line[1024] = {};
                    std::string first;
                    if (std::fgets(line, sizeof(line), pipe))
                        first = line;
                    ::_pclose(pipe);
                    while (!first.empty() && (first.back() == '\n' || first.back() == '\r'))
                        first.pop_back();
                    if (!first.empty())
                        return std::filesystem::path(first);
                }
            }
        }
#else
        (void)arguments;
#endif
        return {};
    }

    std::filesystem::path ResolveMsBuild()
    {
        const std::filesystem::path found =
            VsWhere("-latest -requires Microsoft.Component.MSBuild -find MSBuild\\**\\Bin\\MSBuild.exe");
        if (!found.empty())
            return found;
        return "msbuild";   // PATH fallback (a Developer Command Prompt launch)
    }

    std::filesystem::path ResolveDevenv()
    {
        return VsWhere("-latest -find Common7\\IDE\\devenv.exe");
    }
}
```

- [ ] **Step 5: Regenerate (new ArcaneCore TU — its `files` is a glob, but the vcxproj is a snapshot), build, run GREEN**

Run (PowerShell): `cd D:\dev\starworks\Arcane; cmd /c ".\GenerateProjects.bat"`
Run (bash): build as in Step 2 (Debug), then
`cd /d/dev/starworks/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[build]" | grep -E "seeded|test cases|All tests passed|FAILED"`
Expected: `All tests passed (N assertions in 3 test cases)`.

- [ ] **Step 6: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add ArcaneCore/src/Arcane/Build/Toolchain.hpp ArcaneCore/src/Arcane/Build/Toolchain.cpp ArcaneTests/src/ToolchainTest.cpp && git commit -q -F - <<'EOF'
feat(core): Arcane::Toolchain -- DiscoverSolution / ResolvePremake / VsWhere / ResolveMsBuild / ResolveDevenv, the one home for the build-tool probes (arcbuild Task 1)

Moved out of the editor's ModuleBuild (which keeps its copies until the
editor shim lands) so arcbuild.exe and IdeLaunch share one vswhere probe.
Plan ruling R1 in docs/plans/2026-09-13-arcbuild-driver-plan.md.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 2: The driver's pure core (`arcbuild/src/Driver.{hpp,cpp}`) + `[build]` tests

**Files:**
- Create: `arcbuild/src/Driver.hpp`
- Create: `arcbuild/src/Driver.cpp`
- Create: `ArcaneTests/src/BuildDriverTest.cpp`
- Modify: `premake5.lua` — `project "ArcaneTests"` `files {}` (after the `ClassTemplates.cpp` entry, ~:844) and `includedirs {}` (~:945-971)

**Interfaces:**
- Consumes: `Arcane::Cli` (`ArcaneCore/src/Arcane/Cli/Cli.hpp` — `Option(name, default, help)`, `Flag(name, help)`, `Parse(argc, argv)` starts at `argv[1]`, `Result::Get/Flag/Supplied`); `Arcane::CrtFlavor { Unknown, Release, Debug }` (`ArcaneClient/src/Arcane/Plugin/Module.hpp:22` — header-only enum, no link needed).
- Produces (used by Task 3's `main.cpp` and Task 3's desk tests):

```cpp
namespace arcbuild
{
    enum class Command : std::uint8_t { Generate, Build, Rebuild, Clean, Probe };
    std::optional<Command> ParseCommand(std::string_view word);
    const char* CommandName(Command c);

    struct Request
    {
        Command                              command = Command::Build;
        std::filesystem::path                project;          // --project, as given (dir or .arcproj)
        std::string                          config = "Debug"; // --config
        std::optional<std::filesystem::path> sdk;              // --sdk when supplied
        std::string                          action = "vs2026";// --action
        bool                                 forceRebuild = false;
        bool                                 quiet = false;
    };
    Arcane::Cli MakeCli();
    Request RequestFromCli(Command command, const Arcane::Cli::Result& r);

    std::optional<std::filesystem::path> ResolveSdk(const std::optional<std::filesystem::path>& flag,
                                                    const char* envValue);

    bool ConfigWantsDebugCrt(std::string_view config);   // Debug -> true; Release/Dist -> false

    enum class SlotState : std::uint8_t { Absent, Match, Mismatch, Unreadable };
    const char* SlotStateName(SlotState s);
    SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config);

    struct Verdict { bool rebuild; const char* reason; };
    Verdict Decide(Command command, bool forceRebuild, SlotState slot);

    constexpr int kExitOk = 0;
    constexpr int kExitRefused = 2;
    constexpr int kExitProbeRebuild = 3;
    int ProbeExitCode(SlotState s);
    int ExitFromChild(std::optional<int> childExit);   // nullopt (pipe failed) -> kExitRefused

    struct Layout
    {
        std::filesystem::path root;        // absolute project directory
        std::filesystem::path manifest;    // the .arcproj
        std::string           name;        // manifest name  -> <name>.slnx convention
        std::string           gameModule;  // manifest gameModule (may be empty)
    };
    std::filesystem::path SlotPath(const Layout& l);      // root/Binaries/<gameModule>, empty when no module
    std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered);

    struct Tools { std::filesystem::path premake; std::filesystem::path msbuild; };
    std::string ComposeGenerate(const Layout& l, const Tools& t, std::string_view action);
    enum class MsBuildTarget : std::uint8_t { Build, Rebuild, Clean };
    std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution,
                               std::string_view config, MsBuildTarget target);
    std::vector<std::filesystem::path> CleanTargets(const Layout& l, std::string_view config);
}
```

- [ ] **Step 1: Write the failing tests** — create `ArcaneTests/src/BuildDriverTest.cpp`:

```cpp
// arcbuild's PURE core ([build]): command + flag parsing over Arcane::Cli,
// the --sdk / ARCANE_SDK precedence, the s4.3 decision table, exit-code
// mapping, path conventions and every composed child command line. Nothing
// here spawns a process or reads a PE file -- main.cpp does both, and the
// opt-in [build-desk] cases at the bottom (Task 3) are the only tests that
// reach it, SKIPping unless ARCANE_BUILD_DESK names a project directory.

#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Driver.hpp>

namespace
{
    namespace fs = std::filesystem;
    using namespace arcbuild;

    // argv for Cli::Parse: [0] is the "program" slot Parse skips (main.cpp
    // hands it argv+1, so [0] is the command word there).
    struct Argv
    {
        std::vector<std::string> storage;
        std::vector<char*>       ptrs;
        explicit Argv(std::initializer_list<const char*> words)
        {
            for (const char* w : words) storage.emplace_back(w);
            for (std::string& s : storage) ptrs.push_back(s.data());
        }
        int    argc() const { return static_cast<int>(ptrs.size()); }
        char** argv()       { return ptrs.data(); }
    };

    Layout AphelyonLayout()
    {
        Layout l;
        l.root       = "D:/dev/starworks/Gacha/Game";
        l.manifest   = "D:/dev/starworks/Gacha/Game/Aphelyon.arcproj";
        l.name       = "Aphelyon";
        l.gameModule = "Aphelyon.dll";
        return l;
    }

    Tools SdkTools()
    {
        return { "D:/dev/starworks/Arcane/ThirdParty/premake5/premake5.exe",
                 "C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" };
    }
}

// ---- CLI -------------------------------------------------------------------

TEST_CASE("arcbuild::ParseCommand knows the five commands and nothing else", "[build]")
{
    CHECK(ParseCommand("generate") == Command::Generate);
    CHECK(ParseCommand("build")    == Command::Build);
    CHECK(ParseCommand("rebuild")  == Command::Rebuild);
    CHECK(ParseCommand("clean")    == Command::Clean);
    CHECK(ParseCommand("probe")    == Command::Probe);
    CHECK_FALSE(ParseCommand("Build").has_value());     // exact, lower-case
    CHECK_FALSE(ParseCommand("--project").has_value()); // a flag is not a command
    CHECK_FALSE(ParseCommand("").has_value());
    // Round-trip: the name printed in the log is the word that parses.
    for (Command c : { Command::Generate, Command::Build, Command::Rebuild, Command::Clean, Command::Probe })
        CHECK(ParseCommand(CommandName(c)) == c);
}

TEST_CASE("arcbuild::MakeCli + RequestFromCli carry every flag of spec s3", "[build]")
{
    Arcane::Cli cli = MakeCli();
    SECTION("all flags supplied")
    {
        Argv a{ "build", "--project", "D:/dev/starworks/Gacha/Game", "--config", "Release",
                "--sdk", "D:/dev/starworks/Arcane", "--action", "gmake2", "--force-rebuild", "--quiet" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Build, r);
        CHECK(req.command == Command::Build);
        CHECK(req.project == fs::path("D:/dev/starworks/Gacha/Game"));
        CHECK(req.config  == "Release");
        REQUIRE(req.sdk.has_value());
        CHECK(*req.sdk == fs::path("D:/dev/starworks/Arcane"));
        CHECK(req.action == "gmake2");
        CHECK(req.forceRebuild);
        CHECK(req.quiet);
    }
    SECTION("defaults: Debug, vs2026, no sdk, no force, not quiet")
    {
        Argv a{ "probe", "--project", "X" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        REQUIRE(r.ok);
        const Request req = RequestFromCli(Command::Probe, r);
        CHECK(req.config == "Debug");
        CHECK(req.action == "vs2026");
        CHECK_FALSE(req.sdk.has_value());
        CHECK_FALSE(req.forceRebuild);
        CHECK_FALSE(req.quiet);
    }
    SECTION("--project is required")
    {
        Argv a{ "build" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        CHECK_FALSE(r.ok);
        CHECK(r.exitCode == kExitRefused);
    }
    SECTION("--config is validated against the three configurations")
    {
        Argv a{ "build", "--project", "X", "--config", "Shipping" };
        const Arcane::Cli::Result r = cli.Parse(a.argc(), a.argv());
        CHECK_FALSE(r.ok);
    }
}

// ---- SDK precedence ---------------------------------------------------------

TEST_CASE("arcbuild::ResolveSdk: --sdk beats ARCANE_SDK beats refusal", "[build]")
{
    const fs::path flag = "D:/flag/sdk";
    CHECK(ResolveSdk(flag, "D:/env/sdk") == flag);
    CHECK(ResolveSdk(flag, nullptr)      == flag);
    CHECK(ResolveSdk(std::nullopt, "D:/env/sdk") == fs::path("D:/env/sdk"));
    CHECK_FALSE(ResolveSdk(std::nullopt, nullptr).has_value());
    CHECK_FALSE(ResolveSdk(std::nullopt, "").has_value());   // set-but-empty is unset
}

// ---- the s4.3 decision table ---------------------------------------------------

TEST_CASE("arcbuild::ConfigWantsDebugCrt: Debug only; Dist maps to Release for the probe", "[build]")
{
    CHECK(ConfigWantsDebugCrt("Debug"));
    CHECK_FALSE(ConfigWantsDebugCrt("Release"));
    CHECK_FALSE(ConfigWantsDebugCrt("Dist"));
}

TEST_CASE("arcbuild::ClassifySlot lands on exactly one s4.3 row", "[build]")
{
    using Arcane::CrtFlavor;
    // absent -> nothing to be wrong about, whatever the scanner would say
    CHECK(ClassifySlot(false, CrtFlavor::Unknown, "Debug")   == SlotState::Absent);
    CHECK(ClassifySlot(false, CrtFlavor::Debug,   "Release") == SlotState::Absent);
    // present, flavor matches --config
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Debug")   == SlotState::Match);
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Release") == SlotState::Match);
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Dist")    == SlotState::Match);   // Dist == release CRT
    // present, flavor mismatches
    CHECK(ClassifySlot(true, CrtFlavor::Release, "Debug")   == SlotState::Mismatch);
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Release") == SlotState::Mismatch);
    CHECK(ClassifySlot(true, CrtFlavor::Debug,   "Dist")    == SlotState::Mismatch);
    // present, unreadable -> its own row, never silently "match"
    CHECK(ClassifySlot(true, CrtFlavor::Unknown, "Debug")   == SlotState::Unreadable);
    CHECK(ClassifySlot(true, CrtFlavor::Unknown, "Release") == SlotState::Unreadable);
}

TEST_CASE("arcbuild::Decide: plain for absent/match, /t:Rebuild for mismatch/unreadable, forced by --force-rebuild or `rebuild`",
          "[build]")
{
    // THE INCREMENTAL RULE (spec s4.3) -- the reason the driver exists now. A
    // game project's Binaries\ is ONE slot shared by every configuration; an
    // incremental msbuild reasons about the per-config object tree and can
    // report "up to date" over the OTHER config's DLL (observed live: a
    // 0.24s "All outputs are up-to-date" followed by "the rebuilt module
    // still failed to load"). The editor used to force /t:Rebuild on every
    // build to close that; the driver forces it ONLY when the slot's CRT
    // flavor says it must, so a wizard-made component costs one TU + a link.
    SECTION("build follows the slot")
    {
        CHECK_FALSE(Decide(Command::Build, false, SlotState::Absent).rebuild);
        CHECK_FALSE(Decide(Command::Build, false, SlotState::Match).rebuild);
        CHECK(Decide(Command::Build, false, SlotState::Mismatch).rebuild);
        CHECK(Decide(Command::Build, false, SlotState::Unreadable).rebuild);
    }
    SECTION("--force-rebuild bypasses the probe on every row")
    {
        for (SlotState s : { SlotState::Absent, SlotState::Match, SlotState::Mismatch, SlotState::Unreadable })
            CHECK(Decide(Command::Build, true, s).rebuild);
    }
    SECTION("the rebuild command is unconditional")
    {
        for (SlotState s : { SlotState::Absent, SlotState::Match, SlotState::Mismatch, SlotState::Unreadable })
            CHECK(Decide(Command::Rebuild, false, s).rebuild);
    }
    SECTION("every verdict says why, and the unreadable one says so in words")
    {
        for (SlotState s : { SlotState::Absent, SlotState::Match, SlotState::Mismatch, SlotState::Unreadable })
        {
            const Verdict v = Decide(Command::Build, false, s);
            REQUIRE(v.reason != nullptr);
            CHECK(std::string(v.reason).size() > 10);
        }
        CHECK(std::string(Decide(Command::Build, false, SlotState::Unreadable).reason).find("unreadable") != std::string::npos);
        CHECK(std::string(Decide(Command::Build, true,  SlotState::Match).reason).find("--force-rebuild") != std::string::npos);
        CHECK(std::string(Decide(Command::Rebuild, false, SlotState::Match).reason).find("rebuild") != std::string::npos);
    }
}

// ---- exit codes -------------------------------------------------------------

TEST_CASE("arcbuild exit codes: probe 0 on the plain rows, 3 on the rebuild rows; a dead pipe is a refusal", "[build]")
{
    CHECK(ProbeExitCode(SlotState::Absent)     == kExitOk);
    CHECK(ProbeExitCode(SlotState::Match)      == kExitOk);
    CHECK(ProbeExitCode(SlotState::Mismatch)   == kExitProbeRebuild);
    CHECK(ProbeExitCode(SlotState::Unreadable) == kExitProbeRebuild);
    CHECK(kExitRefused == 2);
    CHECK(kExitProbeRebuild == 3);
    // A child's own status passes through untouched (premake and msbuild
    // both exit 1 on failure; 9009 is cmd's "not found").
    CHECK(ExitFromChild(0)    == 0);
    CHECK(ExitFromChild(1)    == 1);
    CHECK(ExitFromChild(9009) == 9009);
    CHECK(ExitFromChild(std::nullopt) == kExitRefused);
}

// ---- paths ------------------------------------------------------------------

TEST_CASE("arcbuild::SlotPath is <root>/Binaries/<gameModule>, empty for a content-only project", "[build]")
{
    Layout l = AphelyonLayout();
    CHECK(SlotPath(l) == fs::path("D:/dev/starworks/Gacha/Game") / "Binaries" / "Aphelyon.dll");
    l.gameModule.clear();
    CHECK(SlotPath(l).empty());
}

TEST_CASE("arcbuild::SolutionPath: a discovered workspace file wins over the <name>.slnx convention", "[build]")
{
    const Layout l = AphelyonLayout();
    CHECK(SolutionPath(l, "D:/dev/starworks/Gacha/Game/Other.sln") == fs::path("D:/dev/starworks/Gacha/Game/Other.sln"));
    CHECK(SolutionPath(l, {}) == fs::path("D:/dev/starworks/Gacha/Game") / "Aphelyon.slnx");
}

// ---- composition --------------------------------------------------------------

TEST_CASE("arcbuild::ComposeGenerate is cd-first, parenthesised, stderr-folded premake", "[build]")
{
    const std::string cmd = ComposeGenerate(AphelyonLayout(), SdkTools(), "vs2026");
    CHECK(cmd == "( cd /d \"D:/dev/starworks/Gacha/Game\" && "
                 "\"D:/dev/starworks/Arcane/ThirdParty/premake5/premake5.exe\" vs2026 ) 2>&1");
    // The action is the Linux seam (spec s6): it is a parameter, not a constant.
    CHECK(ComposeGenerate(AphelyonLayout(), SdkTools(), "gmake2").find("premake5.exe\" gmake2 )") != std::string::npos);
}

TEST_CASE("arcbuild::ComposeMsBuild: /t:Rebuild ONLY when asked, /t:Clean for clean, no cd, absolute solution", "[build]")
{
    const fs::path sln = "D:/dev/starworks/Gacha/Game/Aphelyon.slnx";
    const std::string plain = ComposeMsBuild(SdkTools(), sln, "Debug", MsBuildTarget::Build);
    CHECK(plain == "( \"C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe\" "
                   "\"D:/dev/starworks/Gacha/Game/Aphelyon.slnx\" /p:Configuration=Debug /m /nologo ) 2>&1");
    CHECK(plain.find("/t:") == std::string::npos);
    CHECK(plain.find("cd /d") == std::string::npos);

    const std::string rebuild = ComposeMsBuild(SdkTools(), sln, "Release", MsBuildTarget::Rebuild);
    CHECK(rebuild.find("/p:Configuration=Release /t:Rebuild /m /nologo") != std::string::npos);

    const std::string clean = ComposeMsBuild(SdkTools(), sln, "Dist", MsBuildTarget::Clean);
    CHECK(clean.find("/p:Configuration=Dist /t:Clean /m /nologo") != std::string::npos);
}

TEST_CASE("arcbuild::CleanTargets is exactly Binaries/ and Intermediate/<config>/", "[build]")
{
    const std::vector<fs::path> t = CleanTargets(AphelyonLayout(), "Debug");
    REQUIRE(t.size() == 2);
    CHECK(t[0] == fs::path("D:/dev/starworks/Gacha/Game") / "Binaries");
    CHECK(t[1] == fs::path("D:/dev/starworks/Gacha/Game") / "Intermediate" / "Debug");
    // Never Source/, Content/, Saved/, the .slnx, nor Intermediate/Artifacts (arccook's).
    for (const fs::path& p : t)
    {
        const std::string s = p.generic_string();
        CHECK(s.find("Source") == std::string::npos);
        CHECK(s.find("Content") == std::string::npos);
        CHECK(s.find("Saved") == std::string::npos);
        CHECK(s.find("Artifacts") == std::string::npos);
        CHECK(s.find(".slnx") == std::string::npos);
    }
}
```

- [ ] **Step 2: Add the TU + include dir to the test exe, regenerate, build; verify RED**

Edit `premake5.lua`, `project "ArcaneTests"` — in `files {}`, directly after the `ClassTemplates.cpp` entry (`"%{wks.location}/ArcaneEditor/src/Project/ClassTemplates.cpp",`) add:

```lua
        -- arcbuild (the game-project build driver, spec docs/specs/
        -- 2026-09-13-arcbuild-driver-design.md): Driver.cpp -- the PURE core
        -- (Cli shape, --sdk precedence, the s4.3 decision table, exit-code
        -- mapping, every composed child command line) -- source-compiles into
        -- the test exe so the [build] units drive it directly, same "pure
        -- logic, no spawn" pattern as ModuleBuild.cpp above. main.cpp (the
        -- spawn + PE probe half) is NOT compiled here; the opt-in [build-desk]
        -- cases run the built arcbuild.exe instead.
        "%{wks.location}/arcbuild/src/Driver.cpp",
```

and in `includedirs {}`, after `"%{IncludeDir.meshoptimizer}",  -- F2c Task 1 ...` add:

```lua
        "%{wks.location}/arcbuild/src",   -- Driver.hpp for the [build] units (arcbuild Task 2)
```

Run (PowerShell): `cd D:\dev\starworks\Arcane; cmd /c ".\GenerateProjects.bat"`
Run (bash): Debug build as Task 1 Step 2, `grep -E "error C1083|Error\(s\)"`.
Expected: `BuildDriverTest.cpp(…): fatal error C1083: Cannot open include file: 'Driver.hpp'` and `Driver.cpp` missing — RED.

- [ ] **Step 3: Write the header** — create `arcbuild/src/Driver.hpp`:

```cpp
#pragma once

// arcbuild::Driver -- the PURE core of the game-project build driver (spec
// docs/specs/2026-09-13-arcbuild-driver-design.md). Everything a build
// DECIDES lives here, testable without a process or a PE file ([build],
// ArcaneTests/src/BuildDriverTest.cpp): the CLI shape (s3), the --sdk /
// ARCANE_SDK precedence, the s4.3 incremental rule as a function over
// (slot exists, slot CRT flavor, --config, --force-rebuild), the exit-code
// table, the path conventions, and every child command line. main.cpp is
// the thin shell that loads the manifest, scans the slot, spawns each line
// and re-emits its output.
//
// NOT a second PE scanner: the slot's CrtFlavor comes from
// Arcane::Module::ScanFileCrtFlavor (main.cpp), the same verdict PluginHost
// uses to refuse a cross-CRT module, so the driver and the host can never
// disagree about what "matches" means. This header only names the enum.

#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Plugin/Module.hpp>   // Arcane::CrtFlavor (enum only; no link)

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace arcbuild
{
    // ---- the CLI (spec s3) ----------------------------------------------------

    enum class Command : std::uint8_t { Generate, Build, Rebuild, Clean, Probe };

    // The exact lower-case word, or nullopt. The command is positional (argv[1])
    // because Arcane::Cli has no subcommands; main.cpp peels it and hands the
    // rest to MakeCli().Parse.
    [[nodiscard]] std::optional<Command> ParseCommand(std::string_view word);
    [[nodiscard]] const char* CommandName(Command c);

    struct Request
    {
        Command                              command = Command::Build;
        std::filesystem::path                project;            // --project, as given (dir or .arcproj)
        std::string                          config = "Debug";   // --config Debug|Release|Dist
        std::optional<std::filesystem::path> sdk;                // --sdk, when supplied
        std::string                          action = "vs2026";  // --action (the premake action; Linux seam)
        bool                                 forceRebuild = false;
        bool                                 quiet = false;      // suppress the driver's own info lines (ruling R5)
    };

    // The option/flag set of s3, registered on an Arcane::Cli. --project is
    // Required(); --config is Choices'd to the three configurations.
    [[nodiscard]] Arcane::Cli MakeCli();
    [[nodiscard]] Request RequestFromCli(Command command, const Arcane::Cli::Result& r);

    // --sdk > ARCANE_SDK > nullopt (the driver refuses). A set-but-empty
    // variable counts as unset.
    [[nodiscard]] std::optional<std::filesystem::path> ResolveSdk(
        const std::optional<std::filesystem::path>& flag, const char* envValue);

    // ---- the incremental rule (spec s4.3) -------------------------------------

    // Which CRT family a configuration's DLL links: Debug <=> the debug CRT
    // (ucrtbased & co); Release AND Dist are both release-CRT, so Dist maps
    // onto Release for the probe -- the same caveat the editor's
    // ModuleBuild::Configuration() documents.
    [[nodiscard]] bool ConfigWantsDebugCrt(std::string_view config);

    // The row of the s4.3 table the slot lands on.
    enum class SlotState : std::uint8_t { Absent, Match, Mismatch, Unreadable };
    [[nodiscard]] const char* SlotStateName(SlotState s);
    [[nodiscard]] SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config);

    // The decision: plain build, or msbuild /t:Rebuild. `reason` is a static
    // string naming the row (printed as the driver's own log line).
    struct Verdict
    {
        bool        rebuild;
        const char* reason;
    };
    [[nodiscard]] Verdict Decide(Command command, bool forceRebuild, SlotState slot);

    // ---- exit codes -------------------------------------------------------------

    constexpr int kExitOk           = 0;
    constexpr int kExitRefused      = 2;   // no SDK / no project / bad flags (s3)
    constexpr int kExitProbeRebuild = 3;   // `probe`: the slot would force /t:Rebuild (ruling R4)

    [[nodiscard]] int ProbeExitCode(SlotState s);
    // A child's own status passes through (premake/msbuild exit 1 on failure,
    // cmd 9009 when the exe is not found); a pipe that could not open is a
    // driver refusal.
    [[nodiscard]] int ExitFromChild(std::optional<int> childExit);

    // ---- paths ------------------------------------------------------------------

    struct Layout
    {
        std::filesystem::path root;        // ABSOLUTE project directory (main.cpp absolutises; ruling R3)
        std::filesystem::path manifest;    // the .arcproj
        std::string           name;        // manifest `name` -> the <name>.slnx convention
        std::string           gameModule;  // manifest `gameModule`; empty = content-only project
    };

    // <root>/Binaries/<gameModule> -- the single slot HostBoot loads from
    // (Arcane/Host/ProjectBoot.hpp). Empty when the project has no module.
    [[nodiscard]] std::filesystem::path SlotPath(const Layout& l);

    // The workspace file msbuild drives: `discovered` (Toolchain::
    // DiscoverSolution's answer) when non-empty, else <root>/<name>.slnx --
    // the committed convention (a project's premake workspace is named after
    // the project), exactly as EditorApp::StartModuleRebuild assumed.
    [[nodiscard]] std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered);

    // ---- composition --------------------------------------------------------------
    // Every line runs through cmd.exe /c (_wpopen). Parenthesised so the
    // trailing 2>&1 folds the member's stderr into the captured stdout. Paths
    // are wrapped in plain quotes -- good for spaces, which real install
    // paths contain; an embedded quote is not defended against.

    struct Tools
    {
        std::filesystem::path premake;
        std::filesystem::path msbuild;
    };

    // ( cd /d "<root>" && "<premake>" <action> ) 2>&1 -- premake reads
    // ./premake5.lua from the cwd, hence the cd.
    [[nodiscard]] std::string ComposeGenerate(const Layout& l, const Tools& t, std::string_view action);

    enum class MsBuildTarget : std::uint8_t { Build, Rebuild, Clean };

    // ( "<msbuild>" "<solution>" /p:Configuration=<cfg> [/t:Rebuild|/t:Clean] /m /nologo ) 2>&1
    // No cd (the solution path is absolute); /t: only when the target is not
    // the default Build.
    [[nodiscard]] std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution,
                                             std::string_view config, MsBuildTarget target);

    // What `clean` removes after msbuild /t:Clean: <root>/Binaries (the whole
    // slot -- it is one slot) and <root>/Intermediate/<config>. Never Source/,
    // Content/, Saved/, the .slnx (generate rewrites it), nor
    // Intermediate/Artifacts (arccook's).
    [[nodiscard]] std::vector<std::filesystem::path> CleanTargets(const Layout& l, std::string_view config);
}
```

- [ ] **Step 4: Write the implementation** — create `arcbuild/src/Driver.cpp`:

```cpp
#include "Driver.hpp"

namespace arcbuild
{
    namespace
    {
        void Quote(std::string& out, const std::filesystem::path& p)
        {
            out += '"';
            out += p.string();
            out += '"';
        }
    }

    // ---- the CLI ----------------------------------------------------------------

    std::optional<Command> ParseCommand(std::string_view word)
    {
        if (word == "generate") return Command::Generate;
        if (word == "build")    return Command::Build;
        if (word == "rebuild")  return Command::Rebuild;
        if (word == "clean")    return Command::Clean;
        if (word == "probe")    return Command::Probe;
        return std::nullopt;
    }

    const char* CommandName(Command c)
    {
        switch (c)
        {
            case Command::Generate: return "generate";
            case Command::Build:    return "build";
            case Command::Rebuild:  return "rebuild";
            case Command::Clean:    return "clean";
            case Command::Probe:    return "probe";
        }
        return "?";
    }

    Arcane::Cli MakeCli()
    {
        Arcane::Cli cli{ "arcbuild <generate|build|rebuild|clean|probe>",
                         "Arcane game-project build driver: premake, then msbuild, with the "
                         "single-slot incremental rule (spec 2026-09-13)" };
        cli.Option("project", "", "project directory or .arcproj (required)").Required();
        cli.Option("config", "Debug", "msbuild configuration").Choices({ "Debug", "Release", "Dist" });
        cli.Option("sdk", "", "Arcane SDK root (else ARCANE_SDK from the environment)");
        cli.Option("action", "vs2026", "premake action");
        cli.Flag("force-rebuild", "msbuild /t:Rebuild regardless of the slot probe");
        cli.Flag("quiet", "suppress the driver's own [arcbuild] info lines (child output still streams)");
        return cli;
    }

    Request RequestFromCli(Command command, const Arcane::Cli::Result& r)
    {
        Request req;
        req.command      = command;
        req.project      = r.Get("project");
        req.config       = r.Get("config");
        req.action       = r.Get("action");
        req.forceRebuild = r.Flag("force-rebuild");
        req.quiet        = r.Flag("quiet");
        // Supplied(), not a compare against the "" default: an explicit
        // --sdk "" would otherwise be indistinguishable from unsupplied.
        if (r.Supplied("sdk") && !r.Get("sdk").empty())
            req.sdk = std::filesystem::path(r.Get("sdk"));
        return req;
    }

    std::optional<std::filesystem::path> ResolveSdk(const std::optional<std::filesystem::path>& flag,
                                                    const char* envValue)
    {
        if (flag && !flag->empty())
            return *flag;
        if (envValue && *envValue)
            return std::filesystem::path(envValue);
        return std::nullopt;
    }

    // ---- the incremental rule ------------------------------------------------------

    bool ConfigWantsDebugCrt(std::string_view config)
    {
        return config == "Debug";
    }

    const char* SlotStateName(SlotState s)
    {
        switch (s)
        {
            case SlotState::Absent:     return "absent";
            case SlotState::Match:      return "match";
            case SlotState::Mismatch:   return "mismatch";
            case SlotState::Unreadable: return "unreadable";
        }
        return "?";
    }

    SlotState ClassifySlot(bool exists, Arcane::CrtFlavor flavor, std::string_view config)
    {
        if (!exists)
            return SlotState::Absent;
        if (flavor == Arcane::CrtFlavor::Unknown)
            return SlotState::Unreadable;
        const bool slotIsDebug = (flavor == Arcane::CrtFlavor::Debug);
        return slotIsDebug == ConfigWantsDebugCrt(config) ? SlotState::Match : SlotState::Mismatch;
    }

    Verdict Decide(Command command, bool forceRebuild, SlotState slot)
    {
        if (command == Command::Rebuild)
            return { true, "rebuild command: msbuild /t:Rebuild unconditionally" };
        if (forceRebuild)
            return { true, "--force-rebuild: msbuild /t:Rebuild, slot probe bypassed" };
        switch (slot)
        {
            case SlotState::Absent:
                return { false, "slot absent: plain build (nothing to be wrong about)" };
            case SlotState::Match:
                return { false, "slot CRT flavor matches --config: plain build (msbuild's incremental view is trustworthy)" };
            case SlotState::Mismatch:
                // THE SINGLE-SLOT HAZARD: Binaries\ holds one DLL for every
                // configuration while the object trees are per-config, so an
                // incremental build would compare this config's objects
                // against this config's link stamp, find both current, relink
                // nothing, and leave the OTHER config's DLL in place for the
                // host to refuse.
                return { true, "slot CRT flavor mismatches --config: msbuild /t:Rebuild (single-slot Binaries/ hazard)" };
            case SlotState::Unreadable:
                return { true, "slot CRT flavor unreadable: msbuild /t:Rebuild (unknown => the safe choice)" };
        }
        return { true, "slot state unknown: msbuild /t:Rebuild" };
    }

    // ---- exit codes ------------------------------------------------------------------

    int ProbeExitCode(SlotState s)
    {
        return (s == SlotState::Absent || s == SlotState::Match) ? kExitOk : kExitProbeRebuild;
    }

    int ExitFromChild(std::optional<int> childExit)
    {
        return childExit ? *childExit : kExitRefused;
    }

    // ---- paths -----------------------------------------------------------------------

    std::filesystem::path SlotPath(const Layout& l)
    {
        if (l.gameModule.empty())
            return {};
        return l.root / "Binaries" / l.gameModule;
    }

    std::filesystem::path SolutionPath(const Layout& l, const std::filesystem::path& discovered)
    {
        if (!discovered.empty())
            return discovered;
        return l.root / (l.name + ".slnx");
    }

    // ---- composition -------------------------------------------------------------------

    std::string ComposeGenerate(const Layout& l, const Tools& t, std::string_view action)
    {
        std::string cmd = "( cd /d ";
        Quote(cmd, l.root);
        cmd += " && ";
        Quote(cmd, t.premake);
        cmd += ' ';
        cmd += action;
        cmd += " ) 2>&1";
        return cmd;
    }

    std::string ComposeMsBuild(const Tools& t, const std::filesystem::path& solution,
                               std::string_view config, MsBuildTarget target)
    {
        std::string cmd = "( ";
        Quote(cmd, t.msbuild);
        cmd += ' ';
        Quote(cmd, solution);
        cmd += " /p:Configuration=";
        cmd += config;
        switch (target)
        {
            case MsBuildTarget::Build:   break;
            case MsBuildTarget::Rebuild: cmd += " /t:Rebuild"; break;
            case MsBuildTarget::Clean:   cmd += " /t:Clean";   break;
        }
        cmd += " /m /nologo ) 2>&1";
        return cmd;
    }

    std::vector<std::filesystem::path> CleanTargets(const Layout& l, std::string_view config)
    {
        return { l.root / "Binaries", l.root / "Intermediate" / std::string(config) };
    }
}
```

- [ ] **Step 5: Build, run GREEN**

Run (bash): Debug build; then `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[build]" | grep -E "seeded|test cases|All tests passed|FAILED"`.
Expected: `All tests passed (… assertions in 16 test cases)` (3 from Task 1 + 13 here).

- [ ] **Step 6: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add arcbuild/src/Driver.hpp arcbuild/src/Driver.cpp ArcaneTests/src/BuildDriverTest.cpp premake5.lua && git commit -q -F - <<'EOF'
feat(arcbuild): the driver's pure core -- CLI shape, --sdk precedence, the s4.3 decision table, exit codes, composed child lines (Task 2)

Source-compiled into ArcaneTests ([build]); no exe yet (Task 3).

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 3: `arcbuild.exe` — main, premake project, `[build-desk]` cases, CLI proof on the Gacha Game

**Files:**
- Create: `arcbuild/src/main.cpp`
- Modify: `premake5.lua` — insert `project "arcbuild"` after `project "arccook"`'s block (after its closing `filter {}` at ~:312, before the `-- Arcane: the engine DLL` banner at ~:314)
- Modify: `ArcaneTests/src/BuildDriverTest.cpp` — append the `[build-desk]` cases

**Interfaces:**
- Consumes: Task 2's core (every name above); `Arcane::Toolchain::{ResolvePremake, ResolveMsBuild, DiscoverSolution}` (Task 1); `Arcane::Project::ResolveManifestFile(pathOrFile) -> optional<path>` (`ArcaneClient/src/Arcane/Project/Project.hpp:63`); `Arcane::ProjectManifest::LoadFile(file) -> optional<ProjectManifest>` with `.name` / `.gameModule` (`ProjectManifest.hpp:101`); `Arcane::Module::ScanFileCrtFlavor(path, std::string* matchedImport) noexcept -> CrtFlavor` (`Module.hpp:56`); `Arcane::Editor::ModuleBuild::RunCapture` + `ExeDir` for the desk cases (compiled into ArcaneTests already).
- Produces: `bin/<cfg>-windows-x86_64-md/arcbuild/arcbuild.exe` (+ `ArcaneClient.dll` beside it). Output contract (used by Task 5's editor severity rule): every driver line is `[arcbuild] …`; refusals are `[arcbuild] error: …`; child lines are `[premake] …` / `[msbuild] …`; the probe row is `[arcbuild] probe: slot=<path> state=<absent|match|mismatch|unreadable> flavor=<debug|release|unknown> config=<cfg> -> <plain build|/t:Rebuild>`.

- [ ] **Step 1: Write the failing desk tests** — append to `ArcaneTests/src/BuildDriverTest.cpp`:

```cpp
// ---------------------------------------------------------------------------
// The opt-in desk probe. Off by default: SKIPs unless ARCANE_BUILD_DESK names
// an ABSOLUTE game-project directory (one with an .arcproj -- e.g.
// D:\dev\starworks\Gacha\Game), so the ordinary suite never spawns the
// driver. Runs the BUILT arcbuild.exe from the dev bin layout
// (../arcbuild/ beside this exe -- the editor's own ResolveDriver rule) via
// the editor's synchronous RunCapture. `probe` must answer with one row and
// exit 0 or 3; `generate` must exit 0 and leave a workspace file behind.
// Both are idempotent against a real project (premake rewrites only files
// whose content changed).
// ---------------------------------------------------------------------------

#include <cstdlib>
#include <Project/ModuleBuild.hpp>        // RunCapture + ExeDir (editor helpers compiled into the tests)
#include <Arcane/Build/Toolchain.hpp>     // DiscoverSolution (the post-generate check)

namespace
{
    fs::path DeskDriverExe()
    {
        return (Arcane::Editor::ModuleBuild::ExeDir() / ".." / "arcbuild" / "arcbuild.exe").lexically_normal();
    }

    std::string DeskLine(const char* command, const fs::path& project)
    {
        std::string cmd = "( \"";
        cmd += DeskDriverExe().string();
        cmd += "\" ";
        cmd += command;
        cmd += " --project \"";
        cmd += project.string();
        cmd += "\" --config Debug ) 2>&1";
        return cmd;
    }
}

TEST_CASE("arcbuild probe answers one s4.3 row against a real project on this desk", "[build-desk]")
{
    const char* env = std::getenv("ARCANE_BUILD_DESK");
    if (!env || !*env)
        SKIP("ARCANE_BUILD_DESK not set -- desk-only probe");
    REQUIRE(fs::is_regular_file(DeskDriverExe()));

    const Arcane::Editor::ModuleBuild::CaptureResult r =
        Arcane::Editor::ModuleBuild::RunCapture(DeskLine("probe", fs::path(env)));
    for (const std::string& line : r.lines)
        INFO(line);
    REQUIRE(r.exit.has_value());
    CHECK((*r.exit == kExitOk || *r.exit == kExitProbeRebuild));

    bool sawRow = false;
    for (const std::string& line : r.lines)
        if (line.rfind("[arcbuild] probe:", 0) == 0 && line.find(" state=") != std::string::npos)
            sawRow = true;
    CHECK(sawRow);
}

TEST_CASE("arcbuild generate writes the project's workspace file on this desk", "[build-desk]")
{
    const char* env = std::getenv("ARCANE_BUILD_DESK");
    if (!env || !*env)
        SKIP("ARCANE_BUILD_DESK not set -- desk-only probe");
    REQUIRE(fs::is_regular_file(DeskDriverExe()));

    const Arcane::Editor::ModuleBuild::CaptureResult r =
        Arcane::Editor::ModuleBuild::RunCapture(DeskLine("generate", fs::path(env)));
    for (const std::string& line : r.lines)
        INFO(line);
    REQUIRE(r.exit.has_value());
    CHECK(*r.exit == kExitOk);

    bool sawPremake = false;
    for (const std::string& line : r.lines)
        if (line.rfind("[premake]", 0) == 0)
            sawPremake = true;
    CHECK(sawPremake);
    CHECK_FALSE(Arcane::Toolchain::DiscoverSolution(fs::path(env)).empty());
}
```

- [ ] **Step 2: Build the tests and run the desk cases; verify RED**

Run (bash): Debug build (the appended cases compile against existing symbols), then
`cd bin/Debug-windows-x86_64-md/ArcaneTests && ARCANE_BUILD_DESK='D:\dev\starworks\Gacha\Game' ./ArcaneTests.exe "[build-desk]" | grep -E "REQUIRE|is_regular_file|test cases|FAILED"`
Expected: both cases FAIL at `REQUIRE( fs::is_regular_file(DeskDriverExe()) )` — the exe does not exist. RED.

- [ ] **Step 3: Write `main.cpp`** — create `arcbuild/src/main.cpp`:

```cpp
// arcbuild -- the game-project build driver (spec docs/specs/
// 2026-09-13-arcbuild-driver-design.md). Unreal's Build.bat analogue for
// Arcane game projects: ONE entry point the editor (ModuleBuild), the Gacha
// scripts and CI all call. It DRIVES premake and msbuild; it never owns
// compilation. The pure core (every decision) is Driver.hpp; this file is
// the shell: manifest, SDK, tool resolution, the slot probe, and the
// spawn-and-stream of each child.
//
//   arcbuild <generate|build|rebuild|clean|probe> --project <dir|.arcproj>
//            [--config Debug|Release|Dist] [--sdk <root>] [--action vs2026]
//            [--force-rebuild] [--quiet]
//
// Output: every child line to stdout prefixed [premake] / [msbuild]; the
// driver's own lines [arcbuild] (refusals: "[arcbuild] error: ..."). Flushed
// per line -- the editor reads this through a pipe and streams it into its
// Console. Exit: the first failing child's status, 2 for a driver refusal
// (no SDK / no project / bad flags), 3 from `probe` when the slot would
// force /t:Rebuild, 0 otherwise.

#include "Driver.hpp"

#include <Arcane/Build/Toolchain.hpp>
#include <Arcane/Plugin/Module.hpp>
#include <Arcane/Project/Project.hpp>
#include <Arcane/Project/ProjectManifest.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
    using namespace arcbuild;

    bool g_quiet = false;

    void Say(const std::string& line)
    {
        if (g_quiet)
            return;
        std::printf("[arcbuild] %s\n", line.c_str());
        std::fflush(stdout);
    }

    // Never quieted: a refusal is the one line the caller must see.
    int Refuse(const std::string& why)
    {
        std::printf("[arcbuild] error: %s\n", why.c_str());
        std::fflush(stdout);
        return kExitRefused;
    }

#ifdef _WIN32
    std::wstring Widen(const std::string& utf8)
    {
        if (utf8.empty())
            return {};
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                            static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                              wide.data(), n);
        return wide;
    }
#endif

    // Point ARCANE_SDK at `sdkRoot` in THIS process's environment block, which
    // the spawned cmd children inherit (the project's premake5.lua consumes it
    // via build/arcane.lua). Deliberately overwrites any setx'd value: --sdk
    // means "build against THIS engine", not whichever one the machine-wide
    // variable last pointed at.
    void SetSdkEnv(const std::filesystem::path& sdkRoot)
    {
#ifdef _WIN32
        ::SetEnvironmentVariableW(L"ARCANE_SDK", sdkRoot.wstring().c_str());
#else
        ::setenv("ARCANE_SDK", sdkRoot.string().c_str(), 1);
#endif
    }

    // Run one composed ( ... ) 2>&1 line through cmd (_wpopen), re-emitting
    // every line with `prefix` as it arrives. nullopt when the pipe itself
    // could not open. The same shape as the editor's ModuleBuild::Runner
    // worker, on the calling thread -- the driver has nothing else to do.
    std::optional<int> Stream(const std::string& commandLine, const char* prefix)
    {
#ifdef _WIN32
        FILE* pipe = ::_wpopen(Widen(commandLine).c_str(), L"r");
        if (!pipe)
            return std::nullopt;
        char buf[4096];
        while (std::fgets(buf, sizeof(buf), pipe))
        {
            std::string line(buf);
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                line.pop_back();
            std::printf("%s %s\n", prefix, line.c_str());
            std::fflush(stdout);
        }
        return ::_pclose(pipe);
#else
        (void)commandLine; (void)prefix;
        std::printf("[arcbuild] error: the vs2026 action is Windows-only today (cmd + msbuild)\n");
        return std::nullopt;
#endif
    }

    const char* FlavorName(Arcane::CrtFlavor f)
    {
        switch (f)
        {
            case Arcane::CrtFlavor::Debug:   return "debug";
            case Arcane::CrtFlavor::Release: return "release";
            case Arcane::CrtFlavor::Unknown: return "unknown";
        }
        return "?";
    }

    struct Probe
    {
        std::filesystem::path slot;
        bool                  exists = false;
        Arcane::CrtFlavor     flavor = Arcane::CrtFlavor::Unknown;
        std::string           matched;   // the import that decided the verdict
        SlotState             state  = SlotState::Absent;
    };

    // The s4.3 probe: Module::ScanFileCrtFlavor over the slot -- the SAME
    // verdict PluginHost uses to refuse a cross-CRT module, never a second
    // scanner. A content-only project (no gameModule) is Absent (ruling R7).
    Probe ProbeSlot(const Layout& layout, std::string_view config)
    {
        Probe p;
        p.slot = SlotPath(layout);
        std::error_code ec;
        p.exists = !p.slot.empty() && std::filesystem::is_regular_file(p.slot, ec);
        if (p.exists)
            p.flavor = Arcane::Module::ScanFileCrtFlavor(p.slot, &p.matched);
        p.state = ClassifySlot(p.exists, p.flavor, config);
        return p;
    }

    std::string ProbeRow(const Probe& p, std::string_view config, const Verdict& v)
    {
        std::string row = "probe: slot=";
        row += p.slot.empty() ? std::string("(no gameModule)") : p.slot.generic_string();
        row += " state=";
        row += SlotStateName(p.state);
        row += " flavor=";
        row += FlavorName(p.flavor);
        if (!p.matched.empty())
        {
            row += " (";
            row += p.matched;
            row += ')';
        }
        row += " config=";
        row += config;
        row += " -> ";
        row += v.rebuild ? "/t:Rebuild" : "plain build";
        return row;
    }

    void PrintUsage()
    {
        std::printf("usage: arcbuild <generate|build|rebuild|clean|probe> --project <dir|.arcproj>\n"
                    "                [--config Debug|Release|Dist] [--sdk <root>] [--action vs2026]\n"
                    "                [--force-rebuild] [--quiet]\n"
                    "  generate   premake <action> in the project root (writes <Name>.slnx + .vcxproj)\n"
                    "  build      generate, then msbuild; /t:Rebuild only when the Binaries/ slot's CRT\n"
                    "             flavor mismatches --config (or is unreadable)\n"
                    "  rebuild    generate, then msbuild /t:Rebuild unconditionally\n"
                    "  clean      msbuild /t:Clean, then remove Binaries/ and Intermediate/<config>/\n"
                    "  probe      print the slot verdict and exit 0 (plain build) or 3 (/t:Rebuild)\n");
        std::fflush(stdout);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")
    {
        PrintUsage();
        return argc < 2 ? kExitRefused : kExitOk;
    }

    const std::optional<Command> command = ParseCommand(argv[1]);
    if (!command)
    {
        PrintUsage();
        return Refuse(std::string("unknown command '") + argv[1] + "'");
    }

    // The command word is positional (Arcane::Cli has no subcommands): hand
    // Cli argv+1 so the word sits in the [0] slot Parse skips.
    const Arcane::Cli cli = MakeCli();
    const Arcane::Cli::Result parsed = cli.Parse(argc - 1, argv + 1);
    if (!parsed.ok)
        return parsed.exitCode;   // Cli printed the reason + usage (2), or --help (0)

    const Request req = RequestFromCli(*command, parsed);
    g_quiet = req.quiet;

    const std::optional<std::filesystem::path> sdk = ResolveSdk(req.sdk, std::getenv("ARCANE_SDK"));
    if (!sdk)
        return Refuse("no SDK: pass --sdk <root> or set ARCANE_SDK to an Arcane engine checkout");

    // The project: a directory (one .arcproj inside) or the .arcproj itself --
    // Project::ResolveManifestFile is the ONE rule the hosts use for the same
    // question; the manifest's name/gameModule drive the solution convention
    // and the slot. Root is absolutised here (ruling R3) so a relative
    // --project from a CI workspace composes correctly after premake's cd.
    const std::optional<std::filesystem::path> manifestFile = Arcane::Project::ResolveManifestFile(req.project);
    if (!manifestFile)
        return Refuse("no project: '" + req.project.string() + "' is not a project directory or .arcproj");
    const std::optional<Arcane::ProjectManifest> manifest = Arcane::ProjectManifest::LoadFile(*manifestFile);
    if (!manifest)
        return Refuse("'" + manifestFile->string() + "' is not a valid .arcproj");

    std::error_code ec;
    Layout layout;
    layout.manifest   = std::filesystem::absolute(*manifestFile, ec).lexically_normal();
    layout.root       = layout.manifest.parent_path();
    layout.name       = manifest->name;
    layout.gameModule = manifest->gameModule;

    SetSdkEnv(*sdk);
    Tools tools;
    tools.premake = Arcane::Toolchain::ResolvePremake(*sdk);
    tools.msbuild = Arcane::Toolchain::ResolveMsBuild();

    Say(std::string(CommandName(*command)) + " " + layout.name + " (" + req.config + ") in " +
        layout.root.generic_string() + " against SDK " + sdk->generic_string());

    switch (*command)
    {
        case Command::Probe:
        {
            const Probe   p = ProbeSlot(layout, req.config);
            const Verdict v = Decide(Command::Build, req.forceRebuild, p.state);
            // The row is the command's whole output -- never quieted.
            std::printf("[arcbuild] %s\n", ProbeRow(p, req.config, v).c_str());
            std::fflush(stdout);
            return ProbeExitCode(p.state);
        }

        case Command::Generate:
        {
            const std::string line = ComposeGenerate(layout, tools, req.action);
            Say(line);
            return ExitFromChild(Stream(line, "[premake]"));
        }

        case Command::Build:
        case Command::Rebuild:
        {
            // Premake FIRST, every build (idempotent; kills the stale-.sln
            // class of failure -- the editor's standing decision).
            const std::string gen = ComposeGenerate(layout, tools, req.action);
            Say(gen);
            const int genExit = ExitFromChild(Stream(gen, "[premake]"));
            if (genExit != kExitOk)
            {
                Say("premake exited with " + std::to_string(genExit) + " -- msbuild not run");
                return genExit;
            }

            const Probe   p = ProbeSlot(layout, req.config);
            const Verdict v = Decide(*command, req.forceRebuild, p.state);
            Say(ProbeRow(p, req.config, v));
            Say(v.reason);

            const std::filesystem::path solution =
                SolutionPath(layout, Arcane::Toolchain::DiscoverSolution(layout.root));
            const std::string build = ComposeMsBuild(tools, solution, req.config,
                                                     v.rebuild ? MsBuildTarget::Rebuild : MsBuildTarget::Build);
            Say(build);
            const int buildExit = ExitFromChild(Stream(build, "[msbuild]"));
            Say(buildExit == kExitOk ? "msbuild succeeded" : "msbuild exited with " + std::to_string(buildExit));
            return buildExit;
        }

        case Command::Clean:
        {
            int exit = kExitOk;
            const std::filesystem::path discovered = Arcane::Toolchain::DiscoverSolution(layout.root);
            if (discovered.empty())
                Say("no workspace file in " + layout.root.generic_string() + " -- skipping msbuild /t:Clean");
            else
            {
                const std::string clean = ComposeMsBuild(tools, discovered, req.config, MsBuildTarget::Clean);
                Say(clean);
                exit = ExitFromChild(Stream(clean, "[msbuild]"));
            }
            for (const std::filesystem::path& dir : CleanTargets(layout, req.config))
            {
                const auto removed = std::filesystem::remove_all(dir, ec);
                Say("removed " + dir.generic_string() + " (" + std::to_string(removed) + " entries)");
            }
            return exit;
        }
    }
    return Refuse("unreachable command");
}
```

- [ ] **Step 4: Add the premake project** — in `premake5.lua`, after `project "arccook"`'s closing `filter {}` (~:312) and before the `-- Arcane: the engine DLL` banner, insert:

```lua
-- ============================================================================
-- arcbuild: the game-project build driver (spec docs/specs/
-- 2026-09-13-arcbuild-driver-design.md) -- Unreal's Build.bat analogue. ONE
-- entry point (generate/build/rebuild/clean/probe) the editor's ModuleBuild,
-- the Gacha scripts and CI all spawn, so the premake+msbuild orchestration
-- lives once. It links ArcaneClient for Module::ScanFileCrtFlavor -- the
-- s4.3 slot probe is the SAME verdict PluginHost refuses a cross-CRT module
-- on, never a second PE scanner -- and for Project/ProjectManifest (the
-- .arcproj rule the hosts use). arccook above is the structural template;
-- the ArcaneClient.dll postbuild copy is ArcaneRuntime's. Its pure core
-- (src/Driver.cpp) is ALSO source-compiled into ArcaneTests ([build]).
-- ============================================================================
project "arcbuild"
    location "arcbuild"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        "%{wks.location}/ArcaneClient/src",
        "%{IncludeDir.ArcaneCore}",
        -- Project.hpp's include closure (ProjectManifest -> <Json.hpp> + glm;
        -- Diagnostics -> spdlog/Mosaic): headers only, same set ArcaneRuntime
        -- takes minus imgui/NRI.
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Mosaic}",
    }

    links { "ArcaneCore", "ArcaneClient" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    -- The driver loads ArcaneClient.dll from its own directory (dev bin
    -- layout: bin/<cfg>/arcbuild/); a packaged layout ships it beside the
    -- editor, where the DLL already is.
    postbuildcommands {
        '{COPYFILE} "%{wks.location}/bin/' .. outputdir .. '/ArcaneClient/ArcaneClient.dll" "%{cfg.buildtarget.directory}/ArcaneClient.dll"',
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning

    filter "configurations:Debug"
        defines { "ARCANE_DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "ARCANE_RELEASE", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        defines { "ARCANE_DIST", "NDEBUG" }
        runtime "Release"
        optimize "speed"
        symbols "off"
    filter {}
```

- [ ] **Step 5: Regenerate, build both configs, run the desk cases GREEN**

Run (PowerShell): `cd D:\dev\starworks\Arcane; cmd /c ".\GenerateProjects.bat"`
Run (bash): Debug build, then Release build (`-p:Configuration=Release`; if `ArcaneEditor.exe` LNK1104s because the desk editor is open, note it — `arcbuild.exe` and `ArcaneTests.exe` still built; check `ls bin/Release-windows-x86_64-md/arcbuild/`).
Then: `cd bin/Debug-windows-x86_64-md/ArcaneTests && ARCANE_BUILD_DESK='D:\dev\starworks\Gacha\Game' ./ArcaneTests.exe "[build-desk]" | grep -E "test cases|All tests passed|FAILED"` — expected `All tests passed (… in 2 test cases)`.
And the SKIP path: `./ArcaneTests.exe "[build-desk]"` without the variable — expected `2 test cases … skipped` (the report says `All tests passed (0 assertions in 2 test cases)` or Catch's skip summary — either is the SKIP, not a pass with assertions).

- [ ] **Step 6: The CLI proof of the incremental rule on the Gacha Game (the product half of R10, first sighting)**

Run (bash), each line separately, keeping the logs:

```bash
AB=/d/dev/starworks/Arcane/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe
G='D:\dev\starworks\Gacha\Game'
# 1. Where is the slot now?
"$AB" probe --project "$G" --config Debug
# 2. Bring the slot to Debug (a /t:Rebuild if the desk left Release there, plain otherwise).
"$AB" build --project "$G" --config Debug --sdk 'D:\dev\starworks\Arcane' > /tmp/ab-first.log 2>&1; tail -3 /tmp/ab-first.log
# 3. Touch the user's wizard-made component (mtime only -- content untouched, file stays untracked and uncommitted).
touch /d/dev/starworks/Gacha/Game/Source/TestComponent.cpp
# 4. Build again: expect the probe row "state=match ... -> plain build", ONE compile line, ONE link line.
"$AB" build --project "$G" --config Debug --sdk 'D:\dev\starworks\Arcane' > /tmp/ab-incr.log 2>&1
grep -E "probe:|\.cpp$|-> .*Aphelyon\.dll|Warning\(s\)|Error\(s\)|Elapsed" /tmp/ab-incr.log
grep -cE "^\[msbuild\]   [A-Za-z0-9_]+\.cpp$" /tmp/ab-incr.log    # compiles
grep -cE "Aphelyon\.vcxproj -> " /tmp/ab-incr.log                  # links
# 5. The cross-config hazard is CLOSED, not just avoided: flip to Release and back.
"$AB" build --project "$G" --config Release --sdk 'D:\dev\starworks\Arcane' > /tmp/ab-rel.log 2>&1; grep -E "probe:|reason|/t:Rebuild" /tmp/ab-rel.log | head -3
"$AB" probe --project "$G" --config Debug; echo "probe exit $?"      # expect state=mismatch, exit 3
"$AB" probe --project "$G" --config Release; echo "probe exit $?"    # expect state=match, exit 0
```

Expected at step 4: probe row `state=match flavor=debug (ucrtbased.dll) config=Debug -> plain build`; compile count **1** (`TestComponent.cpp`); link count **1**. At step 5: the Release build's probe row says `state=mismatch … -> /t:Rebuild`; the two probes exit 3 then 0. Record all numbers in the ledger. If step 4 shows more than one compile, STOP and diagnose (premake rewrote the .vcxproj with a content change? a header the wizard touched?) before proceeding — the plan's headline claim is this number.

Restore the desk's slot afterwards to whatever step 1 reported (the user's running editor is usually Release): `"$AB" build --project "$G" --config <that> --sdk 'D:\dev\starworks\Arcane'`.

- [ ] **Step 7: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git status --short && git add arcbuild/src/main.cpp ArcaneTests/src/BuildDriverTest.cpp premake5.lua && git commit -q -F - <<'EOF'
feat(arcbuild): arcbuild.exe -- manifest, SDK env, tool resolution, the ScanFileCrtFlavor slot probe, spawn-and-stream; [build-desk] cases (Task 3)

Staged in bin/<cfg>/arcbuild/ with ArcaneClient.dll beside it. CLI-proved
on the Gacha Game: a touched wizard-made component = one compile + one
link under `build --config Debug` with a Debug slot; a Release slot probes
mismatch (exit 3) and forces /t:Rebuild.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 4: Full-suite checkpoint before the editor shim

**Files:** none (verification only).

This task exists so the editor rewrite in Task 5 starts from a known-green engine, and so a Release build's state is on record before ModuleBuild changes shape.

- [ ] **Step 1: Debug + Release `~[gpu]`, from the exe dir**

```bash
cd /d/dev/starworks/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "~[gpu]" | grep -E "seeded|test cases|All tests passed|FAILED"
cd /d/dev/starworks/Arcane/bin/Release-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "~[gpu]" | grep -E "seeded|test cases|All tests passed|FAILED"
```
Expected: both `All tests passed`; case counts = baseline + 18 (3 Task 1 + 13 Task 2 + 2 Task 3 desk cases, which SKIP and still count as cases). Record the seed and counts in the ledger. If the Release ArcaneTests is stale because the Release build was blocked (LNK1104 on the editor is fine; ArcaneTests must have linked) — say so.

- [ ] **Step 2: Ledger** — `.superpowers/sdd/2026-09-13-arcbuild-driver/progress.md`: `Task 4: checkpoint green — Debug <cases>/<assertions> seed <s>, Release <cases>/<assertions> seed <s>`. No commit (the ledger is committed with Task 7).

---

### Task 5: The editor shim — `ModuleBuild` spawns `arcbuild`

**Files:**
- Rewrite: `ArcaneEditor/src/Project/ModuleBuild.hpp`
- Rewrite: `ArcaneEditor/src/Project/ModuleBuild.cpp`
- Modify: `ArcaneEditor/src/Project/IdeLaunch.cpp:3` (include) and `:101-104` (`ResolveDevenv`)
- Modify: `ArcaneEditor/src/Project/IdeLaunch.hpp:85` (comment) 
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` — `StartModuleRebuild` (:2593-2646), `OpenInIde`'s discovery (:2685, :2690), `RegenerateSolution` (:2718-2741), `PollModuleBuild`'s severity rule (:2830-2833) and its failure message (:2852)
- Modify: `ArcaneEditor/src/App/EditorApp.hpp:1732-1757` and `:1780-1788` (comments only)
- Rewrite: `ArcaneTests/src/ModuleBuildTest.cpp`
- Modify: `premake5.lua` — the `ModuleBuild.cpp` entry comment in ArcaneTests' `files {}` (~:823-829)

**Interfaces:**
- Consumes: `Arcane::Toolchain::{DiscoverSolution, ResolveDevenv}` (Task 1); `arcbuild.exe`'s CLI + output contract (Task 3).
- Produces (the editor's new pure surface, pinned by `[editor]`):
  ```cpp
  namespace Arcane::Editor::ModuleBuild {
      std::filesystem::path SdkRootFromExeDir(const std::filesystem::path& exeDir);   // unchanged
      constexpr const char* Configuration();                                          // unchanged
      std::vector<std::filesystem::path> DriverCandidates(const std::filesystem::path& editorExeDir);
      std::filesystem::path ResolveDriver(const std::filesystem::path& editorExeDir);  // first existing candidate, else empty
      struct DriverInputs { std::filesystem::path driverExe, projectRoot, sdkRoot; std::string command, configuration; };
      std::string ComposeDriverCommand(const DriverInputs& in);
      struct CaptureResult { std::vector<std::string> lines; std::optional<int> exit; };   // unchanged
      CaptureResult RunCapture(const std::string& commandLine);                       // unchanged
      std::filesystem::path ExeDir();                                                 // unchanged
      class Runner { /* unchanged */ };
  }
  ```

- [ ] **Step 1: Rewrite the test** — replace `ArcaneTests/src/ModuleBuildTest.cpp` with:

```cpp
// ModuleBuild's PURE halves ([editor]) after the arcbuild driver arc: the
// SDK-root walk, the driver-exe candidate list, and THE ONE COMMAND LINE the
// Runner now executes -- arcbuild itself. Composition of premake/msbuild
// lines, solution discovery and the s4.3 rule left the editor for the
// driver (BuildDriverTest.cpp, ToolchainTest.cpp -- [build]). The Runner and
// RunCapture spawn processes and stay desk-verify territory.

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <Project/ModuleBuild.hpp>

namespace
{
    namespace fs = std::filesystem;
    using namespace Arcane::Editor;

    struct TempDir
    {
        fs::path path;
        explicit TempDir(const char* tag)
        {
            path = fs::temp_directory_path() /
                   (std::string("arcane_modulebuild_") + tag + "_" +
                    std::to_string(static_cast<unsigned>(
                        std::hash<const void*>{}(this))));
            fs::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };

    void Touch(const fs::path& p)
    {
        fs::create_directories(p.parent_path());
        std::ofstream(p.string()) << "x";
    }
}

TEST_CASE("SdkRootFromExeDir inverts the bin/<cfg>/<project> targetdir rule", "[editor]")
{
    CHECK(ModuleBuild::SdkRootFromExeDir(
              "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneEditor") ==
          fs::path("D:/dev/starworks/Gacha/Arcane"));
    // A trailing separator must not eat one of the three parent steps.
    CHECK(ModuleBuild::SdkRootFromExeDir(
              "D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneEditor/") ==
          fs::path("D:/dev/starworks/Gacha/Arcane"));
}

TEST_CASE("DriverCandidates: packaged beside the editor first, dev bin layout second", "[editor]")
{
    // The RuntimeLaunch::ExeCandidates rule, applied to arcbuild.exe.
    const auto c = ModuleBuild::DriverCandidates("D:/sdk/bin/Debug-windows-x86_64-md/ArcaneEditor");
    REQUIRE(c.size() == 2);
    CHECK(c[0] == fs::path("D:/sdk/bin/Debug-windows-x86_64-md/ArcaneEditor") / "arcbuild.exe");
    CHECK(c[1] == fs::path("D:/sdk/bin/Debug-windows-x86_64-md/ArcaneEditor") / ".." / "arcbuild" / "arcbuild.exe");
}

TEST_CASE("ResolveDriver returns the first candidate that exists, else empty", "[editor]")
{
    TempDir bin("driver");
    const fs::path editorDir = bin.path / "ArcaneEditor";
    fs::create_directories(editorDir);
    SECTION("neither -> empty (StartModuleRebuild refuses with a Console error)")
    {
        CHECK(ModuleBuild::ResolveDriver(editorDir).empty());
    }
    SECTION("dev layout only -> ../arcbuild/arcbuild.exe")
    {
        Touch(bin.path / "arcbuild" / "arcbuild.exe");
        CHECK(ModuleBuild::ResolveDriver(editorDir).lexically_normal() ==
              (bin.path / "arcbuild" / "arcbuild.exe").lexically_normal());
    }
    SECTION("packaged beside wins over the dev neighbour")
    {
        Touch(bin.path / "arcbuild" / "arcbuild.exe");
        Touch(editorDir / "arcbuild.exe");
        CHECK(ModuleBuild::ResolveDriver(editorDir) == editorDir / "arcbuild.exe");
    }
}

TEST_CASE("ComposeDriverCommand is arcbuild + the three flags, parenthesised, stderr-folded", "[editor]")
{
    // THE LINE THE EDITOR SPAWNS. Pinned exactly: the Runner executes this
    // through cmd, and arcbuild's own [build] tests pin what these flags
    // mean on the other side (spec s5.1).
    ModuleBuild::DriverInputs in;
    in.driverExe     = "D:/sdk/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe";
    in.projectRoot   = "D:/dev/starworks/Gacha/Game";
    in.sdkRoot       = "D:/sdk";
    in.command       = "build";
    in.configuration = "Debug";

    CHECK(ModuleBuild::ComposeDriverCommand(in) ==
          "( \"D:/sdk/bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe\" build"
          " --project \"D:/dev/starworks/Gacha/Game\" --config Debug --sdk \"D:/sdk\" ) 2>&1");

    // RegenerateSolution's spelling: same shape, `generate`.
    in.command = "generate";
    const std::string gen = ModuleBuild::ComposeDriverCommand(in);
    CHECK(gen.find("arcbuild.exe\" generate --project") != std::string::npos);
    CHECK(gen.front() == '(');
    CHECK(gen.rfind(") 2>&1") == gen.size() - 6);

    // A space-laden install path survives inside quotes.
    in.driverExe = "C:/Program Files/Arcane/arcbuild.exe";
    CHECK(ModuleBuild::ComposeDriverCommand(in).find("( \"C:/Program Files/Arcane/arcbuild.exe\" generate") == 0);
}

TEST_CASE("Configuration matches the editor's own build flavor", "[editor]")
{
#ifdef _DEBUG
    CHECK(std::string(ModuleBuild::Configuration()) == "Debug");
#else
    CHECK(std::string(ModuleBuild::Configuration()) == "Release");
#endif
}
```

- [ ] **Step 2: Build; verify RED**

Run (bash): Debug build; `grep -E "error C2039|error C2065|error C3861|Error\(s\)"`.
Expected: `ModuleBuildTest.cpp: error C2039: 'DriverCandidates': is not a member of 'Arcane::Editor::ModuleBuild'` (and `ResolveDriver`, `DriverInputs`, `ComposeDriverCommand`). RED.

- [ ] **Step 3: Rewrite `ModuleBuild.hpp`**:

```cpp
#pragma once

// ModuleBuild: Build -> Rebuild Game Module. Rebuilds the OPEN project's game
// module against the RUNNING editor's SDK by spawning arcbuild.exe -- the
// engine's game-project build driver (spec docs/specs/2026-09-13-arcbuild-
// driver-design.md) -- and streaming its merged stdout+stderr line-by-line,
// on a worker std::thread via _wpopen, into a thread-safe queue the
// EditorApp drains once per frame into the Console ("Build: " lines).
//
// What the driver decides is the driver's: premake first every build, the
// single-slot incremental rule (s4.3 -- /t:Rebuild only when Binaries/
// holds the other configuration's DLL), where premake and msbuild are. This
// file only knows (a) where arcbuild.exe is relative to the editor exe,
// (b) the ONE command line to run it with, (c) how to stream it. The
// COMPOSITION half is pure and unit-tested ([editor], ModuleBuildTest.cpp);
// the Runner and RunCapture spawn processes and are desk-verify territory
// -- the same split RuntimeLaunch.cpp draws around SpawnDetached.
//
// v1 NON-GOALS (arc decision): no Live-Coding patching, no in-editor code
// editing, no MSVC-diagnostic parsing into per-line locators -- raw console
// lines plus ONE failure row in Problems. No auto-build after the class
// wizard's Create (a future opt-in Tools -> Settings item).

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace Arcane::Editor::ModuleBuild
{
    // ---- pure halves ([editor]-tested; nothing here touches a process) ------

    // The engine workspace root ("SDK root", what arcane.lua calls ARCANE_SDK)
    // from the editor exe's directory: three levels up, inverting the premake
    // targetdir rule <sdk>/bin/<cfg>-<system>-<arch>-md/ArcaneEditor/. Pure
    // path math -- no filesystem probe -- so it composes into tests. Passed
    // to the driver as --sdk: "rebuild against the engine you are looking at".
    std::filesystem::path SdkRootFromExeDir(const std::filesystem::path& exeDir);

    // The msbuild configuration this editor build drives. The game DLL must
    // share the editor's CRT + engine import lib flavor, so it follows the
    // editor's own build. Dist caveat: a Dist editor also answers "Release" --
    // Dist is a packaging config of the ENGINE workspace; external projects
    // (build/arcane.lua consumers) map it onto their own Release/Dist pair at
    // generate time, and Release is the one that always exists.
    constexpr const char* Configuration()
    {
#ifdef _DEBUG
        return "Debug";
#else
        return "Release";
#endif
    }

    // Where arcbuild.exe might live relative to the EDITOR exe's directory:
    // packaged layout first (installed side by side), dev bin layout second
    // (premake's bin/<cfg>-<os>-<arch>-md/<Project>/ gives ArcaneEditor's
    // dir an "../arcbuild/" neighbour) -- the RuntimeLaunch::ExeCandidates
    // rule. Existence is NOT checked here; ResolveDriver does that.
    std::vector<std::filesystem::path> DriverCandidates(const std::filesystem::path& editorExeDir);

    // The first candidate that is a regular file, else empty (the caller
    // refuses with a Console error naming both places it looked).
    std::filesystem::path ResolveDriver(const std::filesystem::path& editorExeDir);

    struct DriverInputs
    {
        std::filesystem::path driverExe;
        std::filesystem::path projectRoot;
        std::filesystem::path sdkRoot;
        std::string           command;         // "build" (Rebuild Game Module) / "generate" (RegenerateSolution)
        std::string           configuration;   // Configuration()
    };

    // THE ONE command line the Runner / RunCapture execute:
    //   ( "<arcbuild>" <command> --project "<root>" --config <cfg> --sdk "<sdk>" ) 2>&1
    // Parenthesised so the trailing 2>&1 folds the driver's stderr into the
    // captured stdout (and so a quoted exe at the head survives cmd's
    // outer-quote stripping). Plain quotes around paths -- good for spaces,
    // which real install paths contain; an embedded quote is not defended.
    std::string ComposeDriverCommand(const DriverInputs& in);

    // ---- process halves (desk-verify; not unit-tested) ----------------------

    // Run `commandLine` through cmd (_wpopen) SYNCHRONOUSLY, returning its
    // merged output line-by-line plus the exit status (nullopt when the pipe
    // itself could not be opened). For the short, one-shot steps a click can
    // afford to wait on -- `arcbuild generate` takes well under a second --
    // where the Runner's worker thread would only add a frame of state
    // machine for nothing. NOT for `build`: that is the Runner's job.
    struct CaptureResult
    {
        std::vector<std::string> lines;
        std::optional<int>       exit;
    };
    CaptureResult RunCapture(const std::string& commandLine);

    // THIS process's exe directory (GetModuleFileNameW). Same private pattern
    // as EditorFonts.cpp/EditorAppScene.cpp, hoisted here because the SDK-root
    // walk and the driver lookup both start from it.
    std::filesystem::path ExeDir();

    // ---- the worker ---------------------------------------------------------

    // One build at a time, output pulled main-thread-side per frame. The
    // worker owns the _wpopen pipe for its whole life; the main thread only
    // ever touches the mutex-guarded queue + flags, so there is no handle to
    // race over. Join() blocks until the child exits -- Shutdown calls it, and
    // an editor closed mid-build waits for the driver rather than leaking a
    // worker thread into destructed members.
    class Runner
    {
    public:
        ~Runner() { Join(); }

        // Start `commandLine` on a fresh worker. False (and no effect) while a
        // build is already running.
        bool Start(std::string commandLine);

        [[nodiscard]] bool Running() const;

        // Take every line queued since the last drain (worker -> main thread).
        std::vector<std::string> DrainLines();

        // The finished build's exit code, exactly once: nullopt while running,
        // never started, or already taken. cmd's exit status = arcbuild's own
        // (the first failing child's, 2 for a refusal; -1 when the pipe
        // itself failed).
        std::optional<int> TakeExit();

        // Block until the worker exits (see the class comment). Idempotent.
        void Join();

    private:
        std::thread              m_thread;
        mutable std::mutex       m_mutex;
        std::vector<std::string> m_lines;     // guarded by m_mutex
        bool                     m_running = false;   // guarded by m_mutex
        bool                     m_done    = false;   // guarded by m_mutex (latch for TakeExit)
        int                      m_exit    = -1;      // guarded by m_mutex
    };
}
```

- [ ] **Step 4: Rewrite `ModuleBuild.cpp`** — keep `Widen`, `Quote`, `SdkRootFromExeDir`, `RunCapture`, `ExeDir`, and the whole `Runner` exactly as they are today (`ModuleBuild.cpp:34-56`, `:80-91`, `:128-167`, `:242-328`); delete `LowerExt`, `DiscoverSolution`, `ComposeRebuildCommands`, `ComposeGenerateCommand`, `ResolvePremake`, `VsWhere`, `ResolveMsBuild`, `SetSdkEnv`; add, after `SdkRootFromExeDir`:

```cpp
    std::vector<std::filesystem::path> DriverCandidates(const std::filesystem::path& editorExeDir)
    {
        return {
            editorExeDir / "arcbuild.exe",
            editorExeDir / ".." / "arcbuild" / "arcbuild.exe",
        };
    }

    std::filesystem::path ResolveDriver(const std::filesystem::path& editorExeDir)
    {
        std::error_code ec;
        for (const std::filesystem::path& candidate : DriverCandidates(editorExeDir))
            if (std::filesystem::is_regular_file(candidate, ec))
                return candidate;
        return {};
    }

    std::string ComposeDriverCommand(const DriverInputs& in)
    {
        std::string cmd = "( ";
        Quote(cmd, in.driverExe);
        cmd += ' ';
        cmd += in.command;
        cmd += " --project ";
        Quote(cmd, in.projectRoot);
        cmd += " --config ";
        cmd += in.configuration;
        cmd += " --sdk ";
        Quote(cmd, in.sdkRoot);
        cmd += " ) 2>&1";
        return cmd;
    }
```

Also drop the now-unused `#include <algorithm>` if nothing else in the file uses it (`Runner` does not).

- [ ] **Step 5: `IdeLaunch` → Toolchain** — in `ArcaneEditor/src/Project/IdeLaunch.cpp` replace line 3 with `#include <Arcane/Build/Toolchain.hpp>   // ResolveDevenv (the one vswhere probe, shared with arcbuild)` and the body of `ResolveDevenv` (:101-104) with:

```cpp
    std::filesystem::path ResolveDevenv()
    {
        return Arcane::Toolchain::ResolveDevenv();
    }
```

In `IdeLaunch.hpp:85` change `ModuleBuild::VsWhere("-latest -find Common7\IDE\devenv.exe")` to `Arcane::Toolchain::ResolveDevenv() (ArcaneCore's one vswhere probe, shared with arcbuild.exe)`.

- [ ] **Step 6: `EditorAppProject.cpp`** — replace `StartModuleRebuild` (:2593-2646) with:

```cpp
    void EditorApp::StartModuleRebuild()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        // The menu item is greyed for all three of these; re-checked here so
        // a future keybind or other caller cannot slip past the gates.
        if (!proj || proj->Manifest().gameModule.empty())
        {
            ARC_ERROR("Build: no open project with a game module -- nothing to rebuild");
            return;
        }
        if (InPlayMode())
        {
            ARC_ERROR("Build: refused while Play is running -- stop to rebuild");
            return;
        }
        if (m_moduleBuild.Running())
        {
            ARC_WARN("Build: a rebuild is already running");
            return;
        }

        // arcbuild.exe does the work (spec 2026-09-13): premake first, then
        // msbuild with /t:Rebuild ONLY when Binaries/ holds the other
        // configuration's DLL -- so a wizard-made component costs one TU + a
        // link. The RUNNING editor's SDK goes in as --sdk and wins over any
        // machine-wide ARCANE_SDK: the point of the button is "rebuild
        // against the engine you are looking at".
        const std::filesystem::path exeDir  = ModuleBuild::ExeDir();
        const std::filesystem::path sdkRoot = ModuleBuild::SdkRootFromExeDir(exeDir);
        const std::filesystem::path driver  = ModuleBuild::ResolveDriver(exeDir);
        if (driver.empty())
        {
            ARC_ERROR("Build: arcbuild.exe not found beside the editor nor in ../arcbuild/ -- "
                      "build the engine workspace (Arcane.slnx) first");
            return;
        }

        ModuleBuild::DriverInputs in;
        in.driverExe     = driver;
        in.projectRoot   = proj->Root();
        in.sdkRoot       = sdkRoot;
        in.command       = "build";
        in.configuration = ModuleBuild::Configuration();

        m_moduleBuildRoot = proj->Root();
        const std::string cmd = ModuleBuild::ComposeDriverCommand(in);
        ARC_INFO("Build: rebuilding {} ({}) against SDK {}",
                 proj->Manifest().gameModule, in.configuration, sdkRoot.generic_string());
        ARC_INFO("Build: {}", cmd);
        if (!m_moduleBuild.Start(cmd))
            ARC_WARN("Build: a rebuild is already running");
    }
```

In `OpenInIde`, change both `ModuleBuild::DiscoverSolution(proj->Root())` (:2685 and :2690) to `Arcane::Toolchain::DiscoverSolution(proj->Root())`, and change the `ARC_INFO("IDE: no solution in {} yet -- generating (premake vs2026)", …)` text to `"IDE: no solution in {} yet -- generating (arcbuild generate)"`. Add `#include <Arcane/Build/Toolchain.hpp>` to the file's includes.

Replace `RegenerateSolution` (:2718-2741) with:

```cpp
    bool EditorApp::RegenerateSolution()
    {
        const Arcane::Project* proj = m_runtime->CurrentProject();
        if (!proj)
            return false;
        // `arcbuild generate` against the RUNNING editor's SDK -- the same
        // driver, same --sdk rule as StartModuleRebuild, minus msbuild;
        // synchronous, well under a second.
        const std::filesystem::path exeDir = ModuleBuild::ExeDir();
        const std::filesystem::path driver = ModuleBuild::ResolveDriver(exeDir);
        if (driver.empty())
        {
            ARC_ERROR("Build: arcbuild.exe not found beside the editor nor in ../arcbuild/ -- "
                      "build the engine workspace (Arcane.slnx) first");
            return false;
        }
        ModuleBuild::DriverInputs in;
        in.driverExe     = driver;
        in.projectRoot   = proj->Root();
        in.sdkRoot       = ModuleBuild::SdkRootFromExeDir(exeDir);
        in.command       = "generate";
        in.configuration = ModuleBuild::Configuration();
        const std::string cmd = ModuleBuild::ComposeDriverCommand(in);
        ARC_INFO("Build: {}", cmd);
        const ModuleBuild::CaptureResult gen = ModuleBuild::RunCapture(cmd);
        for (const std::string& line : gen.lines)
            ARC_INFO("Build: {}", line);
        if (!gen.exit || *gen.exit != 0)
        {
            ARC_ERROR("Build: arcbuild generate exited with {}", gen.exit ? std::to_string(*gen.exit) : "no exit code");
            return false;
        }
        return true;
    }
```

In `PollModuleBuild`, replace the severity rule (:2830-2833) with:

```cpp
            // Severity COLORING only -- v1 deliberately does not parse MSVC
            // diagnostics into per-line locators (arc non-goal); these
            // contains-checks just pick the Console severity lane for the
            // raw line. Lines arrive prefixed by the driver ([premake] /
            // [msbuild] / [arcbuild]), so premake's own "Error: ..." and the
            // driver's "[arcbuild] error: ..." refusals are matched after the
            // prefix, not at column 0.
            const bool isError = line.find(": error") != std::string::npos ||
                                 line.find(": fatal") != std::string::npos ||
                                 line.rfind("Error:", 0) == 0 ||
                                 line.find("] Error:") != std::string::npos ||
                                 line.find("] error:") != std::string::npos;
            const bool isWarn  = line.find(": warning") != std::string::npos;
```

and the failure message (:2852) to `ARC_ERROR("Build: arcbuild exited with {} -- rebuild failed", *exit);` (the Problems row text `"Rebuild Game Module failed (exit code N)"` stays).

- [ ] **Step 7: `EditorApp.hpp` comments** — rewrite :1732-1747 to:

```cpp
        // ---- Build -> Rebuild Game Module (ModuleBuild.hpp) -----------------
        // StartModuleRebuild resolves arcbuild.exe beside the editor (else
        // ../arcbuild/), composes `arcbuild build --project <root> --config
        // <editor's flavor> --sdk <the RUNNING editor's root>` and starts the
        // worker; the driver runs premake then msbuild, forcing /t:Rebuild
        // only when Binaries/ holds the other configuration's DLL (spec
        // 2026-09-13 s4.3). The menu item is greyed while playing/building/
        // module-less, and this re-checks the same gates for any future
        // caller. PollModuleBuild is the per-frame drain (EditorAppProject.cpp,
        // called from MainLoop's top-of-frame consume block): worker lines ->
        // "Build: "-prefixed Console entries; on finish, failure publishes ONE
        // Problems row under the "build:<root>" key and success clears it,
        // restamps a stale manifest abi (the module now matches this engine),
        // and -- when the module had been REFUSED at open (stale ABI), which
        // left no PluginHost watching -- re-engages the host the way
        // StagePluginLoad does. A LIVE host needs nothing here: the existing
        // debounced watcher (PluginHost::Poll, EndFrame) sees the fresh DLL
        // mtime and hot-reloads with state on its own; forcing it would race
        // the debounce.
```

and :1780-1788 to:

```cpp
        // Run `arcbuild generate` alone, SYNCHRONOUSLY, for the open project
        // against the running editor's SDK (ModuleBuild::ComposeDriverCommand
        // / RunCapture), its lines to the Console as "Build: ". Two callers:
        // OpenInIde when no .slnx exists yet, and MintCppClass ALWAYS (the
        // .vcxproj must list the new files before Visual Studio opens them).
        // Returns false when the driver failed (exit != 0 or the shell could
        // not start); the caller decides what that means for its own step.
        bool RegenerateSolution();
```

Also :1762-1764's `(ModuleBuild::ComposeGenerateCommand / RunCapture, …)` → `(RegenerateSolution -> arcbuild generate, …)`.

- [ ] **Step 8: premake comment** — in ArcaneTests' `files {}` replace the `ModuleBuild.cpp` entry's comment (~:823-828) with:

```lua
        -- Build -> Rebuild Game Module: ModuleBuild's PURE halves (the SDK-root
        -- walk, the arcbuild.exe candidate list, the composed driver line)
        -- source-compile into the test exe so the [editor] units drive them
        -- directly. The Runner/_wpopen half and RunCapture are compiled too;
        -- no [editor] test invokes them -- only the opt-in [build-desk] cases
        -- (BuildDriverTest.cpp) run RunCapture, against the built driver.
```

- [ ] **Step 9: Regenerate, build Debug, run GREEN**

Run (PowerShell): `cmd /c ".\GenerateProjects.bat"` (comment-only premake change — still regenerate; it is free).
Run (bash): Debug build — expect 0 errors; then
`cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[editor]" | grep -E "test cases|All tests passed|FAILED"` and `./ArcaneTests.exe "*ModuleBuild*,*Driver*,*Toolchain*"` — all green.
Grep proof that the moved names are gone from the editor: `grep -rn "ComposeRebuildCommands\|ComposeGenerateCommand\|ModuleBuild::DiscoverSolution\|ModuleBuild::VsWhere\|ResolveMsBuild\|SetSdkEnv" ArcaneEditor/src ArcaneTests/src` → no hits.

- [ ] **Step 10: The editor path, as far as it can be driven without a click**

Launch the Debug editor on the Gacha project headlessly to prove nothing regressed at boot with the new ModuleBuild in it (the golden gate in Task 7 covers rendering):
`cd bin/Debug-windows-x86_64-md/ArcaneEditor && ./ArcaneEditor.exe --project 'D:\dev\starworks\Gacha\Game' --frames 30 --report /tmp/ed-report.json 2>&1 | tail -5; grep -o '"exitReason": *"[a-z-]*"' /tmp/ed-report.json`
Expected: `exitReason: "frames"` (or the report's clean-exit value), no `Build:` error lines. The Rebuild Game Module CLICK and Build → Open Visual Studio's generate path are **desk-verify owed** (R10); the ledger says so.

- [ ] **Step 11: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add ArcaneEditor/src/Project/ModuleBuild.hpp ArcaneEditor/src/Project/ModuleBuild.cpp ArcaneEditor/src/Project/IdeLaunch.cpp ArcaneEditor/src/Project/IdeLaunch.hpp ArcaneEditor/src/App/EditorAppProject.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneTests/src/ModuleBuildTest.cpp premake5.lua && git commit -q -F - <<'EOF'
feat(editor): ModuleBuild is a spawn-and-stream shim over arcbuild.exe; RegenerateSolution = arcbuild generate; IdeLaunch resolves devenv through Arcane::Toolchain (Task 5)

The premake/msbuild composition, solution discovery, vswhere probe and the
unconditional /t:Rebuild leave the editor; the Runner + Console drain stay.
The spawned line is pinned by [editor]. Desk-verify owed: the Rebuild Game
Module click, Build > Open Visual Studio on a never-generated project, the
class wizard's regenerate.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
```

---

### Task 6: Gacha — `Game/premake5.lua` header, `scripts/setup.ps1`, the Jenkinsfile Game stage, CLAUDE.md

**Files (all in `D:\dev\starworks\Gacha`):**
- Modify: `Game/premake5.lua:1-16` (header comment)
- Modify: `scripts/setup.ps1:125-136` (game step) and `:155-158` (the Game half of `build`)
- Modify: `Jenkinsfile` — new stage after `Build Server` (:47-52)
- Modify: `CLAUDE.md` — the `## Game (Game/)` section's build snippet

**Interfaces:**
- Consumes: `arcbuild.exe` at `$ARCANE_SDK\bin\<cfg>-windows-x86_64-md\arcbuild\arcbuild.exe` (Task 3); its exit codes (0 / child's / 2 / 3).

- [ ] **Step 1: The failing check — `setup.ps1 -DryRun` still emits the same step ids, and the real `game-generate` step does not yet mention arcbuild**

Run (PowerShell): `cd D:\dev\starworks\Gacha; powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -NonInteractive -DryRun -SkipDoctor -Workspaces game | Select-String '@@WIZ'`
Expected today: `step=game-generate status=start/ok`, `done status=ok`. Then `Select-String -Path scripts\setup.ps1 -Pattern 'arcbuild'` → no match. (RED = the script has no driver call; the marker contract is the invariant to preserve.)

- [ ] **Step 2: `scripts/setup.ps1`** — replace :125-136 with:

```powershell
# --- game -----------------------------------------------------------------
# Game/ builds as an external project against the Arcane engine SDK
# (github.com/T3mps/Arcane), located via the ARCANE_SDK env var -- the same
# contract Game/premake5.lua itself enforces. The SDK's own build driver,
# arcbuild.exe (spec: $ARCANE_SDK\docs\specs\2026-09-13-arcbuild-driver-design.md),
# does the generate/build: premake first, then msbuild with /t:Rebuild only
# when Game\Binaries\ holds the other configuration's DLL. It lives in the
# SDK's build output, so the SDK must have been BUILT (Arcane.slnx) before
# Game/ can be generated. The driver's own configuration is irrelevant to
# what it builds -- prefer the one matching -Config, else whichever exists.
function Find-ArcBuild([string]$Config) {
    if (-not $env:ARCANE_SDK) {
        throw "ARCANE_SDK is not set -- point it at an Arcane engine checkout (setx ARCANE_SDK <path>)"
    }
    foreach ($cfg in @($Config, 'Debug', 'Release', 'Dist') | Select-Object -Unique) {
        $exe = "$env:ARCANE_SDK\bin\$cfg-windows-x86_64-md\arcbuild\arcbuild.exe"
        if (Test-Path $exe) { return $exe }
    }
    throw "arcbuild.exe not found under $env:ARCANE_SDK\bin -- build the Arcane SDK (Arcane.slnx) first"
}

if ($Workspaces -contains 'game') {
    Step 'game-generate' {
        & (Find-ArcBuild $Config) generate --project "$RepoRoot\Game" --config $Config
    }
}
```

and :155-158 (inside `Step 'build'`) with:

```powershell
        if ($Workspaces -contains 'game') {
            & (Find-ArcBuild $Config) build --project "$RepoRoot\Game" --config $Config
            if ($LASTEXITCODE -ne 0) { throw "Game build exit $LASTEXITCODE" }
        }
```

(The vswhere/msbuild lookup above it stays — the Server half still needs it.)

- [ ] **Step 3: `Game/premake5.lua` header** — replace :1-16 with:

```lua
-- ============================================================================
-- Aphelyon -- the game client, built as an EXTERNAL project against the Arcane
-- engine SDK (engine-as-SDK, Slice 5). This is the committed build definition
-- (the Build.cs/Target.cs analog): it declares the workspace, then consumes the
-- SDK's arcane.lua to declare the game module. ARCANE_SDK points at the engine
-- repo root (github.com/T3mps/Arcane); the SDK's own build driver, arcbuild.exe
-- (in the SDK's bin/ -- build Arcane.slnx first), generates and builds this:
--
--   setx ARCANE_SDK D:\dev\starworks\Arcane                                  (once)
--   %ARCANE_SDK%\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project . --config Debug
--                                                                   (-> Binaries/Aphelyon.dll)
--   ... generate | rebuild | clean | probe   (arcbuild --help; spec: docs/specs/2026-09-13-arcbuild-driver-design.md)
--
-- The driver runs premake first, then msbuild, and forces /t:Rebuild ONLY when
-- Binaries/ (one slot for every configuration) holds the other configuration's
-- DLL -- so a wizard-made component is one compile + a link. The editor's
-- Tools > Rebuild Game Module and scripts/setup.ps1 spawn the same exe.
--
-- Then host it:  ArcaneRuntime.exe --project <this dir>   (HostBoot resolves Binaries/Aphelyon.dll)
--
-- Generated IDE files (Aphelyon.slnx / *.vcxproj), Binaries/, Intermediate/, and
-- Saved/ are throwaway/derived -- see .gitignore.
-- ============================================================================
```

- [ ] **Step 4: Jenkinsfile** — after the `Build Server` stage (:47-52) insert:

```groovy
                stage('Game (vs ARCANE_SDK)') {
                    // Game/ builds as an external project against the Arcane engine
                    // SDK through arcbuild.exe, the engine's game-project build driver
                    // (spec: $ARCANE_SDK/docs/specs/2026-09-13-arcbuild-driver-design.md).
                    // Activates only on an agent whose ARCANE_SDK names a BUILT engine
                    // checkout (the driver lives in the SDK's own bin/); an agent without
                    // one SKIPS the stage rather than failing it -- the engine has its
                    // own pipeline. Debug then Release, so the single-slot Game/Binaries/
                    // ends holding the Release DLL; the closing `probe` is the
                    // nothing-stale check: exit 0 = the slot's CRT flavor matches
                    // Release (what the last build should have left), 3 = it does not
                    // (the build lied about its output). `bat` fails the stage on any
                    // nonzero exit, so no errorlevel handling is needed.
                    when { expression { env.ARCANE_SDK?.trim() } }
                    environment {
                        ARCBUILD_DEBUG   = "${env.ARCANE_SDK}\\bin\\Debug-windows-x86_64-md\\arcbuild\\arcbuild.exe"
                        ARCBUILD_RELEASE = "${env.ARCANE_SDK}\\bin\\Release-windows-x86_64-md\\arcbuild\\arcbuild.exe"
                    }
                    steps {
                        bat 'if not exist "%ARCBUILD_DEBUG%" ( echo [game] ARCANE_SDK is set but "%ARCBUILD_DEBUG%" is not built -- build Arcane.slnx on this agent & exit /b 1 )'
                        bat '"%ARCBUILD_DEBUG%"   build --project Game --config Debug'
                        bat '"%ARCBUILD_RELEASE%" build --project Game --config Release'
                        bat '"%ARCBUILD_RELEASE%" probe --project Game --config Release'
                    }
                }
```

and update the file's header comment (:5-8) to: `// The Arcane engine builds in its OWN multibranch pipeline over github.com/T3mps/Arcane (2026-08-11 repo extraction). Game/ (the game module vs the engine SDK) builds in the 'Game (vs ARCANE_SDK)' stage through the SDK's arcbuild.exe, on an agent that carries a BUILT ARCANE_SDK checkout; it skips elsewhere.`

- [ ] **Step 5: CLAUDE.md** — in `## Game (Game/)`, replace the fenced `bat` block (the three lines `cd Game` / premake / msbuild) with:

```bat
%ARCANE_SDK%\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project Game --config Debug   # -> Game/Binaries/Aphelyon.dll
%ARCANE_SDK%\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe probe --project Game --config Debug   # the slot verdict (exit 0 plain / 3 would-rebuild)
```

and the sentence after it to: "`arcbuild` (the engine's game-project build driver — spec `$ARCANE_SDK/docs/specs/2026-09-13-arcbuild-driver-design.md`; build `Arcane.slnx` first, it lives in the SDK's `bin/`) runs premake first, then msbuild, forcing `/t:Rebuild` only when the single-slot `Binaries/` holds the other configuration's DLL. Host it with the engine's ArcaneRuntime/ArcaneEditor (`--project Game/` / open `Game/Aphelyon.arcproj`). The editor's Tools → Rebuild Game Module spawns the same exe; the host hot-reloads the DLL on rebuild. `Game/Aphelyon.arcproj` records the project guid, `engine.abi`, `gameModule` and `bootScene`; the host's ABI gate refuses a cross-build mismatch."

- [ ] **Step 6: Verify — DryRun markers unchanged, the real generate and build go through the driver**

```powershell
cd D:\dev\starworks\Gacha
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -NonInteractive -DryRun -SkipDoctor -Workspaces game | Select-String '@@WIZ'
# expected: exactly the Step-1 markers
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -NonInteractive -SkipDoctor -SkipVcpkg -SkipDb -Workspaces game -Config Debug | Select-String '@@WIZ|\[arcbuild\]|\[premake\]'
# expected: game-generate ok, with [arcbuild] generate ... and [premake] lines
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\setup.ps1 -NonInteractive -SkipDoctor -SkipVcpkg -SkipDb -Workspaces game -Config Debug -Build | Select-String '@@WIZ|probe:|msbuild succeeded|Error\(s\)'
# expected: step=build ok, one probe row, msbuild succeeded
```

Jenkinsfile: no linter is reachable offline; review the diff by eye against the existing stages' shape (`bat` strings, `when { expression { … } }`, `environment { }`), and run `git diff Jenkinsfile | grep -c "^+"` to record the size. The stage's first live run is owed to the next push (Global Constraints: no push this session).

- [ ] **Step 7: Commit (Gacha) — the user's untracked files stay out**

```bash
cd /d/dev/starworks/Gacha && git status --short && git add Game/premake5.lua scripts/setup.ps1 Jenkinsfile CLAUDE.md && git commit -q -F - <<'EOF'
chore(game): Game/ generates and builds through the SDK's arcbuild.exe -- setup.ps1 Find-ArcBuild, premake5.lua header, Jenkinsfile 'Game (vs ARCANE_SDK)' stage, CLAUDE.md (arcbuild Task 6)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
git status --short   # TestComponent.* and test.arcscene must still be listed (untouched, uncommitted)
```

---

### Task 7: Closeout — suites, baselines, golden gate both configs, the product proof, docs, memory

**Files:**
- Modify: `scripts/automation-baselines.json`
- Modify: `docs/specs/2026-09-13-arcbuild-driver-design.md` (status line; two one-line pointers)
- Modify: `docs/plans/2026-09-13-arcbuild-driver-plan.md` (`## Closeout`)
- Create/Modify: `.superpowers/sdd/2026-09-13-arcbuild-driver/progress.md`
- Modify (memory): `C:\Users\Ethan Temprovich\.claude\projects\D--dev-starworks-Gacha\memory\project_arcane_arcbuild_driver_arc.md` + its `MEMORY.md` line

- [ ] **Step 1: ABI proof** — `cd /d/dev/starworks/Arcane && git diff --stat 05d8d63d..HEAD -- ArcaneClient ThirdParty/Astra` → empty. `grep -n "kGamePluginABIVersion = " ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` → still 28; Gacha `Game/Aphelyon.arcproj` `"abi": 28` untouched.

- [ ] **Step 2: Rebuild both configs from clean state of the engine + ReferenceProject** — Debug: `ReferenceProject.slnx -t:Rebuild` then `Arcane.slnx`; Release: the same with `-p:Configuration=Release` (if the Release `ArcaneEditor.exe` link is refused by the desk's open editor, record LNK1104 and run the Release gate on the stale editor host as the 2026-09-12 booking did — say so in the ledger).

- [ ] **Step 3: Suites + baselines**

```bash
cd /d/dev/starworks/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests   && ./ArcaneTests.exe "~[gpu]" -r json::out=/tmp/at-debug.json   | grep -E "seeded|test cases|All tests passed|FAILED"
cd /d/dev/starworks/Arcane/bin/Release-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "~[gpu]" -r json::out=/tmp/at-release.json | grep -E "seeded|test cases|All tests passed|FAILED"
cd /d/dev/starworks/Arcane && powershell -NoProfile -ExecutionPolicy Bypass -File scripts/check-baselines.ps1 2>&1 | tail -8
```
Book the measured Debug/Release `~[gpu]` counts into `scripts/automation-baselines.json` (`measured` note names this plan, the seeds, and the per-task attribution: T1 +3 cases, T2 +13, T3 +2 (SKIP), T5 rewrites ModuleBuildTest 5→5). Re-run `check-baselines.ps1` → `+0/+0 exit 0`.

- [ ] **Step 4: Golden gate, both configs**

```bash
cd /d/dev/starworks/Arcane && powershell -NoProfile -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 -Configuration Debug   2>&1 | grep -E "lane|diffCount|gatePassed|PASS|FAIL" | tail -8
cd /d/dev/starworks/Arcane && powershell -NoProfile -ExecutionPolicy Bypass -File scripts/golden-gate.ps1 -Configuration Release 2>&1 | grep -E "lane|diffCount|gatePassed|PASS|FAIL" | tail -8
```
Expected: 4/4 lanes `diffCount=0`, `gatePassed=true`, both configs.

- [ ] **Step 5: The product proof, final form (R10)** — repeat Task 3 Step 6 with the **Release** driver against the Gacha Game for `--config Release` (the desk editor's flavor): touch `Game/Source/TestComponent.cpp`, run the exact line `ComposeDriverCommand` produces for a Release editor —
`( "D:\dev\starworks\Arcane\bin\Release-windows-x86_64-md\arcbuild\arcbuild.exe" build --project "D:\dev\starworks\Gacha\Game" --config Release --sdk "D:\dev\starworks\Arcane" ) 2>&1` — through `cmd /c` and count: **1 compile, 1 link**, probe row `state=match … -> plain build`. Then, for the record, the full-rebuild case the old editor paid every time: `--force-rebuild` and count the compiles (= every TU under `Game/Source/`). Both numbers go in the closeout.

- [ ] **Step 6: Docs** — spec status line (`:3`) → `**Status:** implemented 2026-09-13 (plan docs/plans/2026-09-13-arcbuild-driver-plan.md; rulings R1–R10 there). Follows …`; §4.1 gains `(Plan ruling R1: the probes live in ArcaneCore as Arcane::Toolchain, shared with the editor's IdeLaunch.)`; §3's `probe` line gains `(exit 0 = plain-build rows, 3 = would-rebuild rows — R4)`. Append `## Closeout` to this plan: per-task shas, the measured counts, the gate results, the compile/link numbers from Steps 5, the owed list (desk clicks: Rebuild Game Module, Open Visual Studio on a never-generated project, the wizard's regenerate; the Gacha Jenkins stage's first live run; R8's "no msbuild ⇒ 2" follow-up; the Hub's Build affordance (spec §5.4)). Ledger `progress.md`: `Task 7: complete (sha)`.

- [ ] **Step 7: Memory** — rewrite `project_arcane_arcbuild_driver_arc.md`'s description + body: ARC CLOSED at `<sha>` (Arcane) / `<sha>` (Gacha), both UNPUSHED; what landed (Toolchain in ArcaneCore, arcbuild.exe, editor shim, Gacha adoption); the owed desk pass; the measured "1 compile + 1 link" number; NEXT = `GameModule.hpp` boilerplate (`ARCANE_GAME_MODULE(Name)`) per `project_arcane_source_ide_surface_arc`. Update its `MEMORY.md` line to `**ARCBUILD DRIVER — CLOSED 2026-09-13 @<sha> (desk pass OWED: the Rebuild click). NEXT: GameModule.hpp boilerplate**`. Also update `project_arcane_source_ide_surface_arc.md`'s "Gacha @ee0b19b3" / "Arcane 36 ahead" pointers to the new shas and ahead-counts (`git rev-list --count origin/main..main` in each repo — derived, never recalled).

- [ ] **Step 8: Commit (Arcane)**

```bash
cd /d/dev/starworks/Arcane && git add scripts/automation-baselines.json docs/specs/2026-09-13-arcbuild-driver-design.md docs/plans/2026-09-13-arcbuild-driver-plan.md .superpowers/sdd/2026-09-13-arcbuild-driver/progress.md && git commit -q -F - <<'EOF'
docs(arcbuild): close the driver arc -- baselines booked, spec status + R1/R4 pointers, plan closeout with the measured one-compile-one-link proof (Task 7)

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
EOF
git rev-list --count origin/main..main   # report; do NOT push
```

---

## Self-review (run at plan time; recorded here)

**Spec coverage.** §2 goals: exe beside editor/arccook + ArcaneClient link (T3 premake); five commands (T2/T3); `--config/--sdk/--action` (T2 `MakeCli`); editor thin shim (T5); scripts + Jenkinsfile (T6); testable core (T2). §2 non-goals: engine workspace untouched; no Live Coding; no auto-build; no diagnostic parsing (T5 keeps the contains-rule). §3: positional command + flags (T2/T3 main); `--project` dir-or-arcproj via `ResolveManifestFile` (T3); solution discovery beats convention (T2 `SolutionPath`); `--config` default Debug (T2); `--sdk` overrides env via `SetSdkEnv` (T3); prefixed output + exit-code table (T3, R4). §4.1 (T1 + R1); §4.2 (T2 `ComposeGenerate`, run first every build in T3); §4.3 all four rows + force + rebuild bypass + Dist→Release (T2 `ClassifySlot`/`Decide`/`ConfigWantsDebugCrt`; T3 `ProbeSlot` via `ScanFileCrtFlavor`); §4.4 (T2 `CleanTargets`, T3 `Clean` arm). §5.1 (T5, the pinned line test); §5.2 (T6); §5.3 (T6 Jenkinsfile with `probe`); §5.4 Hub — later, listed in owed. §6: `--action` is a parameter; `--engine` is not built. §7: core `[build]` (T1/T2), desk `[build-desk]` (T3), editor pin (T5), product proof (T3 Step 6 + T7 Step 5), golden 4/4 both configs (T7). §8 order: T1–T4 driver, T5 editor, T6 Gacha, T7 memory/CLAUDE.md.

**Placeholder scan.** No TBD/TODO; every code step carries its code; every command names its expected output.

**Type consistency.** `Arcane::Toolchain::{DiscoverSolution, ResolvePremake, VsWhere, ResolveMsBuild, ResolveDevenv}` used identically in T1/T3/T5. `arcbuild::{Command, Request, MakeCli, RequestFromCli, ResolveSdk, ConfigWantsDebugCrt, SlotState, SlotStateName, ClassifySlot, Verdict, Decide, kExitOk, kExitRefused, kExitProbeRebuild, ProbeExitCode, ExitFromChild, Layout, SlotPath, SolutionPath, Tools, ComposeGenerate, MsBuildTarget, ComposeMsBuild, CleanTargets}` — T2 declares, T2 tests and T3 main use the same names/signatures. `Arcane::Editor::ModuleBuild::{DriverCandidates, ResolveDriver, DriverInputs, ComposeDriverCommand, RunCapture, CaptureResult, ExeDir, SdkRootFromExeDir, Configuration, Runner}` — T5 header, T5 EditorApp, T5 test, and T3's desk cases (which use `RunCapture`/`ExeDir`, both survivors) agree.

---

## Closeout (2026-09-13)

**Commits (Arcane, unpushed):** T1 `4d689b51` Toolchain · T2 `16ef2d41` driver core · T3 `9aa331d3` arcbuild.exe + desk cases · T5 `1b364454` editor shim · T7 = this booking commit. **Gacha (unpushed):** T6 `b5733658`. Ledger: `.superpowers/sdd/2026-09-13-arcbuild-driver/progress.md`.

**ABI:** `git diff --stat 05d8d63d..HEAD -- ArcaneClient ThirdParty/Astra` empty; `kGamePluginABIVersion` 28; Gacha `engine.abi` 28 untouched.

**Suites (`~[gpu]`, FROM the exe dir, after ReferenceProject.slnx `/t:Rebuild` then Arcane.slnx, both configs, 0 errors):** Debug 56933 assertions / 1704 passed (1708 cases, 4 SKIP: 2 `[ide-desk]` + 2 `[build-desk]`), seed 1615132195; Release 56933 / 1704, seed 2096248577. `check-baselines.ps1` +351/+34 before booking (covers the never-booked source/IDE-surface arc tests too), +0/+0 exit 0 after. Per task: T1 `[build]` 10/3 · T2 `[build]` 116/15 · T3 `[build-desk]` 9/2 (with `ARCANE_BUILD_DESK=D:\dev\starworks\Gacha\Game`) · T5 `[editor]` 4271/368, arc filter 127/20.

**Golden gate:** Debug 4/4 (`diffCount=0` every lane; dx12 runtime lane `PassedOnFallback` = the standing shared-level resolution), Release 4/4 `diffCount=0`.

**The product proof (R10) — the exact line `ComposeDriverCommand` produces for the Release editor, run through `cmd /c` against the Gacha Game after touching the wizard-made `Game/Source/TestComponent.cpp`:**
```
[arcbuild] probe: slot=D:/dev/starworks/Gacha/Game/Binaries/Aphelyon.dll state=match flavor=release (MSVCP140.dll) config=Release -> plain build
compiles: 1 (TestComponent.cpp)   links: 1   0 Error(s)   Time Elapsed 00:00:04.45
```
The same line with `--force-rebuild` (what every Rebuild Game Module cost before): compiles 2 (the Game's whole `Source/`), links 1, 17.83 s. Cross-config, at Task 3 with the Debug driver: a Release slot under `--config Debug` probes `mismatch` → `/t:Rebuild` (exit 3 from `probe`); the slot was left as found (Release, `probe --config Release` exit 0).

**Headless editor boot on the Gacha Game (Debug, `--headless --frames 30`):** `exitReason: frames-complete`, exit 0, no `Build:` lines; the module load is refused as expected (Release DLL in the desk slot vs the Debug host — the CrtFlavor gate the probe shares).

**Owed (not done here):**
- Desk pass: the Tools → Rebuild Game Module CLICK on the Gacha Game (expect the Console to show the `[arcbuild] probe:` row, then one compile + one link); Build → Open Visual Studio on a never-generated project (the `arcbuild generate` path); the class wizard's regenerate after Create.
- The Gacha Jenkinsfile `Game (vs ARCANE_SDK)` stage's first live run (needs an agent with a BUILT `ARCANE_SDK`; the stage skips elsewhere). Reviewed by eye only — no offline declarative linter.
- R8: "no msbuild ⇒ exit 2" (spec §3) is not detected in v1; cmd's 9009 passes through.
- Spec §5.4: the Hub's Build affordance, if it grows one, spawns the same exe.
- Linux seam (spec §6): `--action` is a parameter; a non-msbuild build step and `--engine <root>` are not built.
