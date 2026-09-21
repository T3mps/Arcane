# Arcbuild Multibackend Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore the partially refactored `arcbuild` driver to a provably correct Windows/MSBuild baseline, then add direct portable process execution and real Make, Ninja, and Xcode backend contracts without weakening slot, cleanup, output, or exit-code behavior.

**Architecture:** Preserve the approved flow `Request -> Bootstrap -> DriverContext -> BuildPipeline -> BuildExecutor -> Compose -> ProcessPlan -> ProcessRunner`. Land the work in two hard-gated stages: Tasks 1-2 reconcile the existing refactor and must leave `arcbuild` plus focused `[build]` tests green; Tasks 3-7 then add explicit tool discovery, structured process plans, native runners, backend fixtures, and final live validation.

**Tech Stack:** C++23 (`std::expected`, `std::filesystem`), Catch2, Premake 5.0.0-beta8, MSBuild/Visual Studio 2026, Win32 `CreateProcessW`, POSIX `fork`/`execvp`/`waitpid`, GNU Make, Ninja, Xcode.

**Spec:** `docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md`

## Global Constraints

- Work in an isolated Arcane worktree. The current main checkout contains user-owned modified and untracked files; never reset, clean, overwrite, or fold unrelated paths into these commits.
- Preserve `/MD`, the single `Binaries/<gameModule>` slot, exit codes `0`/`2`/`3`, child-exit pass-through, and the existing command/flag surface.
- Tasks 1-2 are the correctness stage. Do not begin Task 3 until both the `arcbuild` target and focused `[build]` tests are green and the correctness commit is independently reviewable.
- Use tests before production changes in every task. Confirm the named RED failure, implement the smallest coherent change, rerun the narrow test, then run the task gate.
- Never execute a rendered command through `cmd.exe`, PowerShell, `/bin/sh`, `_popen`, `_wpopen`, or `system`. Rendering exists only for logs and assertions.
- `Arcane::Toolchain` returns an absolute path or an empty path. Only `arcbuild::BackendResolver` turns missing tools/context into `std::expected<..., std::string>` errors.
- `probe` is the sole command allowed to bootstrap without an SDK. It must not read, resolve, or set `ARCANE_SDK` and must print its verdict even under `--quiet`.
- `clean` treats a missing backend, builder, or generated context as a soft backend-clean skip, always attempts filesystem cleanup, and returns filesystem cleanup failure in preference to any backend result.
- Do not hardcode `D:\dev\_shared\tools` in C++ or Lua. It is a local installation location that participates only through `PATH`.
- Do not commit generated `.slnx`, `.vcxproj`, `Makefile`, `*.make`, `build.ninja`, `*.ninja`, `.xcworkspace`, `.xcodeproj`, `Binaries/`, `Intermediate/`, or compiler intermediates.
- The bundled Premake characterization performed while writing this plan established these beta8 facts and they are part of the implementation contract:
  - `gmake` generates root `Makefile` plus `<module>.make`; configuration values are lowercase.
  - `ninja` generates root `build.ninja` plus `<module>.ninja`; workspace targets are `<module>_Debug`, `<module>_Release`, and `<module>_Dist`.
  - beta8 emits duplicate Ninja rules when every configuration links directly to the same `Binaries/<module>` output. `build/arcane.lua` therefore needs a Ninja-only, configuration-unique link output followed by a copy into the canonical single slot.
  - `xcode4 --os=macosx` generates `<workspace>.xcworkspace` and `<module>.xcodeproj`, but no shared scheme. Drive `<module>.xcodeproj` with `-target <module>`, not an assumed scheme.

## Review Focus

Reviewers must explicitly verify these failure-prone seams:

1. Windows quoting preserves empty arguments, spaces, embedded quotes, trailing backslashes, and non-ASCII paths while `lpApplicationName` remains the separately resolved executable.
2. A launched child returning `2` remains child exit `2`; launch/setup/wait failure becomes `ProcessError` and is translated to driver refusal `kExitRefused`.
3. Make/Ninja rebuild is exactly two processes, stops after a failing clean step, and never executes a shell-composed `clean && build` string.
4. `probe` succeeds without any SDK environment while every other command retains the SDK requirement.
5. Filesystem clean always runs and outranks backend failure, including missing builder/context soft skips.
6. The Ninja staging workaround keeps one final hot-reload slot while removing duplicate generated outputs.
7. POSIX child-side `chdir`, `dup2`, and `execvp` failures cross the close-on-exec error pipe and cannot masquerade as child exit 126/127.

---

## Task 1: Reconcile the canonical pure model and action policy

**Files:**

