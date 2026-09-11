# Astra Adoption — Plan 1: Residency + Change Detection

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the two Astra primitives, vendor Astra `dev` into Arcane at ABI 26, put the engine roster on a Runtime-owned `Resident` module, const-ify every read, track `Transform`, replace the propagation shadow-copy detector with a `Changed<Transform>` pre-pass + early-out, make Inspector/undo writes visible, gate the paused physics reconcile, run Edit-mode propagation once per frame through an editor-owned scheduler, and restamp + rebuild Gacha. Plan 2 owns `PreviousTransform`'s deletion and ABI 27.

**Architecture:** Bottom-up across three repos: Astra primitives (GoogleTest) → merge + vendor + ABI (the one header-layout bump, ReferenceProject built FIRST) → residency (pure Runtime change, pinned by a binder-count test) → mechanical const-ification (no behaviour change, so it lands before anything can observe stamps) → `Transform` tracked + the propagation rewrite (the only behavioural core) → the two editor marks → the physics gate → the editor scheduler → Gacha → closeout. Every task ends with the whole Arcane tree green.

**Tech Stack:** C++23, Astra (vendored header-only ECS), Catch2 (Arcane), GoogleTest (Astra), msbuild `Arcane.slnx` / `ReferenceProject.slnx` / `Aphelyon.slnx` / `Astra.sln`.

