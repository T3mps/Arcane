# `--headless` Rename Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the offscreen-rendering CLI flag from `--offscreen` to `--headless` with a hard break, after freeing the word by moving the engine's existing device-less sense of "headless" to `deviceless`.

**Architecture:** Two mechanical passes in a fixed order. Pass one renames ~112 comment lines (plus one method and two test-case names) that use "headless" to mean *no GPU device*, so the word carries exactly one meaning. Pass two renames the flag itself across 144 live sites and **removes** `--offscreen` in the same commit, since there is no alias. A user desk checkpoint closes the arc.

**Tech Stack:** C++23, Catch2, premake5 + MSBuild (VS 18), PowerShell 5.1, Rust/Tauri (ArcaneHub, read-only here).

**Spec:** `docs/specs/2026-08-27-headless-rename-design.md`

## Global Constraints

Every task's requirements implicitly include this section.

- **`msbuild` is NOT on PATH:** `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`. Build `Arcane.slnx`, never a bare `.vcxproj`. Pass `-nr:false`.
- **`ARCANE_SDK` must be set** and may be stale in the process — override per-invocation if a build behaves oddly.
- **Run test exes FROM THEIR OWN DIRECTORY** (`cd bin\Debug-windows-x86_64-md\ArcaneTests` first). ArcaneTests runs in random order and resolves data relative to cwd.
- **Never run `[gpu]`, `[golden]`, or `[mesh]`** in the dev loop — this machine carries a driver-crash hazard and GPU runs happen at the physical desk. Always append `~[gpu]`.
- **Gate baseline to hold: 52203 assertions / 1254 cases**, 0 warnings across Debug/Release/Dist.
- **A 3-second "successful" msbuild is a NO-OP, not a verification.** If a build returns that fast, it did not compile your change.
- **New or renamed source files are picked up by a glob** (`premake5.lua:186`), so **re-run `GenerateProjects.bat` after adding or renaming any file**, or MSBuild will not see it.
- **DO NOT touch** `ThirdParty/` (~167 "headless" mentions, mostly Vulkan headers).
- **DO NOT rewrite historical docs** — `docs/plans/*`, `docs/specs/2026-08-23-*`, `docs/specs/2026-08-25-*`, `docs/2026-08-27-servitor-rulings-record.md` (70 `--offscreen` mentions between them). They are records; editing them makes them worse.
- **DO NOT rename the render-technique layer**: `OffscreenVehicle`, `CreateOffscreen()`, `IsOffscreen()`, `[offscreen]` log tags all stay. Only the **flag** and `HostConfig::offscreen` change.
- **DO NOT fix the four owed engine defects** (settle unit mismatch, `Saved/Diagnostics/` capture dependency, `settleAttemptsUsed`, `--fixed-dt nan`) or the ArcaneHub `golden_run()` bug. Separate arc.
- **This arc changes no pixels.** If a reference image appears to need re-blessing, that is a bug in the rename. **Never re-bless anything here.**

---

## File Structure

