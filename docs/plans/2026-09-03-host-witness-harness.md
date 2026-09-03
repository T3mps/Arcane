# Host Witness Harness (Arc B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove, with Catch2 cases that spawn the real staged hosts, that the three
never-observed host facts actually occur — settle bounds genuinely spent, capture-path failure
recorded, missing reference reported with its search space — and add `compare.triedPaths` to the
verify report.

**Architecture:** Out-of-process grader in ArcaneTests (`[gpu][witness]` cases + a CreateProcess
spawn helper), Gauntlet's precedence imitated, fresh-scratch-copy hygiene, one shared-code C++
change (TriedPaths, report schema 4→5, plugin ABI 19→20).

**Tech Stack:** C++23 / Catch2 3.15.0 / Win32 CreateProcess / nlohmann::json / premake5 (no
premake edits needed — ArcaneTests globs `src/**`).

**Spec:** `docs/specs/2026-09-03-host-witness-harness-design.md` — read it first; conflicts
resolve against it.

## Global Constraints

- **Build:** `msbuild Arcane.slnx /p:Configuration=<Debug|Release|Dist> /m` from the repo root,
  VS 18 msbuild. Never build a bare `.vcxproj`. `ARCANE_SDK` can be stale in the process
  environment — override per-invocation.
- **Zero warnings.** All three configs build with 0 `warning C`; a gate, not an aspiration.
- **Tests run FROM the exe directory:** `cd bin/<Config>-windows-x86_64-md/ArcaneTests` then
  `.\ArcaneTests.exe`. The staged `automation-exclusions.json` hard-fails if missing.
- **Witness cases are tagged `[witness][gpu]`** — both tags, so `~[gpu]` excludes them
  everywhere the convention already applies (hosted CI, baseline-comparability runs).
- **Fresh-copy hygiene:** scenarios copy the host's staged tree to scratch and mutate the COPY.
  Live staged trees under `bin/` are never mutated. Scratch kept on failure, deleted on pass.
