# Game-module source directory (`sourceDir`) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Arcane project can keep its game module's sources in a subdirectory of `Source/` (Unreal's `Source/<Module>/`), declared ONCE in the `.arcproj` as `"sourceDir": "Source/Game"`, so that the SDK's build glob, the editor's Create C++ Class default and the docs all agree; `Source/` itself stays the `source://` mount so the Asset Browser shows every module (game + services) under one root. ReferenceProject adopts `Source/Game/` as the canonical fixture; a manifest without the field behaves exactly as today (`Source/`), so Aphelyon's current `Game/` project builds unchanged.

**Architecture:** The manifest is the single source of truth (decision record `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md` §5, ruling L3). `ProjectManifest` gains an optional `sourceDir` (default `"Source"`, validated to be `Source` or `Source/<sub…>` with no `..`). `build/arcane.lua`'s `arcane_game_module(name)` locates the one `*.arcproj` beside the invoking `premake5.lua` (`os.matchfiles(_MAIN_SCRIPT_DIR .. "/*.arcproj")`, `json.decode` — both proven available in premake 5.0.0-beta8 on 2026-09-16) and globs `<sourceDir>/**` instead of `Source/**`. The editor keeps `CreateKindRoot(CppClass) == "Source"` (the mount) and derives the Location combo's DEFAULT folder for a C++ class from the manifest (`"Game/"` for `Source/Game`), through one pure function the tests pin. Nothing about mounts, GUID registration, arcbuild's commands or the plugin ABI changes — no ABI bump.

**Tech Stack:** C++23, premake5 (Lua + its bundled `json`), MSBuild (VS 18), Catch2 (ArcaneTests), the arcbuild driver, PowerShell gate scripts.

**Spec:** `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md` §5 + rulings L1–L3 (the decision), `docs/specs/2026-07-22-arcane-project-format-design.md` §4 (manifest schema) and §5 (folder skeleton) — both amended by this plan; `docs/specs/2026-09-13-arcbuild-driver-design.md` (unchanged commands; one sentence added).

## Global Constraints

- **One repo, four commits**, Arcane `D:\dev\starworks\Arcane`, branch `main`, HEAD `a75b87fd` at plan time (fully pushed). **Do not push.** Trailers on every commit:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
  Never stage the strays (`out.txt`, `ArcaneEditor/ArcaneEditor/`, `ArcaneAssetPipeline/ArcaneAs.*/`, `arcbuild/arcbuild/`). Stage by explicit path. Generated `ReferenceProject/*.slnx`, `ReferenceProject/*.vcxproj*`, `ReferenceProject/Binaries/`, `Intermediate/`, `Saved/` are gitignored; never force-add.
- **Engine build ritual (bash, from the repo root):** regenerate with `ThirdParty/premake5/premake5.exe vs2026` (never `GenerateProjects.bat`/`generate.bat` under the Bash tool — they hang); build the SOLUTION, never a bare vcxproj: `MSYS_NO_PATHCONV=1 "/c/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe" Arcane.slnx -p:Configuration=<Debug|Release> -m -nologo -v:m > <log> 2>&1` (FOREGROUND, `timeout` 600000; incremental is fine), then `grep -E " error |Error\(s\)|Warning\(s\)" <log>`. Output: `bin/<cfg>-windows-x86_64-md/<Project>/`.
- **Suites run FROM the exe dir, FOREGROUND:** `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "<filter>" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"` — never `cat` a suite log. The whole-suite filter is `~[gpu]` (last booked baseline at plan time was actually 57588 / 1791 in scripts/automation-baselines.json — the 57562 / 1788 this plan first quoted was a stale recollection, which is exactly why the rule is DERIVE the new numbers, never recall). Record every seed.
- **ReferenceProject through arcbuild:** `bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe build --project ReferenceProject --config Debug` (from the repo root) → `ReferenceProject/Binaries/ReferenceGame.dll`. `ReferenceProject/Binaries/` is a SINGLE slot for every configuration: leave it holding the **Debug** DLL when you finish (the Debug `[gpu]`/golden lanes load it). Do not run the Release gate in this plan.
- **Golden gate (Task 4 only):** `pwsh -NoProfile -File scripts/golden-gate.ps1 -Configuration Debug` from the repo root (read its header for the exact parameters before running; expected `4/4 diffCount=0`). It is the product check that ReferenceGame.dll built from the new path still renders the reference scenes.
- **The Gacha Game project is the "no field" witness** (Task 2): `bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe probe --project /d/dev/starworks/Gacha/Game --config Debug` must still exit 0 or 3 (never a premake error) after the arcane.lua change, and `build` must succeed. `Game/Aphelyon.arcproj` has NO `sourceDir` and must not gain one in this plan. Do not commit anything in the Gacha repo.
- **No ABI bump** (`kGamePluginABIVersion` stays 31): no header a module compiles against changes shape — `ProjectManifest` is host-side. Verify at Task 4 with `grep -n "kGamePluginABIVersion = " ArcaneCore/src/Arcane/Plugin/PluginABI.hpp`.
- **Ledger:** `.superpowers/sdd/2026-09-16-game-module-source-dir-plan/progress.md`.

---

## Plan-time rulings

