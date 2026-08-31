# Automation Coverage Gaps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three automation gaps left open at the end of arc 2 — machine-dependent asset order, an unbuilt Dist configuration, and a golden gate nothing has ever proven capable of failing.

**Architecture:** Three independent changes plus a user desk checkpoint. Task 1 sorts one engine accessor so the editor's Assets panel — and therefore the `editor-ui` golden image — stops depending on hash iteration order. Task 2 is the user's re-bless, which Task 1 forces and which gates Tasks 4 and 5. Task 3 adds Dist to the GitHub Actions lane. Tasks 4 and 5 give `golden-gate.ps1` a `-SelfTest` mode and run it on `main`.

**Tech Stack:** C++23, Catch2, premake5 + MSBuild (VS 18), PowerShell 5.1, GitHub Actions, Jenkins declarative pipeline.

**Spec:** `docs/specs/2026-08-30-automation-coverage-gaps-design.md`

## Global Constraints

Every task's requirements implicitly include this section.

- **`msbuild` is NOT on PATH:** `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`. Build **`Arcane.slnx`**, never a bare `.vcxproj`. Pass `-nr:false`. `-t:<project>` does NOT work against `Arcane.slnx` (MSB4057) — build the whole solution.
- **A 3-second "successful" msbuild is a NO-OP, not a verification.** Force a real recompile if you get one.
- **Run test exes FROM THEIR OWN DIRECTORY** (`cd bin\Debug-windows-x86_64-md\ArcaneTests`). ArcaneTests runs in random order and resolves data relative to cwd.
- **Always append `~[gpu]`** to suite runs — this machine has a driver-crash hazard. It does **not** exclude `[golden]`/`[mesh]`; those 34 declarations carry no `[gpu]` tag, are CPU-side, and are part of the baseline. Use plain `~[gpu]` so counts stay comparable.
- **Baseline entering this plan: Debug/Release 52296 assertions / 1276 cases; Dist 52228 / 1270**, 0 warnings in all three. **State the numbers you actually measure.**
- **Debug Catch2 ASSERTION line numbers are WRONG** — Debug builds use MSVC `/ZI`, which breaks `__LINE__` inside function bodies; the offset is not constant. `TEST_CASE` lines are correct. Trust the case name, read the source, and do not conclude a file is stale because a line number looks impossible.
- **Line endings are mixed per file.** Check each with `git ls-files --eol` before a content match; an LF-written match against a CRLF file silently finds nothing. **Never normalise.**
- **Exclude `out.txt`** — the user's untracked desk log at the repo root. Stage by explicit path; never `git add -A`.
- **Do NOT touch `ThirdParty/`** or rewrite dated docs under `docs/`.
- **`--bless` writes to the level the reference resolved from, and `golden-gate.ps1` deliberately does NOT restage `Verify/`.** A bless must be copied to both hosts' staged trees before the gate can see it.

---

## File Structure

**T1** — Modify: `ArcaneClient/src/Arcane/Project/AssetRegistry.cpp:391-398`. Test: `ArcaneTests/src/AssetRegistryTest.cpp`
**T2** — no files; a user desk checkpoint
**T3** — Modify: `.github/workflows/ci.yml`
**T4** — Modify: `scripts/golden-gate.ps1`
**T5** — Modify: `Jenkinsfile`

---

### Task 1: `AssetRegistry::All()` returns a deterministic order

**Files:**
- Modify: `ArcaneClient/src/Arcane/Project/AssetRegistry.cpp:391-398`
- Test: `ArcaneTests/src/AssetRegistryTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: no new symbol. `AssetRegistry::All()` keeps its signature `std::vector<std::pair<Guid, std::string>> All() const` and gains a guaranteed order: ascending mount path, `Guid` breaking ties.

**Why this matters:** `m_byGuid` is a `std::unordered_map<Guid, std::string>`, so the current walk's order tracks guid hashes and insertion history. Two trees holding identical files enumerate them differently. The editor's Assets panel renders that order, and the panel is inside the `editor-ui` golden capture.

- [ ] **Step 1: Write the failing test**

Add to `ArcaneTests/src/AssetRegistryTest.cpp`. The file already has a `TempDir` helper in its anonymous namespace (`:24`); reuse it. Add `#include <algorithm>` and `#include <string>` to the include block if absent.