- Modify: `arcbuild/src/Action.hpp`
- Modify: `arcbuild/src/Action.cpp`
- Modify: `arcbuild/src/Build.hpp`
- Modify: `arcbuild/src/DriverContext.hpp`
- Modify: `arcbuild/src/Request.hpp`
- Modify: `arcbuild/src/Request.cpp`
- Modify: `arcbuild/src/Compose.hpp`
- Modify: `arcbuild/src/Compose.cpp`
- Modify: `arcbuild/src/Driver.hpp`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`
- Modify: `premake5.lua`

**Interfaces:**

```cpp
enum class HostPlatform : std::uint8_t { Windows, Linux, MacOS };
HostPlatform CurrentHostPlatform() noexcept;
std::string_view DefaultActionFor(HostPlatform platform) noexcept;

enum class BuildBackend : std::uint8_t { None, MsBuild, Make, Ninja, XcodeBuild };
BuildBackend BackendForAction(std::string_view action) noexcept;

enum class BuildOperation : std::uint8_t { Build, Rebuild, Clean };
struct BackendContext {
    std::filesystem::path path;
    std::optional<std::string> scheme;
};

struct DriverContext {
    Request request;
    BuildBackend backend = BuildBackend::None;
    std::optional<std::filesystem::path> sdkRoot;
    ProjectLayout project;
};
```

- [ ] Add focused action/default tests before changing production code. The table must include modern VS, legacy VS, Make, Ninja, Xcode, and an arbitrary valid generator:

```cpp
TEST_CASE("arcbuild action mapping only promises verified build backends", "[build]")
{
    CHECK(BackendForAction("vs2022") == BuildBackend::MsBuild);
    CHECK(BackendForAction("vs2026") == BuildBackend::MsBuild);
    CHECK(BackendForAction("gmake") == BuildBackend::Make);
    CHECK(BackendForAction("gmakelegacy") == BuildBackend::Make);
    CHECK(BackendForAction("ninja") == BuildBackend::Ninja);
    CHECK(BackendForAction("xcode4") == BuildBackend::XcodeBuild);
    CHECK(BackendForAction("vs2019") == BuildBackend::None);
    CHECK(BackendForAction("compilecommands") == BuildBackend::None);
}

TEST_CASE("arcbuild default action is host-specific", "[build]")
{
    CHECK(DefaultActionFor(HostPlatform::Windows) == "vs2026");
    CHECK(DefaultActionFor(HostPlatform::Linux) == "gmake");
    CHECK(DefaultActionFor(HostPlatform::MacOS) == "xcode4");
}
```

- [ ] Replace stale `Layout`, `Tools`, `ActionInfo`, `GeneratorKind`, `BuildTarget`, `BuildContext`, and `BuildDecision` references in the test with `ProjectLayout`, explicit tool paths, `BuildBackend`, `BuildOperation`, `BackendContext`, and `Verdict`. Search must return no results in `arcbuild/src` or `ArcaneTests/src/BuildDriverTest.cpp`:

```powershell
rg -n "ActionInfo|GeneratorKind|BuildTarget|BuildContext|BuildDecision|\bLayout\b|\bTools\b" arcbuild/src ArcaneTests/src/BuildDriverTest.cpp
```

- [ ] Add all pure driver units to the `ArcaneTests` source list in `premake5.lua`: `Action.cpp`, `Request.cpp`, `Slot.cpp`, `ProjectLayout.cpp`, and the current string-based `Compose.cpp`. Update the adjacent comment to match.

- [ ] Regenerate and run the test build to prove RED. Expected failure: declarations/definitions currently disagree (`ActionInfo`, `BuildTarget`, `BuildContext`) and the test source uses removed names.

```powershell
.\GenerateProjects.bat
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
```

- [ ] Implement the canonical types. `BackendForAction` maps only `vs2022` and `vs2026` to MSBuild; valid but unverified actions remain `BuildBackend::None` and are still accepted by `IsValidAction` for generation.

- [ ] Make the CLI default testable: `MakeCli(std::string_view defaultAction = DefaultActionFor(CurrentHostPlatform()))`, and initialize `Request::action` from the same helper. Keep `--action` validation lexical and do not constrain generation to the known table.

- [ ] Keep the correctness-stage compositor string-based only for this gate, but make every signature use `BuildOperation` and `BackendContext`. This is intentionally replaced by structured `ProcessSpec` in Task 4.

- [ ] Regenerate, build, and run focused tests from the executable directory:

```powershell
.\GenerateProjects.bat
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
```

- [ ] Commit only the canonical pure-model change:

```powershell
git add arcbuild/src/Action.hpp arcbuild/src/Action.cpp arcbuild/src/Build.hpp arcbuild/src/DriverContext.hpp arcbuild/src/Request.hpp arcbuild/src/Request.cpp arcbuild/src/Compose.hpp arcbuild/src/Compose.cpp arcbuild/src/Driver.hpp ArcaneTests/src/BuildDriverTest.cpp premake5.lua
git commit -m "fix(arcbuild): reconcile canonical build contracts"
```

---

## Task 2: Restore orchestration correctness and establish the hard gate

**Files:**

- Modify: `arcbuild/src/Backend.hpp`
- Modify: `arcbuild/src/Backend.cpp`
- Modify: `arcbuild/src/Bootstrap.hpp`
- Modify: `arcbuild/src/Bootstrap.cpp`
- Modify: `arcbuild/src/Environment.hpp`
- Modify: `arcbuild/src/Environment.cpp`
- Modify: `arcbuild/src/Output.hpp`
- Modify: `arcbuild/src/Output.cpp`
- Modify: `arcbuild/src/Application.cpp`
- Modify: `arcbuild/src/BuildExecutor.hpp`
- Modify: `arcbuild/src/BuildExecutor.cpp`
- Modify: `arcbuild/src/Pipeline.hpp`
- Modify: `arcbuild/src/Pipeline.cpp`
- Modify: `arcbuild/src/Probe.hpp`
- Modify: `arcbuild/src/Probe.cpp`
- Modify: `arcbuild/src/ProjectCleaner.hpp`
- Modify: `arcbuild/src/ProjectCleaner.cpp`
- Modify: `arcbuild/src/main.cpp`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`
- Modify: `premake5.lua`

