# Arcane Hub Slice 1: engine seam (`--print-engine-info` + the no-project gate) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop `ArcaneEditor.exe` from booting a project-less session, and give the
future Hub a machine-readable way to ask the engine for its plugin ABI.

**Architecture:** Both halves are pure C++ in the existing shared host-boot helper.
`Arcane::HostBoot::EngineInfoJson()` (header-only in `Loom/src/ProjectBoot.hpp`, which
is already source-shared into Loom, ArcaneEditor, and ArcaneTests) produces the probe
payload, so it is unit-testable and both hosts honour the flag rather than one silently
ignoring it. The gate lives in `ArcaneEditor/src/main.cpp`, before any window or device
exists, so refusing costs nothing.

**Tech Stack:** C++23, `Arcane::Cli` (via `LoomConfig`), nlohmann::json, Catch2.

## Scope

This plan covers **slice 1 only** of
`docs/superpowers/specs/2026-07-26-arcane-hub-launcher-design.md`. Slices 2 and 3 (the
Tauri + Rust Hub app and New Project) are a different subsystem with a different
toolchain and get their own plan once the spec's open questions are answered. Slice 1
ships working software alone: it closes the project-less hole even if the Hub slips.

## Global Constraints

- Branch: create from the current branch tip; the Outliner arc branch
  `shader-editor-slice1-material-core` is UNMERGED, so confirm with the user which base
  to use before starting.
- UTF-8 without BOM, **ASCII comments only** (no em dashes, no smart quotes).
- Run the gate **from the exe dir**: `Arcane\bin\Debug-windows-x86_64-md\ArcaneTests\`.
- ArcaneTests runs in **random time-seeded order**; verify under `--rng-seed 6` and
  `--rng-seed 17`. Baseline to beat: **29635 assertions / 521 cases**.
- **Never** write a bare `Arcane::Runtime rt;` in a test — always
  `Arcane::Runtime pin(&Arcane::Test::SharedTypeContext());`.
- MSBuild (from `Arcane\`):
  `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo`

## Decisions locked here (do not re-open mid-implementation)

1. **`--project` and `--plugin` remain bypasses.** CI and the headless
   `ArcaneEditor.exe --project <p> --frames N` harness depend on `--project`; that
   harness is what found the Add Component roster bug on 2026-07-26.
2. **The gate exits non-zero with a stderr message. No message box.** At the point the
   gate fires, no SDL window or device exists; creating one just to say "no" would
   invert the cost. This resolves the spec's open question 3. Revisit when
   `ArcaneHub.exe` actually exists and can be launched from the message.
3. **An explicit `--project` that FAILS to open is out of scope and stays as-is.**
   Today it warns and surfaces the error at first frame — deliberate prior work
   (the console line alone was missed twice). Only the *no-flags* hole closes here.
   This is a conscious boundary, not an oversight.
4. **`Loom` honours `--print-engine-info` too.** The flag lives in the shared
   `LoomConfig`, so a flag that parsed but did nothing on one of the two hosts would be
   a trap. Loom does NOT get the gate — it hosts `Sandbox.dll` by default with no
   flags, by design.

## File Structure

| File | Responsibility |
|---|---|
| `Arcane/Loom/src/ProjectBoot.hpp` (modify) | `HostBoot::EngineInfoJson(exePath)` — the probe payload |
| `Arcane/Tests/src/HostBootTest.cpp` (modify) | ABI tripwire + JSON shape tests |
| `Arcane/Loom/src/LoomConfig.hpp` (modify) | `bool printEngineInfo` |
| `Arcane/Loom/src/LoomConfig.cpp` (modify) | Declare + read the flag |
| `Arcane/Tests/src/LoomConfigTest.cpp` (modify) | Flag parses, defaults false |
| `Arcane/ArcaneEditor/src/main.cpp` (modify) | Probe short-circuit + the gate |
| `Arcane/Loom/src/main.cpp` (modify) | Probe short-circuit only |

---

### Task 1: `HostBoot::EngineInfoJson` + the ABI tripwire

**Files:**
- Modify: `Arcane/Loom/src/ProjectBoot.hpp`
- Modify: `Arcane/Tests/src/HostBootTest.cpp`

**Interfaces:**
- Consumes: `Arcane::kGamePluginABIVersion` (`Arcane/Plugin/PluginABI.hpp:55`),
  `Arcane::BuildInfo()` (`Arcane/Base/Engine.hpp:9`, returns `const char*`).
- Produces (Task 2 calls this):
  `std::string Arcane::HostBoot::EngineInfoJson(const std::filesystem::path& exePath)`

- [ ] **Step 1: Read the file you are about to change**

Read `Arcane/Loom/src/ProjectBoot.hpp` in full before editing. It is header-only and
source-shared into three targets; match its existing namespace layout and comment
density exactly rather than appending in a different style.

- [ ] **Step 2: Write the failing tests**

Append to `Arcane/Tests/src/HostBootTest.cpp` (read its top first for the include
block and tag convention; use the same tag the existing cases use):

```cpp
// --- Engine probe (Arcane Hub slice 1) -----------------------------------------
// The Hub stamps `engine.abi` into every .arcproj it creates. If that number is
// ever sourced from anywhere but the engine itself, New Project silently mints
// stale-ABI projects and they crash on open -- the exact failure recorded in the
// shader-editor arc's ABI LESSON. This suite is the tripwire that keeps the probe
// pinned to the real constant.