```cpp
TEST_CASE("AssetRegistry::All() is ordered deterministically, not by hash", "[project]")
{
    const auto dir = TempDir("all_deterministic_order");

    // Eight assets whose NAMES sort alphabetically but whose GUIDs deliberately
    // do not. m_byGuid is keyed by Guid, so an unordered_map walk tracks the
    // guid hash and has ~1/40320 odds of coming out name-sorted by luck --
    // which is what makes this test capable of failing against the old code.
    const char* names[] = { "h", "c", "a", "f", "b", "g", "d", "e" };
    const char* guids[] = {
        "11111111-1111-4111-8111-111111111111", "22222222-2222-4222-8222-222222222222",
        "33333333-3333-4333-8333-333333333333", "44444444-4444-4444-8444-444444444444",
        "55555555-5555-4555-8555-555555555555", "66666666-6666-4666-8666-666666666666",
        "77777777-7777-4777-8777-777777777777", "88888888-8888-4888-8888-888888888888",
    };
    for (int i = 0; i < 8; ++i)
        std::ofstream(dir / (std::string(names[i]) + ".arcmat"), std::ios::binary)
            << R"({ "id": ")" << guids[i] << R"(" })";

    Arcane::AssetRegistry reg;
    reg.ScanContent(dir, "game");   // if the progress callback is not defaulted
                                    // in this build, pass {} as the third arg

    const auto all = reg.All();
    REQUIRE(all.size() == 8);

    // THE PROPERTY: sortedness over a TOTAL key makes the sequence a function
    // of the content alone -- independent of hash and insertion order -- which
    // is what makes the editor-ui golden image reproducible on any machine.
    CHECK(std::is_sorted(all.begin(), all.end(),
        [](const std::pair<Arcane::Guid, std::string>& a,
           const std::pair<Arcane::Guid, std::string>& b)
        {
            if (a.second != b.second) return a.second < b.second;
            return a.first < b.first;
        }));
}
```

- [ ] **Step 2: Run it and watch it fail for the right reason**

```bash
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "AssetRegistry::All() is ordered deterministically, not by hash"
```
Expected: **FAIL** on the `CHECK(std::is_sorted(...))` — the unordered_map walk is not name-sorted. If it PASSES, the fixture got lucky: say so, and add more entries rather than declaring the test good.

- [ ] **Step 3: Sort inside `All()`**

`ArcaneClient/src/Arcane/Project/AssetRegistry.cpp:391`, replace the body:

```cpp
    std::vector<std::pair<Guid, std::string>> AssetRegistry::All() const
    {
        std::vector<std::pair<Guid, std::string>> out;
        out.reserve(m_byGuid.size());
        for (const auto& [id, mountPath] : m_byGuid)
            out.emplace_back(id, mountPath);
        // DETERMINISTIC ORDER, and it is load-bearing. m_byGuid is an
        // unordered_map, so the walk above tracks guid HASHES and insertion
        // history: two trees holding identical files enumerate them
        // differently. The editor's Assets panel renders this order and the
        // panel sits inside the editor-ui golden capture, so an unsorted
        // walk makes that image machine-dependent -- measured 2026-08-30.
        // Sorted by mount path (what the panel displays), Guid breaking ties
        // so the order is TOTAL. std::string's operator< goes through
        // char_traits::compare -- byte-wise, NOT locale-aware -- so the result
        // is identical on every machine. A locale-aware sort would reintroduce
        // exactly the machine-dependence this removes.
        std::sort(out.begin(), out.end(),
                  [](const std::pair<Guid, std::string>& a,
                     const std::pair<Guid, std::string>& b)
                  {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                  });
        return out;
    }
```

`<algorithm>` is already included at `AssetRegistry.cpp:9` — do not add it again. `Guid::operator<` is `constexpr` and already defined (`ArcaneCore/src/Arcane/Guid.hpp:45`).

- [ ] **Step 4: Build, run the focused test, then the suite**

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx -p:Configuration=Debug -nr:false
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "AssetRegistry::All() is ordered deterministically, not by hash"
.\ArcaneTests.exe ~[gpu]
```
Expected: focused PASS; suite **52298 assertions / 1277 cases** (baseline 52296/1276 plus this case's 1 case and 2 assertions — REQUIRE + CHECK; 52296 + 2 = 52298). **State the number you actually measure**; if it differs, say so rather than adjusting the expectation.

- [ ] **Step 5: Build Release and Dist too**

```bash
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx -p:Configuration=Release -nr:false
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" Arcane.slnx -p:Configuration=Dist -nr:false
```
Expected: 0 warnings, 0 errors in both.

- [ ] **Step 6: Commit**

```bash
git add ArcaneClient/src/Arcane/Project/AssetRegistry.cpp ArcaneTests/src/AssetRegistryTest.cpp
git commit -m "fix(project): AssetRegistry::All() returns a deterministic order