**Spec:** `docs/specs/2026-09-11-astra-adoption-design.md` (rulings R1–R8 in §3). Research: `D:\dev\starworks\Astra\.superpowers\sdd\2026-09-10-astra-change-detection\arcane-adoption-map.md` (anchored at Arcane `9b0ad0f5`, Astra `b664aa8`; its B4 + B10 #3-4 tables are transcribed into Task 4 file by file).

## Global Constraints

- **Repos:** Arcane `D:\dev\starworks\Arcane` `main` @ `864dd777` (ABI **25**); Astra `D:\dev\starworks\Astra` branch `feat/change-detection` @ `b664aa8` (17 commits ahead of `dev` @ `f3e311d`, unmerged); Gacha `D:\dev\starworks\Gacha` @ `5923da65` (`Game/Aphelyon.arcproj` at ABI **21**). **`out.txt` at the Arcane root is the user's — never stage it.** Untracked `ArcaneAssetPipeline/ArcaneAs.25D4CEF5/` and `ArcaneEditor/ArcaneEditor/` are pre-existing strays — never stage them either.
- **Commit per task, in whichever repo the task edits; do NOT push.** The user's desk pass is the gate. Trailer on EVERY commit in EVERY repo, exactly:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
- **The ABI bumps ONCE in this plan, in Task 2 (25 → 26).** No other task touches `kGamePluginABIVersion`; Plan 2 bumps to 27 at the `PreviousTransform` deletion. Byte discipline: the bump exists because plugins compile Astra's headers themselves and the change-detection vendor moves inlined layouts (`Column::ticks`, `ArchetypeColumnMeta::trackedColumns`, the chunk arena's tick regions, `EntityLocation`'s aggregate shape) — same class as v10/v24. `AstraChangeTracked` on `Transform` (Task 5) is a static member: no `sizeof`, reflect-block or serialized-byte change, so it rides the SAME bump.
- **Build order after ANY Astra header change (Task 2), and after any change to a header a game module compiles (`Components.hpp`, `TransformSystems.hpp`, `RenderSystems.hpp`, `SceneResources.hpp`):** `GenerateProjects.bat` → **`ReferenceProject.slnx` FIRST, for every configuration the run targets** (`cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=<cfg> /m`; `ReferenceProject/Binaries/` is a single slot for all configs) → `msbuild Arcane.slnx /p:Configuration=<cfg> /m` (its post-build stages the whole `ReferenceProject/` tree beside the exes, so a stale `ReferenceGame.dll` there fails `[witness][gpu]` with `plugin: initial load failed`). Launch hosts with an ABSOLUTE `--project` path (a relative one resolves to the staged copy).
- **Premake reality:** `ArcaneTests` globs its own `src/**.cpp` — a NEW test TU still needs **`GenerateProjects.bat`** (the vcxproj file list is static until regenerated) — but compiles EDITOR TUs from an **EXPLICIT list** (root `premake5.lua`, the `ArcaneEditor/src/...` entries under the `ArcaneTests` `files` block, ~`:679-909`). An editor `.cpp` a test drives MUST be added there or it will not link (Task 8 adds one).
- **Run tests FROM the exe dir, in the FOREGROUND:** `cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe "<filter>"`. Order is randomized — capture the seed banner into any report that cites a run. Prior implementers stalled on background waits: never background a suite run or a build. Astra: `cd D:\dev\starworks\Astra && premake5 vs2022 && msbuild Astra.sln /p:Configuration=Debug /m && bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1` (call `premake5` directly — `scripts\generate_vs2022.bat` ends in `pause` and blocks a headless run; `D:\dev\_shared\tools\premake5.exe` is on PATH).
- **Baseline at arc start: 56118 assertions / 1620 cases** (`~[gpu]`, Debug and Release identical; measured at the F2c Plan 1 closeout). Every task states its own delta attributed to named cases. **Derive counts from the run, never recall them.** `scripts/automation-baselines.json` still carries the older 55294/1522 (F2c Plan 1 left it stale) — Task 10 Step 4 catches it up to this plan's derived close, in its own commit.
- **Every task ends green** (Debug). Task 2 and Task 10 additionally end green in Release; Task 2 additionally runs the FULL UNFILTERED suite (the only run that exercises `[witness][gpu]`). A task that cannot end green must be split, not merged.
- **Anchors drift.** Every `file:line` below is orientation against `864dd777` / the adoption map. **Re-locate by SYMBOL name before editing.**
- **No render change in this plan ⇒ the golden lanes are untouched; no re-bless.**
- **The dev DB is not involved anywhere in this plan.** Nothing here touches Postgres.

---

### Task 1: Astra primitives — `Registry::Modified(Entity, ComponentID)`, `IsChanged<T>`, `IsAdded<T>` (spec §4, R1)

Both are forwards to machinery on `b664aa8`. `MarkWritten` is already type-erased (`ArchetypeManager.hpp:600-612`: stamps the column, marks a tracked entity, `false` for a stale handle / absent component / tag). `IsChanged`/`IsAdded` read the record → `idToColumn[TypeID<T>]` → per-entity tick when the column is tracked, else the chunk's column version — and **never stamp** (`const` all the way down).

**Files:**
- Modify: `D:\dev\starworks\Astra\include\Astra\Registry\Registry.hpp` (three members beside `Modified<T>`, `:554-563`)
- Modify: `D:\dev\starworks\Astra\README.md` (§"Change detection", the `Registry::Modified<T>(e)` paragraph at `:420`)
- Modify: `D:\dev\starworks\Astra\docs\superpowers\specs\2026-09-10-astra-change-detection-design.md` (§3.7 API changes, `:196`)
- Test: `D:\dev\starworks\Astra\tests\Registry\ChangeDetectionTest.cpp` (three new `TEST`s after `ModifiedStampsAndSetIfNeqStampsOnlyOnInequality`, `:316-335`)

**Interfaces:**

```cpp
        // Type-erased twin of Modified<T>: for hash/descriptor-driven writers (an
        // editor Inspector fanning out over a ComponentDescriptor, an undo Restore
        // through descriptor->deserialize) that hold a ComponentID but no T. Same
        // table as MarkWritten: false for an invalid handle, an absent component,
        // or a tag (no column); true after stamping the column and, for a tracked
        // column, marking the entity.
        bool Modified(Entity entity, ComponentID id)
        {
            AssertContextAffinity();
            if (!m_entityManager.IsValid(entity)) return false;
            return m_archetypeManager->MarkWritten(entity, id);
        }

        // Per-entity change query OUTSIDE a view (spec 2026-09-11 Arcane adoption
        // s4). Tracked T: exact -- IsNewer(ticks[row].changed, since). Untracked
        // T: chunk-coarse -- IsNewer(column version, since), so every entity of a
        // stamped chunk answers true (documented, the same trade Changed<T> makes).
        // NEVER stamps: a read that must not count as a write. False for an
        // invalid handle, an absent component, or a tag.
        template<Component T>
        ASTRA_NODISCARD bool IsChanged(Entity entity, Tick since) const
        {
            AssertContextAffinity();
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) return false;
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) return false;
            const int col = rec->archetype->GetColumnMeta().idToColumn[id];
            if (col < 0) return false;
            if (const EntityTicks* t = rec->chunk->GetTicks(col))
                return IsNewer(t[rec->location.GetEntityIndex()].changed, since);
            return IsNewer(rec->chunk->GetColumnVersion(col), since);
        }

        // IsChanged's twin over the `added` tick. For an untracked T this is
        // IsChanged in effect (only the column version exists) -- the README's
        // Added<T>-on-untracked caveat, restated for the entity form.
        template<Component T>
        ASTRA_NODISCARD bool IsAdded(Entity entity, Tick since) const
        {
            AssertContextAffinity();
            const EntityRecord* rec = m_archetypeManager->GetEntityRecord(entity);
            if (!rec || !rec->chunk) return false;
            const ComponentID id = TypeID<T>::Value();
            if (id >= MAX_COMPONENTS) return false;
            const int col = rec->archetype->GetColumnMeta().idToColumn[id];
            if (col < 0) return false;
            if (const EntityTicks* t = rec->chunk->GetTicks(col))
                return IsNewer(t[rec->location.GetEntityIndex()].added, since);
            return IsNewer(rec->chunk->GetColumnVersion(col), since);
        }
```

- [ ] **Step 1: Write the failing GoogleTest cases.** Insert after `ModifiedStampsAndSetIfNeqStampsOnlyOnInequality` (`:335`). `TrackedPos` is declared at `:506` (`using Astra::Test::TrackedPos;`) — these cases sit BEFORE that line, so spell it fully:

```cpp
// Arcane adoption (2026-09-11): the type-erased Modified for descriptor-driven
// writers. Mirrors the <T> case above, including its three false paths.
TEST(ChangeDetectionStamp, ModifiedByIdStampsAndRefusesTheSameThreeWays)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    // Player is an EMPTY TAG (TestComponents.hpp:150; the RegistrySerializationTest
    // :487 pattern): PRESENT on e but with no storage column, so Modified by its
    // id must refuse -- the third false path, distinct from "absent".
    auto e = reg.CreateEntityWith(Position{1, 2, 3}, Astra::Test::TrackedPos{4, 5, 6}, Astra::Test::Player{});

    AdvanceTo(reg, 3);
    EXPECT_TRUE(reg.Modified(e, Astra::TypeID<Position>::Value()));
    EXPECT_EQ(VersionOf<Position>(reg, e), 3u);
    EXPECT_TRUE(reg.Modified(e, Astra::TypeID<Astra::Test::TrackedPos>::Value()));
    EXPECT_EQ(VersionOf<Astra::Test::TrackedPos>(reg, e), 3u);
    EXPECT_TRUE(reg.IsChanged<Astra::Test::TrackedPos>(e, 2));   // the tracked mark landed too

    EXPECT_FALSE(reg.Modified(e, Astra::TypeID<Velocity>::Value()));                 // absent component
    EXPECT_FALSE(reg.Modified(Astra::Entity{}, Astra::TypeID<Position>::Value()));   // invalid handle
    EXPECT_FALSE(reg.Modified(e, Astra::TypeID<Astra::Test::Player>::Value()));      // present tag: no column
}

TEST(ChangeDetectionStamp, IsChangedIsExactForTrackedAndChunkCoarseForUntracked)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    std::vector<Astra::Entity> ents(64);
    ASSERT_EQ((reg.CreateEntities<Position, Astra::Test::TrackedPos>(64, std::span{ents})), 64u);

    AdvanceTo(reg, 3);
    reg.GetComponent<Astra::Test::TrackedPos>(ents[7])->x = 1.0f;   // stamps the chunk, marks ents[7]

    // Tracked: exactly the one entity.
    EXPECT_TRUE(reg.IsChanged<Astra::Test::TrackedPos>(ents[7], 2));
    EXPECT_FALSE(reg.IsChanged<Astra::Test::TrackedPos>(ents[8], 2));
    EXPECT_FALSE(reg.IsChanged<Astra::Test::TrackedPos>(ents[7], 3));   // not newer than its own tick
    // Untracked: the chunk was NOT stamped for Position by a TrackedPos write...
    EXPECT_FALSE(reg.IsChanged<Position>(ents[7], 2));
    // ...but a Position write stamps the whole chunk, so every neighbour answers true.
    reg.GetComponent<Position>(ents[7])->x = 1.0f;
    EXPECT_TRUE(reg.IsChanged<Position>(ents[7], 2));
    EXPECT_TRUE(reg.IsChanged<Position>(ents[8], 2));   // chunk-coarse, documented
    // Absent / invalid / tag: false.
    EXPECT_FALSE(reg.IsChanged<Velocity>(ents[7], 0));
    EXPECT_FALSE(reg.IsChanged<Position>(Astra::Entity{}, 0));
    // Since 0 == "never": anything stamped answers true.
    EXPECT_TRUE(reg.IsChanged<Astra::Test::TrackedPos>(ents[8], 0));
    // IsAdded reads the added tick: created at 2, so newer than 1, not newer than 2.
    EXPECT_TRUE(reg.IsAdded<Astra::Test::TrackedPos>(ents[8], 1));
    EXPECT_FALSE(reg.IsAdded<Astra::Test::TrackedPos>(ents[8], 2));
    EXPECT_FALSE(reg.IsAdded<Astra::Test::TrackedPos>(ents[7], 2));   // the later write did not re-add it
}

TEST(ChangeDetectionStamp, IsChangedDoesNotStamp)
{
    Astra::Registry reg;
    AdvanceTo(reg, 2);
    auto e = reg.CreateEntityWith(Position{1, 2, 3}, Astra::Test::TrackedPos{});
    AdvanceTo(reg, 5);
    (void)reg.IsChanged<Position>(e, 0);
    (void)reg.IsChanged<Astra::Test::TrackedPos>(e, 0);
    (void)reg.IsAdded<Astra::Test::TrackedPos>(e, 0);
    EXPECT_EQ(VersionOf<Position>(reg, e), 2u);                       // column version unchanged
    EXPECT_EQ(VersionOf<Astra::Test::TrackedPos>(reg, e), 2u);
    EXPECT_FALSE(reg.IsChanged<Astra::Test::TrackedPos>(e, 2));       // and the entity tick unchanged
}
```

- [ ] **Step 2: Run — expect FAIL** (compile error: no `Modified(Entity, ComponentID)` / `IsChanged`). `cd D:\dev\starworks\Astra && premake5 vs2022 && msbuild Astra.sln /p:Configuration=Debug /m`.
- [ ] **Step 3: Add the three members** to `Registry.hpp` immediately after `Modified<T>` (`:563`), verbatim from Interfaces. `EntityRecord`/`EntityTicks`/`IsNewer` are already in scope (`ArchetypeManager.hpp` is included at `:12`).
- [ ] **Step 3b: Park the unrelated dirty files FIRST.** The Astra working tree at `b664aa8` is NOT clean: `tests/Serialization/LoadRobustnessTest.cpp` (+103 — two RED-by-design `LoadRobustness.Archetype*EntityCountDisagreeingWithChunksIsRejected` cases for a "2026-09-11 grading finding S1") and `bench-compare/RESULTS.md` (+207) are modified-tracked and belong to someone else's work. `git stash push -m "S1 grading WIP (not this task)" -- tests/Serialization/LoadRobustnessTest.cpp bench-compare/RESULTS.md` before building, so the suite expectation below is the committed suite plus this task's three cases, and the commit cannot sweep them in. `git stash pop` right after Step 6's commit (and confirm both files are back as modified). **Never `git add -A` / `git commit -a` in the Astra repo.**
- [ ] **Step 4: Build + run — expect PASS.** `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1 --gtest_filter=ChangeDetectionStamp.*` then the whole suite. Then **Release**: `msbuild Astra.sln /p:Configuration=Release /m && bin\Release-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1`.
- [ ] **Step 5: README + spec.** README `:420` paragraph — after "…use `Modified<T>` for late writes." append: "`Registry::Modified(e, ComponentID)` is the type-erased twin for descriptor-driven writers (an editor Inspector, an undo Restore) that hold an id but no `T`; same stamp, same mark, same three `false` paths. `Registry::IsChanged<T>(e, since)` / `IsAdded<T>(e, since)` answer the per-entity question OUTSIDE a view and never stamp: exact for a tracked `T`, chunk-coarse for an untracked one." Spec §3.7 line `:196` becomes: `- `+ Registry::CurrentTick/AdvanceTick/Modified<T>/Modified(Entity, ComponentID)/SetIfNeq<T>/IsChanged<T>(Entity, Tick)/IsAdded<T>(Entity, Tick)`; `+ Tick`.`
- [ ] **Step 6: Commit on `feat/change-detection`** — stage EXACTLY these four paths by name: `git add include/Astra/Registry/Registry.hpp tests/Registry/ChangeDetectionTest.cpp README.md docs/superpowers/specs/2026-09-10-astra-change-detection-design.md`; `git status --short` must show nothing else staged (the two stashed files are absent; `bench-compare/` untracked strays stay untracked). Message: `feat(registry): Modified(Entity, ComponentID) + non-stamping IsChanged<T>/IsAdded<T> for the Arcane adoption` (+ trailer). Then `git stash pop` (Step 3b). Delta: **+3 GoogleTest cases**.

---

### Task 2: Merge, vendor, ABI 26, and the B9 build order (spec §9, R1, R4)

**The executor ASKS THE USER before merging.** `dev` @ `f3e311d` is an ancestor of `feat/change-detection`, so the merge is a fast-forward — say so when asking, and do not proceed until the user says go.

**Files:**
- Modify (Arcane): `ThirdParty/Astra/include/**` + `ThirdParty/Astra/VENDORED.txt` (by `scripts/sync-astra.ps1`), `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (v26 ledger + constant), `ReferenceProject/ReferenceProject.arcproj` (`"abi": 25` → `26`)
- Test: none new — the whole suite is the test.

**Interfaces:** `inline constexpr uint32_t kGamePluginABIVersion = 26;`

- [ ] **Step 1: Ask, then merge (Astra).** `cd D:\dev\starworks\Astra && git checkout dev && git merge --ff-only feat/change-detection && git log --oneline -1` — HEAD must now be Task 1's commit. Stay on `dev` for the sync.
- [ ] **Step 2: Sync.** `cd D:\dev\starworks\Arcane && powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1 -DryRun` (review), then without `-DryRun`. Verify `ThirdParty/Astra/VENDORED.txt` records `branch  : dev` and Task 1's commit SHA. Then `GenerateProjects.bat`.
- [ ] **Step 3: The v26 ledger entry** in `PluginABI.hpp`, appended directly above `kGamePluginABIVersion` (after the v25 entry, `:687`), then the constant:

```cpp
    // v26 (2026-09-11): Astra re-vendored to dev at the change-detection merge
    //     (feat/change-detection, 17 commits over f3e311d + the adoption's two
    //     primitives). SAME FAILURE CLASS AS v10/v24: plugins compile Astra's
    //     headers THEMSELVES, and this diff moves in-memory layouts their
    //     inlined templates index -- ArchetypeChunkPool's `Column` gained
    //     `EntityTicks* ticks` beside `disabledWords`; `ArchetypeColumnMeta`
    //     gained `trackedColumns[MAX_COMPONENTS]` + `trackedColumnCount` (128
    //     slots here: Arcane defines no ASTRA_MAX_COMPONENTS); the chunk arena
    //     now carves a per-column `Tick` version region and per-entity tick
    //     columns, shifting component-array offsets; `EntityLocation` became an
    //     aggregate; and the whole View/Query/ViewIterator ForEach path (which
    //     TransformSystems.hpp / RenderSystems.hpp instantiate INSIDE
    //     ReferenceGame.dll and Aphelyon.dll) now stamps chunk versions on
    //     entry. A v25 plugin under a v26 host would read component arrays at
    //     stale offsets and never stamp. Reject the pairing.
    //     TWO ARCANE FACTS RIDE ALONG. (1) Residency (Plan 1 Task 3): Runtime
    //     now calls SetTypeContext(ctx, ModuleResidency::Resident) and owns the
    //     engine roster through a Runtime-held Astra::ComponentModule "Arcane"
    //     -- the decision the v24 entry deferred, taken 2026-09-11 (spec
    //     docs/specs/2026-09-11-astra-adoption-design.md s5). Reverses the
    //     2026-08-10 ratification: its blocking caveat ("one registry per
    //     context") is gone from the vendored headers, and a Resident binder is
    //     pinned so the last Runtime's Reset reports Retained, never Erased.
    //     Plugins, hosts and the test exe keep the one-arg (Transient) call.
    //     (2) A TRACKED TYPE (Plan 1 Task 5): Transform declares
    //     `static constexpr bool AstraChangeTracked = true` -- a static member,
    //     so sizeof/reflect block/serialized bytes are unchanged and it needs
    //     no bump of its own; it is recorded here because it is a header change
    //     compiled into every plugin, and a non-const view over Transform now
    //     yields Astra::Mut<Transform> (source-compatible via the implicit T&).
    //     MEASURED, not assumed: `grep -rn -E` for every compile-affecting
    //     change in the diff -- EntityLocation, AddEntity, BatchAddEntities,
    //     ViewIterable, ViewIterator, Deserialize(, ComponentDescriptor,
    //     ArchetypeColumnMeta, SystemContext(, ISystemExecutor,
    //     ComponentUpdated, CreateView<, GetComponent<, GetRelations,
    //     ModuleResidency, AstraChangeTracked, Changed<, Modified(, IsChanged
    //     -- over BOTH game modules in the two trees returns NOTHING:
    //
    //       $ grep -rn -E "EntityLocation|AddEntity|BatchAddEntities|ViewIterable|ViewIterator|Deserialize\(|ComponentDescriptor|ArchetypeColumnMeta|SystemContext\(|ISystemExecutor|ComponentUpdated|CreateView<|GetComponent<|GetRelations|ModuleResidency|AstraChangeTracked|Changed<|Modified\(|IsChanged" ReferenceProject/Source/
    //       (no output)
    //       $ grep -rn -E "<same pattern>" D:/dev/starworks/Gacha/Game/Source/
    //       (no output)
    //
    //     Both modules name only Astra::{BinaryReader, BinaryWriter, Entity,
    //     Registry, SetTypeContext} (+ ComponentModule in a ReferenceGame
    //     comment) and AddSystem<TransformPropagationSystem/RenderSubmission
    //     System> -- header-only systems whose bodies change under them, which
    //     is exactly why the gate, not the compiler, is what refuses a stale DLL.
    //     ReferenceProject.arcproj restamped with this change, per the v16+
    //     precedent. Gacha's Game restamp (21 -> 26) is Plan 1's LAST task, in
    //     that repo, together with the Aphelyon.dll rebuild -- not deferred.
    inline constexpr uint32_t kGamePluginABIVersion = 26;
```

Delete the old `= 25;` line. Restamp `ReferenceProject/ReferenceProject.arcproj` `"abi": 25` → `26`.

- [ ] **Step 4: B9 build order, Debug.** `cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=Debug /m` (0 errors) → `cd .. && msbuild Arcane.slnx /p:Configuration=Debug /m` (0 errors; a compile error here contradicts map B1 — stop and report, do not patch around it).
- [ ] **Step 5: Full UNFILTERED suite, Debug** (`cd bin/Debug-windows-x86_64-md/ArcaneTests && ./ArcaneTests.exe`) — ALL pass including `[witness][gpu]` (proves the staged `ReferenceGame.dll` is v26, not stale). Then `./ArcaneTests.exe "~[gpu]"` — expect **56118 / 1620** (no Arcane test changed). Record both seed banners.
- [ ] **Step 6: Release, same order.** `msbuild ReferenceProject.slnx /p:Configuration=Release /m` → `msbuild Arcane.slnx /p:Configuration=Release /m` → `bin/Release-windows-x86_64-md/ArcaneTests/ArcaneTests.exe "~[gpu]"` — **56118 / 1620**. Then `powershell -ExecutionPolicy Bypass -File scripts\check-baselines.ps1 -ReportPath <a -r json run> -Configuration Debug -Invocation "~[gpu]"` — reports a rise over the committed 55294/1522, exit 0.
- [ ] **Step 7: One host launch.** `bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject --frames 60` (ABSOLUTE path) — no `AbiMismatch`, no `plugin: initial load failed` in the log.
- [ ] **Step 8: Commit (Arcane)** — `chore(astra)!: vendor Astra dev at the change-detection merge -- ABI 26, ReferenceProject restamped` (+ trailer). `git status` must show only `ThirdParty/Astra/**`, `PluginABI.hpp`, `ReferenceProject.arcproj` as changes to stage — the two plan documents under `docs/plans/` are NOT in that list because the controller commits them before Task 1 begins; if they still show as untracked, stop and ask rather than sweeping them in. Delta: **+0 cases**.

---

### Task 3: Residency — `Resident` + the Runtime-owned `ComponentModule "Arcane"` (spec §5, R2)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.cpp` (`Impl` members `:75-106`, ctor `:113-167`, includes), `ArcaneClient/src/Arcane/Plugin/PluginHost.cpp` (comment `:259-265`), `ReferenceProject/Source/ReferenceGame.cpp` (comment `:6-8`)
- Test: `ArcaneTests/src/RuntimeTest.cpp` (one new case)

**Interfaces:** none public. `Runtime::Impl` gains `std::optional<Astra::ComponentModule> engineModule;` declared between `components` and `registry`. **Declaration order, stated so nobody "fixes" it toward the spec:** members destruct in reverse declaration order, so `registry` (the `Astra::Registry`, declared after the module) dies FIRST, then `engineModule` (its `Reset` releases the slots), then `components` (the `ComponentRegistry` the module references) — registry → module → components. That is the correct order: the entities and chunks are gone before the descriptors' slots are released, and the module releases before the registry it references dies. Spec §5's parenthetical "destructs before the registry it references" is imprecise (its "registry" is the `ComponentRegistry`, `components`, not the `Astra::Registry`); the plan's placement satisfies it as intended. `RegisterSceneComponents` / `RegisterPhysicsComponents` stay (35 test files register on bare registries); `Runtime` stops calling them.

- [ ] **Step 1: Write the test** (append to `RuntimeTest.cpp`; add `#include <Arcane/Scene/Components.hpp>` and `#include <Astra/Core/TypeID.hpp>` to its include block):

```cpp
// Astra adoption Task 3 (residency, spec s5). The shape that crashed on
// 2026-08-10: several Runtimes against ONE TypeContext, each tearing down its
// own engine ComponentModule -- the last one used to erase the shared TypeMeta.
// Two assertions, and the second is the load-bearing one: GetMeta<Transform>
// would survive even an un-pinned release here, because this test exe drains
// its OWN baseline binder for Transform at test_main.cpp:22 and never drops it.
// BinderCount does not: Arcane.dll's binder stays on the stack ONLY because
// Runtime declared Resident, so its Reset reports Retained instead of dropping
// the binder (MetaRegistry::Release). A module-owned roster without Resident
// makes the count fall by one -- that is the RED this case exists to show.
TEST_CASE("Runtime: two Runtimes against the shared context leave the engine metas pinned",
          "[runtime][residency]")
{
    const std::uint64_t hash = Astra::TypeID<Arcane::Transform>::Hash();
    Astra::MetaRegistry& meta = Arcane::Test::SharedTypeContext().Meta();
    const std::size_t before = meta.BinderCount(hash);
    REQUIRE(before >= 2);   // this exe's baseline + Arcane.dll's (pinned at test_main's throwaway pin)

    {
        Arcane::Runtime a(&Arcane::Test::SharedTypeContext());
        Arcane::Runtime b(&Arcane::Test::SharedTypeContext());
        // The roster is present in BOTH registries (GetComponentDescriptor is the
        // registry's presence query: null when the slot is empty).
        CHECK(a.Components()->GetComponentDescriptor(Astra::TypeID<Arcane::Transform>::Value()) != nullptr);
        CHECK(b.Components()->GetComponentDescriptor(Astra::TypeID<Arcane::PhysicsBodyRef>::Value()) != nullptr);
        CHECK(meta.BinderCount(hash) == before);   // registration ACQUIRES on an existing binder, adds none
    }

    CHECK(meta.BinderCount(hash) == before);       // Retained: the pinned binder did not leave
    const Astra::TypeMeta* still = Astra::GetMeta<Arcane::Transform>();
    REQUIRE(still != nullptr);
    CHECK(still->typeName == "Arcane::Transform");
}
```

`PhysicsBodyRef` needs `#include <Arcane/Scene/PhysicsComponents.hpp>`; `MetaRegistry::BinderCount(uint64_t)` is the vendored diagnostics accessor (`MetaRegistry.hpp:548`).

- [ ] **Step 2: Run** `./ArcaneTests.exe "[residency]"` — **PASSES on the anonymous roster** (anonymous registration takes a ref and registry destruction releases nothing). Expected; the RED is exhibited in Step 4.
- [ ] **Step 3: The module-owned roster.** In `Runtime.cpp`: add `#include <Astra/Component/ComponentModule.hpp>`. In `Impl`, after `components` (`:81`) insert `std::optional<Astra::ComponentModule>   engineModule;   // the engine roster's RAII owner; destructs AFTER registry (declared before it), BEFORE components`. In the ctor replace `:166-167` (`RegisterSceneComponents(*components); RegisterPhysicsComponents(*components);`) with:

```cpp
            engineModule.emplace(Astra::ComponentModule::Open(components, "Arcane"));
            ARC_ASSERT(*engineModule, "Runtime: ComponentModule::Open refused -- the slot above must be installed first");
            // EXACTLY the order RegisterSceneComponents + RegisterPhysicsComponents
            // register in (SceneModule.hpp / PhysicsComponents.hpp): ids are a
            // first-touch counter, so same order == same numbering as before.
            engineModule->Register<Transform, WorldTransform, PreviousTransform, SpriteRenderer,
                                   PostProcess, Identity, Hidden, Camera, MeshRenderer,
                                   RigidBody2D, Collider2D, PhysicsBodyRef>();
```

(`ARC_ASSERT` — use whatever the engine's assert macro is spelled in `Arcane/Base/Assert.hpp`; grep it.) Leave `:114` as the ONE-ARG call for this step. Build, run `[residency]` — **expect FAIL, and at the FIRST assertion**: under a Transient module-owned roster, `test_main.cpp:32`'s throwaway pin Registers → Resets → Releases Arcane.dll's binder at zero refs BEFORE any test runs, so by the time this case starts `before == 1` (only this exe's baseline is left) and `REQUIRE(before >= 2)` fails. That is the same drop the after-scope `BinderCount == before` check would show; it just happens earlier than the two Runtimes in the case. This is the pin biting.
- [ ] **Step 4: Resident.** `:114` becomes `Astra::SetTypeContext(context, Astra::ModuleResidency::Resident);`. Build, run `[residency]` — **PASS**.
- [ ] **Step 5: Rewrite the ratification block** (`:117-165`). Keep the first paragraph (why the roster is registered here). Replace everything from "Plugins now register only the types…" through "…nothing here is ever torn down early." with:

```cpp
            // Plugins register only the types they themselves implement, through
            // their own RAII Astra::ComponentModule (PluginHost.cpp /
            // HotReloadPlugin.cpp). The engine's roster is module-owned too, since
            // 2026-09-11 (spec docs/specs/2026-09-11-astra-adoption-design.md s5),
            // through the Runtime-held handle below -- which REVERSES the
            // 2026-08-10 ratification that kept it anonymous. That ratification
            // rested on ComponentModule.hpp's old "one registry per context"
            // caveat: ReleaseModule erased a type's TypeMeta from the SHARED
            // TypeContext whenever the releasing registry was its sole owner, so
            // the test suite's many short-lived Runtimes (73 construction sites)
            // wiped the metas out from under each other, and retiring the handle
            // into a longer-lived container traded that for a static-destruction
            // crash at exit. The 2026-09-10 binder-stack vendor (ABI v24) removed
            // the caveat -- several registries per context is now the supported
            // shape -- and the residency declaration at SetTypeContext above is
            // what makes the module-owned roster SAFE here: Arcane.dll never
            // unmaps, so its binders are PINNED and every Reset reports Retained;
            // registry-less GetMeta keeps resolving after the last Runtime dies
            // (pinned by RuntimeTest.cpp's "[residency]" case). No plugin-side
            // static is involved: Impl is pimpl-held and reset from ~Runtime.
            //
            // A plugin's InstallOwned shadows whatever is live for an id,
            // module-owned or not, and its own unload restores the shadow -- the
            // "plugin overrides it, unload restores it" behaviour is unchanged.
            // RegisterSceneComponents / RegisterPhysicsComponents survive for the
            // tests that register on bare registries; Runtime no longer calls them.
```

Keep the ComponentID NUMBERING paragraph verbatim. Drop the now-unused `#include <Arcane/Scene/SceneModule.hpp>` only if nothing else in the file needs it (`Scene::LoadBinary` is not called here; `RegisterSceneComponents` no longer is) — keep `PhysicsComponents.hpp` / `Components.hpp` for the type names.
- [ ] **Step 6: Comment sites.** `PluginHost.cpp:259-265` — replace "The engine's own roster is never displaced in the first place -- Runtime's constructor registers it anonymously (owner 0), not through a ComponentModule, so a plugin overriding one of those types shadows the anonymous entry and its own unload restores it via the same owner-stack mechanism" with "The engine's own roster is module-owned too (Runtime's ctor holds an Astra::ComponentModule "Arcane" for it, under a Resident declaration -- Runtime.cpp), so a plugin overriding one of those types shadows the engine's entry and its own unload restores it via the same owner-stack mechanism". `ReferenceGame.cpp:6-8` — "the engine roster is registered by Runtime's ctor" → "the engine roster is registered by Runtime's ctor through its own Resident ComponentModule".
- [ ] **Step 7: Full `~[gpu]` suite — green.** Delta: **+1 case** (`[runtime][residency]`). Commit — stage by name: `ArcaneClient/src/Arcane/Base/Runtime.cpp`, `ArcaneClient/src/Arcane/Plugin/PluginHost.cpp`, `ReferenceProject/Source/ReferenceGame.cpp` (Step 6's comment-only edit — no `ReferenceGame.dll` rebuild is needed for a comment), `ArcaneTests/src/RuntimeTest.cpp`. Message: `feat(runtime): engine roster on a Runtime-owned Resident ComponentModule (reverses the 2026-08-10 ratification)`.

---

### Task 4: Const-ification (spec §6.1; map B4 + B10 #3-4, transcribed file by file)

Mechanical and behaviour-neutral: nothing is tracked yet and nothing reads `Changed<>`, so this lands before anything can observe a stamp. Every read-only `CreateView<T&>` becomes `const T` with `const T&` params; every read-only non-const `GetComponent<T>` goes through `std::as_const(reg)`. Add `#include <utility>` where `std::as_const` is new to a TU.

**Files:** Modify: `ArcaneClient/src/Arcane/Scene/RenderSystems.hpp`, `Scene/MeshSubmissionSystem.hpp`, `Scene/SceneCamera.hpp`, `Scene/PhysicsSystem.hpp`, `Scene/TransformSystems.hpp`, `Render/PickEmit.cpp`, `Host/SceneRenderResolver.cpp`, `Edit/EntityOps.cpp`, `ArcaneEditor/src/Viewport/EditorCamera.cpp`, `ArcaneEditor/src/App/EditorAppFrame.cpp`, `ArcaneEditor/src/Panels/EditorPanels.cpp`, `ArcaneRuntime/src/RuntimeApp.cpp`. Test: none new (the suite is the regression net; every site is a read).

- [ ] **Step 1: Views (B10 #3).** Each row: the `CreateView<…>` args as written → as required, and the lambda params to `const T&`.

| File:line | Now | Becomes |
|---|---|---|
| `RenderSystems.hpp:47-48` | `CreateView<WorldTransform, SpriteRenderer, Astra::Not<Hidden>>()` / `(Astra::Entity e, WorldTransform& world, SpriteRenderer& sprite)` | `CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>()` / `(Astra::Entity e, const WorldTransform& world, const SpriteRenderer& sprite)` |
| `RenderSystems.hpp:77` | `reg.GetComponent<PreviousTransform>(e)` | `std::as_const(reg).GetComponent<PreviousTransform>(e)` |
| `MeshSubmissionSystem.hpp:133-134` | `<WorldTransform, MeshRenderer, Astra::Not<Hidden>>` / `(Astra::Entity, WorldTransform& world, MeshRenderer& renderer)` | `<const WorldTransform, const MeshRenderer, Astra::Not<Hidden>>` / `(Astra::Entity, const WorldTransform& world, const MeshRenderer& renderer)` |
| `PickEmit.cpp:45-46` | `<WorldTransform, SpriteRenderer>` / `(Astra::Entity e, WorldTransform& xf, SpriteRenderer& sp)` | `<const WorldTransform, const SpriteRenderer>` / `(…, const WorldTransform& xf, const SpriteRenderer& sp)` |
| `PickEmit.cpp:105-106` | `<Collider2D, PhysicsBodyRef>` / `(Astra::Entity entity, Collider2D& col, PhysicsBodyRef& ref)` | `<const Collider2D, const PhysicsBodyRef>` / `(…, const Collider2D& col, const PhysicsBodyRef& ref)` |
| `EditorCamera.cpp:194-195` | `<WorldTransform, SpriteRenderer, Astra::Not<Hidden>>` / `(Astra::Entity, WorldTransform& world, SpriteRenderer& sprite)` | `<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>` / `(…, const WorldTransform& world, const SpriteRenderer& sprite)` |
| `SceneCamera.hpp:65-66` and `:208-209` | `CreateView<Camera>()` / `(Astra::Entity e, Camera& cam)` | `CreateView<const Camera>()` / `(Astra::Entity e, const Camera& cam)` |
| `SceneRenderResolver.cpp:197,338` | `CreateView<SpriteRenderer>()` / `(Astra::Entity, SpriteRenderer& s)` | `CreateView<const SpriteRenderer>()` / `(Astra::Entity, const SpriteRenderer& s)` |
| `SceneRenderResolver.cpp:223,383` | `CreateView<MeshRenderer>()` / `(Astra::Entity, MeshRenderer& mr)` | `CreateView<const MeshRenderer>()` / `(…, const MeshRenderer& mr)` |
| `SceneRenderResolver.cpp:238,482` | `CreateView<PostProcess>()` / `(Astra::Entity, PostProcess& pp)` | `CreateView<const PostProcess>()` / `(…, const PostProcess& pp)` |
| `EntityOps.cpp:65-66,458-459` | `CreateView<Identity>()` / `(Astra::Entity, Identity& info)` and `(Astra::Entity e, Identity& info)` | `CreateView<const Identity>()` / `const Identity& info` |
| `RuntimeApp.cpp:770-771` | `CreateView<Arcane::Camera>()` / `(Astra::Entity, Arcane::Camera&)` | `CreateView<const Arcane::Camera>()` / `(Astra::Entity, const Arcane::Camera&)` |
| `PhysicsSystem.hpp:290-295` PASS 2 | `<RigidBody2D, Collider2D, PhysicsBodyRef, Transform>` / `(…, RigidBody2D& rb, Collider2D& col, PhysicsBodyRef& ref, Transform& lt)` | `<const RigidBody2D, const Collider2D, PhysicsBodyRef, const Transform>` / `(…, const RigidBody2D& rb, const Collider2D& col, PhysicsBodyRef& ref, const Transform& lt)` — `ref.handle`/`ref.appliedScale` are the only writes |
| `PhysicsSystem.hpp:430-435` PASS 3.5 | `<PhysicsBodyRef, Transform, Collider2D, RigidBody2D>` / `(…, PhysicsBodyRef& ref, Transform& lt, Collider2D& col, RigidBody2D& /*rb*/)` | `<PhysicsBodyRef, const Transform, const Collider2D, Astra::With<RigidBody2D>>` / `(Astra::Entity /*entity*/, PhysicsBodyRef& ref, const Transform& lt, const Collider2D& col)` — `With<>` yields nothing, so the `rb` param goes |
| `PhysicsSystem.hpp:484-488` PASS 4 | `<PhysicsBodyRef, Transform, RigidBody2D>` / `(…, PhysicsBodyRef& ref, Transform& lt, RigidBody2D& rb)` | `<const PhysicsBodyRef, Transform, RigidBody2D>` / `(…, const PhysicsBodyRef& ref, Transform& lt, RigidBody2D& rb)` |

- [ ] **Step 2: `GetComponent` reads (B10 #4).** Each becomes `std::as_const(reg).GetComponent<T>(e)` (spell the registry expression as at the site) with the receiving pointer `const T*`:
  - `TransformSystems.hpp:245` (`const WorldTransform* w = std::as_const(reg).GetComponent<WorldTransform>(c.order[i]);`) and `:286` (`const Transform* local = std::as_const(reg).GetComponent<Transform>(e);` — `local` is only read: `SamePose`, `ToMatrix`, the copy into `shadow`). `:301` `WorldTransform* world = reg.GetComponent<WorldTransform>(e);` STAYS non-const in this task (it is the write site; Task 5 restructures so it is fetched only on the write path).
  - `SceneCamera.hpp:85,87,222,224` — already `const T*` receivers; the registry call becomes `std::as_const(reg).GetComponent<…>(e)`.
  - `EntityOps.cpp:79` (`const Identity* info = std::as_const(reg).GetComponent<Identity>(e)`), `:236` (`if (!std::as_const(reg).GetComponent<Identity>(e))`), `:392`, `:682` (`const Transform* t = std::as_const(reg).GetComponent<Transform>(*it)` — the gizmo/clipboard `WorldMatrix` chain: without this every gizmo frame would mark the primary's Transform changed). `:189` (`Hidden`, a tag), `:208-218` and `:593-596` (real writes) are UNTOUCHED.
  - `EditorAppFrame.cpp:973+977` (`const Arcane::Transform* lt = nullptr; … lt = std::as_const(*regPtr).GetComponent<Arcane::Transform>(m_selection.Primary());` — presence only), `:1021` (`const Arcane::Transform* et = std::as_const(*regPtr).GetComponent<Arcane::Transform>(e);` — presence only), `:1093` (`std::as_const(*regPtr).GetComponent<Arcane::Identity>(e)`), `:1676` (`const Arcane::Transform* lt = std::as_const(drawReg).GetComponent<Arcane::Transform>(…)`), `:2215` (`std::as_const(m_runtime->Registry()).GetComponent<Arcane::Identity>(…)`). `:1151-1190` (the gizmo WRITE) is UNTOUCHED — it is the stamp the pre-pass relies on.
  - `EditorPanels.cpp:1367,1568,1683,1746` — `std::as_const(registry).GetComponent<Arcane::Identity>(…)`.
  - `EditorCamera.cpp:166,170` — `std::as_const(reg).GetComponent<WorldTransform>(e)` / `<SpriteRenderer>`.
  - `RuntimeApp.cpp:949` — `std::as_const(m_runtime->Registry()).GetComponent<Arcane::Identity>(hit)`.
- [ ] **Step 3: The comment lie.** `TransformSystems.hpp:84-85` "Astra has no component change tracking and Task 4 is explicitly not allowed to add one, so" → delete those words so the sentence reads "shadow/shadowValid are the change detector: "did this local pose move?" is answered by comparing…" (Task 5 deletes the whole block anyway). `RenderSystems.hpp:38`'s `Reads<…>` trait is now TRUE — add the one-line comment `// Reads<> is honest now: the view below is const (Astra adoption 2026-09-11).`
- [ ] **Step 4: Build order** (a game-module header changed): `ReferenceProject.slnx` Debug → `Arcane.slnx` Debug. Full `~[gpu]` suite — green, **1621 cases** (1620 + Task 3's one; nothing here adds a case; the assertion count is whatever the run says — derive it, do not predict it). Commit — `refactor(scene): const-correct every read-only view and GetComponent (Astra adoption s6.1)`.

---

### Task 5: `Transform` tracked + the `Changed<Transform>` propagation (spec §6.2, §6.3, §6.6, R8)

**The core of the plan.** `TransformOrder` gains `lastRun`, `moved[]`, `rowOf`, and two counters; `shadow`/`shadowValid`/`SamePose` are deleted; `dirty[]`/`world[]` stay. `operator()` becomes: Rebuild (resets `lastRun = 0`) → **materialisation probe** → `Changed<Transform>` pre-pass → early-out or the linear pass → `lastRun = CurrentTick(); AdvanceTick()`. **The end-of-pass `AdvanceTick()` makes the system `RequiresExclusive`** (a checker finding, applied): Astra forbids advancing the counter while any other system of the same group may be running (`ArchetypeManager.hpp` CurrentTick contract; `SystemExecutor.hpp`'s group-shared tick), and the spec's "a double advance inside a scheduler is harmless" holds only because each scheduler that owns this system today holds it alone. The flag makes the scheduler give it its own group, so the advance is never concurrent with another system's stamps.

**One deliberate addition to spec §6.3, stated so the user can veto it:** the spec's early-out ("nothing moved ⇒ return") would skip the unconditional WorldTransform presence check that `TransformOrderTest.cpp:865` ("a WorldTransform removed behind the system's back is healed") pins by construction — removing a `WorldTransform` moves the entity to another archetype WITHOUT marking its (untouched) `Transform`, so the pre-pass yields nothing and the early-out would leave the entity un-healed forever. The probe — one `CreateView<const Transform, Astra::Not<WorldTransform>>()` walk whose matching archetypes are EMPTY in steady state (every spatial row leaves the pass with a WorldTransform), so it costs one chunk-list check per frame — keeps the heal contract under the early-out and also collapses the old two-Compose materialisation dance into one pass.

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/Components.hpp` (`Transform`, `:48-52`), `ArcaneClient/src/Arcane/Scene/TransformSystems.hpp` (whole cache + system; includes)
- Test: `ArcaneTests/src/TransformOrderTest.cpp` (three new cases; every existing case re-pinned unchanged), `TransformPropagationTest.cpp` / `AuthoredTransformSyncTest.cpp` / `PhysicsPauseTest.cpp` (re-pinned unchanged: `TransformPropagationTest.cpp:119-120` and `:180-182` keep their non-const `Transform&` lambdas — Astra's `Mut<T>` implicit conversion keeps them compiling, marking harmlessly)

**Interfaces:**

```cpp
    // Components.hpp -- inside struct Transform, first line of the body:
        // Astra change tracking (spec 2026-09-11 s6.2): 8 B per entity of
        // {added, changed} ticks so Changed<Transform> is EXACT per entity --
        // TransformPropagationSystem's pre-pass and PhysicsSystem's paused
        // reconcile both need entity precision, and a coarse chunk stamp would
        // recompose every row of a touched chunk. Nothing else is tracked
        // (WorldTransform's readers all rebuild per frame; Hidden is a tag).
        static constexpr bool AstraChangeTracked = true;
```

```cpp
    // TransformSystems.hpp -- the cache (replaces :73-141 wholesale)
    struct TransformOrder
    {
        static constexpr std::uint32_t kNoParent = 0xFFFFFFFFu;

        // ---- structure: rebuilt only when StructureVersion()/root moves ----
        std::vector<Astra::Entity> order;        // BFS from the scene root; order[0] IS the root
        std::vector<std::uint32_t> parentIndex;  // index INTO order, always strictly < own index
        // entity -> row in `order`, filled by Rebuild (which already walks every
        // entity). THE bridge between Astra's entity-keyed Changed<Transform>
        // yield and this cache's row-indexed arrays. An entity absent here is
        // outside the scene root's subtree and is never written by the pass.
        Astra::FlatMap<Astra::Entity, std::uint32_t> rowOf;

        // ---- per-row value state, parallel to `order` ----
        // `moved` is the change detector (Astra adoption 2026-09-11, spec s6.3):
        // set by the Changed<Transform> pre-pass for exactly the rows whose LOCAL
        // pose was written since `lastRun` -- exact per entity because Transform
        // is AstraChangeTracked (Components.hpp) -- and consumed (reset to 0) by
        // Compose. It replaced a per-row shadow copy of the last-composed pose
        // compared ten floats at a time every frame: the registry now answers
        // "did this local move?" with one chunk-version compare per chunk and one
        // tick compare per entity, and a scene in which nothing moved skips the
        // linear pass entirely (the early-out in operator()).
        std::vector<std::uint8_t> moved;         // 1 => row i's local was written since lastRun
        std::vector<std::uint8_t> dirty;         // decided this pass; read by children
        // (world[] comment unchanged from before -- keep it verbatim)
        std::vector<glm::mat4>    world;

        // Scratch, kept here so a steady frame allocates nothing at all.
        std::vector<Astra::Entity>    needsWorld;
        Astra::FlatSet<Astra::Entity> visited;   // Rebuild's cycle guard

        // ---- invalidation keys ----
        Astra::Entity root{};
        std::uint32_t structureVersion = 0;      // (comment unchanged -- keep it)

        // The "since" tick of the last pass: the pre-pass yields Transforms
        // written STRICTLY after it. 0 == never (Astra's tick sentinel), so a
        // fresh cache -- and one a Rebuild just reset -- sees every stamped
        // Transform on its next pass. Lives on the RESOURCE, not the system: the
        // editor's per-frame scheduler and the game module's fixedUpdate
        // scheduler drive this same cache, and a swapped registry
        // (RestoreRegistry, ResetRegistry, scene load) restarts its ticks at 1 --
        // a since-tick stored anywhere else would be stale against it.
        Astra::Tick lastRun = 0;

        std::uint32_t rebuilds = 0;   // (comment unchanged)
        // Instrumentation for the adoption's tests: operator() calls that reached
        // this cache, and rows Compose actually recomposed. An early-out leaves
        // `composed` unchanged -- that is the assertion "a static scene does no
        // matrix work" is made of.
        std::uint32_t runs = 0;
        std::uint32_t composed = 0;

        template<typename Archive> void Serialize(Archive& /*ar*/) {}
    };
```

- [ ] **Step 1: Write the failing tests** (append to `TransformOrderTest.cpp`):

```cpp
// ============================================================================
// Astra adoption (2026-09-11): the Changed<Transform> pre-pass + early-out.
// ============================================================================
TEST_CASE("a static scene early-outs: the second pass composes nothing", "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    AwkwardScene s = BuildAwkwardScene(reg);

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);
    const std::uint32_t composedAfterFirst = OrderOf(reg).composed;
    REQUIRE(composedAfterFirst == s.all.size());   // the first pass composes every spatial row

    for (int i = 0; i < 8; ++i)
        propagate(reg);
    CHECK(OrderOf(reg).composed == composedAfterFirst);   // no matrix work at all
    CHECK(OrderOf(reg).runs == 9u);

    // One moved leaf costs exactly one composition, not a full pass.
    reg.GetComponent<Arcane::Transform>(s.deepLeaf)->position.x += 1.0f;
    propagate(reg);
    CHECK(OrderOf(reg).composed == composedAfterFirst + 1);
}

TEST_CASE("a Rebuild forces a full recompose after a reparent under a clean parent",
          "[scene][transform-order]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    AwkwardScene s = BuildAwkwardScene(reg);

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);
    propagate(reg);                                          // settled: lastRun is fresh
    const std::uint32_t before = OrderOf(reg).composed;

    // Nobody's LOCAL moves; only the structure does. The reparented subtree's
    // world matrices are wrong until recomposed, and the pre-pass alone would
    // see nothing -- Rebuild's lastRun = 0 is what makes every row moved.
    reg.SetParent(s.lateChild, s.fanParent);
    const WorldMap reference = ReferenceWorlds(reg, s.root);
    propagate(reg);
    CHECK(OrderOf(reg).composed == before + static_cast<std::uint32_t>(s.all.size()));
    CheckMatchesReference(reg, reference);
}