TEST_CASE("EngineInfoJson reports the engine's real plugin ABI", "[loom]")
{
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:/some/ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);

    REQUIRE(j.contains("engineAbi"));
    REQUIRE(j["engineAbi"].is_number_unsigned());
    // THE assertion: bump kGamePluginABIVersion without the probe following and
    // this fails.
    CHECK(j["engineAbi"].get<std::uint32_t>() == Arcane::kGamePluginABIVersion);
}

TEST_CASE("EngineInfoJson carries build + exe path and parses cleanly", "[loom]")
{
    const std::string s = Arcane::HostBoot::EngineInfoJson("C:/some dir/ArcaneEditor.exe");
    const nlohmann::json j = nlohmann::json::parse(s);   // must not throw

    REQUIRE(j.contains("build"));
    CHECK(j["build"].is_string());
    CHECK_FALSE(j["build"].get<std::string>().empty());

    REQUIRE(j.contains("exePath"));
    // Forward slashes: generic_string(), so the Hub never has to unescape
    // backslashes out of JSON.
    CHECK(j["exePath"].get<std::string>() == "C:/some dir/ArcaneEditor.exe");
}

TEST_CASE("EngineInfoJson is a single line", "[loom]")
{
    // The Hub reads one line from stdout. A pretty-printed payload would make it
    // guess where the object ends.
    const std::string s = Arcane::HostBoot::EngineInfoJson("x.exe");
    CHECK(s.find('\n') == std::string::npos);
}
```

- [ ] **Step 3: Run them to verify they fail**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
```
Expected: **compile error** — `EngineInfoJson` is not a member of `Arcane::HostBoot`.
That is the failing state for a new function; do not proceed until you see it.

- [ ] **Step 4: Implement**

Add to `Arcane/Loom/src/ProjectBoot.hpp` inside `namespace Arcane::HostBoot` — the
namespace is confirmed by its existing call sites, e.g.
`Arcane::HostBoot::GameModule(...)` at `EditorApp.cpp:235`:

```cpp
    // One-line JSON describing this engine build, for `--print-engine-info`.
    //
    // This exists so the Arcane Hub never HARDCODES a plugin ABI. A .arcproj
    // requires `engine.abi` (ProjectManifest.hpp), so a hub that guessed it would
    // mint stale-ABI projects the moment the engine bumps, and those crash on
    // open. The Hub probes, then stamps whatever the engine reports.
    //
    // Single line on purpose: the caller reads one line from stdout. Paths use
    // generic_string() so the Hub never has to unescape backslashes.
    inline std::string EngineInfoJson(const std::filesystem::path& exePath)
    {
        nlohmann::json j;
        j["engineAbi"] = Arcane::kGamePluginABIVersion;
        j["build"]     = Arcane::BuildInfo();
        j["exePath"]   = exePath.generic_string();
        return j.dump();   // compact: no indent argument
    }
```

Add whatever includes this needs to the file's existing include block —
`<Arcane/Plugin/PluginABI.hpp>`, `<Arcane/Base/Engine.hpp>`, `<Json.hpp>`,
`<filesystem>`, `<string>` — only those not already present.