**Interfaces:**

```cpp
using BootstrapResult = std::expected<DriverContext, std::string>;
BootstrapResult Bootstrap::Prepare(Request request) const;

class IEnvironment {
public:
    virtual ~IEnvironment() = default;
    virtual std::optional<std::filesystem::path> ArcaneSdk() const = 0;
    virtual bool SetArcaneSdk(const std::filesystem::path& root) const = 0;
};
class Environment final : public IEnvironment {
public:
    std::optional<std::filesystem::path> ArcaneSdk() const override;
    bool SetArcaneSdk(const std::filesystem::path& root) const override;
};

class IOutput {
public:
    virtual ~IOutput() = default;
    virtual void SetQuiet(bool quiet) noexcept = 0;
    virtual void Info(std::string_view message) const = 0;
    virtual void Always(std::string_view message) const = 0;
    virtual void Error(std::string_view message) const = 0;
    virtual void Child(std::string_view prefix, std::string_view message) const = 0;
};
class Output final : public IOutput {
public:
    void SetQuiet(bool quiet) noexcept override;
    void Info(std::string_view message) const override;
    void Always(std::string_view message) const override;
    void Error(std::string_view message) const override;
    void Child(std::string_view prefix, std::string_view message) const override;
private:
    bool quiet_ = false;
};

class IBuildExecutor {
public:
    virtual ~IBuildExecutor() = default;
    virtual int Generate(const DriverContext& context) const = 0;
    virtual int Build(const DriverContext& context, BuildOperation operation) const = 0;
    virtual int CleanBackend(const DriverContext& context) const = 0;
};

class ISlotInspector {
public:
    virtual ~ISlotInspector() = default;
    virtual SlotProbe Inspect(const ProjectLayout&, std::string_view config) const = 0;
    virtual std::string Describe(const SlotProbe&, std::string_view config,
                                 const Verdict&) const = 0;
};

class IProjectCleaner {
public:
    virtual ~IProjectCleaner() = default;
    virtual int Clean(const ProjectLayout&, std::string_view config) const = 0;
};

Verdict Decide(SlotState state);
BuildOperation OperationForBuild(bool forceRebuild, const Verdict& verdict);
int MergeCleanResults(int backendExit, int filesystemExit) noexcept;
```

- [ ] Add tests for the orchestration decisions before repairing the callers:

```cpp
TEST_CASE("arcbuild selects rebuild only for forced or rebuilding slot verdicts", "[build]")
{
    CHECK(OperationForBuild(false, Decide(SlotState::Absent)) == BuildOperation::Build);
    CHECK(OperationForBuild(false, Decide(SlotState::Match)) == BuildOperation::Build);
    CHECK(OperationForBuild(false, Decide(SlotState::Mismatch)) == BuildOperation::Rebuild);
    CHECK(OperationForBuild(true, Decide(SlotState::Match)) == BuildOperation::Rebuild);
}

TEST_CASE("filesystem clean failure outranks backend clean failure", "[build]")
{
    CHECK(MergeCleanResults(0, 0) == 0);
    CHECK(MergeCleanResults(7, 0) == 7);
    CHECK(MergeCleanResults(0, kExitRefused) == kExitRefused);
    CHECK(MergeCleanResults(7, kExitRefused) == kExitRefused);
}
```

- [ ] Make `Bootstrap` depend on `IEnvironment&` and use a fake environment in temporary-project tests, without mutating the developer environment. Prove:
  - probe with a valid `.arcproj` succeeds with no SDK and never calls `SetArcaneSdk`;
  - build with neither `--sdk` nor `ARCANE_SDK` returns an error containing `no SDK`;
  - explicit `--sdk` is absolutized and exported for non-probe commands.

- [ ] Make `Application`, `BuildExecutor`, `BuildPipeline`, `ProjectCleaner`, and `ProcessRunner` depend on `IOutput&`. Use a recording fake to test that `Always` is visible under quiet mode and `Error` is never suppressed. Do not redirect global stdout inside parallel tests.