TEST_CASE("a Transform written through Registry::Modified(id) is seen by the next pass",
          "[scene][transform-order]")
{
    // The descriptor path the editor's Inspector and undo use: GetComponentByHash
    // hands out a raw pointer and stamps NOTHING (Astra Registry.hpp), so the
    // write is invisible to Changed<Transform> until the caller says Modified.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    Astra::Entity root = Spatial(reg, {0.0f, 0.0f, 0.0f});
    Astra::Entity leaf = Spatial(reg, {1.0f, 0.0f, 0.0f});
    reg.SetParent(leaf, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    const std::uint64_t hash = Astra::TypeID<Arcane::Transform>::Hash();
    const Astra::ComponentID id = Astra::TypeID<Arcane::Transform>::Value();
    auto* raw = static_cast<Arcane::Transform*>(reg.GetComponentByHash(leaf, hash));
    REQUIRE(raw != nullptr);
    raw->position = glm::vec3(1.0f, 9.0f, 0.0f);
    propagate(reg);
    CHECK(reg.GetComponent<Arcane::WorldTransform>(leaf)->matrix[3].y == Catch::Approx(0.0f));   // invisible: no stamp

    REQUIRE(reg.Modified(leaf, id));
    propagate(reg);
    CHECK(reg.GetComponent<Arcane::WorldTransform>(leaf)->matrix[3].y == Catch::Approx(9.0f));   // seen
}
```

- [ ] **Step 2: Run** `./ArcaneTests.exe "[transform-order]"` — **expect FAIL** (compile: no `composed`/`runs`; `reg.Modified(e, id)` exists since Task 2).
- [ ] **Step 3: `Transform` opts in** — the `AstraChangeTracked` line from Interfaces at the top of `struct Transform`'s body.
- [ ] **Step 4: Rewrite `TransformSystems.hpp`.** Includes: add `#include <Astra/Container/FlatMap.hpp>`, `#include <Astra/Core/Tick.hpp>`, `#include <utility>`. Replace the `TransformOrder` struct with the Interfaces version (carry the `world[]`, `structureVersion` and `rebuilds` comments over verbatim). Replace the system with:

```cpp
    struct TransformPropagationSystem
        : Astra::SystemTraits<Astra::Reads<Transform>, Astra::Writes<WorldTransform>>
    {
        // EXCLUSIVE, because of the end-of-pass AdvanceTick below. Astra's tick
        // contract (ArchetypeManager.hpp: CurrentTick "NEVER advanced concurrently
        // with a running system"; SystemExecutor.hpp: systems in one group SHARE
        // the group's tick) forbids a system advancing the counter while any
        // other system of its group may be running or stamping. The scheduler
        // honours this flag by giving the system its own group (SystemScheduler
        // .hpp reads T::RequiresExclusive into the metadata), so the advance
        // happens with nothing else in flight. Today both schedulers that own
        // this system hold it alone, which is why the violation was latent; the
        // flag makes the contract hold by construction, not by roster luck.
        static constexpr bool RequiresExclusive = true;

        // The ONE overload, everywhere: the game module's fixedUpdate scheduler,
        // the editor's Edit-mode scheduler (EditModeSchedule) and the tests all
        // drive this. Time base per pass: `since` is the cache's lastRun; at the
        // end the cache takes CurrentTick() and the tick is ADVANCED -- mirroring
        // the scheduler's post-segment advance -- so a write made after this pass
        // is strictly newer than lastRun and IsNewer(t, t) never hides it (the
        // "a clean leaf is not rewritten" canary in TransformOrderTest.cpp writes
        // between two bare calls). Under a scheduler that advance is one extra
        // tick per pass, taken in an exclusive group (above): ticks are cheap and
        // only ever compared, and no other system can be mid-stamp when it moves.
        void operator()(Astra::Registry& reg)
        {
            const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
            if (!sceneRoot) return;
            const Astra::Entity root = sceneRoot->entity;

            TransformOrder* cache = reg.GetResource<TransformOrder>();
            if (!cache)
                cache = reg.EmplaceResource<TransformOrder>();
            if (!cache) return;
            TransformOrder& c = *cache;
            ++c.runs;

            // 1. Structure. StructureVersion covers attach/detach/reparent/destroy/
            // clear; the root comparison covers the one structural change Astra
            // cannot see. A Rebuild resets lastRun to 0 (inside Rebuild): a reparent
            // must recompose everything, and a rebuilt row order invalidates any
            // per-row memory.
            const std::uint32_t version = reg.StructureVersion();
            const bool rebuilt = (c.structureVersion != version || c.root != root);
            if (rebuilt)
                Rebuild(reg, root, version, c);

            // 2. Materialise missing WorldTransforms (the heal contract), 3. mark the
            // rows whose local moved. Both are cheap in steady state: the probe's
            // archetypes are empty, and the pre-pass chunk-rejects untouched chunks.
            bool work = rebuilt;
            work = Materialise(reg, c) || work;
            work = MarkMoved(reg, c) || work;

            // 4. THE EARLY-OUT. Nothing moved and nothing was rebuilt or
            // materialised => nothing inherited => no row can need a matrix.
            if (work)
                Compose(reg, c);

            // 5. Advance, on EVERY path (early-out included, so the pre-pass's
            // chunk reject stays tight instead of re-scanning chunks stamped by
            // unrelated writes until the next full pass).
            c.lastRun = reg.CurrentTick();
            reg.AdvanceTick();
        }

    private:
        static void Rebuild(Astra::Registry& reg, Astra::Entity root,
                            std::uint32_t version, TransformOrder& c)
        {
            const Astra::RelationshipGraph& graph = reg.GetRelationshipGraph();

            c.order.clear();
            c.parentIndex.clear();
            c.order.push_back(root);
            c.parentIndex.push_back(TransformOrder::kNoParent);

            // (cycle-guard comment unchanged -- keep it)
            Astra::FlatSet<Astra::Entity>& visited = c.visited;
            visited.Clear();
            visited.Reserve(64);
            visited.Insert(root);

            for (std::size_t i = 0; i < c.order.size(); ++i)
            {
                const Astra::Entity parent = c.order[i];
                for (Astra::Entity child : graph.GetChildren(parent))
                {
                    if (!visited.Insert(child).second)
                        continue;
                    c.order.push_back(child);
                    c.parentIndex.push_back(static_cast<std::uint32_t>(i));
                }
            }

            const std::size_t n = c.order.size();
            c.rowOf.Clear();
            c.rowOf.Reserve(n);
            for (std::size_t i = 0; i < n; ++i)
                c.rowOf[c.order[i]] = static_cast<std::uint32_t>(i);
            c.moved.assign(n, 0);
            c.dirty.assign(n, 0);
            c.world.resize(n);
            c.needsWorld.clear();

            // (mirror-seed comment unchanged -- keep it)
            for (std::size_t i = 0; i < n; ++i)
            {
                const WorldTransform* w = std::as_const(reg).GetComponent<WorldTransform>(c.order[i]);
                c.world[i] = w ? w->matrix : glm::mat4(1.0f);
            }

            c.root = root;
            c.structureVersion = version;
            // Everything recomposes on the pass after a rebuild: with lastRun at
            // "never", the pre-pass yields every Transform that was ever stamped.
            c.lastRun = 0;
            ++c.rebuilds;
        }

        // WorldTransform is DERIVED, never authored: a subtree row with a
        // Transform but no WorldTransform (Edit::CreateEntity, SceneAsset::
        // CreateEmpty's root, a pre-fix .arcscene, an Inspector Add Component --
        // or one REMOVED behind our back by a plugin) gets one here, or it can
        // never satisfy RenderSubmissionSystem's view. Probed through the
        // registry rather than per row so the early-out below cannot skip it:
        // gaining a Transform marks the entity (the pre-pass sees it), but LOSING
        // a WorldTransform marks nothing on Transform, and the archetypes this
        // view matches are empty in steady state, so the probe is one chunk-list
        // check per frame. Adds happen AFTER the walk (a structural change during
        // a ForEach is refused) and BEFORE any component pointer is held.
        static bool Materialise(Astra::Registry& reg, TransformOrder& c)
        {
            c.needsWorld.clear();
            auto missing = reg.CreateView<const Transform, Astra::Not<WorldTransform>>();
            missing.ForEach([&](Astra::Entity e, const Transform&)
            {
                if (c.rowOf.TryGet(e))
                    c.needsWorld.push_back(e);
            });
            for (Astra::Entity e : c.needsWorld)
            {
                if (!reg.AddComponent<WorldTransform>(e, WorldTransform{}))
                    continue;
                c.moved[*c.rowOf.TryGet(e)] = 1;   // a fresh identity matrix must be composed
            }
            return !c.needsWorld.empty();
        }

        // The pre-pass: chunk-reject first, then -- because Transform is tracked
        // -- exactly the entities whose Transform was written since lastRun.
        // Entities outside the subtree (not in rowOf) are ignored.
        static bool MarkMoved(Astra::Registry& reg, TransformOrder& c)
        {
            bool any = false;
            auto changed = reg.CreateView<const Transform, Astra::Changed<Transform>>();
            changed.Since(c.lastRun).ForEach([&](Astra::Entity e, const Transform&)
            {
                if (const std::uint32_t* row = c.rowOf.TryGet(e))
                {
                    c.moved[*row] = 1;
                    any = true;
                }
            });
            return any;
        }

        // The linear pass. THE SUBTLETY is unchanged: `inherited` is what makes a
        // moved parent drag its whole subtree; one forward pass is enough because
        // `order` is topological -- row p was decided before row i is read.
        // A clean, non-inherited row does no lookup at all. A non-spatial node
        // (never had a Transform, or the Inspector removed one) contributes no
        // dirtiness and keeps its mirror row, so its children compose against
        // exactly what they used to read off the component.
        static void Compose(Astra::Registry& reg, TransformOrder& c)
        {
            const std::size_t n = c.order.size();
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::uint32_t p = c.parentIndex[i];
                const bool inherited = (p != TransformOrder::kNoParent) && c.dirty[p] != 0;
                const bool moved     = c.moved[i] != 0;
                c.moved[i] = 0;   // consumed: the next pre-pass starts clean
                if (!inherited && !moved)
                {
                    c.dirty[i] = 0;
                    continue;   // the point of the exercise: no matrix work at all
                }

                const Astra::Entity e = c.order[i];
                const Transform* local = std::as_const(reg).GetComponent<Transform>(e);
                if (!local)
                {
                    c.dirty[i] = 0;
                    continue;
                }
                // The ONE non-const fetch: this is the write. Compose is the sole
                // writer of WorldTransform::matrix in engine source (see world[]).
                WorldTransform* world = reg.GetComponent<WorldTransform>(e);
                if (!world)
                {
                    c.dirty[i] = 0;   // Materialise refused it this frame; it retries next frame
                    continue;
                }

                const glm::mat4 localMat = local->ToMatrix();
                c.world[i] = (p == TransformOrder::kNoParent) ? localMat
                                                              : c.world[p] * localMat;
                world->matrix = c.world[i];
                c.dirty[i]    = 1;
                ++c.composed;
            }
        }
    };
```

Delete `SamePose` and every `shadow`/`shadowValid` mention. Update the file-header comment's "DIRTY FLAGS" bullet (`:28-32`) to name the `Changed<Transform>` pre-pass as the `moved` source and the early-out, and the `TransformOrder` banner (`:61-72`): "the editor calls the system as a temporary" → "the editor's EditModeSchedule and the game module's fixedUpdate scheduler both own a long-lived instance".

- [ ] **Step 5: Build order** (game-module headers changed): `ReferenceProject.slnx` → `Arcane.slnx`. Run `[transform-order]` — **PASS**, every pre-existing case included (the clean-leaf canary, the rebuild counts, the healed WorldTransform, both LegacyPropagate differentials). Then `[scene]`, `[transform-sync]`, `[physics]`, `[interp]`, `[outliner]` — PASS.
- [ ] **Step 6: Full `~[gpu]` suite — green.** Delta: **+3 cases** (`[transform-order]`). Commit — `feat(scene): Transform is change-tracked; propagation runs a Changed<Transform> pre-pass with an early-out (deletes the shadow-copy detector)`.

---

### Task 6: Editor writes mark (spec §6.4)

Two sites, one line each, plus a test TU that pins the descriptor path with the Inspector's own pure writer.

**Files:**
- Modify: `ArcaneEditor/src/Panels/InspectorView.cpp` (`ForEachTarget`, `:176-186`), `ArcaneClient/src/Arcane/Edit/ComponentEditCommand.cpp` (`Restore`, `:41-49`)
- Test: `ArcaneTests/src/ChangeDetectionMarksTest.cpp` (NEW — run `GenerateProjects.bat`)

- [ ] **Step 1: Write the failing tests:**

```cpp
// Astra adoption Task 6 (spec s6.4): the two editor write paths that bypass
// Astra's stamping -- Registry::GetComponentByHash hands out a raw pointer and
// stamps NOTHING -- must say Modified, or an Inspector edit / an undo of a
// Transform field is invisible to the Changed<Transform> propagation.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Edit/ComponentEditCommand.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Registry/Registry.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace
{
    struct Scene
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Astra::Entity root{}, leaf{};
        Scene()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
            leaf = reg.CreateEntity();
            Arcane::Transform t; t.position = glm::vec3(1.0f, 0.0f, 0.0f);
            reg.AddComponent<Arcane::Transform>(leaf, t);
            reg.AddComponent<Arcane::WorldTransform>(leaf, Arcane::WorldTransform{});
            reg.SetParent(leaf, root);
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        }
        float WorldY(Astra::Entity e) { return reg.GetComponent<Arcane::WorldTransform>(e)->matrix[3].y; }
    };

    const Astra::ComponentDescriptor* TransformDescriptor(Astra::Registry& reg, Astra::Entity e)
    {
        for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            if (ci.meta && ci.meta->typeName == "Arcane::Transform")
                return ci.descriptor;
        return nullptr;
    }

    const Astra::FieldInfo* PositionField()
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::Transform>();
        if (!meta) return nullptr;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == "position")
                return &f;
        return nullptr;
    }
}

TEST_CASE("an undo Restore through the descriptor is seen by the next propagation",
          "[scene][change-detection][edit]")
{
    Scene s;
    Arcane::TransformPropagationSystem propagate;
    propagate(s.reg);
    const Astra::ComponentDescriptor* desc = TransformDescriptor(s.reg, s.leaf);
    REQUIRE(desc != nullptr);

    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(s.reg, s.leaf, desc);
    s.reg.GetComponent<Arcane::Transform>(s.leaf)->position.y = 5.0f;   // stamps (non-const)
    std::vector<std::byte> after = Arcane::ComponentEditCommand::Snapshot(s.reg, s.leaf, desc);
    propagate(s.reg);
    REQUIRE(s.WorldY(s.leaf) == Catch::Approx(5.0f));

    Arcane::ComponentEditCommand cmd([&s]() -> Astra::Registry& { return s.reg; }, s.leaf, desc,
                                     before, after, "Edit Transform");
    cmd.Undo();                       // Restore: GetComponentByHash + deserialize + Modified
    propagate(s.reg);
    CHECK(s.WorldY(s.leaf) == Catch::Approx(0.0f));
    cmd.Redo();
    propagate(s.reg);
    CHECK(s.WorldY(s.leaf) == Catch::Approx(5.0f));
}

TEST_CASE("an Inspector descriptor-path edit is seen only once it is marked",
          "[scene][change-detection][editor]")
{
    // InspectorView::ForEachTarget is ImGui-bound and cannot be driven here; this
    // pins the exact mechanism it relies on -- GetComponentByHash hands out the
    // raw instance, the edit lands through a raw float* into a reflected field
    // (the same bytes InspectorFields' per-component writers reach through
    // FieldInfo::GetPtr), then Registry::Modified(e, descriptor->id) -- and
    // proves the mark is load-bearing by showing the write invisible without it.
    Scene s;
    Arcane::TransformPropagationSystem propagate;
    propagate(s.reg);
    const Astra::ComponentDescriptor* desc = TransformDescriptor(s.reg, s.leaf);
    const Astra::FieldInfo* position = PositionField();
    REQUIRE(desc != nullptr);
    REQUIRE(position != nullptr);

    void* instance = s.reg.GetComponentByHash(s.leaf, desc->hash);
    REQUIRE(instance != nullptr);
    // position is a vec3: the Inspector writes one float component at a time
    // through the field's GetPtr; y sits 4 bytes in.
    float* y = reinterpret_cast<float*>(static_cast<std::byte*>(instance) + position->offset + sizeof(float));
    *y = 7.0f;
    propagate(s.reg);
    CHECK(s.WorldY(s.leaf) == Catch::Approx(0.0f));   // unmarked: invisible

    REQUIRE(s.reg.Modified(s.leaf, desc->id));
    propagate(s.reg);
    CHECK(s.WorldY(s.leaf) == Catch::Approx(7.0f));   // marked: seen
}
```

(`Astra::FieldInfo::offset` — confirm the member name in `Astra/Reflection/TypeMeta.hpp`; `InspectorFields.cpp:135` uses `f.offset`.)

- [ ] **Step 2: `GenerateProjects.bat`, build, run** `./ArcaneTests.exe "[change-detection]"` — the undo case **FAILS** (`WorldY` stays 5 after Undo: Restore stamps nothing); the Inspector case passes already (it calls `Modified` itself — it is the mechanism pin).
- [ ] **Step 3: The two marks.** `ComponentEditCommand::Restore` — after `m_descriptor->deserialize(reader, instance);` add:

```cpp
        // Declare the write (spec 2026-09-11 s6.4): GetComponentByHash stamped
        // nothing, so without this an undone Transform is invisible to the
        // Changed<Transform> propagation until something else touches it.
        (void)reg.Modified(m_entity, m_descriptor->id);
```

with `Astra::Registry& reg = m_resolve();` hoisted above the `GetComponentByHash` call (it is currently `m_resolve().GetComponentByHash(...)`). `InspectorView.cpp` `ForEachTarget`, both arms:

```cpp
            // Every target is MARKED after fn (spec 2026-09-11 s6.4): GetComponentByHash
            // stamps nothing, so without this an Inspector edit is invisible to the
            // Edit-mode Changed<Transform> propagation (EditModeSchedule). Deliberate
            // OVER-MARK: this same fan-out also serves the gesture-begin SNAPSHOT
            // pass (BeginGestureIfActivated's SnapshotComponent lambda, and the
            // multi-select seed reads), which is a READ -- so a widget activation
            // that changes nothing still marks its component once. One spurious
            // recompose per click on the selected entities, never a missed edit;
            // a false positive is the safe direction here (TransformSystems.hpp).
            template<typename Fn>
            void ForEachTarget(void* primaryInstance, Fn&& fn)
            {
                if (!registry || selection.empty())
                {
                    fn(entity, primaryInstance);
                    if (registry && descriptor)
                        (void)registry->Modified(entity, descriptor->id);
                    return;
                }
                for (Astra::Entity e : selection)
                    if (void* data = registry->GetComponentByHash(e, descriptor->hash))
                    {
                        fn(e, data);
                        (void)registry->Modified(e, descriptor->id);
                    }
            }
```

- [ ] **Step 4: Build (editor + Arcane.slnx), run `[change-detection]` + `[edit]` — PASS.** Full `~[gpu]` — green. Delta: **+2 cases**. Commit — `feat(editor): Inspector fan-out and undo Restore declare their writes (Registry::Modified by descriptor id)`.

---

### Task 7: The paused physics reconcile gate (spec §6.5)

PASS 3.5's view gains `Astra::Changed<Transform>` + `.Since(res->lastReconcile)`; the exact `appliedScale` compare and the pos/rot divergence test stay inside. The advance goes at the END of `operator()` (after PASS 4), not right after PASS 3.5: PASS 4 writes every body's `Transform` (a non-const yield marks it) at the current tick, so an advance placed before it would make every body read as changed on the next paused pass and the gate would filter nothing. `PreviousTransform`'s PASS 4 stash is untouched (Plan 2).

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/PhysicsSystem.hpp` (`PhysicsResource` `:109-126`; PASS 3.5 `:428-476`; the tail of `operator()` `:545`)
- Test: `ArcaneTests/src/AuthoredTransformSyncTest.cpp` (two new cases; all existing re-pinned unchanged), `PhysicsPauseTest.cpp` (re-pinned unchanged)

**Interfaces:**

```cpp
        // PhysicsResource gains (after entityToBody):
        // The paused reconcile's "since" tick (spec 2026-09-11 s6.5): PASS 3.5
        // visits only bodies whose Transform was written after it. Taken at the
        // END of every pass and the registry tick advanced, so PASS 4's own
        // write-back marks land AT lastReconcile (not newer) and an author edit
        // made between passes lands after it. On the resource, not the system:
        // the registry that owns the ticks owns this too.
        Astra::Tick   lastReconcile = 0;
        // Instrumentation: bodies PASS 3.5 actually visited, cumulative. The gate's
        // whole effect is "an untouched body is not visited"; this is how a test
        // says so.
        std::uint32_t reconciled = 0;
```

- [ ] **Step 1: Write the failing tests** (append to `AuthoredTransformSyncTest.cpp`):

```cpp
// ---- Astra adoption Task 7: the Changed<Transform> gate on PASS 3.5 ---------

TEST_CASE("untouched paused bodies are not visited by the reconcile", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {2,3}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // first pass: since == never, the body is seen once
    auto* res = reg.GetResource<PhysicsResource>();
    REQUIRE(res->reconciled == 1u);

    for (int i = 0; i < 5; ++i) paused(reg);       // nothing written between passes
    CHECK(res->reconciled == 1u);                  // PASS 4's own write-back did not re-trigger it

    reg.GetComponent<Transform>(e)->position = glm::vec3(5.0f, 3.0f, 0.0f);   // an author edit
    paused(reg);
    CHECK(res->reconciled == 2u);
    CHECK(static_cast<float>(res->world->Position(res->entityToBody.at(e)).x) == Approx(5.0f).margin(1e-4f));
}

TEST_CASE("a position-only paused edit does not rebuild fixtures", "[transform-sync]")
{
    // The exact appliedScale compare STAYS inside the gate: a position edit
    // changes Transform (so the gate admits the body) but must not cost a
    // RebuildScaledFixtures.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle bh = res->entityToBody.at(e);
    const Phys::FixtureHandle before = res->world->GetBodyFixture(bh, 0u);

    reg.GetComponent<Transform>(e)->position = glm::vec3(4.0f, 0.0f, 0.0f);
    paused(reg);
    const Phys::FixtureHandle after = res->world->GetBodyFixture(bh, 0u);
    CHECK(before.index == after.index);
    CHECK(before.generation == after.generation);
    CHECK(static_cast<float>(res->world->Position(bh).x) == Approx(4.0f).margin(1e-4f));
}
```

- [ ] **Step 2: Run** `./ArcaneTests.exe "[transform-sync]"` — **FAIL** (compile: no `reconciled`).
- [ ] **Step 3: Implement.** Add the two members to `PhysicsResource` (include `<Astra/Core/Tick.hpp>`). PASS 3.5 becomes:

```cpp
            if (!m_stepWorld)
            {
                // Gated on Changed<Transform> since the last pass (spec 2026-09-11
                // s6.5): chunk-reject over every body, then exactly the bodies whose
                // Transform was written -- Transform is tracked. The exact compares
                // below STAY: a position-only edit changes Transform but must not
                // rebuild every fixture, and only a real divergence teleports.
                auto view = reg.CreateView<PhysicsBodyRef, const Transform, Astra::Changed<Transform>,
                                           const Collider2D, Astra::With<RigidBody2D>>();
                view.Since(res->lastReconcile).ForEach([&](Astra::Entity   /*entity*/,
                                                            PhysicsBodyRef&  ref,
                                                            const Transform& lt,
                                                            const Collider2D& col)
                {
                    if (ref.handle == Phys::kInvalidBody) return;
                    if (!world.IsValid(ref.handle))       return;
                    ++res->reconciled;

                    // ... unchanged: lines 440-474 -- the SCALE block (planarScale
                    //     compare, RebuildScaledFixtures, appliedScale write-back) and
                    //     the POS/ROT block (bp/ba read, the divergence test,
                    //     SetPosition/SetAngle/SetVelocity/SetAngularVelocity) ...
                });
            }
```

and the tail of `operator()`, after PASS 4's closing brace and before the function's closing brace:

```cpp
            // Time base for the paused reconcile (see PhysicsResource::lastReconcile):
            // AFTER PASS 4, so its write-back marks are not newer than this tick.
            // Tick contract note: PhysicsSystem is never AddSystem'd anywhere (map
            // B3) -- tests and (one day) a game module call it bare -- so this
            // advance never runs inside a scheduler group. The day it is scheduled
            // it needs `static constexpr bool RequiresExclusive = true;` for the
            // same reason TransformPropagationSystem carries it (Task 5).
            res->lastReconcile = reg.CurrentTick();
            reg.AdvanceTick();
```

- [ ] **Step 4: Build order** (`PhysicsSystem.hpp` is engine-only, but `PhysicsComponents.hpp` unchanged — Arcane.slnx suffices). Run `[transform-sync]`, `[physics]`, `[interp]` — **PASS**, every pre-existing case unchanged.
- [ ] **Step 5: Full `~[gpu]` — green.** Delta: **+2 cases**. Commit — `feat(physics): paused reconcile gated on Changed<Transform> (exact compares stay inside)`.

---

### Task 8: The editor scheduler + the pending camera-frame request (spec §7, R5)

`EditModeSchedule` — a pure editor unit (no ImGui) owning an `Astra::SystemScheduler` with `TransformPropagationSystem`, and ONE pending frame request. `EditorApp` runs it at phase 9 when `!InPlayMode()` and services the request right after, before the camera push. The three temporaries (`EditorAppFrame.cpp:1364`, `EditorAppScene.cpp:117` inside `FrameSceneIfPending`, `:416` inside `FrameCamera`) are deleted; `FrameCamera` and the scene-open flag become request recorders.

**Files:**
- Create: `ArcaneEditor/src/Scene/EditModeSchedule.hpp`, `ArcaneEditor/src/Scene/EditModeSchedule.cpp`
- Modify: `premake5.lua` (ArcaneTests' explicit editor-TU list: add `EditModeSchedule.cpp` beside `Scene/EditGesture.cpp` with a comment in the house style), `ArcaneEditor/src/App/EditorApp.hpp` (member + include; delete `m_frameOnSceneOpen` and `FrameSceneIfPending`), `EditorAppFrame.cpp` (phase 9; the F/Home calls stay as `FrameCamera(...)`), `EditorAppScene.cpp` (`FrameSceneIfPending` deleted; `FrameCamera` body; the three `m_frameOnSceneOpen = true` sites at `:98,:187,:235`), `EditorApp.cpp` (`:1080` site)
- Test: `ArcaneTests/src/EditModeScheduleTest.cpp` (NEW — `GenerateProjects.bat`)

**Interfaces:**

```cpp
// ArcaneEditor/src/Scene/EditModeSchedule.hpp
#pragma once

// EditModeSchedule (Astra adoption 2026-09-11, spec s7): the editor's OWN
// scheduler for Edit mode, plus the single pending camera-frame request that is
// serviced right after it runs.
//
// Edit mode holds the RunLoop paused, so the game module's fixedUpdate -- which
// owns TransformPropagationSystem -- never runs there, while update/render run
// every frame. Three ad-hoc temporaries used to paper over that (a propagation
// per frame in RefreshSceneResolution, another on scene open, another on the
// F/Home key at input time -- two of them in the same frame as the first). Now
// propagation runs EXACTLY ONCE per Edit-mode frame, through a real scheduler,
// and anything that wants the camera framed records a request that is serviced
// after that pass: bounds computed on fresh WorldTransforms, camera moved, same
// frame, before the scene renders. In Play mode fixedUpdate owns propagation and
// this runs nothing. Both schedulers share TransformOrder (a registry resource)
// and its lastRun; a Play->Stop switch costs one full recompose, which the
// registry restore forces anyway.
//
// ImGui-free on purpose: ArcaneTests compiles this TU directly (root
// premake5.lua) and drives it against a bare registry.

#include "Viewport/EditorCamera.hpp"

#include <Astra/Entity/Entity.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/System/SystemScheduler.hpp>

#include <glm/vec2.hpp>

#include <cstdint>
#include <span>

namespace Arcane::Editor
{
    // What to frame. The request is a SINGLE slot: the last one recorded in a
    // frame wins. SceneOpen differs from Scene only when nothing is framable --
    // a just-opened empty scene centres the origin (the user is about to build
    // there); a Home press on an empty scene leaves the view alone.
    enum class FrameRequest : std::uint8_t
    {
        None = 0,
        Selection,   // F with a selection: SelectionFramingBounds over it
        Scene,       // F with no selection, or Home: SceneFramingBounds
        SceneOpen,   // a scene just became current: SceneFramingBounds, origin fallback
    };

    class EditModeSchedule
    {
    public:
        EditModeSchedule();

        // Phase 9. Runs the Edit-mode systems (today: TransformPropagationSystem)
        // exactly once when !inPlayMode; a no-op in Play. Returns true iff it ran.
        bool RunFrame(Astra::Registry& reg, bool inPlayMode);

        void RequestFrame(FrameRequest request) noexcept { m_pending = request; }
        [[nodiscard]] FrameRequest Pending() const noexcept { return m_pending; }

        // Services and clears the pending request against the registry's
        // WorldTransforms AS THEY ARE NOW (call after RunFrame). A zero-sized
        // viewport keeps a SceneOpen request for a later frame (the panel has not
        // been laid out yet) and drops the other two, exactly as EditorCamera::
        // Frame's own guard used to. Returns true iff the camera moved.
        bool ServicePendingFrame(Astra::Registry& reg, std::span<const Astra::Entity> selection,
                                 EditorCamera& camera, glm::vec2 viewportSize);

    private:
        Astra::SystemScheduler m_schedule;
        FrameRequest           m_pending = FrameRequest::None;
    };
}
```

```cpp
// ArcaneEditor/src/Scene/EditModeSchedule.cpp
#include "Scene/EditModeSchedule.hpp"

#include <Arcane/Scene/TransformSystems.hpp>

#include <tuple>

namespace Arcane::Editor
{
    EditModeSchedule::EditModeSchedule()
    {
        // The one Edit-mode system. AddSystem's Result is [[nodiscard]]; its only
        // failure is a duplicate registration, impossible on a fresh scheduler.
        std::ignore = m_schedule.AddSystem<Arcane::TransformPropagationSystem>();
    }

    bool EditModeSchedule::RunFrame(Astra::Registry& reg, bool inPlayMode)
    {
        if (inPlayMode)
            return false;
        m_schedule.Execute(reg);   // sequential executor; one system, one group
        return true;
    }

    bool EditModeSchedule::ServicePendingFrame(Astra::Registry& reg,
                                               std::span<const Astra::Entity> selection,
                                               EditorCamera& camera, glm::vec2 viewportSize)
    {
        const FrameRequest request = m_pending;
        m_pending = FrameRequest::None;
        if (request == FrameRequest::None)
            return false;
        if (!(viewportSize.x > 0.0f) || !(viewportSize.y > 0.0f))
        {
            if (request == FrameRequest::SceneOpen)
                m_pending = request;   // deferred: the viewport has no size yet
            return false;
        }

        const FramingBounds bounds = (request == FrameRequest::Selection)
            ? SelectionFramingBounds(reg, selection)
            : SceneFramingBounds(reg);
        if (bounds.Valid())
        {
            camera.Frame(bounds.min, bounds.max, viewportSize);
            return true;
        }
        if (request == FrameRequest::SceneOpen)
        {
            camera.offset = viewportSize * 0.5f;   // an empty scene: centre the origin
            return true;
        }
        return false;   // nothing framable: leave the user's view where it is
    }
}
```

- [ ] **Step 1: Write the failing tests** (`EditModeScheduleTest.cpp`):

```cpp
// Astra adoption Task 8 (spec s7): the editor-owned Edit-mode scheduler and its
// single pending camera-frame request, driven headlessly.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>

#include <Scene/EditModeSchedule.hpp>
#include <Viewport/EditorCamera.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <span>
#include <vector>

using Catch::Approx;
using Arcane::Editor::EditModeSchedule;
using Arcane::Editor::FrameRequest;

namespace
{
    struct Scene
    {
        std::shared_ptr<Astra::ComponentRegistry> components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg{components};
        Astra::Entity root{}, sprite{};
        Scene()
        {
            Arcane::RegisterSceneComponents(reg);
            root = reg.CreateEntity();
            reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
            reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
            reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        }
        // A 1x1 m untextured sprite (no SpriteTable -> unit base), centre pivot.
        Astra::Entity AddSprite(glm::vec2 pos)
        {
            Astra::Entity e = reg.CreateEntity();
            Arcane::Transform t; t.position = glm::vec3(pos, 0.0f);
            reg.AddComponent<Arcane::Transform>(e, t);
            reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
            reg.AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
            reg.SetParent(e, root);
            return e;
        }
        std::uint32_t Runs() { return reg.GetResource<Arcane::TransformOrder>()->runs; }
    };
    const glm::vec2 kViewport(800.0f, 600.0f);
}

TEST_CASE("EditModeSchedule runs propagation exactly once per Edit-mode frame and never in Play",
          "[editor][change-detection]")
{
    Scene s;
    s.AddSprite({1.0f, 0.0f});
    EditModeSchedule schedule;

    for (int frame = 0; frame < 5; ++frame)
        CHECK(schedule.RunFrame(s.reg, /*inPlayMode*/ false));
    CHECK(s.Runs() == 5u);

    CHECK_FALSE(schedule.RunFrame(s.reg, /*inPlayMode*/ true));
    CHECK(s.Runs() == 5u);
}

TEST_CASE("a moved entity is framed at its NEW bounds on the next frame service",
          "[editor][change-detection]")
{
    Scene s;
    const Astra::Entity e = s.AddSprite({1.0f, 0.0f});
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    // Frame N: the entity moves (gizmo-style, a stamping write) and the user
    // presses Home in the same frame. The request must see the moved pose.
    s.reg.GetComponent<Arcane::Transform>(e)->position = glm::vec3(50.0f, 20.0f, 0.0f);
    schedule.RequestFrame(FrameRequest::Scene);
    REQUIRE(schedule.RunFrame(s.reg, false));
    REQUIRE(schedule.ServicePendingFrame(s.reg, std::span<const Astra::Entity>{}, cam, kViewport));

    const glm::vec2 centre = cam.WorldToScreen(glm::vec2(50.0f, 20.0f));
    CHECK(centre.x == Approx(kViewport.x * 0.5f));
    CHECK(centre.y == Approx(kViewport.y * 0.5f));
    CHECK(schedule.Pending() == FrameRequest::None);          // consumed
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));   // nothing pending now
}