- [ ] **Step 5: Run the tests to verify they pass**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[loom]" --rng-seed 6
```
Expected: `All tests passed`, including the three new cases.

- [ ] **Step 6: Commit**

```bash
git add Arcane/Loom/src/ProjectBoot.hpp Arcane/Tests/src/HostBootTest.cpp
git commit -m "feat(arcane): HostBoot::EngineInfoJson engine probe payload + ABI tripwire"
```

---

### Task 2: the `--print-engine-info` flag

**Files:**
- Modify: `Arcane/Loom/src/LoomConfig.hpp`
- Modify: `Arcane/Loom/src/LoomConfig.cpp`
- Modify: `Arcane/Tests/src/LoomConfigTest.cpp`
- Modify: `Arcane/ArcaneEditor/src/main.cpp`
- Modify: `Arcane/Loom/src/main.cpp`

**Interfaces:**
- Consumes: Task 1's `Arcane::HostBoot::EngineInfoJson(exePath)`.
- Produces: `LoomConfig::printEngineInfo` (bool, default false). Task 3 reads the
  same config object.

- [ ] **Step 1: Write the failing test**

Append to `Arcane/Tests/src/LoomConfigTest.cpp` (read its top first — copy the exact
argv-building idiom the existing cases use rather than inventing one):

```cpp
TEST_CASE("--print-engine-info parses and defaults off", "[loom]")
{
    {
        char arg0[] = "ArcaneEditor.exe";
        char* argv[] = { arg0 };
        const LoomConfig::ParseOutcome r = LoomConfig::Parse(1, argv);
        REQUIRE(r.config.has_value());
        CHECK_FALSE(r.config->printEngineInfo);
    }
    {
        char arg0[] = "ArcaneEditor.exe";
        char arg1[] = "--print-engine-info";
        char* argv[] = { arg0, arg1 };
        const LoomConfig::ParseOutcome r = LoomConfig::Parse(2, argv);
        REQUIRE(r.config.has_value());
        CHECK(r.config->printEngineInfo);
    }
}
```

- [ ] **Step 2: Run it to verify it fails**

Build as in Task 1 Step 3. Expected: **compile error** — `printEngineInfo` is not a
member of `LoomConfig`.

- [ ] **Step 3: Add the field**

In `Arcane/Loom/src/LoomConfig.hpp`, after the `projectPath` member (line 18):

```cpp
    // Print one line of engine-identity JSON to stdout and exit, without creating
    // a window or device. The Arcane Hub probes this to learn the plugin ABI it
    // must stamp into a new .arcproj -- see HostBoot::EngineInfoJson.
    bool                    printEngineInfo = false;
```

- [ ] **Step 4: Declare and read the flag**

In `Arcane/Loom/src/LoomConfig.cpp`, after the `project` option (line 11):

```cpp
    cli.Flag  ("print-engine-info",       "print engine identity JSON to stdout and exit");
```

and after `cfg.projectPath = r.Get("project");` (line 22):

```cpp
    cfg.printEngineInfo = r.Flag("print-engine-info");
```

- [ ] **Step 5: Honour it in the editor entry point**

In `Arcane/ArcaneEditor/src/main.cpp`, replace the body between the parse and the
`EditorApp` construction so it reads:

```cpp
    const LoomConfig::ParseOutcome parsed = LoomConfig::Parse(argc, argv);
    if (!parsed.config) return parsed.exitCode;

    // Probe: identity to stdout, nothing else. Deliberately BEFORE any engine
    // boot -- the Arcane Hub calls this to read the plugin ABI, and it must not
    // pay for a window, a device, or a registry to answer.
    if (parsed.config->printEngineInfo)
    {
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(argv[0]).c_str());
        return 0;
    }

    Arcane::Editor::EditorApp app(*parsed.config);
    return app.Run();