m_byGuid is an unordered_map, so All()'s walk tracked guid hashes and
insertion history -- two trees with identical files enumerated them
differently. The editor's Assets panel renders that order and sits inside the
editor-ui golden capture, so the image was machine-dependent.

Sorted by mount path with Guid breaking ties, through std::string's byte-wise
operator< rather than any locale-aware comparison.

THIS CHANGES THE editor-ui GOLDEN IMAGE -- row order is part of the picture.
The reference needs a re-bless and a restage before the gate will pass."
```

**After this task the gate is expected to be RED on `editor-ui` until Task 2 completes. That is not a regression; it is the change being visible.**

---

### Task 2: Desk checkpoint — USER

**This task is the user's and cannot be marked complete by an agent.** It needs a GPU and a display. **It gates Tasks 4 and 5 absolutely.**

- [ ] **A. Re-bless `editor-ui` against the sorted order**

```
cd D:\dev\starworks\Arcane
bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project ReferenceProject --headless --backend dx12 --frames 60 --settle 30 --report bless-editor.json --compare editor-ui --bless
```

`--report` is REQUIRED — `--settle` is refused without `--screenshot` or `--report`. Bless once, on dx12: `editor-ui` is the shared reference slot (it is `runtime-scene` that is backend-split).

- [ ] **B. Restage the blessed reference to both hosts**

```
robocopy ReferenceProject\Verify bin\Debug-windows-x86_64-md\ArcaneEditor\ReferenceProject\Verify /E /NFL /NDL /NJH /NJS
robocopy ReferenceProject\Verify bin\Debug-windows-x86_64-md\ArcaneRuntime\ReferenceProject\Verify /E /NFL /NDL /NJH /NJS
```

Required because `golden-gate.ps1` deliberately does not restage `Verify/` — it must not trample a bless. Skip this and the gate compares against the pre-sort reference.

- [ ] **C. Confirm all four lanes green**

```
scripts\golden-gate.ps1 -Configuration Debug
type bin\Debug-windows-x86_64-md\golden-gate-summary.json
```

Expect all four lanes `diffCount=0` and `"gatePassed": true`. **Assert on `gatePassed` and the per-lane `verdict`, never the exit code.**

- [ ] **D. Commit the re-blessed reference**

```bash
git add ReferenceProject/Verify/References/editor-ui.png
git commit -m "chore(verify): re-bless editor-ui for the deterministic asset order"
```

Delete the scratch `bless-editor.json` afterwards; leave `out.txt` alone.

---

### Task 3: Dist joins the GitHub Actions lane

**Files:**
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing. **Independent of Task 2** — it can proceed while the desk checkpoint is outstanding.
- Produces: nothing consumed later.

- [ ] **Step 1: Add the Dist build step**

In `.github/workflows/ci.yml`, immediately after the `Build Release` step:

```yaml
      - name: Build Dist
        run: msbuild Arcane.slnx /p:Configuration=Dist /m /nologo /v:minimal
```

- [ ] **Step 2: Add the Dist test step**

Immediately after the `Test Release (~[gpu])` step (which ends at `:61`):

```yaml
      - name: Test Dist (~[gpu])
        working-directory: bin/Dist-windows-x86_64-md/ArcaneTests
        run: .\ArcaneTests.exe "~[gpu]"
```

Running the suite and not merely the build is deliberate: `ARCANE_DIST` compiles code out, so the Dist binary is genuinely different — **52228 assertions / 1270 cases** against Debug's 52296 / 1276.

- [ ] **Step 3: Verify the YAML parses and the steps are where you think**

```bash
powershell -NoProfile -Command "python -c \"import yaml,sys; d=yaml.safe_load(open('.github/workflows/ci.yml')); print([s['name'] for s in d['jobs']['build']['steps'] if 'name' in s])\""
```
If `python`/`pyyaml` is unavailable, instead read the file and confirm by eye that the four build steps and three test steps appear in the order Debug, Release, Dist. **Say which check you actually ran.**

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: build and test Dist on the GitHub Actions lane

The standing rule is 0 warnings across Debug/Release/Dist, but no CI lane
built Dist -- the shipping configuration, and the one with ARCANE_DIST
compiling code OUT. GH Actions rather than Jenkins because it runs on every
push and needs no GPU. Expected: 52228 assertions in 1270 test cases."
```

---

### Task 4: `golden-gate.ps1` gains `-SelfTest`

**Files:**
- Modify: `scripts/golden-gate.ps1`