| # | Ruling | Why |
|---|---|---|
| S1 | **The manifest, not a premake option, carries the module source dir.** `arcane_game_module(name)` keeps its one-argument signature and READS the manifest. | Two places to state the directory (a premake option and the editor's create default) is a drift the editor cannot detect: a class written to `Source/` that the build never globs. One field, three readers (build, editor, docs). premake's `os.matchfiles` + `json.decode` were proven on 2026-09-16 (`_MAIN_SCRIPT_DIR` is the project root for every in-repo and external project — both `premake5.lua` files sit beside their manifest). |
| S2 | **`source://` stays mounted at `Source/`; only the C++-class DEFAULT folder moves.** `CreateKindRoot(CppClass)` is unchanged. | The Asset Browser is meant to show the whole codebase (game module + services) under one `Source/` root — that is the feature the layout buys. Registration/resolution by GUID keeps working for anything under the mount; a user who deliberately picks a non-module folder gets today's behaviour. |
| S3 | **Validation is strict and loud:** `sourceDir` must be exactly `Source` or start with `Source/`, contain no `..` segment and no backslash; anything else is a schema violation (`FromJson` → nullopt) and arcane.lua `error()`s with the same rule. | A module directory outside the `source://` mount could never register a created class; a silently-accepted bad value is the failure class this engine keeps meeting (cvar spec §5.4's argument). |
| S4 | **ReferenceProject adopts `Source/Game/`** (its one file, `ReferenceGame.cpp`, moves; `.gitkeep` goes with it) and declares the field. | The canonical fixture must exercise the new path or the path is untested by every golden and `[build]` lane. Its module name stays `ReferenceGame` — the field decouples directory from module name by design. |
| S5 | **Exactly one manifest beside the premake script, else `error()`.** Zero manifests → error naming the directory; two or more → error listing them. | A project with two manifests is already malformed for the hosts; guessing would build the wrong module. |
| S6 | **No ABI bump, no Gacha commit.** Aphelyon's `Game/` keeps no field until the relocation plan moves it to `Source/Game/`. | The field is additive and optional; the relocation is its own plan. |

---

## File structure

| File | Change | Responsibility |
|---|---|---|
| `ArcaneCore/src/Arcane/Project/ProjectManifest.hpp` | Modify (T1): `std::string sourceDir = "Source";` + comment | The parsed field. |
| `ArcaneCore/src/Arcane/Project/ProjectManifest.cpp` | Modify (T1): parse + validate after `guid` | S3's rule, once. |
| `ArcaneTests/src/ProjectManifestTest.cpp` | Modify (T1): 3 new cases | Parses / defaults / rejects. |
| `docs/specs/2026-07-22-arcane-project-format-design.md` | Modify (T1: §4 schema + §5 skeleton note) | The living schema. |
| `build/arcane.lua` | Modify (T2): manifest lookup, glob + includedir from `sourceDir`, header comment | The SDK's single reader of the field. |
| `ReferenceProject/Source/ReferenceGame.cpp` → `ReferenceProject/Source/Game/ReferenceGame.cpp`, `.gitkeep` likewise; `ReferenceProject/ReferenceProject.arcproj` (+ `"sourceDir"`, description) | Move + Modify (T2) | The canonical fixture on the new path. |
| `docs/specs/2026-09-13-arcbuild-driver-design.md` | Modify (T2: one sentence) | Records that the glob root is the manifest's. |
| `ArcaneEditor/src/Panels/CreateAssetDialog.hpp` | Modify (T3): `CppClassDefaultFolder(std::string_view sourceDir)`; `CreateAssetRequest`/state gains `std::string cppDefaultFolder` | The editor's single derivation of the default. |
| `ArcaneEditor/src/Panels/CreateAssetDialog.cpp` | Modify (T3): `BuildFolderChoices` + the seed use the request's default for CppClass | Location combo default. |
| `ArcaneEditor/src/App/EditorAppProject.cpp` (and wherever the app raises a CppClass create request) | Modify (T3): seed `cppDefaultFolder` from `Manifest().sourceDir`; the `Source/**` comment | The manifest reaches the dialog. |
| `ArcaneTests/src/CreateAssetDialogTest.cpp` | Modify (T3): pin `CppClassDefaultFolder` | The derivation's contract. |
| `CLAUDE.md` (Arcane) | Modify (T4): the project-skeleton sentence, if it names `Source/` as the module dir | Living doc. |
| `scripts/automation-baselines.json` | Modify (T4) | Booked suite numbers. |
| `docs/plans/2026-09-16-game-module-source-dir-plan.md` (this file) | Modify (T4: Closeout) | The record. |

---

### Task 1: `ProjectManifest.sourceDir` — parse, validate, default (Core)

**Files:**
- Modify: `ArcaneCore/src/Arcane/Project/ProjectManifest.hpp` (after the `guid` member, ~line 76)
- Modify: `ArcaneCore/src/Arcane/Project/ProjectManifest.cpp` (after `m.guid = …`, ~line 46)
- Modify: `ArcaneTests/src/ProjectManifestTest.cpp` (append)
- Modify: `docs/specs/2026-07-22-arcane-project-format-design.md` (§4 schema block ~line 100-117; §5 bullet on `Source/` ~line 139)

**Interfaces:**
- Produces: `std::string ProjectManifest::sourceDir` — always non-empty, `"Source"` by default, forward-slashed, no trailing slash. Task 3 reads it; Task 2's Lua applies the identical rule independently.

- [ ] **Step 1: Write the failing tests.** Append to `ArcaneTests/src/ProjectManifestTest.cpp`:

```cpp
// sourceDir (2026-09-16, decision record docs/research/2026-09-16-multiplayer-
// shape-and-project-layout.md s5, ruling L3): the directory under Source/ that
// build/arcane.lua compiles into gameModule and the editor's Create C++ Class
// defaults to. ONE field, three readers (build, editor, docs) -- so the rule
// lives here once and the test pins it.
TEST_CASE("ProjectManifest parses sourceDir and defaults it to Source", "[project]")
{
    auto full = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": "Source/Game"
    })"));
    REQUIRE(full.has_value());
    CHECK(full->sourceDir == "Source/Game");

    auto bare = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }
    })"));
    REQUIRE(bare.has_value());
    CHECK(bare->sourceDir == "Source");

    // A trailing slash is normalised away; "Source" itself is allowed explicitly.
    auto slash = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": "Source/Game/"
    })"));
    REQUIRE(slash.has_value());
    CHECK(slash->sourceDir == "Source/Game");
    auto plain = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": "Source"
    })"));
    REQUIRE(plain.has_value());
    CHECK(plain->sourceDir == "Source");
}

TEST_CASE("ProjectManifest rejects a sourceDir outside Source/ or escaping it", "[project]")
{
    auto reject = [](const char* value)
    {
        const nlohmann::json doc = {
            { "formatVersion", 1 }, { "name", "X" }, { "engine", { { "abi", 4 } } }, { "sourceDir", value }
        };
        return !Arcane::ProjectManifest::FromJson(doc).has_value();
    };
    CHECK(reject("Src"));                 // not under Source/
    CHECK(reject("Sources/Game"));        // prefix trick: "Source" + "s"
    CHECK(reject("Source/../Other"));     // escapes the mount
    CHECK(reject("Source\\Game"));        // backslashes: the manifest is forward-slashed
    CHECK(reject(""));                    // empty: say Source or omit the key
    CHECK(reject("/Source/Game"));        // absolute
    // Wrong TYPE follows the other optionals' contract: type_error -> nullopt.
    CHECK_FALSE(Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "X", "engine": { "abi": 4 }, "sourceDir": 7
    })")).has_value());
}
```

- [ ] **Step 2: Build ArcaneTests and run the two cases to see them fail.** From the repo root: `ThirdParty/premake5/premake5.exe vs2026 | tail -1`, then the Debug solution build (Global Constraints). Expected: a compile error `'sourceDir': is not a member of 'Arcane::ProjectManifest'` in `ProjectManifestTest.cpp`.

- [ ] **Step 3: The field.** In `ProjectManifest.hpp`, after the `guid` member's comment block and declaration, add:

```cpp
        // The directory, relative to the project root and UNDER Source/, that
        // holds the game module's sources: build/arcane.lua's game-module
        // glob compiles `<sourceDir>/**` (the manifest is the ONE place this
        // is stated -- the SDK reads it beside the project's premake5.lua),
        // and the editor's Create C++ Class defaults its Location to the same
        // folder. "Source" (the default, an absent key) is today's flat
        // layout; "Source/Game" is the Unreal-style Source/<Module>/ layout
        // that lets a project keep other code (its services) under Source/
        // without it being compiled into the module. Always forward-slashed,
        // no trailing slash. Decision record: docs/research/2026-09-16-
        // multiplayer-shape-and-project-layout.md s5 (L3).
        std::string            sourceDir = "Source";
```

- [ ] **Step 4: Parse + validate.** In `ProjectManifest.cpp`, immediately after `m.guid = doc.value("guid", std::string{});` add:

```cpp
        // sourceDir: optional, default "Source". Strict and loud (plan ruling
        // S3): it must name Source/ itself or a directory under it, with no
        // ".." segment, no backslash and no leading slash -- a module
        // directory outside the source:// mount could never register a
        // created class, and a silently-accepted bad value is the failure
        // class this engine keeps meeting. build/arcane.lua applies the
        // identical rule on the build side.
        if (doc.contains("sourceDir"))
        {
            std::string dir = doc.value("sourceDir", std::string{});   // type_error -> nullopt via the try/catch
            while (!dir.empty() && dir.back() == '/')
                dir.pop_back();
            const bool underSource = dir == "Source" || dir.rfind("Source/", 0) == 0;
            const bool escapes = dir.find("..") != std::string::npos
                              || dir.find('\\') != std::string::npos
                              || dir.find("//") != std::string::npos;
            if (!underSource || escapes)
                return std::nullopt;
            m.sourceDir = std::move(dir);
        }
```
  (Confirm the surrounding code is inside the function-try-block the file's comments describe, so a non-string `sourceDir` throws `type_error` and lands on the existing `catch` → nullopt. If the try-block does not cover this point, wrap the `doc.value` in the same `try { … } catch (const nlohmann::json::exception&) { return std::nullopt; }` shape the file uses.)

- [ ] **Step 5: Build, run GREEN.** Rebuild Debug (incremental); `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[project]" | grep -E "^Randomness|^test cases|^assertions|All tests passed|FAILED"`. Expected: all `[project]` cases pass (12 previous + 2 new = 14 in this file, plus whatever other files tag `[project]` — report the count). Then the whole CPU suite: `./ArcaneTests.exe "~[gpu]" | grep -E …` — `All tests passed`; record the seed and the counts.

- [ ] **Step 6: Spec §4 + §5.** In `docs/specs/2026-07-22-arcane-project-format-design.md`:
  (a) §4's schema block: after the `"gameModule": "Aphelyon.dll",       // the project's primary module (game-as-DLL)` line insert
  ```jsonc
  "sourceDir": "Source/Game",         // OPTIONAL (default "Source"): the module's source dir UNDER Source/ (Source/<Module>/, 2026-09-16)
  ```
  (b) §5's `Source/` bullet: replace `- **`Source/`** holds the C++ game module *with* the project (self-contained, Unreal-style), built against the engine SDK (Decision #6).` with `- **`Source/`** holds the project's C++ *with* the project (self-contained, Unreal-style) and is the `source://` mount. The game module's own sources live at the manifest's `sourceDir` (default `Source/` itself; `Source/Game/` is the Unreal `Source/<Module>/` shape that lets a project keep other code — its services — under `Source/` without compiling it into the module; decision record `docs/research/2026-09-16-multiplayer-shape-and-project-layout.md` §5). Built against the engine SDK (Decision #6), whose `arcane_game_module` reads `sourceDir` from the manifest beside the project's `premake5.lua`.`

- [ ] **Step 7: Commit.**

```bash
git add ArcaneCore/src/Arcane/Project/ProjectManifest.hpp ArcaneCore/src/Arcane/Project/ProjectManifest.cpp ArcaneTests/src/ProjectManifestTest.cpp docs/specs/2026-07-22-arcane-project-format-design.md
git commit -m "feat(project): .arcproj sourceDir -- the game module's source directory under Source/, default Source, validated strictly (Source/<Module>/ layout, decision L3)"
```
(+ the two trailers.)

---

### Task 2: `arcane.lua` reads `sourceDir`; ReferenceProject moves to `Source/Game/` (SDK)

**Files:**
- Modify: `build/arcane.lua` — the header comment block (lines ~1-24), `arcane_game_module` (lines ~49-124: the `files` and `includedirs` entries that name `Source`)
- Move: `ReferenceProject/Source/ReferenceGame.cpp` → `ReferenceProject/Source/Game/ReferenceGame.cpp`; `ReferenceProject/Source/.gitkeep` → `ReferenceProject/Source/Game/.gitkeep`
- Modify: `ReferenceProject/ReferenceProject.arcproj` (add `"sourceDir": "Source/Game"`, description)
- Modify: `docs/specs/2026-09-13-arcbuild-driver-design.md` (one sentence beside its mention of premake's `Source/` glob, ~line 154)

**Interfaces:**
- Consumes: the manifest field's rule (Task 1, S3), re-implemented in Lua.
- Produces: `arcane_game_module(name)` unchanged signature; the module's files glob is `<root>/<sourceDir>/**`.

- [ ] **Step 1: RED — the flat layout is what the SDK globs.**

```bash
grep -n 'Source/\*\*' build/arcane.lua
grep -c "sourceDir" build/arcane.lua ReferenceProject/ReferenceProject.arcproj
ls ReferenceProject/Source
```
Expected: two glob hits (`.cpp`, `.hpp`); `0` and `0`; `ReferenceGame.cpp` + `.gitkeep`.

- [ ] **Step 2: The manifest lookup in `build/arcane.lua`.** Immediately BEFORE `function arcane_game_module(name)` insert:

```lua
-- The game module's source directory comes from the project's MANIFEST, never
-- from a premake option (decision record docs/research/2026-09-16-multiplayer-
-- shape-and-project-layout.md s5, ruling L3; plan ruling S1): `sourceDir`,
-- default "Source" (today's flat layout), or e.g. "Source/Game" for the
-- Unreal-style Source/<Module>/ layout. One field, three readers -- this
-- glob, the editor's Create C++ Class default, the docs -- so they cannot
-- drift. _MAIN_SCRIPT_DIR is the directory of the premake5.lua being run,
-- which is the project root for every consumer (the manifest sits beside it).
-- The validation mirrors ProjectManifest::FromJson exactly (Source itself, or
-- under Source/, no "..", no backslash, no leading slash); a bad value or an
-- ambiguous root is an error(), never a guess.
local function arcane_module_source_dir()
    local root = _MAIN_SCRIPT_DIR
    local manifests = os.matchfiles(root .. "/*.arcproj")
    if #manifests == 0 then
        error("arcane_game_module: no .arcproj manifest beside " .. root .. "/premake5.lua (a game module needs its project manifest)")
    elseif #manifests > 1 then
        error("arcane_game_module: more than one .arcproj beside " .. root .. "/premake5.lua: " .. table.concat(manifests, ", "))
    end
    local text = io.readfile(manifests[1])
    local doc, err = json.decode(text)
    if not doc then
        error("arcane_game_module: cannot parse " .. manifests[1] .. ": " .. tostring(err))
    end
    local dir = doc.sourceDir
    if dir == nil then
        return "Source"
    end
    if type(dir) ~= "string" then
        error("arcane_game_module: " .. manifests[1] .. ": sourceDir must be a string")
    end
    dir = dir:gsub("/+$", "")
    local underSource = (dir == "Source") or (dir:sub(1, 7) == "Source/")
    local escapes = dir:find("..", 1, true) or dir:find("\\", 1, true) or dir:find("//", 1, true)
    if not underSource or escapes or dir == "" then
        error("arcane_game_module: " .. manifests[1] .. ": sourceDir '" .. tostring(doc.sourceDir) ..
              "' must be Source or a directory under Source/ (no '..', no backslash)")
    end
    return dir
end
```
  Then inside `arcane_game_module(name)`: add as its FIRST statement `local sourceDir = arcane_module_source_dir()`, and replace
  `files { "%{wks.location}/Source/**.cpp", "%{wks.location}/Source/**.hpp" }` with
  `files { "%{wks.location}/" .. sourceDir .. "/**.cpp", "%{wks.location}/" .. sourceDir .. "/**.hpp" }`
  and the `"%{wks.location}/Source",` includedir entry with `"%{wks.location}/" .. sourceDir,`.
  Header comment (the file's top block): after the `-- Usage from a project's premake5.lua:` example add the line `--   (arcane_game_module reads the game module's source directory -- Source/ by default, or the manifest's "sourceDir" e.g. Source/Game -- from the one .arcproj beside this premake5.lua)`.

- [ ] **Step 3: ReferenceProject adopts `Source/Game/`.**

```bash
mkdir -p ReferenceProject/Source/Game
git mv ReferenceProject/Source/ReferenceGame.cpp ReferenceProject/Source/Game/ReferenceGame.cpp
git mv ReferenceProject/Source/.gitkeep ReferenceProject/Source/Game/.gitkeep
```
  In `ReferenceProject/ReferenceProject.arcproj`: after the `"gameModule": "ReferenceGame.dll",` line add `  "sourceDir": "Source/Game",` and change the description to `"The in-repo sample: an SDK-built game module (Source/Game/ -> Binaries/ReferenceGame.dll, the Unreal-style Source/<Module>/ layout) plus a simple data scene."`.

- [ ] **Step 4: GREEN — ReferenceProject builds from the new path through arcbuild, and a module without the field still builds.** From the repo root (arcbuild is already built in `bin/Debug-windows-x86_64-md/arcbuild/` — do not rebuild the engine for this step):

```bash
bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe build --project ReferenceProject --config Debug 2>&1 | tail -5; echo "exit=$?"
ls -la ReferenceProject/Binaries/ReferenceGame.dll
grep -c "Source\\\\Game\\\\ReferenceGame.cpp\|Source/Game/ReferenceGame.cpp" ReferenceProject/ReferenceGame.vcxproj
bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe probe --project /d/dev/starworks/Gacha/Game --config Debug 2>&1 | tail -2; echo "probe exit=$?"
bin/Debug-windows-x86_64-md/arcbuild/arcbuild.exe build --project /d/dev/starworks/Gacha/Game --config Debug 2>&1 | tail -3; echo "gacha exit=$?"
grep -c "Source\\\\\|Source/" /d/dev/starworks/Gacha/Game/Aphelyon.vcxproj
```
Expected: `exit=0`, the DLL freshly timestamped, the vcxproj lists the file under `Source\Game\`; the Gacha probe exits 0 or 3 and the build `exit=0` with its vcxproj still globbing `Source\` (no field → default). Then `git -C /d/dev/starworks/Gacha status --short | grep -v '^??'` must show only the pre-existing `M Game/Content/scenes/test.arcscene` (arcbuild rewrote gitignored files only).
  Negative check, in the scratchpad (not the repo): copy `ReferenceProject/premake5.lua` + a manifest with `"sourceDir": "Src"` into a temp dir with a `Source/` folder and run `ThirdParty/premake5/premake5.exe --file=<temp>/premake5.lua vs2026`; expected: the `error()` message naming the rule, exit ≠ 0. Record the message.

- [ ] **Step 5: The `[build]` and `[project]` lanes still pass, and the postbuild staging mirrors the new tree.** Rebuild the engine Debug solution once (ArcaneTests source-compiles arcbuild's core; ReferenceProject's `Source/` is mirrored into the hosts' staged copies at post-build): then `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "[build],[project],[plugin]" | grep -E …` → all passed; and `ls bin/Debug-windows-x86_64-md/ArcaneRuntime/ReferenceProject/Source/Game/ReferenceGame.cpp` exists while `bin/Debug-windows-x86_64-md/ArcaneRuntime/ReferenceProject/Source/ReferenceGame.cpp` does NOT (the mirror deleted the old location).

- [ ] **Step 6: arcbuild spec.** In `docs/specs/2026-09-13-arcbuild-driver-design.md`, at the sentence that says the driver never touches `Source/` (near line 154), append one sentence: `The game module's source root is the manifest's `sourceDir` (default `Source/`; `Source/Game/` for the `Source/<Module>/` layout, 2026-09-16) — premake reads it, the driver never needs to.`

- [ ] **Step 7: Commit.**

```bash
git add build/arcane.lua ReferenceProject/Source/Game/ReferenceGame.cpp ReferenceProject/Source/Game/.gitkeep ReferenceProject/ReferenceProject.arcproj docs/specs/2026-09-13-arcbuild-driver-design.md
git commit -m "feat(sdk): arcane_game_module reads the manifest's sourceDir; ReferenceProject moves to Source/Game/ (the Source/<Module>/ layout)"
```
(`git mv` already staged the removals; confirm with `git status --short` that the old paths show as renames.) (+ the two trailers.)

---

### Task 3: The editor's Create C++ Class defaults to the module's folder (Editor)

**Files:**
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.hpp` (~line 118-135 near `CreateKindDefaultFolder`; the request/state struct that carries `kind`)
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.cpp:98-140` (`BuildFolderChoices`, the seed at ~136)
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp` (~line 2908-2913 comment) and the site(s) where the app raises/opens a CppClass create request (find with `grep -rn "CreateAssetKind::CppClass" ArcaneEditor/src/App`)
- Modify: `ArcaneTests/src/CreateAssetDialogTest.cpp` (extend the C++ Class vocabulary case)

**Interfaces:**
- Consumes: `ProjectManifest::sourceDir` (Task 1) via `m_runtime->Core().Project().Manifest()` (or the app's existing manifest accessor — `grep -n "Manifest()" ArcaneEditor/src/App/*.cpp` shows the idiom).
- Produces: `[[nodiscard]] inline std::string CppClassDefaultFolder(std::string_view sourceDir)` — `""` for `"Source"`, `"Game/"` for `"Source/Game"` (the remainder after `Source/`, with a trailing slash, the model's folder-string shape); `CreateAssetRequest::cppDefaultFolder` (a `std::string`, default `""`) the app seeds when it raises a CppClass request.

- [ ] **Step 1: Write the failing test.** In `ArcaneTests/src/CreateAssetDialogTest.cpp`, inside the existing case `"Create-kind vocabulary: C++ Class lands under Source/ and bridges from AssetKind::Source"` after the `CreateKindDefaultFolder(CppClass) == ""` check, add:

```cpp
    // The manifest's sourceDir moves the DEFAULT folder, never the root: the
    // source:// mount stays Source/ (the browser shows every module), and the
    // Location combo pre-selects the game module's own directory. Same
    // string shape the model uses for folders (trailing slash, "" = root).
    CHECK(CppClassDefaultFolder("Source")        == "");
    CHECK(CppClassDefaultFolder("Source/Game")   == "Game/");
    CHECK(CppClassDefaultFolder("Source/Game/")  == "Game/");     // tolerant of a trailing slash
    CHECK(CppClassDefaultFolder("Source/a/b")    == "a/b/");
```

- [ ] **Step 2: Build ArcaneTests → fails to compile** (`'CppClassDefaultFolder': identifier not found`).

- [ ] **Step 3: The function + the request field.** In `CreateAssetDialog.hpp`, after `CreateKindDefaultFolder`:

```cpp
    // The Location combo's default for a C++ CLASS specifically, derived from
    // the manifest's sourceDir (ProjectManifest.hpp): the game module's own
    // directory, relative to the source:// mount root (Source/), in the
    // model's folder-string shape -- "" for the root itself, "Game/" for
    // "Source/Game". CreateKindDefaultFolder(CppClass) stays "" as the
    // manifest-less fallback; CreateKindRoot(CppClass) stays "Source" (the
    // MOUNT does not move -- plan ruling S2).
    [[nodiscard]] inline std::string CppClassDefaultFolder(std::string_view sourceDir)
    {
        std::string dir(sourceDir);
        while (!dir.empty() && dir.back() == '/')
            dir.pop_back();
        if (dir == "Source" || dir.rfind("Source/", 0) != 0)
            return "";
        return dir.substr(7) + "/";
    }
```
  In the request struct the app fills when it opens the dialog (the one carrying `CreateAssetKind kind`), add `std::string cppDefaultFolder;   // CppClass only: CppClassDefaultFolder(manifest.sourceDir), seeded by the app` with a comment pointing here.

- [ ] **Step 4: The dialog uses it.** In `CreateAssetDialog.cpp` `BuildFolderChoices(model, kind)` gains a third parameter `const std::string& cppDefaultFolder` and inserts `kind == CreateAssetKind::CppClass ? cppDefaultFolder : std::string(CreateKindDefaultFolder(kind))` instead of `CreateKindDefaultFolder(kind)`; the seed at ~line 136 (`MakeFolderChoice(CreateKindDefaultFolder(kind), CreateKindRoot(kind)).relative`) makes the same substitution; the call at ~line 488 passes `st.request.cppDefaultFolder`. In the app, at every site that raises a CppClass request (the Assets menu entry in `EditorPanels.cpp` produces a request the app consumes — find where `CreateAssetKind::CppClass` requests become the dialog's request in `ArcaneEditor/src/App/`), set `request.cppDefaultFolder = Arcane::Editor::CppClassDefaultFolder(<project>.Manifest().sourceDir);` when a project is open. Update the comment at `EditorAppProject.cpp` ~2911: `// The .vcxproj is premake's glob over the manifest's sourceDir (Source/**` → `// by default, Source/Game/** for the Source/<Module>/ layout): regenerate so the new`.

- [ ] **Step 5: Build (Debug), run GREEN.** `./ArcaneTests.exe "[editor][create]" | grep -E …` → all passed; then `"~[gpu]"` → all passed, record seed + counts. Build the Release solution too (the editor is Release-built in CI): `0 Error(s)`.

- [ ] **Step 6: Commit.**

```bash
git add ArcaneEditor/src/Panels/CreateAssetDialog.hpp ArcaneEditor/src/Panels/CreateAssetDialog.cpp ArcaneEditor/src/App/EditorAppProject.cpp ArcaneTests/src/CreateAssetDialogTest.cpp
```
  (plus any other `ArcaneEditor/src/App/*.cpp` file Step 4 touched — name them explicitly)
```bash
git commit -m "feat(editor): Create C++ Class defaults its Location to the manifest's sourceDir (Source/Game/); the source:// mount stays Source/"
```
(+ the two trailers.)

---

### Task 4: Closeout — both-config suites, the golden gate, baselines, docs (Arcane)

**Files:**
- Modify: `scripts/automation-baselines.json` (the Debug + Release `arcanetests.assertions` / `arcanetests.cases` rows; Dist untouched)
- Modify: `CLAUDE.md` (Arcane) — only if `grep -n "Source/" CLAUDE.md` shows a sentence describing `Source/` as "the game module's directory"; amend it to name `sourceDir`
- Modify: `docs/plans/2026-09-16-game-module-source-dir-plan.md` (this file: Closeout)

- [ ] **Step 1: Both configs, whole CPU suite.** Build Release if not already current; run `~[gpu]` from the Debug AND Release ArcaneTests dirs; both `All tests passed`; record seeds and the assertion/case counts. Expected rise over the booked 57562/1788: the Task 1 + Task 3 cases (derive the exact delta from the runs).

- [ ] **Step 2: The golden gate, Debug.** `pwsh -NoProfile -File scripts/golden-gate.ps1 -Configuration Debug` (read the script's header first for its parameters) → `4/4 diffCount=0`. This proves ReferenceGame.dll, now built from `Source/Game/`, still drives the reference scenes. Leave `ReferenceProject/Binaries/` holding the Debug DLL.

- [ ] **Step 3: Baselines.** In `scripts/automation-baselines.json` update the Debug and Release `arcanetests.assertions` and `arcanetests.cases` rows to the Step 1 numbers; run `pwsh -NoProfile -File scripts/check-baselines.ps1` (read its header for the parameters) — expected `+0/+0`, exit 0, for each config.

- [ ] **Step 4: ABI unchanged + docs.** `grep -n "kGamePluginABIVersion = " ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` → still 31. `grep -n "Source/" CLAUDE.md`: amend only a sentence that calls `Source/` the module directory. Append the Closeout below (commit shas T1-T3, the arcbuild witnesses from Task 2 Step 4 incl. the Gacha no-field build and the negative check's error text, the suite numbers + seeds per config, the gate verdict, the baseline delta, "ABI 31 unchanged, no Gacha commit").

- [ ] **Step 5: Commit.**

```bash
git add scripts/automation-baselines.json docs/plans/2026-09-16-game-module-source-dir-plan.md   # + CLAUDE.md if amended
git commit -m "docs(sdk): close the game-module sourceDir plan -- baselines booked, golden gate 4/4, closeout"
```
(+ the two trailers.) Do not push.

---

## Self-review (run at plan-writing time)

- **Spec coverage:** decision record §5 L3 ("SDK gains an explicit game-module source dir before the move") → T1 (the field) + T2 (the SDK reads it) ✓; "ReferenceProject adopts Source/Game/" → T2 S4 ✓; "the editor's Source rows / Create C++ Class learn the module subdirectory" → T3 (rows unchanged by S2, create default learns it) ✓; "the arcbuild spec and the project-format spec §5 record it" → T2 Step 6, T1 Step 6 ✓; "a manifest without the field behaves as today" → T2 Step 4's Gacha witness ✓.
- **Placeholder scan:** every code step carries its text; the only open values are Task 4's measured numbers and Task 3's "find the request site" instruction, which names the grep that finds it.
- **Type consistency:** `sourceDir` (manifest, `std::string`), `arcane_module_source_dir()` (Lua, returns the same string), `CppClassDefaultFolder(std::string_view)` → `std::string`, `CreateAssetRequest::cppDefaultFolder` (`std::string`) — consistent; `CreateKindRoot(CppClass)` remains `"Source"` in T3's test and the dialog.

<!-- CLOSEOUT -->

## Closeout

**Commits.**
- Task 1 (`ProjectManifest::sourceDir` -- parse, validate, default; tests; format-spec §4/§5): `0cf74883`
- Task 2 (`arcane.lua` reads `sourceDir`; ReferenceProject moves to `Source/Game/`; arcbuild-spec sentence): `5b652285`
- Task 3 (editor's Create C++ Class default folder learns the manifest's `sourceDir`): `aaf6bf9e`
- Task 4 (this closeout): this commit

**Task 2's arcbuild witnesses (verbatim from the Task 2 report).**

ReferenceProject built from `Source/Game/`:
```
[msbuild]          ReferenceGame.vcxproj -> D:\dev\starworks\Arcane\ReferenceProject\Binaries\ReferenceGame.dll
[msbuild] Build succeeded.
[msbuild]     0 Warning(s)
[msbuild]     0 Error(s)
[arcbuild] msbuild succeeded
exit=0
```
`<ClCompile Include="Source\Game\ReferenceGame.cpp" />` confirmed in the generated `.vcxproj`.

The Gacha "no field" witness (Aphelyon's `Game/` project, no `sourceDir` in its `.arcproj`):
```
$ arcbuild.exe probe --project /d/dev/starworks/Gacha/Game --config Debug
[arcbuild] probe: slot=D:/dev/starworks/Gacha/Game/Binaries/Aphelyon.dll state=match flavor=debug (MSVCP140D.dll) config=Debug -> plain build
probe exit=0

$ arcbuild.exe build --project /d/dev/starworks/Gacha/Game --config Debug
[msbuild]          Aphelyon.vcxproj -> D:\dev\starworks\Gacha\Game\Binaries\Aphelyon.dll
[msbuild] Build succeeded.  0 Warning(s)  0 Error(s)
[arcbuild] msbuild succeeded
gacha exit=0
```
`Aphelyon.vcxproj` still globs the default `Source\` (3 hits for `Source\`, unaffected by the field). Gacha's own `git status --short` showed only its pre-existing `test.arcscene` modification -- no Gacha file was edited or committed by this plan.

The negative check (run in the scratchpad, outside either repo, a synthetic `Neg.arcproj` with `"sourceDir": "Src"`):
```
$ ThirdParty/premake5/premake5.exe --file=<scratch>/neg/premake5.lua vs2026
Error: D:/dev/starworks/Arcane/build/arcane.lua:83: arcane_game_module: .../scratchpad/neg/Neg.arcproj: sourceDir 'Src' must be Source or a directory under Source/ (no '..', no backslash)
exit=1
```

**Step 1 -- both configs, whole CPU suite (this task's own runs).**
- Debug: `All tests passed` -- `assertions: 57607 | 57607 passed`, `test cases: 1797 | 1793 passed | 4 skipped`. Seed `2428212758`.
- Release: `All tests passed` -- `assertions: 57607 | 57607 passed`, `test cases: 1797 | 1793 passed | 4 skipped`. Seed `435624649` (see "Gate" paragraph below for why the first Release attempt showed 2 failures/9 failed assertions -- a single-slot `ReferenceProject/Binaries/` CRT-flavor precondition issue, unrelated to this plan, corrected before this number was taken).
- Per `check-baselines.ps1`'s counting rule (`assertions`/`cases` = `passed + failed`, skipped excluded from `cases`): both configs book as **57607 assertions / 1793 cases**.

**Step 2 -- the golden gate, Debug: 4/4 passed.**
```
ArcaneRuntime/dx12/runtime-scene   PassedOnFallback  exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared (expected backend)
ArcaneRuntime/vulkan/runtime-scene Passed            exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=backend
ArcaneEditor/dx12/editor-ui        Passed            exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared
ArcaneEditor/vulkan/editor-ui      Passed            exitReason=frames-complete diffCount=0 maxLocalDifference=0.0 resolvedLevel=shared
golden-gate: 4 lane(s) passed, 0 red
```
`ReferenceProject/Binaries/` left holding the Debug DLL (`MSVCP140D.dll`), as required.

**Final review + fix wave.** Ready to merge; one Important (the "sourceDir": null asymmetry between premake's json.decode and FromJson — recorded in arcane.lua's header, the host being the stricter side) and five doc/comment minors fixed in this commit; deferred: a negative CppClassDefaultFolder test, a `Source//Game` rejection test, the gate-script imgui.ini deletion (engine follow-up).

**Gate: a stale imgui.ini.** The first Debug golden-gate run (before the fix below) reported the two `ArcaneRuntime/*/runtime-scene` lanes `compare-failed` with ~7840 differing pixels each, while both `ArcaneEditor/*/editor-ui` lanes passed clean (`diffCount=0`). The diff image showed the *only* differing pixels were the module's small ImGui debug window (the top-left "ArcaneRuntime" box), drawn ~15 px apart -- the scene itself was identical. Cause: `bin/Debug-windows-x86_64-md/ArcaneRuntime/imgui.ini` (a gitignored, generated file beside the exe) held:
```
[Window][ArcaneRuntime]
Pos=58,45
Size=142,82
Collapsed=0

[Window][Aphelyon]
Pos=67,184
Size=325,65
Collapsed=0
```
The `[Window][Aphelyon]` entry could only have been written by an interactive run of the Aphelyon project against this same standalone `ArcaneRuntime.exe` -- an earlier desk pass dragged the overlay, the standalone runtime persisted the drag to its `imgui.ini`, and the headless `--compare` run silently read that persisted position back instead of the default `Pos=60,60` the golden was blessed against. `bin/Release-windows-x86_64-md/ArcaneRuntime/imgui.ini` was a second, similarly stale copy (from this task's own Release suite runs, also all `Pos=60,60` except the `ArcaneRuntime` window). A controlled bisect confirmed the file move is not the cause: re-running the gate against the flat, pre-move layout reproduced the identical `diffCount=7844`/`7838` -- disproving `Source/Game/` as the culprit and confirming a stale, unrelated `imgui.ini` beside the exe. Deleting both stale `.ini` files (`rm bin/{Debug,Release}-windows-x86_64-md/ArcaneRuntime/imgui.ini`) and re-running the gate produced the clean 4/4 result above. `bin/Debug-windows-x86_64-md/ArcaneEditor/imgui.ini` does not exist (checked, not deleted) -- the editor lanes were never at risk from this and its layout, when present, is a real input, not a byproduct to discard.

**Follow-up (deferred engine defect, not fixed here):** headless `--compare` runs must not read a persisted `imgui.ini`. Neither the runtime host nor the gate script guards against this today: `ArcaneRuntime` should set `io.IniFilename = nullptr` when `--headless` is passed, or `golden-gate.ps1` should delete the host-dir `imgui.ini` before each launch. Until one of those lands, any interactive drag of the runtime overlay silently poisons the next gate run with a false "scene changed" failure that has nothing to do with the scene.

**Step 3 -- baselines.** `scripts/automation-baselines.json`'s Debug and Release `arcanetests.assertions`/`arcanetests.cases` rows: `57588` -> `57607` (assertions, both configs), `1791` -> `1793` (cases, both configs). Dist untouched (`55226`/`1516`). `check-baselines.ps1` against both reports:
```
telemetry: arcanetests.assertions [Debug/~[gpu]]   = 57607 assertions, baseline 57607 (+0)
telemetry: arcanetests.cases      [Debug/~[gpu]]   = 1793 cases,      baseline 1793 (+0)
telemetry: arcanetests.assertions [Release/~[gpu]] = 57607 assertions, baseline 57607 (+0)
telemetry: arcanetests.cases      [Release/~[gpu]] = 1793 cases,      baseline 1793 (+0)
```
Both exit 0. The +19 assertions / +2 cases rise is fully attributable to this plan: Task 1's two new `ProjectManifestTest` `[project]` cases (the 2 new cases) plus Task 3's four new `CHECK`s appended to an existing `CreateAssetDialogTest` case (assertions only, no new case) -- `cac776b2`'s own +20/+2 rise (`0cf74883`'s parent, `a75b87fd`) is an ancestor of this plan's starting commit and was already folded into the `57588`/`1791` value this task started from, so no further Astra-resync remainder is owed beyond what the note already recorded for it.

**Step 4 -- ABI unchanged, no Gacha commit.** `grep -n "kGamePluginABIVersion = " ArcaneCore/src/Arcane/Plugin/PluginABI.hpp` -> `869:    inline constexpr uint32_t kGamePluginABIVersion = 31;`. **ABI 31 unchanged, no Gacha commit.** `CLAUDE.md` (Arcane) carries no sentence describing `Source/` as "the game module's directory" (confirmed by grep before this task began) -- left untouched, per the brief.