```

Add `#include <ProjectBoot.hpp>` and `#include <cstdio>` to that file's include block
(match the existing `<LoomConfig.hpp>` include style — `Loom/src` is already on
ArcaneEditor's include path per `Arcane/premake5.lua:454`).

- [ ] **Step 6: Honour it in Loom's entry point**

Read `Arcane/Loom/src/main.cpp`, then insert this immediately after its
`LoomConfig::Parse` result is known good and before any engine boot:

```cpp
    // Same probe as the editor: identity to stdout, no window, no device. A flag
    // that parses on both hosts but only works on one would be a trap.
    if (parsed.config->printEngineInfo)
    {
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(argv[0]).c_str());
        return 0;
    }
```

Adjust `parsed.config` to whatever Loom's main actually names its parse outcome. Add
`#include <ProjectBoot.hpp>` and `#include <cstdio>` if absent. Loom does NOT get the
gate from Task 3 — it hosts `Sandbox.dll` by default with no flags, by design.

- [ ] **Step 7: Verify by running the real binaries**

```
cd D:\dev\starworks\Gacha\Arcane
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug /m /nodeReuse:false /v:minimal /nologo
.\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --print-engine-info
.\bin\Debug-windows-x86_64-md\Loom\Loom.exe --print-engine-info
```
Expected from BOTH: exactly one line of JSON containing `"engineAbi":7`, exit code 0,
**no window appears**. Confirm the exit code with `echo $LASTEXITCODE`.

- [ ] **Step 8: Run the gate and commit**

```
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu] --rng-seed 6
```
Expected: `All tests passed`, count above 29635/521.

```bash
git add Arcane/Loom/src/LoomConfig.hpp Arcane/Loom/src/LoomConfig.cpp Arcane/Tests/src/LoomConfigTest.cpp Arcane/ArcaneEditor/src/main.cpp Arcane/Loom/src/main.cpp
git commit -m "feat(arcane): --print-engine-info on both hosts"
```

---

### Task 3: the no-project gate

**Files:**
- Modify: `Arcane/ArcaneEditor/src/main.cpp`

**Interfaces:**
- Consumes: `LoomConfig::projectPath`, `LoomConfig::pluginPath`.
- Produces: no new API. Behavioural change only.

- [ ] **Step 1: Check nothing in the repo launches the editor bare**

Before changing behaviour, find every invocation:

```
cd D:\dev\starworks\Gacha
git grep -n "ArcaneEditor.exe" -- Jenkinsfile ci scripts Arcane/scripts
```
Expected: review each hit. If ANY launches `ArcaneEditor.exe` with neither `--project`
nor `--plugin`, STOP and report it — that caller must be fixed in this task or the gate
will break it. `Arcane/scripts/launch.bat` and `launch.ps1` are untracked but present;
read them too.

- [ ] **Step 2: Add the gate**

In `Arcane/ArcaneEditor/src/main.cpp`, after the `--print-engine-info` block from Task 2
and before constructing `EditorApp`:

```cpp
    // No project and no explicit plugin: refuse rather than boot a project-less
    // session. Until now this fell through to a "data/-next-to-exe" boot with no
    // asset registry, no mounts and no identity -- a half-configured editor
    // nothing downstream expects.
    //
    // Both flags stay as bypasses ON PURPOSE: CI and the headless
    // `--project <p> --frames N` harness depend on --project, and --plugin is the
    // engine-dev path (hosting a plugin without a project). Exiting beats a
    // message box here because no window or device exists yet.
    if (parsed.config->projectPath.empty() && parsed.config->pluginPath.empty())
    {
        std::fprintf(stderr,
            "Arcane Editor: no project selected.\n"
            "  Pass --project <folder-or-.arcproj> to open one,\n"
            "  or --plugin <dll> to host a plugin without a project.\n");
        return 2;
    }
```

- [ ] **Step 3: Verify the gate and every bypass by hand**

```
cd D:\dev\starworks\Gacha\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor
.\ArcaneEditor.exe
echo "bare exit=$LASTEXITCODE"
.\ArcaneEditor.exe --project D:\dev\starworks\Aphelyon --frames 12
echo "project exit=$LASTEXITCODE"
.\ArcaneEditor.exe --print-engine-info
echo "probe exit=$LASTEXITCODE"
```
Expected: bare = the message on stderr, **exit 2, no window**; project run = exit 0 and
the harness still works; probe = one JSON line, exit 0.

- [ ] **Step 4: Remove the now-dead fallback comment**

`Arcane/ArcaneEditor/src/EditorApp.cpp:88` documents "no --project => legacy
data/-next-to-exe boot, unchanged". With the gate in place that is only reachable via
`--plugin`. Update that comment to say so. Do NOT change the `--project`-failed
fallback at `EditorApp.cpp:179-189` — decision 3 in Global Constraints keeps it.

- [ ] **Step 5: Gate under both seeds and commit**

```
cd ..\ArcaneTests
.\ArcaneTests.exe ~[gpu] --rng-seed 6
.\ArcaneTests.exe ~[gpu] --rng-seed 17
```
Expected: `All tests passed` both times.

```bash
git add Arcane/ArcaneEditor/src/main.cpp Arcane/ArcaneEditor/src/EditorApp.cpp
git commit -m "feat(arcane-editor): refuse to boot without a project or an explicit plugin"
```

---

### Task 4: record it

**Files:**
- Modify: `docs/superpowers/specs/2026-07-26-arcane-hub-launcher-design.md`
- Modify: `.superpowers/sdd/progress.md`

- [ ] **Step 1** Mark slice 1 BUILT in the spec, and resolve its open question 3 in
place (gate exits 2 with a stderr message; message box revisited when `ArcaneHub.exe`
exists).
- [ ] **Step 2** Ledger in `.superpowers/sdd/progress.md`: commits, gate counts and
seeds, the hand-run binary checks from Task 2 Step 7 and Task 3 Step 3, and explicitly
that **the Hub itself does not exist yet, so the probe has no consumer** — its only
current protection is the ABI tripwire test.
- [ ] **Step 3** Commit.

## Verification Summary

| What | How | Where |
|---|---|---|
| Probe reports the real ABI | Catch2 tripwire vs `kGamePluginABIVersion` | Task 1 |
| Probe JSON shape / single line | Catch2 | Task 1 |
| Flag parses, defaults off | Catch2 | Task 2 |
| Probe works on both real binaries, no window | Hand-run, exit code checked | Task 2 Step 7 |
| Gate refuses bare launch | Hand-run, exit 2 | Task 3 Step 3 |
| `--project` + `--frames` harness still works | Hand-run against Aphelyon | Task 3 Step 3 |
| No repo caller launches the editor bare | `git grep` audit | Task 3 Step 1 |