- [ ] Make `BuildExecutor`, `SlotInspector`, and `ProjectCleaner` implement the three narrow interfaces above, and make `BuildPipeline` depend on those interfaces. Use recording fakes to prove the exact order and short-circuit rules: build is `generate -> inspect -> build`; failed generation stops before inspect/build; forced build is `generate -> rebuild` without slot inspection; probe only inspects/reports; clean is `backend clean -> filesystem clean` even when backend clean fails.

- [ ] Compile `Bootstrap.cpp`, `Environment.cpp`, `Output.cpp`, `Probe.cpp`, and `ProjectCleaner.cpp` into `ArcaneTests`. Do not source-compile `main.cpp` or spawn processes in ordinary `[build]` tests.

- [ ] Run the `arcbuild` build to prove RED. Expected failure: `BootstrapResult` vs `std::expected`, path vs optional SDK, stale slot decision names, and backend declaration/definition mismatches.

```powershell
msbuild arcbuild\arcbuild.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
```

- [ ] Reconcile `Bootstrap`, `Application`, `Pipeline`, and `BuildExecutor` to the canonical types. Dereference `context.sdkRoot` only in non-probe paths after bootstrap has guaranteed it. The pipeline status line must render `against SDK <none>` for probe rather than dereferencing an empty optional.

- [ ] Keep `BackendResolver` on its temporary correctness-stage path/optional contract only as needed to restore the build; Task 3 replaces it with the approved `std::expected` boundary. Preserve bundled Premake and MSBuild behavior during this gate.

- [ ] Implement soft backend-clean skips and `MergeCleanResults`. `ProjectCleaner::Clean` must execute after `CleanBackend` regardless of backend result.

- [ ] Build the driver and tests, then run the correctness gate:

```powershell
.\GenerateProjects.bat
msbuild Arcane.slnx /t:arcbuild /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
```

- [ ] Run the no-SDK desk probe in a clean child environment. Expected exit is `0` or `3`, with one `probe:` line and no SDK/tool diagnostic:

```powershell
$driver = Resolve-Path .\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe
$project = Resolve-Path .\ReferenceProject
cmd.exe /d /c "set ARCANE_SDK=& `"$driver`" probe --project `"$project`" --quiet"
if ($LASTEXITCODE -notin 0,3) { throw "probe exit $LASTEXITCODE" }
```

- [ ] Commit the correctness stage. Do not begin Task 3 unless all commands above are green:

```powershell
git add arcbuild/src/Backend.hpp arcbuild/src/Backend.cpp arcbuild/src/Bootstrap.hpp arcbuild/src/Bootstrap.cpp arcbuild/src/Environment.hpp arcbuild/src/Environment.cpp arcbuild/src/Output.hpp arcbuild/src/Output.cpp arcbuild/src/Application.cpp arcbuild/src/BuildExecutor.hpp arcbuild/src/BuildExecutor.cpp arcbuild/src/Pipeline.hpp arcbuild/src/Pipeline.cpp arcbuild/src/Probe.hpp arcbuild/src/Probe.cpp arcbuild/src/ProjectCleaner.hpp arcbuild/src/ProjectCleaner.cpp arcbuild/src/main.cpp ArcaneTests/src/BuildDriverTest.cpp premake5.lua
git commit -m "fix(arcbuild): restore driver orchestration"
```

---

## Task 3: Formalize tool discovery and expected-based backend resolution

**Files:**

- Modify: `ArcaneCore/src/Arcane/Build/Toolchain.hpp`
- Modify: `ArcaneCore/src/Arcane/Build/Toolchain.cpp`
- Modify: `arcbuild/src/Backend.hpp`
- Modify: `arcbuild/src/Backend.cpp`
- Modify: `ArcaneTests/src/ToolchainTest.cpp`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`

**Interfaces:**

```cpp
namespace Arcane::Toolchain {
std::filesystem::path FindOnPath(
    std::string_view command,
    std::string_view searchPath,
    std::string_view pathExt = {});
std::filesystem::path ResolvePremake(const std::filesystem::path& sdkRoot);
std::filesystem::path ResolveMsBuild();
std::filesystem::path ResolveMake();
std::filesystem::path ResolveNinja();
std::filesystem::path ResolveXcodeBuild();
}

class BackendResolver {
public:
    std::expected<std::filesystem::path, std::string>
    ResolvePremake(const std::filesystem::path& sdkRoot) const;
    std::expected<std::filesystem::path, std::string>
    ResolveBuilder(BuildBackend backend) const;
    std::expected<BackendContext, std::string>
    ResolveBackendContext(BuildBackend backend, const ProjectLayout& project) const;
};
```

- [ ] Add temp-directory tests for explicit search paths. On Windows, cover `PATHEXT`, first-directory precedence, absolute normalized output, and missing-empty behavior. On POSIX, cover executable permission and ignore non-executable regular files.

```cpp
CHECK(FindOnPath("ninja", search, ".COM;.EXE;.BAT;.CMD") == expectedNinjaExe);
CHECK(FindOnPath("missing", search, ".EXE").empty());
```