- **A missing or unparseable report is a scenario failure regardless of exit code** (the spec's
  refusal of UE's pass-on-missing-report fallback).
- **Never commit `out.txt`** (repo root, untracked, the user's). Stage files by name.
- **Commit after every task. Never squash tasks together.**
- Report schema constants live in THREE places after this arc: `VerifyReport.hpp`,
  the published `automation-vocabulary.txt` (written by the `[verdict]` publishing case from the
  header constants — updates itself), and `scripts/golden-gate.ps1:326-327`. Task 6 watches the
  `-SelfTest` pin catch the third one.
- Baseline figures at plan time (Arc A close): `~[gpu]` Debug/Release **52431/1304**, Dist
  **52363/1298**. Task 7 re-derives from real runs — these are reference points only, and
  identical post-arc figures mean the new tests did not register.

---

## File Structure

**New files:**

| Path | Responsibility |
|---|---|
| `ArcaneTests/src/Helpers/HostWitness.hpp` | `WitnessInvocation`, `WitnessRun`, `RunWitness()`, `GradeProcessFacts()`, `WitnessScratch` (fresh-copy RAII). |
| `ArcaneTests/src/Helpers/HostWitness.cpp` | Their definitions (CreateProcess, watchdog, stdio capture, report load). |
| `ArcaneTests/src/HostWitnessTest.cpp` | Unit tests for the helper against `cmd.exe` targets. `[witness-unit]` tag (no GPU needed). |
| `ArcaneTests/src/WitnessScenariosTest.cpp` | W1/W2/W3 `[witness][gpu]` cases. |
| `ReferenceProject/Content/scenes/witness-nocamera.arcscene` | W2's lever: a valid scene with no camera entity. |

**Modified files:**

| Path | Change |
|---|---|
| `ArcaneClient/src/Arcane/Host/ReferenceImages.hpp:49-66` | `ReferenceResolution` gains `triedPaths`; `ResolveReference` fills it in try order. |
| `ArcaneClient/src/Arcane/Host/ReferenceImages.cpp` | Record every candidate path as it is tried. |
| `ArcaneClient/src/Arcane/Host/VerifyReport.hpp:146-147, 303-308` | `kSchemaVersion` 4→5; `SetCompare` gains 12th param `std::vector<std::string> triedPaths`. |
| `ArcaneClient/src/Arcane/Host/VerifyReport.cpp` | Emit `compare.triedPaths`; store the member. |
| `ArcaneRuntime/src/RuntimeApp.cpp:1327` region | Pass `m_compareResolution.triedPaths` (stringified) into `SetCompare`. |
| `ArcaneEditor/src/App/EditorApp.cpp` (its one `SetCompare` call site; find with `grep -n "SetCompare" EditorApp.cpp`) | Same. |
| `ArcaneTests/src/ReferenceImagesTest.cpp` | Append triedPaths cases. |
| `ArcaneTests/src/VerifyReportTest.cpp` | Append schema-5 + triedPaths emission cases. |
| `scripts/golden-gate.ps1:327` | `$script:ReportSchemaMax = 5` (Task 6, AFTER watching the pin fail). |
| `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp:417` | ABI 19→20 + comment-block entry. |
| `ReferenceProject/ReferenceProject.arcproj:6` + Gacha `Game/Aphelyon.arcproj:6` | Restamp 20. |
| `scripts/automation-baselines.json` | Task 7 re-derives (new cases move `~[gpu]`? see Task 7 — `[witness-unit]` cases DO move it; `[witness][gpu]` cases do not). |

---

### Task 1: TriedPaths in the resolver

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/ReferenceImages.hpp:49-66`, `ReferenceImages.cpp`
- Test: `ArcaneTests/src/ReferenceImagesTest.cpp` (append)

**Interfaces:**
- Produces: `ReferenceResolution::triedPaths` — `std::vector<std::filesystem::path>`, the
  candidate reference paths in the exact order `ResolveReference` tried them; filled on every
  call, including refused names (empty) and successful resolutions (every candidate up to and
  including the one that resolved).

- [ ] **Step 1: Read the current resolver.** Read `ReferenceImages.cpp`'s `ResolveReference`
  end to end before touching it. Identify each `std::filesystem::exists`-style probe — those
  probes ARE the try order.

- [ ] **Step 2: Write the failing tests** (append to `ReferenceImagesTest.cpp`, matching its
  existing style — read a neighbouring case first):

```cpp
TEST_CASE("reference resolution records the ordered search space it tried", "[host][reference]")
{
    // Layout: a temp project root with NO references at all.
    const auto root = std::filesystem::temp_directory_path() / "arc-triedpaths-none";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Verify" / "References");

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "vulkan");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);
    // Both levels were tried, backend-specific first, in order.
    REQUIRE(res.triedPaths.size() == 2);
    REQUIRE(res.triedPaths[0].filename() == "runtime-scene.png");
    REQUIRE(res.triedPaths[0].parent_path().filename() == "vulkan");
    REQUIRE(res.triedPaths[1].parent_path().filename() == "References");
    std::filesystem::remove_all(root);
}

TEST_CASE("a resolved reference still records what was tried before it", "[host][reference]")
{
    const auto root = std::filesystem::temp_directory_path() / "arc-triedpaths-shared";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Verify" / "References");
    // Only the SHARED level exists; the backend level will be tried and missed first.
    std::ofstream(root / "Verify" / "References" / "runtime-scene.png") << "x";

    const auto res = Arcane::ResolveReference(root, "runtime-scene", "vulkan");
    REQUIRE(res.level == Arcane::ReferenceLevel::Shared);
    REQUIRE(res.triedPaths.size() == 2);          // backend miss, then shared hit
    REQUIRE(res.triedPaths.back() == res.path);   // last tried == resolved
    std::filesystem::remove_all(root);
}

TEST_CASE("a refused reference name records an empty search space", "[host][reference]")
{
    const auto res = Arcane::ResolveReference(std::filesystem::temp_directory_path(),
                                              "../evil", "vulkan");
    REQUIRE(res.level == Arcane::ReferenceLevel::None);
    REQUIRE(res.triedPaths.empty());   // refusal is not a search
}
```

Adjust the first test's expected order to what Step 1 found if backend-first is wrong — the
TEST pins the real order, whatever it is; the assertion of ORDER itself is the requirement.

- [ ] **Step 3: Run, verify they fail** (`triedPaths` does not exist → compile failure is the
  expected RED): from the repo root build Debug, or compile-check the test TU. Expected:
  error naming `triedPaths`.

- [ ] **Step 4: Implement.** Add to `ReferenceResolution` (`ReferenceImages.hpp`), with a
  comment in the file's voice:

```cpp
        // The candidate paths ResolveReference probed, in the exact order it
        // probed them -- including the one that resolved, which is always
        // last. Empty on a REFUSED name: refusal is not a search, and an
        // empty list distinguishes "never looked" from "looked everywhere"
        // (UE logs this list and then discards it; the report carries ours).
        std::vector<std::filesystem::path> triedPaths;