TEST_CASE("the frame request is a single slot: last wins, and Selection frames the selection",
          "[editor][change-detection]")
{
    Scene s;
    const Astra::Entity a = s.AddSprite({-10.0f, 0.0f});
    const Astra::Entity b = s.AddSprite({30.0f, 0.0f});
    (void)a;
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    schedule.RequestFrame(FrameRequest::Scene);
    schedule.RequestFrame(FrameRequest::Selection);
    CHECK(schedule.Pending() == FrameRequest::Selection);
    const std::vector<Astra::Entity> sel{ b };
    REQUIRE(schedule.ServicePendingFrame(s.reg, sel, cam, kViewport));
    const glm::vec2 centre = cam.WorldToScreen(glm::vec2(30.0f, 0.0f));   // b, not the scene's midpoint
    CHECK(centre.x == Approx(kViewport.x * 0.5f));
}

TEST_CASE("SceneOpen on an empty scene centres the origin; Scene leaves the view alone; zero viewport defers",
          "[editor][change-detection]")
{
    Scene s;   // root only: nothing framable
    EditModeSchedule schedule;
    Arcane::Editor::EditorCamera cam;
    schedule.RunFrame(s.reg, false);

    schedule.RequestFrame(FrameRequest::SceneOpen);
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, glm::vec2(0.0f)));   // not laid out yet
    CHECK(schedule.Pending() == FrameRequest::SceneOpen);                          // kept
    REQUIRE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.offset.x == Approx(kViewport.x * 0.5f));
    CHECK(cam.offset.y == Approx(kViewport.y * 0.5f));

    const glm::vec2 before = cam.offset;
    schedule.RequestFrame(FrameRequest::Scene);
    CHECK_FALSE(schedule.ServicePendingFrame(s.reg, {}, cam, kViewport));
    CHECK(cam.offset == before);
}
```

- [ ] **Step 2: Premake + build — expect FAIL** (no `Scene/EditModeSchedule.hpp`). Add to the ArcaneTests editor-TU list, after `Scene/EditGesture.cpp`:

```lua
        -- Astra adoption Task 8: EditModeSchedule (the Edit-mode scheduler + the
        -- pending camera-frame request) source-compiles into the test exe so the
        -- [editor] units drive it headlessly -- ImGui-free by construction, same
        -- pattern as EditGesture/EditorCamera above.
        "%{wks.location}/ArcaneEditor/src/Scene/EditModeSchedule.cpp",