**Task 1 — `deviceless` rename** (~40 files, ~112 lines, overwhelmingly comments):
- Modify: `ArcaneClient/src/Arcane/Audio/AudioDevice.cpp` — `PumpHeadless()` → `PumpDeviceless()` (declaration `:350`, call `:779`), plus comments `:436`, `:775`
- Modify: `ArcaneClient/src/Arcane/Audio/AudioDevice.hpp:43`, `AudioTypes.hpp:72`
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.cpp:175`, `Runtime.hpp:64`
- Modify: `ArcaneClient/src/Arcane/Host/SceneRenderResolver.hpp:76` (**the load-bearing one**), `:211`
- Modify: `ArcaneClient/src/Arcane/Render/Nri/**` — `NriDevice.hpp:102,171`, `NriGraphContext.{cpp,hpp}`, `NriDiagnostics.hpp:97`, `RenderGraph.hpp`, `RenderGraphExec.cpp`, `NriUploadRing.{cpp,hpp}`, `nodes/*`
- Modify: `ArcaneClient/src/Arcane/Render/{GpuInstrumentation.hpp:67, DeviceCreationVulkan.cpp:48-50}`, `ImGui/ImGuiNri.hpp:269`, `Assets/ImageIo.hpp:3`
- Modify: test files whose comments use the device-less sense (`RenderGraphTest.cpp`, `NriHostFlavorTest.cpp`, `NriSubstrateTest.cpp`, `NriDiagnosticsTest.cpp`, `NriGraphPixelTest.cpp`, `AudioDeviceTest.cpp`, …)

**Task 2 — the flag** (144 live sites):
- Modify: `ArcaneClient/src/Arcane/Host/HostConfig.cpp` (10), `HostConfig.hpp` (3)
- Modify: `ArcaneRuntime/src/{RuntimeApp.cpp (11), RuntimeFrame.cpp (11), RuntimeApp.hpp (5), RuntimeFrame.hpp (5), main.cpp (2)}`
- Modify: `ArcaneEditor/src/{App/EditorApp.cpp (12), main.cpp (8), App/EditorAppFrame.cpp (6), App/EditorApp.hpp (4)}`
- Modify: `ArcaneClient/src/Arcane/Host/VerifyReport.{cpp,hpp}` (1 each), `Render/Nri/NriGraphContext.{cpp,hpp}` (1 each), `Host/OffscreenVehicle.{cpp,hpp}` (1 each)
- Modify: `ArcaneTests/src/{HostConfigTest.cpp (46), GoldenImageTest.cpp (1), VerifyReportTest.cpp (1)}`
- Modify: `scripts/golden-gate.ps1:349`, `scripts/desk-verify-servitor.ps1:261`
- Modify (**data, easy to miss**): `ReferenceProject/Saved/verify-layout.ini` (3, incl. a runnable command), `ReferenceProject/.gitignore:11`, `ReferenceProject/Verify/Traps/README.md` (4)
- Modify: `ArcaneHub/src/lib/views/PackagesView.svelte` (1, prose), `docs/2026-08-26-servitor-closeout-and-desk-verify.md` (2)

**Task 3** — no files; a user desk checkpoint.

---

### Task 1: Free the word — the `deviceless` rename

**Files:** as listed above under "Task 1". ~40 files, ~112 lines.

**Interfaces:**
- Consumes: nothing.
- Produces: `PumpDeviceless(double dtSeconds)` replacing `PumpHeadless(double)` on `AudioDevice`'s impl (private; no external callers). No other signature changes.

**Why this task is first:** until the engine's device-less sense stops using the word, Task 2's new `--headless` flag lands beside ~112 comment lines using "headless" to mean the opposite. Doing it second would mean reviewing the flag rename against text that contradicts it.

- [ ] **Step 1: Classify every non-ThirdParty use, and write the classification down**

Run:
```bash
cd D:/dev/starworks/Arcane
git grep -in "headless" -- '*.cpp' '*.hpp' ':(exclude)ThirdParty' > /tmp/headless-audit.txt
wc -l /tmp/headless-audit.txt
```
Expected: **228** lines.

For each line decide, from the sentence it sits in:
- **DEVICE-LESS** (rename → `deviceless` / `device-less`): it means *no GPU device, no backend, the NONE backend, a null audio device*. Example, `NriDevice.hpp:102`: "The NONE backend, for HEADLESS TESTS ONLY".
- **NO-WINDOW** (leave): it means *no window, no UI, no display*. Example, `ImGuiTest.cpp:17`: "imgui: context creates headlessly".

Expected split: roughly **112 device-less / 116 no-window**.

**These 8 files hold BOTH meanings and need line-by-line judgment — do not treat any of them as uniform:**
`ArcaneClient/src/Arcane/Host/HostConfig.hpp`, `ArcaneClient/src/Arcane/Render/Nri/NriGraphContext.cpp`, `…/NriGraphContext.hpp`, `ArcaneEditor/src/App/EditorApp.cpp`, `ArcaneEditor/src/main.cpp`, `ArcaneRuntime/src/RuntimeApp.cpp`, `ArcaneRuntime/src/RuntimeFrame.cpp`, `ArcaneTests/src/HostConfigTest.cpp`.

**A blind `sed -i s/headless/deviceless/g` is a plan failure.** It would rename both senses and destroy the distinction this task exists to create.

- [ ] **Step 2: Rename `PumpHeadless` — the only real identifier**

`ArcaneClient/src/Arcane/Audio/AudioDevice.cpp:350`:
```cpp
void PumpDeviceless(double dtSeconds)
```
and its sole call site, `:779`:
```cpp
m_impl->PumpDeviceless(dtSeconds);
```

- [ ] **Step 3: Apply the DEVICE-LESS renames from Step 1's classification**

Comment text only, apart from Step 2. Use the natural form for the sentence — `deviceless` as an identifier, `device-less` in prose. Two worked examples:

`SceneRenderResolver.hpp:76` (the load-bearing site — it decides material binding):
```cpp
// The frame batcher registered materials bind into. Null (a device-less host
// or a test) disables material binding; sprite resolution still works.
```

`NriDevice.hpp:102`:
```cpp
// The NONE backend, for DEVICE-LESS TESTS ONLY (Task 6's [nri] graph
```

- [ ] **Step 4: Rename the two test-case name strings**

`ArcaneTests/src/GraphCanvasHeadlessTest.cpp:23`:
```cpp
TEST_CASE("Graph document survives device-less ImGui frames", "[editor][graphcanvas]")
```
`ArcaneTests/src/ImGuiTest.cpp:17`:
```cpp
TEST_CASE("imgui: context creates device-lessly", "[imgui]")
```

**Leave the FILENAME `GraphCanvasHeadlessTest.cpp` alone.** Renaming it forces a `GenerateProjects.bat` re-run for zero benefit, and the tags (`[editor][graphcanvas]`) are what anything selects on.

- [ ] **Step 5: Verify the classification held — the check that can actually fail**

```bash
cd D:/dev/starworks/Arcane
git grep -in "headless" -- '*.cpp' '*.hpp' ':(exclude)ThirdParty' | grep -icE "device|batcher|gpu|nri|null backend|backend|swapchain|audio"
```
Expected: **0** — no surviving use of "headless" sits in device/backend vocabulary.

```bash
git grep -ci "headless" -- '*.cpp' '*.hpp' ':(exclude)ThirdParty'
```
Expected: **~116**, all no-window sense.

If the first command returns non-zero, read each hit: either it is a device-less use you missed, or it is a no-window use that merely mentions a device nearby. Say which in the report — do not silently adjust the grep to make it pass.

- [ ] **Step 6: Build and run the suite**

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug -nr:false
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```
Expected: **52203 assertions in 1254 test cases**, all passing, 0 warnings. The count must not move — this task changes comments and one private method name.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "refactor(vocab): the device-less sense of 'headless' becomes 'deviceless'

Frees the word for the CLI flag. Comment text throughout, plus
AudioDevice's private PumpHeadless -> PumpDeviceless and two test-case
name strings. No behaviour change; assertion count unmoved."
```

---

### Task 2: Rename the flag, and remove `--offscreen`

**Files:** as listed above under "Task 2". 144 live sites.

**Interfaces:**
- Consumes: Task 1's freed vocabulary — after Task 1 no engine comment uses "headless" for the device-less sense.
- Produces: `HostConfig::headless` (was `HostConfig::offscreen`), `bool`, default `false`. The CLI flag `--headless`. **`--offscreen` no longer exists** and is refused as an unknown argument.

**This entire task is ONE commit.** A tree where `--offscreen` is gone but `golden-gate.ps1` still passes it is broken. There is no alias and no grace period.

- [ ] **Step 1: Write the failing test — the removal is the requirement**

Add to `ArcaneTests/src/HostConfigTest.cpp`, beside the other refusal cases:

```cpp
// --offscreen was RENAMED to --headless (2026-08-27), hard break, no alias.
// This case exists because the rename could otherwise HALF-LAND: --headless
// added, --offscreen never removed, every existing caller still working, and
// nothing to show the job was unfinished. It asserts the removal, which is
// the part with no other witness.
TEST_CASE("host config: --offscreen is gone and is refused as unknown", "[hostconfig]")
{
    const auto old = Run({"--offscreen", "--frames", "60"});
    REQUIRE_FALSE(old.config.has_value());
    CHECK(old.exitCode == 2);

    // ...and the replacement does what the old flag did.
    const auto now = Run({"--headless", "--frames", "60"});
    REQUIRE(now.config.has_value());
    CHECK(now.config->headless);
}
```

- [ ] **Step 2: Run it and watch it fail for the RIGHT reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "host config: --offscreen is gone and is refused as unknown"
```
Expected: **compile error** — `HostConfig` has no member `headless`. That is the correct first failure. (Once it compiles, expect the first `REQUIRE_FALSE` to fail, because `--offscreen` still parses.)

- [ ] **Step 3: Rename the flag and the config field**

`ArcaneClient/src/Arcane/Host/HostConfig.cpp:21` — registration:
```cpp
        cli.Flag  ("headless",           "render with no window shown and no swapchain; "
```
`:88` — read:
```cpp
        cfg.headless       = r.Flag("headless");
```
`HostConfig.hpp` — the field (was `bool offscreen = false;`):
```cpp
        bool            headless = false;
```
Update its doc comment, which currently explains why the flag is *not* called `--headless` (`HostConfig.hpp:45-47`). Replace that paragraph with the new rationale: the flag is `--headless` because that is what every comparable tool calls it; "offscreen" remains the name of the *technique* (`OffscreenVehicle`, `CreateOffscreen`), which is deliberately unchanged.

- [ ] **Step 4: Update every remaining `--offscreen` mention in help text and refusals**

In `HostConfig.cpp`: option help at `:24` (`"(--offscreen only)"`), `:33`, `:39`; comments `:102`, `:246`, `:306`, `:312`, `:383`, `:393`; and the refusal messages at `:162`, `:318-320`, `:398-399`. Every user-visible string must say `--headless`.

Then the hosts (`RuntimeApp.cpp`, `RuntimeFrame.cpp`, `RuntimeApp.hpp`, `RuntimeFrame.hpp`, `ArcaneRuntime/src/main.cpp`, `EditorApp.cpp`, `EditorApp.hpp`, `EditorAppFrame.cpp`, `ArcaneEditor/src/main.cpp`), `VerifyReport.{cpp,hpp}`, `NriGraphContext.{cpp,hpp}`, `OffscreenVehicle.{cpp,hpp}`, and the remaining test files.

**Every `cfg.offscreen` / `config->offscreen` / `m_config.offscreen` / `io.config.offscreen` becomes `.headless`.** These are real code, not comments.

- [ ] **Step 5: Update the non-C++ callers — the ones most likely to be missed**

```
scripts/golden-gate.ps1:349                     '--offscreen',   ->  '--headless',
scripts/desk-verify-servitor.ps1:261            '--offscreen',   ->  '--headless',
ReferenceProject/Saved/verify-layout.ini        3 comment lines, incl. a runnable command at :16
ReferenceProject/.gitignore:11                  1 comment
ReferenceProject/Verify/Traps/README.md         4 regeneration commands
ArcaneHub/src/lib/views/PackagesView.svelte     1 prose mention
docs/2026-08-26-servitor-closeout-and-desk-verify.md   2 runnable commands
```

`verify-layout.ini` and `.gitignore` are **data files**, invisible to a code-only search. They are the two most likely misses in this task.

- [ ] **Step 6: Verify the removal is complete and nothing historical was touched**

```bash
cd D:/dev/starworks/Arcane
git grep -c -- "--offscreen" -- . ":(exclude)docs/plans" ":(exclude)docs/specs" ":(exclude)docs/2026-08-27-servitor-rulings-record.md"
```
Expected: **no matches** (git grep exits 1). Any hit is a missed caller.

```bash
git grep -c -- "--offscreen" -- 'docs/plans/*' 'docs/specs/2026-08-23*' 'docs/specs/2026-08-25*'
```
Expected: **unchanged at 69** — historical docs must NOT have been edited.

- [ ] **Step 7: Build all three configurations and run the suite**

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Debug -nr:false
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Release -nr:false
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx /p:Configuration=Dist -nr:false
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe ~[gpu]
```
Expected: 0 warnings in all three. Suite: **52207 assertions in 1255 test cases** — the baseline 52203/1254 **plus** Step 1's new case, which contributes 1 case and 4 assertions (`REQUIRE_FALSE`, `CHECK`, `REQUIRE`, `CHECK`). State the actual number in the report; **if it differs, say so rather than adjusting the expectation** — a moved count means something else changed too, and that is worth knowing.

- [ ] **Step 8: Commit — one commit, everything**

```bash
git add -A
git commit -m "feat(cli)!: rename --offscreen to --headless, and remove --offscreen

Hard break, no alias: every comparable tool calls this headless, and
the word was only unavailable because the engine used it for
device-less. Task 1 freed it.

Safe because the parser refuses unregistered arguments with exit 2
before any window or device exists -- had it ignored them, this rename
would have silently downgraded every gate invocation to a WINDOWED run.

All 144 live sites in one commit, callers included: both hosts, the
tests, golden-gate.ps1, desk-verify-servitor.ps1, verify-layout.ini,
ReferenceProject/.gitignore, the trap README, PackagesView.svelte and
the close-out doc. Historical docs deliberately untouched."
```

---

### Task 3: Desk checkpoint — USER

**This task is the user's and cannot be marked complete by an agent.** It needs a GPU and a display.

The rename changes no pixels, so the gate is the instrument that proves it: if the hosts still boot, still render the same frames, and still compare 0-diff, the rename was faithful.

- [ ] **A. The gate still runs and still passes its runtime lanes**

```
cd D:\dev\starworks\Arcane
scripts\golden-gate.ps1 -Configuration Debug -AdvisoryLanes ArcaneEditor
```
Expect **exit 0**, both `ArcaneRuntime` lanes **PASS with diffCount=0**, both `ArcaneEditor` lanes **KNOWN-RED advisory at 24 px** — unchanged from before this arc. The two editor lanes are still red on the owed defects; **this arc does not fix them and must not appear to.**

Then confirm the machine-readable verdict still lands:
```
type bin\Debug-windows-x86_64-md\golden-gate-summary.json
```
Expect `"gatePassed": true` with `"advisoryFailures": 2`.

- [ ] **B. The desk-verify script still completes**

```
scripts\desk-verify-servitor.ps1
```
Expect phases A–C as before: A1 passes, A2 **fails on the broken scene** (the gate catching it), B's bless round-trip passes, C clean.

- [ ] **C. The old flag is gone, observably**

```
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --offscreen --frames 1
```
Expect `error: unknown argument '--offscreen'` and **exit 2** — no window, no device.

- [ ] **D. Your ArcaneHub saved launch args**

Any project with `--offscreen` saved in its Hub launch args will now fail to launch with that same parse error. These live in your Hub data, not the repo, so no commit can fix them. Edit them to `--headless`.

---

## Notes for whoever executes this

- **Plan-supplied code is unrun code.** Every snippet here was written against the tree at `162c3fd7` with citations re-derived rather than recalled — but if a snippet does not compile, fix it and say so in the report. Do not contort the design to match a plan that was wrong.
- **Ask of every check: would this FAIL if the thing it names were wrong?** Task 1 Step 5 and Task 2 Step 6 are written to be capable of failing. That question has found a real hole in every task of the last two arcs.
- **A search whose parameters cannot reach the answer returns nothing, and nothing is not evidence.** This project has hit that nine times in nine different tools, including a `git grep` that missed `cli.Flag(` because it searched for `cli.Option(`. Before trusting any zero, ask what the command cannot see.
- **The 8 dual-meaning files in Task 1 are where this goes wrong.** Everywhere else, a file's uses are usually all one sense.