```

In `ReferenceImages.cpp`, push each candidate into `res.triedPaths` immediately before its
existence probe. Do not restructure the probe order.

- [ ] **Step 5: Build Debug, run the covering tests from the exe dir:**
  `ArcaneTests.exe "[reference]"` — expected: all pass, including the pre-existing cases.

- [ ] **Step 6: Commit** — `git add` the three files by name;
  `feat(host): ResolveReference records the ordered search space it tried`.

---

### Task 2: The report carries triedPaths — schema 5

**Files:**
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.hpp:146-147` (constants), `:303-308`
  (`SetCompare`), `VerifyReport.cpp` (member + emit)
- Modify: `ArcaneRuntime/src/RuntimeApp.cpp:1327` region; `ArcaneEditor/src/App/EditorApp.cpp`
  (locate with `grep -n "SetCompare" ArcaneEditor/src/App/EditorApp.cpp`)
- Test: `ArcaneTests/src/VerifyReportTest.cpp` (append)

**Interfaces:**
- Consumes: `ReferenceResolution::triedPaths` (Task 1).
- Produces: `SetCompare(std::string reference, std::string resolvedLevel,
  std::string referencePath, bool passed, std::uint64_t diffCount, double diffRatio,
  std::uint64_t maxDiffPixels, bool sizesMismatch, std::string diffPath,
  std::string errorMessage, double maxLocalDifference,
  std::vector<std::string> triedPaths)` — 12th param, non-defaulted (both call sites are ours;
  a defaulted param would let a host silently not supply it). JSON: `compare.triedPaths`,
  array of strings, present whenever the `compare` block is (both keys or neither, the block's
  existing contract). `kSchemaVersion = 5`; `kOldestSupportedSchemaVersion` stays 3.

- [ ] **Step 1: Write the failing tests** (append to `VerifyReportTest.cpp`, mirroring the
  existing schema-pin case style — read one first):

```cpp
TEST_CASE("schema 5: compare block carries triedPaths in try order", "[host][verify]")
{
    Arcane::VerifyReport r;
    r.SetCompare("runtime-scene", "none", "", false, 0, 0.0, 0, false, "", 
                 "no reference resolved", 0.0,
                 { "Verify/References/vulkan/runtime-scene.png",
                   "Verify/References/runtime-scene.png" });
    const auto j = nlohmann::json::parse(r.ToJson());
    REQUIRE(j["schemaVersion"].get<int>() == 5);
    REQUIRE(j["compare"]["triedPaths"].size() == 2);
    REQUIRE(j["compare"]["triedPaths"][0].get<std::string>()
            == "Verify/References/vulkan/runtime-scene.png");
}

TEST_CASE("schema 5: no compare, no triedPaths", "[host][verify]")
{
    Arcane::VerifyReport r;   // SetCompare never called
    const auto j = nlohmann::json::parse(r.ToJson());
    REQUIRE_FALSE(j.contains("compare"));
}
```

(Adapt construction/serialization calls to the file's existing pattern — the neighbouring
tests show the real API for building and dumping a report; the assertions above are the
requirement.)

- [ ] **Step 2: Verify RED** — compile fails on the 12-arg call.

- [ ] **Step 3: Implement.** Constants: `kSchemaVersion = 5` and extend the version-history
  comment ("5 added compare.triedPaths. 3 and 4 remain readable..."). Member
  `std::vector<std::string> m_compareTriedPaths;`, stored by `SetCompare`, emitted inside the
  existing `compare` block as `j["compare"]["triedPaths"]`.

- [ ] **Step 4: Update both host call sites.** `RuntimeApp.cpp:1327` region and the editor's
  equivalent: build the string vector from `resolution.triedPaths`
  (`path.generic_string()` each) and pass it. Also check `RuntimeApp.cpp:564`'s earlier
  resolution — pass from whichever `ReferenceResolution` the `SetCompare` call already reads.
  Update the two pre-existing `VerifyReportTest.cpp` cases that call `SetCompare` with 11 args
  (compile errors will name them) — give them `{}` and, where the case pins the schema
  version, move it 4→5. **Renaming a test title to match new reality is correct; leaving a
  title that lies is not** (Arc A precedent).