- [ ] Add resolver tests proving bundled Premake wins over PATH, missing bundled Premake falls back to a concrete PATH file rather than bare `premake5`, and missing Make/Ninja/Xcode returns an empty low-level path.

- [ ] Add backend wrapper tests proving empty low-level results become descriptive `std::unexpected` values and `BuildBackend::None` is refused for build resolution.

- [ ] Run the focused tests to prove RED. Expected failure: current resolvers return optimistic bare names and `Backend.hpp` still disagrees with `Backend.cpp`.

```powershell
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
```

- [ ] Implement `FindOnPath` without spawning a shell. Split the supplied path list by the native separator; on Windows try the explicit extension first and then `PATHEXT`; on POSIX require a regular file with an executable bit. Return `std::filesystem::absolute(...).lexically_normal()` only for a real file.

- [ ] Implement exact precedence:
  - Premake: `<sdk>/ThirdParty/premake5/premake5[.exe]`, then PATH.
  - MSBuild: `vswhere`, then concrete `msbuild`/`MSBuild.exe` on PATH.
  - Make: Windows `mingw32-make` then `make`; POSIX `make`.
  - Ninja: `ninja` on PATH.
  - xcodebuild: macOS `/usr/bin/xcodebuild`, then PATH; empty elsewhere.

- [ ] Update `BuildExecutor` callers for expected-based resolver errors. Build/rebuild/generate report the resolver error and return `kExitRefused`; clean logs the error as a soft skip and continues to filesystem cleanup.

- [ ] Verify there is no compiled local path policy:

```powershell
rg -n -F "D:\dev\_shared\tools" ArcaneCore arcbuild build
```

Expected: no matches.

- [ ] Run the task gate and commit:

```powershell
msbuild Arcane.slnx /t:ArcaneCore,arcbuild,ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
git add ArcaneCore/src/Arcane/Build/Toolchain.hpp ArcaneCore/src/Arcane/Build/Toolchain.cpp arcbuild/src/Backend.hpp arcbuild/src/Backend.cpp arcbuild/src/BuildExecutor.cpp ArcaneTests/src/ToolchainTest.cpp ArcaneTests/src/BuildDriverTest.cpp
git commit -m "feat(build): formalize backend tool discovery"
```

---

## Task 4: Replace shell strings with structured backend process plans

**Files:**

- Modify: `arcbuild/src/Process.hpp`
- Modify: `arcbuild/src/Compose.hpp`
- Modify: `arcbuild/src/Compose.cpp`
- Modify: `arcbuild/src/Backend.cpp`
- Modify: `build/arcane.lua`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`
- Create: `ArcaneTests/data/arcbuild-fixture/Fixture.arcproj`
- Create: `ArcaneTests/data/arcbuild-fixture/premake5.lua`
- Create: `ArcaneTests/data/arcbuild-fixture/Source/Game/Fixture.cpp`

**Interfaces:**

```cpp
struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::optional<std::filesystem::path> workingDirectory;
};

struct ProcessPlan { std::vector<ProcessSpec> steps; };

ProcessSpec ComposeGenerate(
    const ProjectLayout& project,
    const std::filesystem::path& premake,
    std::string_view action);
ProcessPlan ComposeBuild(
    BuildBackend backend,
    const std::filesystem::path& builder,
    const BackendContext& context,
    std::string_view config,
    BuildOperation operation);