```

- [ ] **Step 3: Create the two files** verbatim from Interfaces. `GenerateProjects.bat`; build; run `[change-detection]` — **PASS**.
- [ ] **Step 4: Wire `EditorApp`.** `EditorApp.hpp`: `#include "Scene/EditModeSchedule.hpp"`; beside `m_camera` (`:1023`, after `m_runtime` so it destructs first) add `Arcane::Editor::EditModeSchedule m_editSchedule;   // Edit-mode propagation + the pending frame request (spec 2026-09-11 s7)`; delete `void FrameSceneIfPending();` and `bool m_frameOnSceneOpen = false;` (+ their comments); keep `void FrameCamera(bool selectionOnly);` with its comment amended to "records a frame request serviced after this frame's propagation". `EditorAppScene.cpp`: delete `FrameSceneIfPending` (`:102-126`); `FrameCamera` becomes:

```cpp
    void EditorApp::FrameCamera(bool selectionOnly)
    {
        // Recorded, not executed: WorldTransform is DERIVED and Edit mode's
        // propagation runs once per frame in phase 9 (EditModeSchedule). The
        // request is serviced right after that pass, so framing an entity created
        // or moved THIS frame reads its real world pose -- same frame, before the
        // scene renders. No propagation runs at input time any more.
        m_editSchedule.RequestFrame(selectionOnly ? Arcane::Editor::FrameRequest::Selection
                                                  : Arcane::Editor::FrameRequest::Scene);
    }
```