- [ ] **Step 5: Build Debug; run `ArcaneTests.exe "[verify]"` and `"[reference]"` from the exe
  dir.** Expected: pass.

- [ ] **Step 6: Commit** — `feat(host): verify report schema 5 -- compare.triedPaths`.

---

### Task 3: The spawn helper

**Files:**
- Create: `ArcaneTests/src/Helpers/HostWitness.hpp`, `HostWitness.cpp`
- Test: `ArcaneTests/src/HostWitnessTest.cpp` (create)

**Interfaces:**
- Consumes: `Arcane::Verdict`, `Arcane::ToString(Verdict)` (`Arcane/Host/Verdict.hpp`).
- Produces (all in namespace `Arcane::Test`):

```cpp
struct WitnessInvocation {
    std::filesystem::path exePath;
    std::vector<std::string> args;
    std::filesystem::path workingDir;
    std::filesystem::path reportPath;    // where the host was told to write --report
    std::uint32_t hardCapMs = 60000;
};

struct WitnessRun {
    int  exitCode = -1;
    bool timedOut = false;          // hard cap expired; the process was killed
    bool progressingAtKill = false; // stdout grew OR CPU advanced in the final poll window
    bool reportFound = false;
    bool reportParsed = false;
    nlohmann::json report;          // valid only when reportParsed
    std::uint64_t wallMs = 0;       // spawn -> exit/kill, measured
    std::filesystem::path stdoutPath, stderrPath; // captured beside reportPath
};

WitnessRun RunWitness(const WitnessInvocation& inv);

// The PROCESS-LEVEL half of the verdict only. Engaged => graded without
// opening the report (Errored / Indeterminate); nullopt => a parsed report
// exists and the SCENARIO judges its contents. Deliberately refuses UE's
// pass-on-missing-report fallback: exit 0 with no report is Indeterminate.
std::optional<Arcane::Verdict> GradeProcessFacts(const WitnessRun& run);

// Fresh-copy hygiene (spec section 6): copies srcStagedDir to a scratch dir,
// deletes it on clean destruction, KEEPS it when destroyed during a Catch2
// failure unwind (std::uncaught_exceptions), printing the kept path.
class WitnessScratch {
public:
    WitnessScratch(const std::filesystem::path& srcStagedDir, std::string scenarioName);
    ~WitnessScratch();
    const std::filesystem::path& Dir() const;   // the copy's root
};
```

Grading contract for `GradeProcessFacts`: `timedOut` → `Errored`; else `!reportFound` or
`!reportParsed` → `Indeterminate` (regardless of exit code — including 0); else `nullopt`.
Precedence is that order, full stop.

- [ ] **Step 1: Write the failing unit tests** (`HostWitnessTest.cpp`, tag `[witness-unit]` —
  no GPU, no host binaries; targets are `cmd.exe`):