std::string RenderProcess(const ProcessSpec& process);
```

- [ ] Replace string assertions with structural tests. Pin these exact plans:
  - Premake: executable resolved Premake, arguments `{action}`, cwd project root.
  - MSBuild build: `{solution, "/p:Configuration=Debug", "/m", "/nologo"}`; rebuild/clean add `/t:Rebuild` or `/t:Clean`.
  - Make build: `{"-C", root, "config=debug"}`; clean appends `clean`; rebuild is clean then build.
  - Ninja build: `{"-C", root, "Fixture_Debug"}`; clean is `{"-C", root, "-t", "clean", "Fixture_Debug"}`; rebuild is clean then build.
  - Xcode build: `{"-project", project, "-target", "Fixture", "-configuration", "Debug", "build"}`; rebuild is one Xcode invocation ending `clean build`; clean ends `clean`.

```cpp
REQUIRE(plan.steps.size() == 2);
CHECK(plan.steps[0].arguments.back() == "clean");
CHECK(plan.steps[1].arguments == std::vector<std::string>{"-C", root.string(), "config=debug"});
```

- [ ] Add `RenderProcess` tests for readable quoting only. Explicitly state in the implementation comment that the rendered text is never executable input.

- [ ] Add the committed input-only fixture. Its manifest uses `name: Fixture`, `gameModule: Fixture.dll`, and `sourceDir: Source/Game`; its Premake file declares Debug/Release/Dist, includes `$ARCANE_SDK/build/arcane.lua`, and calls `arcane_game_module("Fixture")`.

- [ ] Prove the current Ninja generation is RED by generating a temporary copy and inspecting `<module>.ninja`: beta8 emits three link edges for the same `Binaries/Fixture.dll` output.

```powershell
$fixtureSource = Resolve-Path .\ArcaneTests\data\arcbuild-fixture
$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('arcbuild-ninja-red-' + [guid]::NewGuid().ToString('N'))
Copy-Item -LiteralPath $fixtureSource -Destination $fixtureRoot -Recurse
$env:ARCANE_SDK = (Resolve-Path .).Path
Push-Location $fixtureRoot
& (Join-Path $env:ARCANE_SDK 'ThirdParty\premake5\premake5.exe') ninja
Select-String -Path .\Fixture.ninja -Pattern '^build Binaries/Fixture'
Pop-Location
```

Expected before the fix: three matches. The generated files live only under the printed unique temp root; do not copy or stage them.

- [ ] In `build/arcane.lua`, add an `action:ninja` filter that links to a configuration-unique derived location such as `Intermediate/Ninja/%{cfg.buildcfg}/Binaries`, then copies the completed module to `%{wks.location}/Binaries/%{cfg.buildtarget.name}` in a post-build command. Keep the normal target directory unchanged for other actions. The generated `.ninja` file must now have unique link outputs while each successful build updates the canonical slot.

- [ ] Resolve backend contexts from real artifacts:
  - Make: root containing `Makefile`.
  - Ninja: root containing both `build.ninja` and `<module-stem>.ninja`; derive target `<module-stem>_<Config>` at composition time.
  - Xcode: `<module-stem>.xcodeproj` must be a directory; store `<module-stem>` in `BackendContext::scheme`, but compose it as `-target` because beta8 creates no shared scheme.

- [ ] Regenerate the fixture for `gmake`, `ninja`, and `--os=macosx xcode4`; assert the characterized file names and inspect Ninja for unique link outputs. Remove generated outputs afterwards.

- [ ] Run structural tests and commit:

```powershell
.\GenerateProjects.bat
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
git add arcbuild/src/Process.hpp arcbuild/src/Compose.hpp arcbuild/src/Compose.cpp arcbuild/src/Backend.cpp build/arcane.lua ArcaneTests/src/BuildDriverTest.cpp ArcaneTests/data/arcbuild-fixture
git commit -m "feat(arcbuild): compose structured backend plans"
```

---

## Task 5: Implement the Windows native runner and sequential executor

**Files:**

- Modify: `arcbuild/src/Process.hpp`
- Modify: `arcbuild/src/Process.cpp`
- Modify: `arcbuild/src/BuildExecutor.hpp`
- Modify: `arcbuild/src/BuildExecutor.cpp`
- Modify: `arcbuild/src/Exit.hpp`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`
- Create: `ArcaneTests/process-fixture/ProcessFixtureMain.cpp`
- Modify: `premake5.lua`

**Interfaces:**

```cpp
struct ProcessError { std::string message; };
using ProcessResult = std::expected<int, ProcessError>;

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    virtual ProcessResult Run(const ProcessSpec&, std::string_view prefix) const = 0;
};

class ProcessRunner final : public IProcessRunner {
public:
    explicit ProcessRunner(Output& output);
    ProcessResult Run(const ProcessSpec&, std::string_view prefix) const override;
};

int ExecutePlan(const ProcessPlan& plan, IProcessRunner& runner,
                IOutput& output, std::string_view prefix);
```

- [ ] Add pure Windows quoting tests for `QuoteWindowsArgument`/`BuildWindowsCommandLine`. Cover `""`, spaces, embedded quotes, one and multiple trailing backslashes before a closing quote, and a non-ASCII path. The expected command line must follow the Microsoft C runtime parsing rules.

- [ ] Add a separate `arcbuild-process-fixture` console project from `ArcaneTests/process-fixture/ProcessFixtureMain.cpp`; do not add this `main` to the `ArcaneTests` file glob. The fixture prints each received UTF-8 argument as one numbered line, optionally prints a stderr line, reports its cwd, tests a numeric Windows handle passed with `--probe-handle`, and exits from `--exit <n>`. Use it for `[build]` integration tests of exact argv, merged stdout/stderr, cwd, Unicode, and child exit propagation.

- [ ] Add two Windows contract tests beyond ordinary argv echoing:
  - Put a same-named decoy executable earlier on `PATH`, pass the fixture's absolute path in `ProcessSpec::executable`, and prove the fixture runs. This pins `lpApplicationName` binding.
  - Create an unrelated inheritable sentinel handle, pass its numeric value as ordinary text to the fixture, and prove the child reports it invalid. The stdout/stderr pipe remains functional. This pins `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` rather than broad handle inheritance.

- [ ] Add a fake `IProcessRunner` test that records specs and returns configured results. Prove a two-step plan stops after the first non-zero result and a child exit `2` is returned unchanged:

```cpp
fake.results = { ProcessResult{9}, ProcessResult{0} };
CHECK(ExecutePlan(plan, fake, output, "[ninja]") == 9);
CHECK(fake.seen.size() == 1);

fake.results = { ProcessResult{2} };
CHECK(ExecutePlan(oneStep, fake, output, "[tool]") == 2);
```

- [ ] Add a launch-error test using a guaranteed-missing absolute executable. Assert `ProcessResult` is unexpected and `ExecutePlan` returns `kExitRefused`, not a synthetic child code.

- [ ] Implement Windows `Run` with `CreateProcessW`:
  - UTF-8/UTF-16 conversion that reports conversion errors.
  - `lpApplicationName = spec.executable.c_str()` as an absolute wide path.
  - a separate mutable command-line buffer containing quoted executable plus arguments.
  - optional `lpCurrentDirectory`.
  - one pipe assigned to both stdout and stderr.
  - non-inheritable parent read handle.
  - `STARTUPINFOEXW` plus `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` containing only the child write handle.
  - streaming reads into `Output::Child`, including a final unterminated line.
  - `WaitForSingleObject`, `GetExitCodeProcess`, and deterministic RAII cleanup for every handle and attribute list.

- [ ] Migrate `BuildExecutor::Generate`, `Build`, and `CleanBackend` to `ProcessSpec`/`ProcessPlan`. Log `RenderProcess(step)` before each launch; execute the structured step. Resolver/process errors return `kExitRefused`; a child exit passes through.

- [ ] Run the Windows runner gate, then a real MSBuild generation/build/probe against `ReferenceProject`:

```powershell
.\GenerateProjects.bat
msbuild Arcane.slnx /t:arcbuild,ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
& .\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe build --project .\ReferenceProject --config Debug --action vs2026
& .\bin\Debug-windows-x86_64-md\arcbuild\arcbuild.exe probe --project .\ReferenceProject --config Debug --quiet
if ($LASTEXITCODE -notin 0,3) { throw "probe exit $LASTEXITCODE" }
```

- [ ] Confirm no shell execution boundary remains:

```powershell
rg -n "_popen|_wpopen|system\(|cmd\.exe /c|/bin/sh" arcbuild/src
```

Expected: no matches.

- [ ] Commit:

```powershell
git add arcbuild/src/Process.hpp arcbuild/src/Process.cpp arcbuild/src/BuildExecutor.hpp arcbuild/src/BuildExecutor.cpp arcbuild/src/Exit.hpp ArcaneTests/src/BuildDriverTest.cpp ArcaneTests/process-fixture/ProcessFixtureMain.cpp premake5.lua
git commit -m "feat(arcbuild): execute native Windows process plans"
```

---

## Task 6: Implement the POSIX runner and portable compile contract

**Files:**