The three `m_frameOnSceneOpen = true;` (`EditorAppScene.cpp:98,:187,:235`) and `EditorApp.cpp:1080` become `m_editSchedule.RequestFrame(Arcane::Editor::FrameRequest::SceneOpen);`. Drop `#include <Arcane/Scene/TransformSystems.hpp>` from `EditorAppScene.cpp`. `EditorAppFrame.cpp` phase 9 (`:1362-1371`) becomes:

```cpp
        if (!InPlayMode())
        {
            // Once per Edit-mode frame, through the editor's own scheduler (spec
            // 2026-09-11 s7) -- Play mode's fixedUpdate owns propagation, so
            // nothing runs twice. Then the ONE pending camera-frame request, on
            // the WorldTransforms this pass just refreshed and before the camera
            // push below, which every render path reads.
            m_editSchedule.RunFrame(m_runtime->Registry(), /*inPlayMode*/ false);
            m_editSchedule.ServicePendingFrame(m_runtime->Registry(), m_selection.Entities(), m_camera,
                                               glm::vec2((float)ViewportWidth(), (float)ViewportHeight()));
        }
```

Rewrite the comment above it (`:1349-1361`) to say the scheduler owns this now. Drop `#include <Arcane/Scene/TransformSystems.hpp>` from `EditorAppFrame.cpp` if nothing else there uses it (grep `TransformPropagationSystem` — expect no hits after this step).
- [ ] **Step 5: Zero-legacy sweep** — `git grep -n "FrameSceneIfPending\|m_frameOnSceneOpen\|TransformPropagationSystem{}" -- ArcaneEditor` returns nothing.
- [ ] **Step 6: Build `Arcane.slnx` Debug; full `~[gpu]` — green.** Then one observable launch: `bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject --frames 120 --screenshot D:\dev\starworks\Arcane\bin\t8-frame.png` (`--screenshot` writes the last rendered frame, `HostConfig.cpp:19`; pairs with `--frames`). Look at the PNG: the reference scene's content sits CENTRED in the viewport panel with a margin on the fitted axis (the SceneOpen request serviced through `EditorCamera::Frame`), not pinned to the panel's top-left corner as the identity camera would leave it. Also confirm the process exited 0 and the log carries no `ASSERT`/`ENSURE` line. Delete the PNG afterwards (never stage it). Delta: **+4 cases** (`[editor][change-detection]`). Commit — `feat(editor): Edit-mode propagation runs once per frame through EditModeSchedule; camera framing is one pending request`.