```cpp
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/Verdict.hpp>

using Arcane::Test::WitnessInvocation;
using Arcane::Test::RunWitness;
using Arcane::Test::GradeProcessFacts;

static WitnessInvocation CmdInv(std::string cmdArg, std::filesystem::path reportPath = {})
{
    WitnessInvocation inv;
    inv.exePath    = "C:/Windows/System32/cmd.exe";
    inv.args       = { "/c", std::move(cmdArg) };
    inv.workingDir = std::filesystem::temp_directory_path();
    inv.reportPath = std::move(reportPath);
    return inv;
}

TEST_CASE("witness: exit code is captured", "[witness-unit]")
{
    auto run = RunWitness(CmdInv("exit 3"));
    REQUIRE(run.exitCode == 3);
    REQUIRE_FALSE(run.timedOut);
}

TEST_CASE("witness: hard cap kills and reports timedOut", "[witness-unit]")
{
    auto inv = CmdInv("ping -n 30 127.0.0.1 >nul");
    inv.hardCapMs = 1500;
    auto run = RunWitness(inv);
    REQUIRE(run.timedOut);
    REQUIRE(GradeProcessFacts(run) == Arcane::Verdict::Errored);
}

TEST_CASE("witness: exit 0 with NO report is Indeterminate, never a pass", "[witness-unit]")
{
    // UE's cascade grades this Passed ("no report to parse"). We refuse that.
    auto inv = CmdInv("exit 0",
        std::filesystem::temp_directory_path() / "arc-witness-absent-report.json");
    std::filesystem::remove(inv.reportPath);
    auto run = RunWitness(inv);
    REQUIRE(run.exitCode == 0);
    REQUIRE_FALSE(run.reportFound);
    REQUIRE(GradeProcessFacts(run) == Arcane::Verdict::Indeterminate);
}

TEST_CASE("witness: malformed report is Indeterminate", "[witness-unit]")
{
    const auto rp = std::filesystem::temp_directory_path() / "arc-witness-junk.json";
    { std::ofstream f(rp); f << "{not json"; }
    auto run = RunWitness(CmdInv("exit 0", rp));
    REQUIRE(run.reportFound);
    REQUIRE_FALSE(run.reportParsed);
    REQUIRE(GradeProcessFacts(run) == Arcane::Verdict::Indeterminate);
    std::filesystem::remove(rp);
}

TEST_CASE("witness: a parsed report defers to the scenario", "[witness-unit]")
{
    const auto rp = std::filesystem::temp_directory_path() / "arc-witness-ok.json";
    { std::ofstream f(rp); f << R"({"schemaVersion":5})"; }
    auto run = RunWitness(CmdInv("exit 0", rp));
    REQUIRE(run.reportParsed);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    std::filesystem::remove(rp);
}

TEST_CASE("witness: stdout is captured to a file", "[witness-unit]")
{
    auto run = RunWitness(CmdInv("echo witness-marker"));
    REQUIRE(std::filesystem::exists(run.stdoutPath));
    std::ifstream f(run.stdoutPath); std::string all((std::istreambuf_iterator<char>(f)), {});
    REQUIRE(all.find("witness-marker") != std::string::npos);
}

TEST_CASE("witness scratch: copies, and deletes on clean exit", "[witness-unit]")
{
    const auto src = std::filesystem::temp_directory_path() / "arc-scratch-src";
    std::filesystem::create_directories(src / "sub");
    { std::ofstream f(src / "sub" / "a.txt"); f << "x"; }
    std::filesystem::path copied;
    {
        Arcane::Test::WitnessScratch s(src, "unit");
        copied = s.Dir();
        REQUIRE(std::filesystem::exists(copied / "sub" / "a.txt"));
    }
    REQUIRE_FALSE(std::filesystem::exists(copied));
    std::filesystem::remove_all(src);
}
```

- [ ] **Step 2: Verify RED** — compile failure naming `HostWitness.hpp`.

- [ ] **Step 3: Implement `HostWitness.cpp`.** Win32: `CreateProcessW` with
  `hStdOutput`/`hStdError` redirected to files opened beside `reportPath` (or in a temp dir
  when `reportPath` is empty) — inherit handles, `CREATE_NO_WINDOW`. Watchdog loop:
  `WaitForSingleObject(process, pollMs)` in 250 ms slices up to `hardCapMs`; each slice, record
  stdout file size and `GetProcessTimes` CPU; on cap expiry set
  `progressingAtKill = (stdoutGrew || cpuAdvanced)` for the final window, then
  `TerminateProcess` + wait (Arc A's unkillable-process guard: if the wait after terminate
  itself times out, report `timedOut` and move on — never hang the suite). `wallMs` from
  steady_clock around the whole run. Report: `reportFound = exists(reportPath)` (only when a
  path was given); parse with `nlohmann::json::parse(..., nullptr, false)`;
  `reportParsed = !is_discarded()`. `WitnessScratch`: `std::filesystem::copy` recursive to
  `%TEMP%/arcane-witness/<scenario>-<GetCurrentProcessId()>`; destructor keeps the dir and
  prints `KEPT WITNESS ARTIFACT: <path>` when `std::uncaught_exceptions() > 0`.

- [ ] **Step 4: Build Debug; run `ArcaneTests.exe "[witness-unit]"` from the exe dir.**
  Expected: all pass, no stray output.

- [ ] **Step 5: Commit** — `feat(tests): HostWitness spawn helper -- process facts, watchdog, fresh-copy scratch`.

---