**Interfaces:**
- Consumes: **Task 2's green four-lane run.** Do not start without it — a self-test against a stale reference reports a failure that proves nothing about the gate.
- Produces: `golden-gate.ps1 -SelfTest`, consumed by Task 5.

**The discipline this must not break:** the script carries an explicit *"this script only ever CHECKS"* rule. `-SelfTest` mutates, so the mutation must be bounded and said out loud: `Content/` only, never `Verify/`, never blesses, always restores.

- [ ] **Step 1: Add the switch**

`scripts/golden-gate.ps1`, replace the `param(...)` block at the top:

```powershell
param(
    [string]$Configuration = 'Debug',
    # SELF-TEST: prove this gate is CAPABLE OF FAILING. A gate never observed
    # failing is not a gate. Mutates ReferenceProject's scene, runs the four
    # lanes, and asserts ALL FOUR go FAIL -- then restores. This is the ONE
    # mode in which this script writes to the tree; it touches Content/ only,
    # never Verify/, never blesses, and restores in a finally block so an
    # error or a Ctrl-C still leaves the tree clean.
    [switch]$SelfTest
)
```

- [ ] **Step 2: Mutate the scene before staging, when `-SelfTest` is set**

Insert immediately **before** the `foreach ($stageHost in @('ArcaneRuntime', 'ArcaneEditor'))` staging loop (the one that copies `Binaries\*`, near `:136`). It must run before staging so the mutation reaches both staged copies:

```powershell
# ---- -SelfTest: break the scene, so the lanes have something to catch. ----
$sceneRelative = 'ReferenceProject\Content\scenes\main.arcscene'
$scenePath     = Join-Path $repoRoot $sceneRelative
if ($SelfTest) {
    # The SAME mutation desk-verify-golden-gate.ps1 uses -- MeshCube's
    # Transform position x, 0.4 -> 0.6. One mutation vocabulary, not two: a
    # second one would drift from this one exactly the way hand-copied logic
    # drifted elsewhere in this tree.
    $sceneText = Get-Content -Raw $scenePath
    $broken    = $sceneText -replace '(?<="x"\s*:\s*)0\.4', '0.6'
    if ($broken -eq $sceneText) {
        Write-Error "-SelfTest: could not break $sceneRelative -- no `"x`": 0.4 found. The fixture moved; fix this script rather than reporting a pass."
        exit 1
    }
    Set-Content -Path $scenePath -Value $broken -NoNewline
    Write-Host "-- -SelfTest: scene deliberately broken (MeshCube x 0.4 -> 0.6) --" -ForegroundColor Yellow
}
```

- [ ] **Step 3: Assert and restore, after the summary is written**

Insert immediately **after** the `$summary | ConvertTo-Json` write and **before** the script's final exit. Find the tail by content — the block that writes `golden-gate-summary.json`:

```powershell
# ---- -SelfTest: every lane must have NOTICED. ----
if ($SelfTest) {
    # Restore FIRST, in a finally-equivalent position, so a failed assertion
    # below still leaves the tree clean.
    try {
        $notFailed = @($results | Where-Object { $_.Verdict -ne 'FAIL' })
        if ($notFailed.Count -gt 0) {
            Write-Host ""
            Write-Host "SELF-TEST FAILED -- the gate did NOT notice a broken scene." -ForegroundColor Red
            $notFailed | ForEach-Object { Write-Host "  $($_.Combo) reported $($_.Verdict)" -ForegroundColor Red }
            Write-Host "A gate that cannot fail is not a gate. Fix the gate, not this check." -ForegroundColor Red
            $selfTestOk = $false
        } else {
            Write-Host ""
            Write-Host "SELF-TEST PASSED -- all $($results.Count) lane(s) caught the broken scene." -ForegroundColor Green
            $selfTestOk = $true
        }
    } finally {
        & git -C $repoRoot checkout -- $sceneRelative
        $dirty = & git -C $repoRoot status --porcelain -- $sceneRelative
        if ($dirty) { Write-Error "-SelfTest: FAILED TO RESTORE $sceneRelative -- fix by hand before continuing."; exit 1 }
        Write-Host "-- -SelfTest: scene restored --" -ForegroundColor DarkGray
    }
    # The self-test INVERTS the ordinary verdict: red lanes are the pass.
    if ($selfTestOk) { exit 0 } else { exit 1 }
}
```

**Assert on the per-lane `Verdict`, not on any `exitReason`** — a lane that fails by erroring instead of by comparing is still a lane that noticed.

- [ ] **Step 4: Update the script header**

The header's usage block must show the new mode, and the *"only ever CHECKS"* claim must be reconciled rather than left to quietly contradict `-SelfTest`. Add to the usage examples:

```
#   powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -SelfTest
#       Prove the gate can FAIL: break the scene, assert all four lanes go red,
#       restore. The ONE mode that writes to the tree -- Content/ only, never
#       Verify/, never a bless, always restored in a finally.
```

- [ ] **Step 5: Verify it parses, and that the self-test itself can fail**

```bash
powershell -NoProfile -ExecutionPolicy Bypass -Command "$null = [ScriptBlock]::Create((Get-Content -Raw scripts\golden-gate.ps1)); 'parses OK'"
git status --porcelain -- ReferenceProject
```
Expected: `parses OK`, and `git status` clean.

**Do NOT run `-SelfTest` here** — it launches four GPU hosts and this machine has a driver-crash hazard. Its first real run is Task 5's, at the desk or on the Jenkins agent. **Say plainly in your report that the mode is unexecuted**, and that the check which would catch a broken self-test — running it against an *unmutated* tree and confirming it reports the lanes did NOT fail — is owed to whoever first runs it.

- [ ] **Step 6: Commit**

```bash
git add scripts/golden-gate.ps1
git commit -m "feat(gate): -SelfTest proves the gate is capable of failing

