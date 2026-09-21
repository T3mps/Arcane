# Arcbuild Multibackend Hardening Design

**Date:** 2026-09-20

**Status:** Implemented (2026-09-21, Task 7 of the delivery sequence in
§12 — correctness, robustness, and documentation/verification all landed;
see this task's report,
`.superpowers/sdd/2026-09-20-arcbuild-multibackend-hardening/task-7-report.md`,
for the full acceptance evidence this line certifies).
**Supersedes:** The Windows-only backend and deferred non-MSBuild execution
statements in `2026-09-13-arcbuild-driver-design.md`. The existing CLI,
single-slot CRT rule, exit codes, and clean guarantees remain binding.

**Live-validation limits at close (all §11.2 gates otherwise green):**
- **Xcode/macOS**: resolution, context, and argument composition are unit-
  tested on every platform, but live `xcodebuild` execution requires macOS
  and was not exercised — no macOS host was available for this plan. This is
  the one gate §10/§11 always anticipated staying open on a Windows desk.
- **Make and Ninja mechanics are fully live-verified** on Windows
  (`scripts/verify-arcbuild-backends.ps1`): real generation, real child
  processes, real exit-code propagation (including a genuinely distinct
  child exit code, not a coincidental match with `kExitRefused`), and all
  four clean-precedence edge cases (§9) against a real filesystem. A
  **full compiled build** of the fixture module additionally requires a
  working compiler for each backend's Premake-beta8-selected toolset (GCC
  for `gmake`, MSVC for `ninja` on Windows — two independent tool
  installs, see the Task 7 report) — on this desk, both toolchains compile
  the fixture but fail to fully LINK it, for two separate, pre-existing,
  engine/Premake-configuration reasons unrelated to arcbuild's own
  correctness (an ArcaneCore header not yet portable to GCC, and the
  `ninja` action's generated link line omitting a Windows system import
  library `vs2026`'s MSBuild project system supplies implicitly). Both are
  newly-discovered, out-of-scope findings recorded in the Task 7 report for
  a follow-up — not a gap in arcbuild's own resolve/compose/execute/
  propagate contract, which the exit-code-fidelity checks in the verify
  script confirm directly.

## 1. Purpose

`arcbuild` has begun a decomposition from one Windows-only translation unit
into explicit request, bootstrap, pipeline, backend, process, probe, output,
and cleanup layers. The direction is sound, but the current working tree mixes
two incompatible API generations and does not compile. It also classifies
Make, Ninja, and Xcode actions without having a portable process runner, so the
reported capability exceeds the executable capability.

This change finishes the refactor in two ordered stages:

1. **Correctness:** reconcile the contracts, restore a compiling driver, and
   prove that every existing Windows/MSBuild behavior is preserved.
2. **Robustness:** make Make, Ninja, and Xcode real backend contracts with
   platform-aware tool discovery and direct, portable child-process execution.

Correctness must be green before robustness work begins. The second stage may
not weaken the first stage's slot safety, exit-code propagation, cleanup, or
output contracts.

## 2. Goals

- Preserve the five commands: `generate`, `build`, `rebuild`, `clean`, and
  `probe`.
- Preserve `--project`, `--config`, `--sdk`, `--action`, `--force-rebuild`,
  and `--quiet` behavior.
- Preserve the single-slot CRT probe: absent/matching slots permit an
  incremental build; mismatching/unreadable slots require a rebuild.
- Standardize the refactor on one vocabulary:
  `BuildBackend`, `BuildOperation`, `BackendContext`, and
  `std::expected<T, std::string>` for fallible preparation and arcbuild-level
  backend resolution.
- Give Visual Studio, GNU Make, Ninja, and Xcode explicit generation,
  discovery, build, rebuild, and clean contracts.
- Execute tools without an intermediary command shell.
- Stream merged child stdout/stderr with the established prefixes and return
  the real child exit code.
- Keep tool installation locations outside source policy. Bundled tools and
  `PATH` participate; no developer-specific absolute path is compiled in.
- Provide unit coverage for pure policy and command construction plus live
  coverage where the host platform can execute the backend.

## 3. Non-goals

- Making the entire Arcane engine source tree compile on Linux or macOS. This
  design makes `arcbuild` and its backend orchestration portable; unrelated
  engine-port failures remain owned by the Linux/macOS port milestones.
- Installing Xcode or validating `xcodebuild` on Windows. Xcode command and
  discovery policy are unit-tested on Windows and live-tested only on macOS.
- Bundling Make or Ninja into the Arcane SDK.
- Replacing Premake, introducing CMake, or creating a home-grown build graph.
- Adding the deferred `--engine` target.
- Changing the game module's single-slot `Binaries/<module>` layout.

## 4. Canonical Model

### 4.1 Request and context

`Request` remains the parsed CLI value. `Bootstrap` validates it, resolves the
SDK and project manifest, classifies the action, exports `ARCANE_SDK` to child
processes, and returns:

```cpp
struct DriverContext {
    Request request;
    BuildBackend backend = BuildBackend::None;
    std::optional<std::filesystem::path> sdkRoot;
    ProjectLayout project;
};
```

Fallible preparation returns
`std::expected<DriverContext, std::string>`. There is no parallel
`BootstrapResult` wrapper. Bootstrap resolves and exports `ARCANE_SDK` for
`generate`, `build`, `rebuild`, and `clean`. `probe` does not require an SDK,
does not modify `ARCANE_SDK`, and leaves `sdkRoot` empty because slot inspection
needs only the project manifest and module DLL.

### 4.2 Backend vocabulary

```cpp
enum class BuildBackend : std::uint8_t {
    None,
    MsBuild,
    Make,
    Ninja,
    XcodeBuild,
};

enum class BuildOperation : std::uint8_t {
    Build,
    Rebuild,
    Clean,
};

struct BackendContext {
    std::filesystem::path path;
    std::optional<std::string> scheme;
};
```

No `ActionInfo`, `GeneratorKind`, `BuildTarget`, `BuildContext`, or
`BuildDecision` synonym remains. Slot policy continues to return `Verdict`,
whose `rebuild` flag selects `BuildOperation::Rebuild`.

The controlling flow is:

```text
Request
  -> Bootstrap -> DriverContext
  -> BuildPipeline
  -> BuildExecutor
  -> Compose -> ProcessPlan
  -> ProcessRunner
```

### 4.3 Action mapping

| Premake action | Backend |
|---|---|
| `vs2022`, `vs2026` | MSBuild |
| `gmake`, `gmakelegacy` | Make |
| `ninja` | Ninja |
| `xcode4` | xcodebuild |
| all other valid identifiers | None |

Any valid Premake action may be used with `generate`. `build` and `rebuild`
refuse with exit 2 when the action has no backend. `clean` still performs the
filesystem cleanup when no backend or generated backend context exists.

When `--action` is omitted, the default is platform-specific and pinned by
tests:

| Platform | Default action |
|---|---|
| Windows | `vs2026` |
| Linux | `gmake` |
| macOS | `xcode4` |

Legacy Visual Studio actions remain valid Premake generation actions, but
arcbuild does not promise to drive their generated formats until the matching
MSBuild compatibility has been verified.

## 5. Correctness Stage

The first stage reconciles the current partial refactor without adding new
runtime capability.

- Make every declaration and definition use the canonical model in section 4.
- Compile every new pure `arcbuild` source required by unit tests into
  `ArcaneTests`; do not leave the test target on the former three-file subset.
- Update the existing decision and composition tests to the canonical APIs.
- Add tests for action classification and orchestration decisions before
  changing production behavior.
- Preserve these observable behaviors:
  - `build` always generates first;
  - a generation failure prevents the backend build;
  - `--force-rebuild` and `rebuild` select a full rebuild;
  - `probe` never resolves or executes build tools;
  - `probe` prints even under `--quiet`;
  - child exit codes pass through;
  - filesystem cleanup runs after a backend-clean failure;
  - cleanup failure outranks backend-clean failure in the returned status;
  - errors are never silenced by `--quiet`.
- Add the deliberate probe exception to the old all-commands SDK rule:
  `arcbuild probe --project <project>` succeeds without `--sdk` or
  `ARCANE_SDK` when the project and slot are readable.
- Build the `arcbuild` target and run focused `[build]` tests before starting
  the robustness stage.

The correctness stage should be committed separately so later backend work
cannot hide a regression in the refactor repair.

## 6. Portable Process Execution

### 6.1 Process specification and plan

Shell command strings are replaced by structured process specifications:

```cpp
struct ProcessSpec {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::optional<std::filesystem::path> workingDirectory;
};

struct ProcessPlan {
    std::vector<ProcessSpec> steps;
};
```

Generation composition returns one `ProcessSpec`. Backend composition returns
a `ProcessPlan`:

```cpp
ProcessSpec ComposeGenerate(...);

ProcessPlan ComposeBuild(
    BuildBackend backend,
    const std::filesystem::path& builder,
    const BackendContext& context,
    std::string_view config,
    BuildOperation operation);
```

Build, Clean, and MSBuild/Xcode Rebuild produce one step. Make/Ninja Rebuild
produce a clean step followed by a build step. `BuildExecutor` executes plan
steps sequentially and stops on the first non-zero child exit or process
error. A rendering helper produces the human-readable command shown in logs
and unit tests. The rendered form is not executed.

This removes `cmd.exe /c`, `/bin/sh -c`, embedded `cd`, and shell quoting from
the execution boundary. The validated Premake action is passed as an argument,
not concatenated into executable text.

### 6.2 Process result

Process launch/setup failure is distinct from a successfully launched child
returning a non-zero status:

```cpp
struct ProcessError {
    std::string message;
};

using ProcessResult = std::expected<int, ProcessError>;

ProcessResult Run(
    const ProcessSpec& process,
    std::string_view prefix);
```

A child that returns 2 produces `ProcessResult{2}`. Failure to launch or wait
for the child produces `std::unexpected(ProcessError{...})`; `BuildExecutor`
reports the message and translates it to the driver's `kExitRefused`. This
prevents a child exit code from being mistaken for a driver-originated refusal.

### 6.3 Windows runner

The Windows runner uses `CreateProcessW` with:

- `lpApplicationName` set to the resolved executable path;
- a separate mutable `lpCommandLine` containing the correctly quoted argument
  command line;
- an explicitly constructed Windows command line using the documented
  backslash/quote escaping rules;
- an optional `lpCurrentDirectory` from `workingDirectory`;
- one inheritable pipe attached to both stdout and stderr;
- non-inheritable parent handles and an explicit
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, so only the intended child pipe handles
  are inherited even though process creation enables handle inheritance;
- line streaming through `Output::Child`;
- `WaitForSingleObject` and `GetExitCodeProcess`;
- deterministic closure of process, thread, and pipe handles on every exit.

Failure to create or wait for the child is a driver refusal (exit 2), with a
diagnostic containing the platform error.

### 6.4 POSIX runner

Linux and macOS use `pipe`, `fork`, `dup2`, optional `chdir`, `execvp`, and
`waitpid`. Arguments are passed as an `argv` array. The child writes stdout and
stderr to the same pipe. Normal exit returns `WEXITSTATUS`; signal termination
returns `128 + signal`.

A second close-on-exec error pipe communicates child-side `chdir`, `dup2`, and
`execvp` failures to the parent. Successful `execvp` closes this pipe through
`FD_CLOEXEC`; a reported `errno` becomes `ProcessError`, not a synthetic child
exit such as 126 or 127. Parent-side `pipe`, `fork`, read, and `waitpid`
failures also become `ProcessError` with the relevant `errno` text.

The driver itself must compile without Win32 headers on POSIX.

## 7. Toolchain Resolution

`Arcane::Toolchain` is the single owner of tool discovery.

### 7.1 Common rules

- The low-level `Arcane::Toolchain` API retains its filesystem-path convention:
  it returns an absolute path when it discovers a concrete file and an empty
  path when a tool is not found.
- `arcbuild::BackendResolver` wraps that API and returns
  `std::expected<std::filesystem::path, std::string>`. Its error names the
  missing backend tool and platform. This keeps the canonical arcbuild API
  explicit without forcing ArcaneCore's low-level utility to adopt arcbuild's
  error vocabulary.
- The arcbuild-level signatures are explicit at each fallible boundary:

  ```cpp
  std::expected<std::filesystem::path, std::string>
  ResolvePremake(const std::filesystem::path& sdkRoot);

  std::expected<std::filesystem::path, std::string>
  ResolveBuilder(BuildBackend backend);

  std::expected<BackendContext, std::string>
  ResolveBackendContext(
      BuildBackend backend,
      const ProjectLayout& project);
  ```

- Toolchain does not return an optimistic bare name that makes an unavailable
  tool appear installed.
- PATH lookup is platform-aware (`PATHEXT` on Windows, executable permission on
  POSIX) and is factored through a pure-enough helper that tests can exercise
  with an explicit search path.
- Local machine layouts such as `D:\dev\_shared\tools` are not hardcoded.
  Adding that directory to `PATH` makes its tools discoverable.

### 7.2 Resolver precedence

- **Premake:** `<sdk>/ThirdParty/premake5/premake5[.exe]`, then `premake5` on
  PATH. Arcane's bundled copy therefore wins over the global installation.
- **MSBuild:** the existing `vswhere` query, then `msbuild` on PATH.
- **Make:** on Windows, `mingw32-make` then `make`; on POSIX, `make`.
- **Ninja:** `ninja` on PATH.
- **xcodebuild:** `/usr/bin/xcodebuild`, then `xcodebuild` on PATH, on macOS
  only; empty on Windows and Linux. Toolchain does not spawn `xcrun` and does
  not depend on arcbuild's process runner.

Tool lookup happens only for commands that need the tool. `probe` remains
independent of all generator/backend installations.

## 8. Backend Contracts

### 8.1 Premake generation

Every backend uses the same process shape:

- executable: resolved Premake;
- arguments: one validated action token;
- working directory: project root.

### 8.2 MSBuild

- Context: discovered `.slnx`/`.sln`, falling back to `<name>.slnx` only when
  that file exists.
- Build: `<solution> /p:Configuration=<config> /m /nologo`.
- Rebuild: add `/t:Rebuild`.
- Clean: add `/t:Clean`.

### 8.3 Make

- Context: project root containing `Makefile`.
- Configuration: lowercase Premake configuration name (`debug`, `release`, or
  `dist`). The generated Makefile is the authority; focused tests pin the
  expected spelling for Arcane's workspace.
- Build: `-C <root> config=<config>`.
- Rebuild: a two-step process plan containing clean followed by build;
  the build runs only when clean succeeds.
- Clean: `-C <root> config=<config> clean`.

### 8.4 Ninja

- The generated context and configuration model are established from a real
  game-project fixture produced by Arcane's bundled Premake `ninja` action.
  Composition is not considered implemented until that fixture pins the actual
  files and invocation expected from Premake 5.0.0-beta8.
- Build, rebuild, and clean then use the verified Ninja-native invocations;
  rebuild is a two-step clean/build process plan, and build runs only after a
  successful clean.

### 8.5 Xcode

- The exact `.xcworkspace`/`.xcodeproj` artifact, target or scheme selection,
  and configuration spelling are established from an Arcane game fixture
  generated for macOS by the bundled Premake. These details become the backend
  convention only after the fixture verifies Premake's output;
  project-name-as-scheme is not assumed. Live `xcodebuild` verification still
  requires macOS.
- Build, rebuild, and clean process plans use that verified convention.
- Resolution/execution is refused outside macOS, while pure command tests run
  on every platform.

## 9. Pipeline and Error Semantics

`BuildPipeline` remains the command orchestrator:

- `generate`: execute only Premake.
- `build`: require a backend, generate, inspect the slot unless forced, then
  execute build or rebuild.
- `rebuild`: require a backend, generate, then rebuild.
- `clean`: attempt backend clean when resolvable, then always run project
  filesystem cleanup.
- `probe`: inspect and report the slot without resolving an SDK or tools.

For a multi-process backend operation, the first failing child stops that
operation and its exit code is returned. Clean remains the exception described
above: project filesystem cleanup always follows backend clean.

Failure to resolve a builder or generated backend context during `clean` is a
soft backend-clean skip and is logged. Project filesystem cleanup still runs;
if it succeeds, the command returns success. A filesystem cleanup failure
returns `kExitRefused`. When a backend process was launched and failed, its
exit code is returned unless filesystem cleanup also fails, in which case the
filesystem refusal takes precedence.

## 10. Installation and Local Validation

The implementation may install Ninja and a suitable GNU Make distribution
under `D:\dev\_shared\tools`, as authorized by the repository owner. Any
installed executable directory must participate through the user's PATH rather
than a source-code constant.

Installation is not a product prerequisite: CI and other developers may use
their platform package manager. Xcode is not installable or live-verifiable on
Windows.

Local validation consists of:

- bundled and global Premake resolution;
- MSBuild generation/build/probe against a real game project;
- Ninja generation and a live operation when the generated workspace and
  installed compiler support it;
- Make generation plus live invocation when a compatible Make/compiler pair
  exists;
- pure Xcode resolution/context/argument tests;
- an explicit report of any backend whose live build requires another OS or a
  compiler port outside this design.

## 11. Tests and Acceptance Criteria

### 11.1 Unit and integration tests

- Action-to-backend table, including unknown generate-only actions.
- SDK, project, and environment preparation errors.
- PATH lookup precedence and platform suffix behavior.
- Process-spec construction for generation and every backend operation.
- Process-plan construction, including two-step Make/Ninja rebuilds.
- Process-error versus child-exit propagation.
- Windows argument escaping, including spaces, quotes, trailing backslashes,
  and non-ASCII paths.
- Windows application-path binding and restricted handle inheritance.
- POSIX exec-error-pipe interpretation.
- Pipeline ordering and stop-on-first-failure behavior through injected
  execution functions or a small runner interface.
- Slot decisions and probe exit codes.
- Quiet/error/child-output behavior.
- Backend-clean and filesystem-clean error precedence.
- Existing desk tests for real project probe/generation.
- Platform-default action selection.
- Probe operation without an SDK environment.

### 11.2 Completion gates

The change is complete when:

1. The correctness-stage commit builds `arcbuild` and passes focused `[build]`
   tests before robustness code is applied.
2. The final Debug and Release `arcbuild` targets build with zero errors.
3. Focused `[build]` tests pass with no unexpected warnings.
4. Existing MSBuild game-project build, rebuild, clean, and probe behavior is
   preserved.
5. Make and Ninja each have a complete resolver, verified generated-context,
   build, rebuild, clean, and diagnostic contract. Xcode has the same contract
   backed by a macOS-targeted generated fixture rather than an assumed scheme
   layout.
6. Windows uses direct process execution; POSIX code compiles behind its native
   platform guards and has unit coverage for platform-independent policy.
7. Missing tools produce clear exit-2 refusals instead of deferred shell
   “command not found” failures.
8. No developer-specific path is present in committed source.
9. Generated project/intermediate artifacts are ignored and are not committed.

## 12. Delivery Sequence

1. **Correctness commit:** tests first, canonical API reconciliation, Windows
   behavior restored, `arcbuild` and `[build]` green.
2. **Robustness commit(s):** tests first, structured processes and platform
   runners, formal tool discovery, then Make/Ninja/Xcode backend contracts.
3. **Documentation/verification commit:** update the original arcbuild docs and
   repository guidance, record platform validation limits, and run final gates.

The second sequence does not begin until the first sequence is independently
green and reviewable.