### Task 4: W1 + W2 — the settle facts, witnessed

**Files:**
- Create: `ArcaneTests/src/WitnessScenariosTest.cpp`,
  `ReferenceProject/Content/scenes/witness-nocamera.arcscene`
- Test: the scenarios ARE the tests.

**Interfaces:**
- Consumes: everything Task 3 produced; the staged runtime at
  `bin/<Config>-windows-x86_64-md/ArcaneRuntime/` (sibling of the test exe dir).
- Produces: the `[witness][gpu]` tag convention later tasks and CI rely on.

- [ ] **Step 1: Derive W2's lever RED-first.** Author
  `ReferenceProject/Content/scenes/witness-nocamera.arcscene` by copying
  `ReferenceProject/Content/scenes/main.arcscene` and removing every camera component/entity
  (keep at least one sprite so the scene is non-empty; give it a fresh guid — follow the
  existing scene's guid field format). Rebuild Debug (restages content), then BY HAND from
  `bin/Debug-windows-x86_64-md/ArcaneRuntime/`:
  `.\ArcaneRuntime.exe --project ReferenceProject --headless --backend vulkan --frames 10 --settle 2 --settle-timeout 1000 --scene <the-new-guid> --report %TEMP%\w2probe.json`
  and inspect the report: is `settleBailReason` `"capture-failed"`? If YES, the lever holds —
  proceed. If NO, STOP and derive the real lever from `RuntimeFrame.cpp:860-900` (what makes
  `previousCaptureValid` never set) — the constraint is content-or-CLI only, and the scenario
  is written against whatever lever is real. Record the probe output in the task report either
  way.

- [ ] **Step 2: Write W1 and W2** (`WitnessScenariosTest.cpp`):

```cpp
#include "Helpers/HostWitness.hpp"
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Host/Verdict.hpp>

using namespace Arcane::Test;

// The staged runtime lives beside the test exe dir. Tests run FROM the exe
// dir, so this resolves relative to cwd. HARD requirement, not a skip: an
// unbuilt host tree on a capable machine is an incomplete build (spec s7).
static std::filesystem::path StagedRuntimeDir()
{
    auto p = std::filesystem::absolute("../ArcaneRuntime");
    REQUIRE_MESSAGE(std::filesystem::exists(p / "ArcaneRuntime.exe"),
        "staged ArcaneRuntime not found -- build Arcane.slnx first: " << p.string());
    return p;
}
// (If REQUIRE_MESSAGE is not available in this Catch2 config, use
//  INFO(...) + REQUIRE(std::filesystem::exists(...)) -- same effect.)

static WitnessInvocation HostInv(const WitnessScratch& scratch,
                                 std::vector<std::string> extraArgs)
{
    WitnessInvocation inv;
    inv.exePath    = scratch.Dir() / "ArcaneRuntime.exe";
    inv.workingDir = scratch.Dir();
    inv.reportPath = scratch.Dir() / "witness-report.json";
    inv.args = { "--project", "ReferenceProject", "--headless", "--backend", "vulkan",
                 "--frames", "10", "--report", inv.reportPath.generic_string() };
    inv.args.insert(inv.args.end(), extraArgs.begin(), extraArgs.end());
    inv.hardCapMs = 120000;
    return inv;
}

TEST_CASE("W1: settle spends BOTH bounds when the compare conjunct cannot pass",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "w1-bounds-spent");
    // The lever: a wrong reference at EVERY level (Arc A: one level just
    // falls back). Overwrite both with a tiny valid-but-wrong PNG -- the
    // 1x1 diff artifact trick: reuse an existing unrelated PNG from the
    // staged tree, which is guaranteed valid and guaranteed wrong.
    const auto refs = scratch.Dir() / "ReferenceProject" / "Verify" / "References";
    const auto wrong = refs / "editor-ui.png";   // valid PNG, wrong content for runtime-scene
    std::filesystem::copy_file(wrong, refs / "runtime-scene.png",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(wrong, refs / "vulkan" / "runtime-scene.png",
                               std::filesystem::copy_options::overwrite_existing);

    const std::uint32_t timeoutMs = 2000;
    auto run = RunWitness(HostInv(scratch,
        { "--settle", "4", "--settle-timeout", std::to_string(timeoutMs),
          "--compare", "runtime-scene" }));

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());  // report exists and parsed
    REQUIRE(run.exitCode != 0);
    // THE FACT UNDER TEST: both bounds were genuinely spent.
    REQUIRE(run.wallMs >= timeoutMs);
    REQUIRE(run.report["settleAttemptsUsed"].get<std::uint64_t>() >= 4);
    const auto bail = run.report["settleBailReason"].get<std::string>();
    REQUIRE((bail == "attempts-bound" || bail == "timeout-bound"));
    REQUIRE(run.report["exitReason"].get<std::string>() == "compare-failed");
    // The mirror half: capture WORKED. W2 asserts the opposite.
    REQUIRE(bail != "capture-failed");
}

TEST_CASE("W2: a capture that never lands is recorded as capture-failed, not as a bound",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "w2-capture-failed");
    auto run = RunWitness(HostInv(scratch,
        { "--settle", "2", "--settle-timeout", "1000",
          "--scene", "<GUID-FROM-STEP-1>" }));   // literal guid, pasted from the authored scene

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode != 0);
    REQUIRE(run.report["settleBailReason"].get<std::string>() == "capture-failed");
    // The mirror half: no screenshot artifact was written.
    REQUIRE_FALSE(std::filesystem::exists(scratch.Dir() / "witness-shot.png"));
}
```

Adjust W2's exact args to the Step 1 probe's working invocation (e.g. `--settle` requires
`--screenshot` or `--report`; `--report` is already passed). Paste the real guid.

- [ ] **Step 3: Verify RED honestly.** These cases cannot fail-then-pass like a unit test —
  the RED evidence is Step 1's hand probe plus this negative control: run W1 once with
  CORRECT references (no overwrite) and confirm it FAILS its own `wallMs >= timeoutMs`
  assertion (a converging run exits early). Record that output, then restore the overwrite.
  This proves the assertions bite.

- [ ] **Step 4: Build Debug; run `ArcaneTests.exe "[witness]"` from the exe dir.** Expected:
  W1 + W2 pass; scratch dirs gone (pass-path cleanup); a deliberate second run confirms
  repeatability.

- [ ] **Step 5: Commit** — `feat(tests): witness scenarios W1+W2 -- settle bounds and capture-failed, observed`.

---

### Task 5: W3 — the absence, with its search space

**Files:**
- Modify: `ArcaneTests/src/WitnessScenariosTest.cpp` (append)

**Interfaces:**
- Consumes: `compare.triedPaths` (Task 2), the helpers (Task 3), the tag/fixture conventions
  (Task 4).

- [ ] **Step 1: Write W3:**

```cpp
TEST_CASE("W3: a reference missing at EVERY level reports the ordered search space",
          "[witness][gpu]")
{
    WitnessScratch scratch(StagedRuntimeDir(), "w3-missing-reference");
    const auto refs = scratch.Dir() / "ReferenceProject" / "Verify" / "References";
    // Every level of the chain has to go (Arc A: one level just falls back).
    std::filesystem::remove(refs / "runtime-scene.png");
    std::filesystem::remove(refs / "vulkan" / "runtime-scene.png");

    auto run = RunWitness(HostInv(scratch,
        { "--settle", "2", "--settle-timeout", "1000", "--compare", "runtime-scene" }));

    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.report["compare"]["resolvedLevel"].get<std::string>() == "none");
    const auto& tried = run.report["compare"]["triedPaths"];
    REQUIRE(tried.size() == 2);
    // Ordered: backend level first, shared second (as Task 1 pinned).
    REQUIRE(tried[0].get<std::string>().find("vulkan") != std::string::npos);
    REQUIRE(tried[1].get<std::string>().find("runtime-scene.png") != std::string::npos);
}
```

(If Task 1 pinned the opposite order, flip the two assertions — the ORDER assertion itself is
the requirement. Exit-code expectation: derive from the run — a missing reference may exit
nonzero or zero-with-none; assert whichever the probe shows and COMMENT why, citing the run.)

- [ ] **Step 2: Verify it bites** — negative control: run once against an unmutated scratch
  copy and confirm the `resolvedLevel == "none"` assertion FAILS (it resolves). Record output.

- [ ] **Step 3: Run `ArcaneTests.exe "[witness]"` — all three scenarios pass, twice.**

- [ ] **Step 4: Commit** — `feat(tests): witness scenario W3 -- the absence carries its search space`.

---

### Task 6: ABI 20, restamps, and the pin watched failing

**Files:**
- Modify: `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp:417` (+ comment block above it),
  `ReferenceProject/ReferenceProject.arcproj:6`, `scripts/golden-gate.ps1:327`
- Modify (OTHER REPO): `D:\dev\starworks\Gacha\Game\Aphelyon.arcproj:6` — separate commit
  there, subject exactly `chore(game): restamp Aphelyon.arcproj to engine ABI 20`, do not push.

- [ ] **Step 1: Bump.** `kGamePluginABIVersion = 20`. Extend the comment block in the
  established voice: v20 taken because `VerifyReport` gained `m_compareTriedPaths` + a
  non-defaulted 12th `SetCompare` param (mangled name moved) and `ReferenceResolution` gained
  a member (`ARCANE_API` surface layout). Record what was MEASURED: grep both game modules for
  `SetCompare|ReferenceResolution` and state the result, as the v18/v19 entries do.

- [ ] **Step 2: Restamp** both `.arcproj` files 19→20 (source files, not `bin/` copies).
  Commit the Gacha one in the Gacha repo.

- [ ] **Step 3: Watch Arc A's pin catch the schema move.** Rebuild Debug (all projects), then
  from the repo root: `powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1
  -Configuration Debug -SelfTest`. `golden-gate.ps1:327` still says `ReportSchemaMax = 4`
  while the header (and therefore the published `automation-vocabulary.txt`) now says 5 —
  **expected: `-SelfTest` FAILS on the vocabulary/schema pin, exit 1.** Paste the failing
  lines. This is the spec's required watched-failure of the three-place constant.

- [ ] **Step 4: Fix and watch it pass.** Set `$script:ReportSchemaMax = 5`; re-run
  `-SelfTest`; expected `SELF-TEST PASSED`, exit 0, tree restored. Also run the plain gate:
  `golden-gate.ps1 -Configuration Debug` — expected 4 lanes, `gatePassed: true` (schema 5 is
  inside the gate's now-widened supported range; lanes carry `triedPaths` harmlessly).

- [ ] **Step 5: Commit** — `chore(abi)!: plugin ABI 19 -> 20 -- VerifyReport and ReferenceResolution moved` (Arcane), plus the Gacha restamp commit.

---

### Task 7: Full verification and closeout

**Files:**
- Modify: `scripts/automation-baselines.json` (re-derived values),
  `docs/specs/2026-09-03-host-witness-harness-design.md` (status line)

- [ ] **Step 1: Rebuild all three configs** (`msbuild Arcane.slnx /p:Configuration=<C> /m`),
  zero `warning C` in each — paste the counts.

- [ ] **Step 2: Re-derive the baselines.** From each exe dir: `ArcaneTests.exe "~[gpu]"`.
  NOTE what moved and why: Tasks 1-3 added `[reference]`/`[verify]`/`[witness-unit]` cases
  (inside `~[gpu]`) — figures MUST rise from Debug/Release 52431/1304, Dist 52363/1298;
  identical figures mean the new tests did not register. `[witness][gpu]` scenarios are
  correctly OUTSIDE `~[gpu]` — spot-check with `ArcaneTests.exe "[witness]" --list-tests`
  and `"~[gpu] [witness]" --list-tests` (the latter must list zero). Update the six committed
  values from each run's own final line; paste them.

- [ ] **Step 3: Full witness + gpu sweep on this desk:** `ArcaneTests.exe "[gpu]"` from the
  Debug exe dir (includes the three scenarios). Expected: pass; paste counts.

- [ ] **Step 4: Run the checker per config** with `-Invocation "~[gpu]"` against fresh JSON
  reports — green, exit 0.

- [ ] **Step 5: Gate + SelfTest, Release:** `golden-gate.ps1 -Configuration Release` then
  `-SelfTest` — `gatePassed: true` (read from the summary JSON, never the exit code) and
  `SELF-TEST PASSED`.

- [ ] **Step 6: Update the spec status line** to LANDED with the measured figures and the
  scenario outcomes (which levers held, what W2's probe showed). Commit —
  `docs(spec): record Arc B as landed`.

- [ ] **Step 7: Desk checkpoint:** `git status --porcelain` clean in both repos apart from
  `out.txt`; both repos' HEADs named in the task report.