---

### Task 9: Gacha — restamp `Aphelyon.arcproj` 21 → 26 and rebuild `Aphelyon.dll` (spec §9, R7)

In `D:\dev\starworks\Gacha`. `Game/Binaries/` is gitignored, so the commit is the restamp + one comment; the DLL rebuild is the proof.

**Files:**
- Modify (Gacha): `Game/Aphelyon.arcproj` (`"abi": 21` → `26`), `Game/Source/Aphelyon.cpp` (comment `:80-84`)
- Test: one launch in a v26 host.

- [ ] **Step 1: Restamp.** `"abi": 26`. `Aphelyon.cpp:80-84` comment: "Engine components are registered anonymously by Runtime's constructor; this module owns no component types and registers nothing. (A module that DID own a type would shadow the anonymous entry via its own ComponentModule and the owner stack would restore it automatically on unload.)" → "Engine components are registered by Runtime's constructor through its own Resident Astra::ComponentModule (Arcane.dll never unmaps); this module owns no component types and registers nothing. (A module that DID own a type would shadow the engine's entry via its own ComponentModule and the owner stack would restore it automatically on unload.)"
- [ ] **Step 2: Rebuild against the synced SDK** (`ARCANE_SDK=D:\dev\starworks\Arcane`, set per-invocation if the process env is stale): `cd D:\dev\starworks\Gacha\Game && %ARCANE_SDK%\ThirdParty\premake5\premake5.exe vs2026 && msbuild Aphelyon.slnx /p:Configuration=Debug /m /t:Rebuild` — 0 errors; `Game/Binaries/Aphelyon.dll` timestamp is now. (`/t:Rebuild`: single-slot `Binaries\`, and the header set changed underneath it.)
- [ ] **Step 3: One launch in a v26 host.** `D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Gacha\Game --frames 120` — no `AbiMismatch`, no `engine.abi` stale-stamp WARN (Runtime.cpp's manifest check), boot scene loads.
- [ ] **Step 4: Commit (Gacha)** — `chore(game): restamp Aphelyon.arcproj to engine ABI 26; roster comment follows the Resident ComponentModule` (+ trailer). `git status` shows only the two files.

---

### Task 10: Plan 1 close — sweeps, final counts, the handoff to Plan 2

- [ ] **Step 1: Zero-legacy sweeps** (path-exclude + `-riw`): `git grep -riw "shadowValid\|SamePose" -- ':!docs' ':!ThirdParty' ':!bin' ':!out.txt'` returns nothing. `PreviousTransform`/`LerpPose` are NOT swept — Plan 2 deletes them. `git grep -n "registers it anonymously\|registered anonymously" -- ':!docs'` returns nothing (Runtime.cpp, PluginHost.cpp, ReferenceGame.cpp rewritten; Aphelyon.cpp in Gacha).
- [ ] **Step 2: Build order, both configs, foreground.** `ReferenceProject.slnx` Debug + Release → `Arcane.slnx` Debug + Release (0 warnings / 0 errors each).
- [ ] **Step 3: Suites.** Debug: full UNFILTERED (all pass, `[witness][gpu]` included) then `"~[gpu]"`; Release: `"~[gpu]"`. Derive the final counts and attribute against **56118 / 1620**: T3 +1, T5 +3, T6 +2, T7 +2, T8 +4 = **+12 cases → 1632 expected**; Debug and Release must agree. If the derived number differs, find which task's count was wrong and say so — **derive, never recall.** Ledger both seed banners. Run `scripts/check-baselines.ps1 -Configuration Debug -Invocation "~[gpu]"` on a `-r json` report: a rise, exit 0.
- [ ] **Step 4: Baselines catch-up (controller ruling 2026-09-11).** `scripts/automation-baselines.json` was left stale by F2c Plan 1 at **55294 / 1522** while the suite it guards measured 56118 / 1620 at that arc's close — a rise the script reports but never rewrites, so the guard has run on unearned slack since. Rewrite the Debug and Release `~[gpu]` rows to THIS close's derived counts from Step 3 (both configs must agree; paste each figure from its own run's final line, never from Step 3's expectation). Leave the Dist rows as committed — Dist is not built in this plan — and append one sentence to the file's `note` field saying so. Append to `note` the attribution paragraph in the file's own house style: the F2c Plan 1 rise (+824 assertions / +98 cases over 55294/1522, per that plan's closeout ledger) booked as a backfill, then this plan's +12 cases attributed T3 +1 / T5 +3 / T6 +2 / T7 +2 / T8 +4 with the assertion delta derived from a `-r json` Debug run. Update `measured` with today's date, the source commit, and both seed banners. Re-run `powershell -ExecutionPolicy Bypass -File scripts\check-baselines.ps1 -ReportPath <the -r json Debug report> -Configuration Debug -Invocation "~[gpu]"` — **+0 / +0, exit 0**; same for Release with its own report. Commit — `chore(tests): baselines catch-up -- Astra adoption plan 1 (F2c plan 1 left the file at 55294/1522)`.
- [ ] **Step 5: Astra state.** `cd D:\dev\starworks\Astra && git status --short` shows exactly the pre-existing state Task 1 found and restored: the two modified-tracked files `tests/Serialization/LoadRobustnessTest.cpp` and `bench-compare/RESULTS.md` (someone else's S1 grading WIP, un-stashed after Task 1's commit) plus the untracked `bench-compare/` strays — nothing of this arc's; `git log --oneline -1 dev` == the vendored SHA in `ThirdParty/Astra/VENDORED.txt`.
- [ ] **Step 6: Hand off.** Plan 2 begins at this commit. State in the handoff note: the ABI is **26**; Astra `dev` is at Task 1's commit and vendored; `Transform` is tracked; `TransformOrder` carries `lastRun`/`moved`/`rowOf`/`runs`/`composed`; `PhysicsResource` carries `lastReconcile`/`reconciled`; the Runtime roster list in `Runtime.cpp` still names `PreviousTransform` (Plan 2 removes it); `RenderSystems.hpp:38`'s trait still names `PreviousTransform`; `RenderSubmissionSystem` still lerps by `PreviousTransform`; Gacha is restamped at 26 and rebuilt (Plan 2's 27 restamp is that repo's follow-up); `scripts/automation-baselines.json` now reads this close's Debug/Release counts (Dist rows untouched, noted in the file).
- [ ] **Step 7: Commit** — `docs: Astra adoption plan 1 closeout notes` (only if any doc changed; otherwise no commit, and say so in the handoff).

---

## Self-review record (run at authoring time)

**Spec coverage — every clause to a task:**

| Spec | Task |
|---|---|
| §1 / §2 in-scope (Plan 1 list) | T1 (primitives), T2 (vendor + ABI 26), T3 (residency), T4 (const), T5 (tracked + propagation), T6 (marks), T7 (gate), T8 (scheduler + request), T9 (Gacha) |
| §2 non-goals | Each recorded at the site its trigger names: `WorldTransform` untracked → T5's `AstraChangeTracked` comment; no other tracked type → same comment; `PhysicsSystem` unscheduled / `PhysicsInterpBuffer` unwired → untouched, Plan 2's finding; `IsChanged` consumers beyond tests → T1 tests only; Resident for hosts/test exe → T3 keeps them one-arg |
| §3 R1 | T1, T2 |
| §3 R2 | T3 |
| §3 R3 | **Plan 2** |
| §3 R4 | T2 (26); Plan 2 (27) |
| §3 R5 | T8 |
| §3 R6 | T5 (untracked, comment) |
| §3 R7 | T9 |
| §3 R8 | T5 |
| §4 (primitives, tests, README, spec §3.7, suite, FF, sync) | T1, T2 |
| §5 (Resident, `engineModule`, register order, comments, test) | T3 |
| §6.1 | T4 (every B4 / B10 #3-4 row, transcribed) |
| §6.2 | T5 Step 3 |
| §6.3 steps 1–5 | T5 Step 4 (Rebuild resets `lastRun`; pre-pass; early-out; linear pass with `moved[i]`; end-of-pass advance) — **plus the materialisation probe, stated in T5's preamble as an addition** |
| §6.4 | T6 |
| §6.5 | T7 |
| §6.6 re-pinned + new | T5 (canary, rebuild counts, static-scene early-out, Rebuild recompose, `Modified(id)` seen), T6 (Inspector path, undo), T7 (`PhysicsPauseTest`/`AuthoredTransformSyncTest` re-pinned + gate) |
| §7 (scheduler, pending request, temporaries deleted, tests) | T8 |
| §8 | **Plan 2** |
| §9 ABI/build/Gacha/baseline | T2 (bump + B9 order + unfiltered run), T9, T10 |
| §10 hazards | rebuilt row order → T5 Rebuild `lastRun = 0`; same-tick write → T5 step 5 / T7 tail advance; Inspector/undo bypass → T6; position-only rebuild → T7 test; last Runtime erasing metas → T3 test; stale `ReferenceGame.dll` → T2 Steps 4-5 + Global Constraints; Play↔Edit double drive → T8 `inPlayMode` gate; `Changed<WorldTransform>` → non-goal comment in T5 |

**Type consistency across tasks:** `Registry::Modified(Entity, ComponentID)` is named in T1 and consumed in T5's test and T6 (both sites pass `descriptor->id`). `TransformOrder::{lastRun, moved, rowOf, runs, composed}` are named in T5 and read by T8's test (`runs`) and T5's tests (`composed`). `PhysicsResource::{lastReconcile, reconciled}` are named and consumed in T7 only. `FrameRequest`/`EditModeSchedule` are named in T8 and consumed by `EditorApp` in the same task; the spec's `m_pendingFrame{selectionOnly}` is spelled as the three-valued `FrameRequest` because the existing scene-open path carries an origin-centring fallback the bool cannot express. The Runtime roster list in T3 names `PreviousTransform` on purpose (id order preserved); Plan 2 removes it.

**Contradictions found in the code, resolved visibly (the user may veto either):**
1. **Spec §6.3 early-out vs `TransformOrderTest.cpp:865`** ("a WorldTransform removed behind the system's back is healed"): removing a `WorldTransform` does not mark `Transform`, so the spec's early-out skips the heal forever. T5 adds the `Not<WorldTransform>` materialisation probe (empty archetypes in steady state) and keeps the early-out. Alternative if vetoed: delete that test case and its contract.
2. **Spec §6.5 "advanced as in 6.3 step 5"** read literally (right after PASS 3.5) defeats the gate, because PASS 4 marks every body's `Transform` at the same tick. T7 advances at the END of `operator()`.

**Known intentional gaps, each with its owner:**
- `InspectorView::ForEachTarget`'s mark is not driven by a test (ImGui TU); T6's second case pins the mechanism with the Inspector's pure writer and the desk pass covers the site.
- `scripts/automation-baselines.json`'s Dist rows stay at their committed values (Dist is not built in this plan); T10 Step 4 rewrites only Debug/Release and says so in the file's `note`.
- `EntityOps.cpp:189` (`GetComponent<Hidden>`, a tag) is left non-const: tags never stamp.
- Test-file non-const views (`TransformPropagationTest.cpp:119,180`, `SceneJsonTest.cpp`, …) are left as written: harmless stamps, and the `Mut<T>` conversion keeps them compiling — map B4 "optional".
- `RenderSystems.hpp:38` keeps `PreviousTransform` in its trait and `:77` its lerp until Plan 2.

## Closeout (2026-09-11)

Plan 1 is closed at this document's HEAD range **`c1a29ad3..f8701cda`** (Task 10's baselines-catch-up commit; the closeout-notes commit for this section follows immediately after). Plan 2 begins from that HEAD.

**State handed off:**
- Engine ABI is **26**.
- Astra `dev` is vendored at **`a08bb04`** (`ThirdParty/Astra/VENDORED.txt` agrees) — the commit Task 1 produced on top of the branch-state drift below.
- `Transform` is Astra-change-tracked.
- `TransformOrder` carries `lastRun` / `moved` / `rowOf` / `runs` / `composed`.
- `PhysicsResource` carries `lastReconcile` / `reconciled`.
- The Runtime roster list in `Runtime.cpp` still names `PreviousTransform` (kept on purpose, id order preserved) — **Plan 2 removes it.**
- `RenderSystems.hpp:38`'s trait still names `PreviousTransform`, and `RenderSubmissionSystem` (`:77`) still lerps by it — both stay until Plan 2.
- Gacha (`D:\dev\starworks\Gacha`) is restamped to ABI 26 at **`83aa9813`** and rebuilt (Plan 2's ABI-27 restamp is that repo's own follow-up).
- `scripts/automation-baselines.json` now reads this close's Debug/Release counts; the Dist rows are untouched and carried forward at their asset-manager Plan 3 committed values (55226/1516), with the file's `note` saying so.

**Final derived counts (Step 3, both configs agreeing):**
- Debug unfiltered (`ArcaneTests.exe`, all tests incl. `[witness][gpu]`): **118491 assertions / 1665 test cases**, all passing, seed 4189552635 (`Randomness seeded to:`). This run is what proves the staged `ReferenceProject/Binaries/ReferenceGame.dll` is current (`arccook: cooked=0 upToDate=1 failed=0` on both the Debug and Release `Arcane.slnx` builds).
- Debug `~[gpu]`: **56217 assertions / 1632 test cases**, all passing, seed 1660673166.
- Release `~[gpu]`: **56217 assertions / 1632 test cases**, all passing, seed 2716699730 — matches Debug exactly.
- Attribution against the 56118/1620 F2c Plan 1 figure: +99 assertions / +12 cases, T3 +1 / T5 +3 / T6 +2 / T7 +2 / T8 +4 cases, the assertion delta accounted for in full by the 12 new cases' own assertions (7+51+10+7+24=99) with no change to any pre-existing case — see `scripts/automation-baselines.json`'s note for the full per-case table.
- `check-baselines.ps1` against the rewritten file: Debug and Release both **+0 / +0, exit 0**.

**Astra branch-state drift, controller-ruled:** the plan's Global Constraints anchored Astra at `feat/change-detection` @ `b664aa8` (17 commits ahead of `dev` @ `f3e311d`); by the time Task 1 ran, the user had committed the S1 load-robustness fix on top, landing the branch at **`dad6b6a`** (18 commits ahead) — a strict superset, so Task 1 proceeded from `dad6b6a` (ruling recorded in `.superpowers/sdd/2026-09-11-astra-adoption-plan1-residency-change-detection/progress.md`), which is why Task 10 Step 5 found only one modified-tracked file (`bench-compare/RESULTS.md`) instead of the plan's originally-expected two.