- Modify: `arcbuild/src/Process.cpp`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`
- Modify: `premake5.lua`
- Create: `scripts/verify-arcbuild-posix.sh`

- [ ] Add POSIX-only integration cases using the process fixture or `/bin/sh` as the child executable (never as an intermediary): normal exit, signal termination mapped to `128 + signal`, missing executable, missing working directory, merged stdout/stderr, and an argument containing spaces/quotes passed unchanged.

- [ ] Add a focused test seam for decoding the child error-pipe record:

```cpp
enum class ChildSetupStage : std::uint8_t { Chdir, DupStdout, DupStderr, Exec };
struct ChildSetupError { ChildSetupStage stage; int errorNumber; };
ProcessError DescribeChildSetupError(const ChildSetupError& error);
```

Assert that `ENOENT` at `Exec` produces an unexpected process result and cannot be returned as child exit `127`.

- [ ] Implement the POSIX branch with two pipes. Set `FD_CLOEXEC` on the error-pipe write end before `fork`. In the child, report `{stage, errno}` with an EINTR-safe write for `chdir`, `dup2`, or `execvp` failure, then `_exit(127)`. In the parent, EOF on the error pipe means `execvp` succeeded; a record means launch failure.

- [ ] Make parent reads and `waitpid` EINTR-safe. Return `WEXITSTATUS`, `128 + WTERMSIG`, or `ProcessError` for platform/setup failures. Close every inherited descriptor on all parent and child paths.

- [ ] Keep Win32 headers and APIs wholly under `_WIN32`; keep POSIX headers wholly outside it. Add `scripts/verify-arcbuild-posix.sh` to generate `gmake` on Linux/macOS, build `arcbuild` and the process-focused tests, then run `[build]`.

- [ ] On Windows, run the full compile/test regression. On the first Linux/macOS environment available, run the script and record the command/output in the final verification notes. Lack of that host is a documented live-validation limit, not permission to weaken the implementation or tests.

```powershell
msbuild Arcane.slnx /t:arcbuild,ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
```

- [ ] Commit:

```powershell
git add arcbuild/src/Process.cpp ArcaneTests/src/BuildDriverTest.cpp premake5.lua scripts/verify-arcbuild-posix.sh
git commit -m "feat(arcbuild): add POSIX process execution"
```

---

## Task 7: Install local tools, run backend acceptance, and update docs

**Files:**

- Modify: `.gitignore`
- Modify: `docs/specs/2026-09-13-arcbuild-driver-design.md`
- Modify: `docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md`
- Modify: `CLAUDE.md`
- Modify: `README.md`
- Modify: `ArcaneTests/src/BuildDriverTest.cpp`
- Create: `scripts/verify-arcbuild-backends.ps1`

- [ ] Add root ignore rules for nested generated MSBuild directories (`arcbuild/arcbuild/`) and the fixture's generated Make/Ninja/Xcode artifacts. Do not ignore the committed fixture inputs.

- [ ] If `Get-Command ninja` or `Get-Command mingw32-make,make` is still missing, install the verified Winget packages into the shared tools tree and add their executable directories to the user PATH:

```powershell
winget install --id Ninja-build.Ninja -e --location D:\dev\_shared\tools\ninja
winget install --id GnuWin32.Make -e --location D:\dev\_shared\tools\make
$sharedToolDirs = @('D:\dev\_shared\tools\ninja', 'D:\dev\_shared\tools\make\bin')
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
$pathParts = @($userPath -split ';' | Where-Object { $_ })
foreach ($dir in $sharedToolDirs) {
    if ($pathParts -notcontains $dir) { $pathParts += $dir }
    if (($env:Path -split ';') -notcontains $dir) { $env:Path += ";$dir" }
}
[Environment]::SetEnvironmentVariable('Path', ($pathParts -join ';'), 'User')
Get-Command premake5,ninja,make,mingw32-make -ErrorAction SilentlyContinue
```

The implementation report must name which Make executable was found. GnuWin32 Make 3.81 is sufficient for the generated beta8 fixture only if its live invocation passes; if it cannot handle the generated file, record the incompatibility and install a newer MinGW/MSYS2 GNU Make before claiming Make acceptance.

- [ ] Add an opt-in `[build-generator]` test that copies `ArcaneTests/data/arcbuild-fixture` to a unique temp directory, runs bundled Premake through `ProcessRunner`, and checks the characterized artifacts for `gmake`, `ninja`, and `--os=macosx xcode4`. The test must delete only its resolved unique temp directory and must never modify the committed fixture.

- [ ] Create `scripts/verify-arcbuild-backends.ps1`. It must copy the committed input fixture to a unique directory below the system temp root, validate that the resolved destination remains below that root, run generation/build/rebuild/clean there, and remove only that validated unique directory in `finally`. Because Ninja's generated MSVC rules require a Visual Studio developer environment, the script must refuse with a clear message when `cl.exe` does not resolve. Verify Make and Ninja build, rebuild, and clean; the fake-runner test remains the proof that a failing rebuild-clean step suppresses its build step.

```powershell
Get-Command cl,ninja,make,mingw32-make -ErrorAction SilentlyContinue
.\scripts\verify-arcbuild-backends.ps1 -Configuration Debug
```

- [ ] Verify clean edge cases with the temp fixture: missing builder soft skip plus successful filesystem clean returns `0`; backend child failure plus successful filesystem clean returns the child code; filesystem failure returns `2` regardless of backend result.

- [ ] Update the original 2026-09-13 design and repository guidance with platform defaults, direct-process semantics, the supported backend table, tool prerequisites, probe's SDK exception, and the verified Xcode target convention. Mark the 2026-09-20 design `Implemented` only after the gates pass; explicitly record that live Xcode execution still requires macOS.

- [ ] Build Debug and Release driver/test targets with zero errors, then run focused tests in both configurations:

```powershell
.\GenerateProjects.bat
msbuild Arcane.slnx /t:arcbuild,ArcaneTests /p:Configuration=Debug /p:Platform=x64 /m /nologo /v:minimal
msbuild Arcane.slnx /t:arcbuild,ArcaneTests /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
Push-Location .\bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
Push-Location .\bin\Release-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[build]"
Pop-Location
```

- [ ] Re-run real MSBuild build, rebuild, clean, and probe against a disposable copy of `ReferenceProject`. Verify the existing Windows flow, child exit propagation, slot behavior, and cleanup precedence.

- [ ] Confirm generated artifacts and local tools are not staged:

```powershell
git status --short
git diff --cached --name-only
rg -n -F "D:\dev\_shared\tools" ArcaneCore arcbuild build
```

Expected: only source/docs/test/ignore changes intended by this task are staged, and the path search has no matches.

- [ ] Commit documentation and final acceptance changes:

```powershell
git add .gitignore docs/specs/2026-09-13-arcbuild-driver-design.md docs/specs/2026-09-20-arcbuild-multibackend-hardening-design.md CLAUDE.md README.md ArcaneTests/src/BuildDriverTest.cpp scripts/verify-arcbuild-backends.ps1
git commit -m "docs(build): record multibackend arcbuild contract"
```

- [ ] Request a final code review centered on the seven Review Focus items, fix every HIGH/CRITICAL finding, rerun the Debug/Release gates, and only then report completion.