A gate never observed failing is not a gate, and nothing automated has ever
checked that property -- it lived in a desk script someone had to remember to
run. -SelfTest breaks the scene with the same mutation desk-verify uses,
asserts ALL FOUR lanes go FAIL, and restores in a finally.

Asserts on the per-lane Verdict, not on any exitReason: a lane that fails by
erroring still noticed. This is the one mode that writes to the tree --
Content/ only, never Verify/, never a bless."
```

---

### Task 5: Jenkins runs the self-test on `main`

**Files:**
- Modify: `Jenkinsfile`

**Interfaces:**
- Consumes: Task 4's `-SelfTest` switch, and **Task 2's evidence**. Do not start without both.
- Produces: nothing consumed later.

- [ ] **Step 1: Add the stage**

In `Jenkinsfile`, immediately after the existing `stage('Golden gate')` block closes:

```groovy
                stage('Golden gate self-test') {
                    // A gate never observed failing is not a gate. This breaks
                    // the scene on purpose and asserts all four lanes catch it.
                    //
                    // main/milestone ONLY: the property under test belongs to
                    // the GATE, which changes rarely, and this runs the full
                    // four-lane gate a SECOND time -- roughly doubling its wall
                    // clock on the one agent that runs everything else.
                    when { anyOf { branch 'main'; branch 'milestone/*' } }
                    steps {
                        bat 'powershell -NoProfile -ExecutionPolicy Bypass -File scripts\\golden-gate.ps1 -Configuration Debug -SelfTest'
                    }
                }
```

- [ ] **Step 2: Verify the pipeline still parses**

```bash
git diff Jenkinsfile
```
Read the diff and confirm: the `when` block is inside the stage and before `steps`, the braces balance, and the stage sits inside the same `stages { }` as `Golden gate`. **If the repo has a Jenkins CLI or a linter configured, run it and say so; if not, say that the syntax was verified by reading only.**

- [ ] **Step 3: Commit**

```bash
git add Jenkinsfile
git commit -m "ci(jenkins): run the golden-gate self-test on main and milestone

Nothing automated had ever proven the gate could fail. Gated to
main/milestone because the property belongs to the gate, which changes
rarely, and the self-test runs the four-lane gate a second time on the single
GPU agent."
```

---

## Notes for whoever executes this

- **Task 1 leaves the gate RED on `editor-ui` until Task 2 completes.** That is the change becoming visible, not a regression. Do not "fix" it by reverting the sort, and do not re-bless from an agent — the bless needs a GPU.
- **Task 3 is independent of Task 2** and can proceed while the desk checkpoint is outstanding. Tasks 4 and 5 cannot.
- **Task 4's `-SelfTest` ships unexecuted.** That is unavoidable here — running it needs four GPU host launches — but it must be reported as unexecuted rather than implied to be verified. Plan-supplied code is unrun code.
- **Ask of every check: would this FAIL if the thing it names were wrong?** Task 1 Step 2 exists to fail. If it passes before the fix, the fixture got lucky — enlarge it rather than declaring success.
- **This plan does not close Finding Q4-F1** (declarable exclusion with a mandatory reason, reported as Skipped) or the host-level witnesses (`settleCaptureFailed`, the `compare-missing-reference` key absence, `--settle-timeout` actually being spent). Both are named non-goals in the spec.
