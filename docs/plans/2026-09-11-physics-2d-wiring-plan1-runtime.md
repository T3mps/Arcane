# 2D Physics Wiring — Plan 1: engine-owned runtime, editor integration, overlay, data

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `RigidBody2D` + `Collider2D` authored in an `.arcscene` falls under Play in both hosts, sits still in Edit mode with its collider outlined, and returns to its authored pose on Stop — with no game module naming physics at all.

**Architecture:** Astra's `FieldInfo` grows vector-element access (the one upstream primitive), the JSON bridge grows a container branch on it, and `Runtime` grows a Manifold2D-free facade (`InstallEngineSystems` / `EnsurePhysics` / `PhysicsEditPass`) that owns the world and the `fixedUpdate` slot; the editor calls the edit pass before propagation and draws `DrawPhysicsDebug` after the scene submit. `PhysicsSystem` becomes schedulable (`RequiresExclusive`, `Before<TransformPropagationSystem>`, paused-pass gates). Settings: `.arcproj` `"physics"` block, overridden by a `PhysicsSettings` component on the scene root. One demonstration scene + one witness.

**Tech Stack:** C++23, Astra (gtest suite, its own repo), Manifold2D (vendored), Catch2, msbuild `Astra.sln` / `Arcane.slnx` / `ReferenceProject.slnx` / `Aphelyon.slnx`, nlohmann::json, ImGui.

**Spec:** `docs/specs/2026-09-11-physics-2d-wiring-design.md` (§4–§9). The Inspector `FieldKind::Vector` editor (spec §7.3) is **Plan 2**, not here.

## Global Constraints

- **Repos:** Arcane `D:\dev\starworks\Arcane` on `main` at **9eabce7c** (the spec commit; ABI **27**). Astra `D:\dev\starworks\Astra` on `dev` at **a08bb04** (its tree carries ` M bench-compare/RESULTS.md` + untracked `bench-compare/*` strays — the user's; **never stage them**; stash `RESULTS.md` by path only if a checkout demands it). Gacha `D:\dev\starworks\Gacha` on `main` at **eba12d36** (arcproj 27). Manifold2D is vendored (`ThirdParty/Manifold2D/VENDORED.txt`) and **not edited** by this plan.
- **Arcane strays, never staged:** `out.txt` (repo root), `ArcaneAssetPipeline/ArcaneAs.25D4CEF5/`, `ArcaneEditor/ArcaneEditor/`, anything under `bin/`, generated `.vcxproj`/`.slnx`, any PNG/JSON a task writes for evidence.
- **Commit per task, do NOT push.** Trailer on every commit in every repo, exactly:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01Djskfodj1WnyR2g5v9V1Qc
  ```
- **The ABI bumps ONCE, in Task 2 (27 → 28)**, with the v28 ledger entry written in full there (the v26 precedent: later tasks' header changes are named in advance). `ReferenceProject.arcproj` → 28 in Task 2; `Game/Aphelyon.arcproj` → 28 in Task 10.
- **Build order after ANY Astra header change (Task 2) or any change to a header a game module compiles** (`Components.hpp`, `SceneModule.hpp`, `SceneResources.hpp`, `RenderSystems.hpp`, `TransformSystems.hpp`, `PluginABI.hpp`, `build/arcane.lua`): `GenerateProjects.bat` → **`ReferenceProject.slnx` FIRST for every configuration the task targets** (`cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=<cfg> /m /t:Rebuild` — `/t:Rebuild` always: `Binaries\` is a single slot and a config flip silently no-ops) → `msbuild Arcane.slnx /p:Configuration=<cfg> /m`. Debug is built and its suites run BEFORE Release in any task that does both (the Debug unfiltered suite's `[witness][gpu]` launches a host against whatever DLL is staged). `PhysicsSystem.hpp` / `PhysicsComponents.hpp` are **not** on the game-module surface (`build/arcane.lua` has no Manifold2D row) — a change there still rebuilds `Arcane.slnx` (editor + tests compile them) but needs no ReferenceProject rebuild; do the ReferenceProject rebuild anyway when in doubt, it is cheap.
- **msbuild:** `"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"` (VS 18). Every summary line must read `0 Warning(s)` / `0 Error(s)` — record them. `GenerateProjects.bat` must run before the first msbuild of a task (a stale shader-prebuild path otherwise fails the build).
- **Astra:** `cd D:\dev\starworks\Astra && premake5 vs2022 && msbuild Astra.sln /p:Configuration=Debug /m` then `bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1` (`premake5` is on PATH from `D:\dev\_shared\tools`; never run `scripts\generate_vs2022.bat`, it ends in `pause`). A new test TU needs the `premake5 vs2022` regenerate.
- **Run tests FROM the exe dir, in the FOREGROUND**, 10-minute (600000 ms) tool timeout, never backgrounded: `cd D:\dev\starworks\Arcane\bin\<cfg>-windows-x86_64-md\ArcaneTests && .\ArcaneTests.exe "<filter>"`. Capture the `Randomness seeded to:` banner and the totals line of every run. A NEW test TU needs `GenerateProjects.bat` (ArcaneTests globs `src/**.cpp` statically); an editor `.cpp` a test drives must be in root `premake5.lua`'s explicit ArcaneTests list (`EditModeSchedule.cpp` and `PlayMode.cpp` already are; `EditorPanels.cpp` deliberately is not).
- **Baseline at plan start: 56216 assertions / 1632 cases** (`~[gpu]`, Debug and Release, Astra adoption Plan 2 close). Per-task deltas attributed to named cases. **Derive counts from the run, never recall them.**
- **Every task ends green** (Debug `~[gpu]`); Task 2, Task 9 and Task 11 also run the Debug UNFILTERED suite (only it runs `[witness][gpu]`) and Release `~[gpu]`.
- **Anchors drift** — every `file:line` is orientation against 9eabce7c; **re-locate by SYMBOL before editing.**
- **No render change ⇒ the four golden lanes are untouched, no re-bless.** The demonstration scene is NOT the boot scene. `scripts/golden-gate.ps1 -Configuration Release` at the close must report 4/4 lanes `diffCount=0`.
- **Absolute `--project` on every host launch.** `+Y is DOWN` in world space (screen-space world; Manifold2D's Box2D-v3 y-down default).
- **Catch2 assertion line numbers in Debug are skewed** (`/ZI`); judge failures by assertion text.

---

### Task 1: Astra — `FieldInfo` vector element access (spec §7.1)

**Files:**
- Modify (Astra repo): `include/Astra/Reflection/FieldInfo.hpp` (struct members after `setterAny`; `MakeFieldInfo` after the `isVector` derivation)
- Test (Astra repo, NEW TU): `tests/Reflection/FieldInfoVectorTest.cpp`

**Interfaces:**
- Produces (consumed by Tasks 2, 3): on every `FieldInfo` whose `isVector` is true and whose element type is default-constructible and not `bool`:
  ```cpp
  uint64_t elementTypeHash;   // TypeID<ContainerTraits<T>::ValueType>::Hash(); 0 otherwise
  size_t   elementSize;       // sizeof(element); 0 otherwise
  std::function<size_t(const void* instance)>      vectorSize;
  std::function<void(void* instance, size_t n)>    vectorResize;   // default-constructs growth
  std::function<void*(void* instance, size_t i)>   vectorElement;  // nullptr when i >= size
  std::function<void(void* instance, size_t i)>    vectorErase;    // i >= size: no-op
  std::function<void(void* instance, size_t i)>    vectorInsert;   // default element before i; i >= size appends
  ```
  Every accessor takes the **containing** instance (the `getter`/`setter` convention), never the vector itself. Unset (`!vectorSize`) on non-vector fields.

- [ ] **Step 1: Write the failing tests** — create `tests/Reflection/FieldInfoVectorTest.cpp`:

```cpp
// FieldInfo's std::vector element access (2026-09-11, for Arcane's reflection
// -> JSON container branch and Inspector list editor): a consumer holding only
// a FieldInfo and the CONTAINING instance can size, grow, address, erase and
// insert elements type-erased, and resolve the element's own TypeMeta.
#include <gtest/gtest.h>

#include <Astra/Reflection/Reflection.hpp>

#include <string_view>
#include <vector>

namespace
{
    struct Slot { int id = 0; float weight = 1.0f; };
    struct Bag
    {
        int               tag = 0;
        std::vector<Slot> slots;
        std::vector<int>  counts;
    };
}

ASTRA_REFLECT_TYPE(Slot)
    ASTRA_REFLECT_FIELD(Slot, id)
    ASTRA_REFLECT_FIELD(Slot, weight)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(Bag)
    ASTRA_REFLECT_FIELD(Bag, tag)
    ASTRA_REFLECT_FIELD(Bag, slots)
    ASTRA_REFLECT_FIELD(Bag, counts)
ASTRA_END_REFLECT_TYPE()

namespace
{
    const Astra::FieldInfo* Field(std::string_view name)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<Bag>();
        if (!meta) return nullptr;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.name == name) return &f;
        return nullptr;
    }
}

TEST(FieldInfoVector, ElementMetadataIsDerivedForVectorFieldsOnly)
{
    const Astra::FieldInfo* slots = Field("slots");
    ASSERT_NE(slots, nullptr);
    ASSERT_TRUE(slots->isVector);
    EXPECT_EQ(slots->elementTypeHash, Astra::TypeID<Slot>::Hash());
    EXPECT_EQ(slots->elementSize, sizeof(Slot));
    EXPECT_NE(Astra::GetMeta(slots->elementTypeHash), nullptr);   // the element's own TypeMeta resolves
    EXPECT_TRUE(static_cast<bool>(slots->vectorSize));
    EXPECT_TRUE(static_cast<bool>(slots->vectorResize));
    EXPECT_TRUE(static_cast<bool>(slots->vectorElement));
    EXPECT_TRUE(static_cast<bool>(slots->vectorErase));
    EXPECT_TRUE(static_cast<bool>(slots->vectorInsert));

    const Astra::FieldInfo* tag = Field("tag");
    ASSERT_NE(tag, nullptr);
    EXPECT_FALSE(tag->isVector);
    EXPECT_EQ(tag->elementTypeHash, 0u);
    EXPECT_EQ(tag->elementSize, 0u);
    EXPECT_FALSE(static_cast<bool>(tag->vectorSize));
}

TEST(FieldInfoVector, AccessorsOperateOnTheContainingInstance)
{
    const Astra::FieldInfo* slots = Field("slots");
    ASSERT_NE(slots, nullptr);
    Bag bag;
    EXPECT_EQ(slots->vectorSize(&bag), 0u);

    slots->vectorResize(&bag, 2);                       // growth default-constructs
    ASSERT_EQ(bag.slots.size(), 2u);
    EXPECT_EQ(bag.slots[1].id, 0);

    static_cast<Slot*>(slots->vectorElement(&bag, 1))->id = 7;
    EXPECT_EQ(bag.slots[1].id, 7);
    EXPECT_EQ(slots->vectorElement(&bag, 2), nullptr);  // past the end

    slots->vectorInsert(&bag, 0);                       // default element BEFORE index 0
    ASSERT_EQ(bag.slots.size(), 3u);
    EXPECT_EQ(bag.slots[0].id, 0);
    EXPECT_EQ(bag.slots[2].id, 7);

    slots->vectorInsert(&bag, 99);                      // i >= size appends
    EXPECT_EQ(bag.slots.size(), 4u);

    slots->vectorErase(&bag, 0);
    ASSERT_EQ(bag.slots.size(), 3u);
    EXPECT_EQ(bag.slots[1].id, 7);

    slots->vectorErase(&bag, 99);                       // out of range: no-op
    EXPECT_EQ(bag.slots.size(), 3u);

    slots->vectorResize(&bag, 0);
    EXPECT_EQ(slots->vectorSize(&bag), 0u);
}

TEST(FieldInfoVector, ScalarElementsGetTheSameAccessors)
{
    const Astra::FieldInfo* counts = Field("counts");
    ASSERT_NE(counts, nullptr);
    EXPECT_EQ(counts->elementTypeHash, Astra::TypeID<int>::Hash());
    EXPECT_EQ(counts->elementSize, sizeof(int));
    Bag bag;
    counts->vectorResize(&bag, 1);
    *static_cast<int*>(counts->vectorElement(&bag, 0)) = 42;
    EXPECT_EQ(bag.counts[0], 42);
}
```

- [ ] **Step 2: Regenerate + build — expect FAIL** (compile: `FieldInfo` has no `elementTypeHash`). `cd D:\dev\starworks\Astra && premake5 vs2022 && msbuild Astra.sln /p:Configuration=Debug /m`.
- [ ] **Step 3: `FieldInfo.hpp`.** In `struct FieldInfo`, directly after the `setterAny` member and before `attributes`, add:

```cpp
        // ---- std::vector element access (2026-09-11) --------------------------
        // Populated ONLY when isVector (and the element type is default-
        // constructible and not bool -- vector<bool> has no addressable
        // elements). Every accessor takes the CONTAINING instance, the same
        // convention as getter/setter above, never the vector itself.
        // elementTypeHash is TypeID<ValueType>::Hash(), so a consumer resolves
        // the element's own TypeMeta with GetMeta() exactly as it does for a
        // nested struct field. Growth default-constructs; vectorElement is
        // nullptr past the end; erase past the end is a no-op; insert past the
        // end appends. None of these throw.
        uint64_t elementTypeHash = 0;
        size_t   elementSize     = 0;
        std::function<size_t(const void* instance)>    vectorSize;
        std::function<void(void* instance, size_t n)>  vectorResize;
        std::function<void*(void* instance, size_t i)> vectorElement;
        std::function<void(void* instance, size_t i)>  vectorErase;
        std::function<void(void* instance, size_t i)>  vectorInsert;
```

In `Detail::MakeFieldInfo`, directly after the `info.isVector = ...;` statement, add:

```cpp
            // Element access for std::vector fields (the same fingerprint that
            // set isVector above), computed at compile time per field.
            if constexpr (ContainerTraits<DecayedType>::IsSequence
                          && ContainerTraits<DecayedType>::HasContiguousStorage
                          && !ContainerTraits<DecayedType>::HasFixedSize
                          && !ContainerIsStringTrait<DecayedType>::value
                          && !std::is_const_v<FieldType>)
            {
                using Element = typename ContainerTraits<DecayedType>::ValueType;
                if constexpr (std::is_default_constructible_v<Element> && !std::is_same_v<Element, bool>)
                {
                    info.elementTypeHash = TypeID<Element>::Hash();
                    info.elementSize     = sizeof(Element);
                    info.vectorSize = [](const void* instance) -> size_t {
                        return (static_cast<const Class*>(instance)->*FieldPtr).size();
                    };
                    info.vectorResize = [](void* instance, size_t n) {
                        (static_cast<Class*>(instance)->*FieldPtr).resize(n);
                    };
                    info.vectorElement = [](void* instance, size_t i) -> void* {
                        auto& v = static_cast<Class*>(instance)->*FieldPtr;
                        return i < v.size() ? static_cast<void*>(&v[i]) : nullptr;
                    };
                    info.vectorErase = [](void* instance, size_t i) {
                        auto& v = static_cast<Class*>(instance)->*FieldPtr;
                        if (i < v.size()) v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
                    };
                    info.vectorInsert = [](void* instance, size_t i) {
                        auto& v = static_cast<Class*>(instance)->*FieldPtr;
                        if (i >= v.size()) v.emplace_back();
                        else               v.insert(v.begin() + static_cast<std::ptrdiff_t>(i), Element{});
                    };
                }
            }
```

(`<cstddef>` for `std::ptrdiff_t` and `<type_traits>` are already included by this header; verify, add if not.)

- [ ] **Step 4: Build + run — expect PASS.** `msbuild Astra.sln /p:Configuration=Debug /m && bin\Debug-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1 --gtest_filter=FieldInfoVector.*` then the whole suite (all pass). Then Release: `msbuild Astra.sln /p:Configuration=Release /m && bin\Release-windows-x86_64\AstraTest\AstraTest.exe --gtest_brief=1`.
- [ ] **Step 5: Commit (Astra, on `dev`)** — `git add include/Astra/Reflection/FieldInfo.hpp tests/Reflection/FieldInfoVectorTest.cpp` (NOTHING under `bench-compare/`) — `feat(reflection): FieldInfo std::vector element access -- elementTypeHash + size/resize/element/erase/insert accessors` + trailer. Record the SHA for Task 2.

---

### Task 2: Vendor + ABI 28 (spec §9)

**Files:**
- Modify (Arcane): `ThirdParty/Astra/include/**` + `ThirdParty/Astra/VENDORED.txt` (by `scripts/sync-astra.ps1`), `ArcaneClient/src/Arcane/Plugin/PluginABI.hpp` (v28 ledger + constant, directly above `kGamePluginABIVersion`, after the v27 entry), `ReferenceProject/ReferenceProject.arcproj` (`"abi": 27` → `28`)
- Test: `ArcaneTests/src/VendorSmokeTest.cpp` (one new `[vendor][astra]` case)

**Interfaces:** Consumes Task 1's accessors. Produces: the vendored `FieldInfo` every later task compiles against; ABI 28.

- [ ] **Step 1: Sync.** `cd D:\dev\starworks\Arcane && powershell -ExecutionPolicy Bypass -File scripts\sync-astra.ps1 -DryRun` (review — expect `FieldInfo.hpp` changed; ~63 false CRLF "modified" are the known fan-out, verify with `git diff --stat --ignore-cr-at-eol` that only `FieldInfo.hpp` + `VENDORED.txt` carry content), then without `-DryRun`. `VENDORED.txt` must record `branch : dev` and Task 1's SHA. Then `GenerateProjects.bat`.
- [ ] **Step 2: The smoke case** — append to `ArcaneTests/src/VendorSmokeTest.cpp` (it already includes `<Astra/Reflection/Reflection.hpp>`? check; add if not):

```cpp
namespace
{
    struct SmokeSlot { int id = 0; };
    struct SmokeBag  { std::vector<SmokeSlot> slots; };
}
ASTRA_REFLECT_TYPE(SmokeSlot)
    ASTRA_REFLECT_FIELD(SmokeSlot, id)
ASTRA_END_REFLECT_TYPE()
ASTRA_REFLECT_TYPE(SmokeBag)
    ASTRA_REFLECT_FIELD(SmokeBag, slots)
ASTRA_END_REFLECT_TYPE()

TEST_CASE("Astra: FieldInfo carries std::vector element access (2026-09-11 vendor)", "[vendor][astra]")
{
    // The primitive the reflection->JSON container branch and the Inspector
    // list editor stand on. Pinned here so a future re-vendor that drops it
    // fails HERE, by name, not deep inside SceneJsonTest.
    const Astra::TypeMeta* meta = Astra::GetMeta<SmokeBag>();
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 1);
    const Astra::FieldInfo& f = meta->fields[0];
    REQUIRE(f.isVector);
    CHECK(f.elementTypeHash == Astra::TypeID<SmokeSlot>::Hash());
    CHECK(f.elementSize == sizeof(SmokeSlot));
    SmokeBag bag;
    REQUIRE(static_cast<bool>(f.vectorResize));
    f.vectorResize(&bag, 3);
    CHECK(f.vectorSize(&bag) == 3);
    static_cast<SmokeSlot*>(f.vectorElement(&bag, 2))->id = 9;
    CHECK(bag.slots[2].id == 9);
}
```

- [ ] **Step 3: The v28 ledger entry**, appended directly above `kGamePluginABIVersion` (after the v27 entry, which ends "...the grep is what proves it safe to defer, not evidence it was done."). Replace `<sha>` with Task 1's SHA. Delete the `= 27;` line.

```cpp
    // v28 (2026-09-11, 2D physics wiring Plan 1): Astra re-vendored at <sha>
    //     (dev, one commit over a08bb04): FieldInfo grew std::vector element
    //     access -- elementTypeHash, elementSize and five std::function
    //     accessors (vectorSize / vectorResize / vectorElement / vectorErase /
    //     vectorInsert) -- so sizeof(FieldInfo) and every reflect block's
    //     static-init shape moved. SAME FAILURE CLASS AS v10/v24/v26: plugins
    //     compile Astra's reflect macros THEMSELVES (Components.hpp's blocks
    //     are instantiated inside ReferenceGame.dll and Aphelyon.dll), so a v27
    //     plugin would hand the host FieldInfo records laid out for the old
    //     struct. Reject the pairing.
    //     FOUR ARCANE FACTS RIDE ALONG, all from spec docs/specs/2026-09-11-
    //     physics-2d-wiring-design.md and landing across this plan's tasks
    //     (this entry is written at the vendor task, as v26's was):
    //     (1) a new engine roster component, Arcane::PhysicsSettings
    //     {glm::vec2 gravity} (Components.hpp), APPENDED after MeshRenderer in
    //     RegisterSceneComponents and in Runtime's Resident roster, so no id
    //     before it shifts; (2) Collider2D and RigidBody2D declare
    //     AstraChangeTracked = true (static members -- no byte change; recorded
    //     per the v26 precedent for Transform); (3) PhysicsSystem is
    //     schedulable -- RequiresExclusive, Astra::Before<TransformPropagation
    //     System>, PASS 4 gated on stepWorld, paused-pass re-mint criteria in
    //     PASS 1 -- and Runtime installs it into fixedUpdate ITSELF: no game
    //     module names it, and PhysicsSystem.hpp / PhysicsComponents.hpp are
    //     NOT on the game-module include surface (build/arcane.lua carries no
    //     Manifold2D row); (4) the reflection->JSON bridge gained a container
    //     branch and Collider2D::fixtures lost Serializable(false) (scene
    //     schema v5). The game-module include surface (build/arcane.lua) is
    //     UNCHANGED.
    //     MEASURED, not assumed: `grep -rn -E "FieldInfo|isVector|
    //     PhysicsSettings|PhysicsSystem|PhysicsResource|Collider2D|RigidBody2D|
    //     PhysicsBodyRef|EnsurePhysics|InstallEngineSystems|PhysicsEditPass"`
    //     over BOTH game modules -- ReferenceProject/Source/ and Gacha's
    //     Game/Source/ -- returns NOTHING in either tree:
    //
    //       $ grep -rn -E "FieldInfo|isVector|PhysicsSettings|PhysicsSystem|PhysicsResource|Collider2D|RigidBody2D|PhysicsBodyRef|EnsurePhysics|InstallEngineSystems|PhysicsEditPass" ReferenceProject/Source/
    //       (no output)
    //       $ grep -rn -E "<same pattern>" D:/dev/starworks/Gacha/Game/Source/
    //       (no output)
    //
    //     so neither breaks at compile time; the gate, not the compiler,
    //     refuses the stale DLL. ReferenceProject.arcproj restamped with this
    //     change. Gacha's Game restamp (27 -> 28) is this plan's Task 10, in
    //     that repo, together with the Aphelyon.dll rebuild -- not deferred.
    inline constexpr uint32_t kGamePluginABIVersion = 28;
```

Run both greps before committing; both must be empty (they are at plan authoring). `ReferenceProject.arcproj` → `"abi": 28`.
- [ ] **Step 4: Build order, Debug:** `cd ReferenceProject && ..\ThirdParty\premake5\premake5.exe vs2026 && msbuild ReferenceProject.slnx /p:Configuration=Debug /m /t:Rebuild` → `cd .. && msbuild Arcane.slnx /p:Configuration=Debug /m`. Full UNFILTERED suite — all pass, `[witness][gpu]` included (the staged DLL is v28 and loads). Then `"~[gpu]"` — **+1 case** (the smoke case).
- [ ] **Step 5: Release, same order** (`/t:Rebuild` on ReferenceProject — a config flip); `"~[gpu]"` — same counts as Debug.
- [ ] **Step 6: Commit** — `git add ThirdParty/Astra ArcaneClient/src/Arcane/Plugin/PluginABI.hpp ReferenceProject/ReferenceProject.arcproj ArcaneTests/src/VendorSmokeTest.cpp` — `chore(deps)!: vendor Astra <sha7> (FieldInfo vector element access); ABI 28; ReferenceProject restamped` + trailer.

---

### Task 3: The JSON container branch; `Collider2D::fixtures` serialises; scene schema v5 (spec §7.2)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Serialization/ReflectionJson.hpp` (`Detail::IsHandledType` ~:293; `ReflectionJsonWriter::Visit` ~:358 + new `WriteVector`; `ReflectionJsonReader::Visit` ~:462 + new `ReadVector`), `ArcaneClient/src/Arcane/Scene/PhysicsComponents.hpp` (the `Collider2D` reflect block ~:218-253: attribute off, comment rewritten), `ArcaneClient/src/Arcane/Serialization/SceneSerializer.hpp` (`kSceneJsonVersion` 4 → 5 + its comment ~:70-77)
- Test: `ArcaneTests/src/ReflectionJsonTest.cpp` (one new case + a comment on the existing `HasVector` case), `ArcaneTests/src/SceneJsonTest.cpp` (one new case), `ArcaneTests/src/SceneAssetTest.cpp` (one new case beside "a v3 scene still loads after the v4 bump")

**Interfaces:**
- Consumes: Task 1's `FieldInfo::isVector / elementTypeHash / vectorSize / vectorResize / vectorElement`.
- Produces: a `std::vector<T>` field where `T` is a **reflected struct** (has `TypeMeta`, is not an enum) writes as a JSON array of objects and reads back in place. Vectors of scalars / glm / enums stay "unsupported field type" (fail loud) — no roster field has one; recorded as a follow-up in the spec. `Collider2D` writes `"fixtures": [ {...}, ... ]`.

- [ ] **Step 1: Write the failing tests.** `ReflectionJsonTest.cpp` — add after the existing `Versioned` reflect blocks:

```cpp
namespace
{
    // A vector of REFLECTED STRUCTS -- the shape Collider2D::fixtures and
    // MeshAssetData::slots have. Vectors of scalars stay unsupported (the
    // HasVector case below), which is deliberate: no roster field needs them.
    struct Slot { int id = 0; float weight = 1.0f; glm::vec2 offset{0.0f, 0.0f}; };
    struct Bag  { int tag = 0; std::vector<Slot> slots; };
}
ASTRA_REFLECT_TYPE(Slot)
    ASTRA_REFLECT_FIELD(Slot, id)
    ASTRA_REFLECT_FIELD(Slot, weight)
    ASTRA_REFLECT_FIELD(Slot, offset)
ASTRA_END_REFLECT_TYPE()
ASTRA_REFLECT_TYPE(Bag)
    ASTRA_REFLECT_FIELD(Bag, tag)
    ASTRA_REFLECT_FIELD(Bag, slots)
ASTRA_END_REFLECT_TYPE()

TEST_CASE("a vector of reflected structs round-trips as a JSON array", "[json][reflection]")
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Bag>();
    REQUIRE(meta != nullptr);

    Bag a; a.tag = 3;
    a.slots = { Slot{ 1, 0.5f,  glm::vec2(1.0f, 2.0f) },
                Slot{ 2, 0.25f, glm::vec2(3.0f, 4.0f) } };
    nlohmann::json j;
    Arcane::ReflectionJsonWriter writer(j);
    VisitMetaFields(*meta, &a, writer);
    REQUIRE_FALSE(writer.HasError());
    REQUIRE(j["slots"].is_array());
    REQUIRE(j["slots"].size() == 2);
    CHECK(j["slots"][1]["id"].get<int>() == 2);
    CHECK(j["slots"][0]["offset"][1].get<float>() == Approx(2.0f));

    SECTION("read REPLACES the live vector -- it never appends to what was there")
    {
        Bag out; out.slots.resize(5);
        Arcane::ReflectionJsonReader reader(j);
        VisitMetaFields(*meta, &out, reader);
        REQUIRE_FALSE(reader.HasError());
        REQUIRE(out.slots.size() == 2);
        CHECK(out.slots[1].weight == Approx(0.25f));
        CHECK(out.slots[0].offset.y == Approx(2.0f));
        CHECK(out.tag == 3);
    }
    SECTION("an absent key keeps the default (forward/back compat)")
    {
        nlohmann::json only; only["tag"] = 9;
        Bag out; out.slots.resize(1);
        Arcane::ReflectionJsonReader reader(only);
        VisitMetaFields(*meta, &out, reader);
        REQUIRE_FALSE(reader.HasError());
        CHECK(out.slots.size() == 1);
        CHECK(out.tag == 9);
    }
    SECTION("a non-array node is malformed")
    {
        nlohmann::json bad = j; bad["slots"] = 5;
        Bag out;
        Arcane::ReflectionJsonReader reader(bad);
        VisitMetaFields(*meta, &out, reader);
        REQUIRE(reader.HasError());
        CHECK(reader.Error().find("slots") != std::string::npos);
    }
    SECTION("a non-object element is malformed and nothing is read into the vector")
    {
        nlohmann::json bad = j; bad["slots"] = { 1, 2 };
        Bag out; out.slots.resize(3);
        Arcane::ReflectionJsonReader reader(bad);
        VisitMetaFields(*meta, &out, reader);
        REQUIRE(reader.HasError());
        CHECK(out.slots.size() == 3);   // untouched: the shape check precedes the resize
    }
    SECTION("a malformed sub-field inside an element latches for the whole field")
    {
        nlohmann::json bad = j; bad["slots"][1]["offset"] = { 1.0 };   // vec2 with one number
        Bag out;
        Arcane::ReflectionJsonReader reader(bad);
        VisitMetaFields(*meta, &out, reader);
        REQUIRE(reader.HasError());
        CHECK(out.slots.empty());       // refused whole, not left half-read
    }
}
```

On the existing `TEST_CASE("unsupported field type fails loud instead of silently dropping", …)` add one comment line above it: `// std::vector<int> stays UNSUPPORTED after the 2026-09-11 container branch: the branch handles vectors of REFLECTED STRUCTS only (see the Bag case above) -- a scalar element has no TypeMeta to walk.`

`SceneJsonTest.cpp` — add:

```cpp
TEST_CASE("scene round-trips Collider2D fixtures through JSON", "[json][scene][physics]")
{
    // The 2026-09-11 container branch: fixtures were Serializable(false) before
    // (the bridge had no container branch), so a Collider2D authored in the
    // Inspector saved as a present-but-empty component. Now it round-trips.
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Arcane::RegisterPhysicsComponents(reg);

        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        Arcane::RigidBody2D rb; rb.type = Manifold2D::Physics::BodyType::Dynamic; rb.fixedRotation = true;
        reg.AddComponent<Arcane::RigidBody2D>(root, rb);
        Arcane::Collider2D col;
        Arcane::Fixture a; a.kind = Manifold2D::Physics::ShapeKind::Aabb;   a.halfW = 0.5f; a.halfH = 0.25f; a.friction = 0.7f;
        Arcane::Fixture b; b.kind = Manifold2D::Physics::ShapeKind::Circle; b.radius = 0.3f; b.localPos = glm::vec2(1.0f, 0.0f); b.isSensor = true;
        col.fixtures = { a, b };
        reg.AddComponent<Arcane::Collider2D>(root, col);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }
    const std::string colName(Astra::GetMeta<Arcane::Collider2D>()->typeName);
    REQUIRE(doc["entities"][0]["components"][colName]["fixtures"].is_array());
    REQUIRE(doc["entities"][0]["components"][colName]["fixtures"].size() == 2);
    CHECK(doc["entities"][0]["components"][colName]["fixtures"][1]["kind"] == "Circle");

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    const Arcane::SceneRoot* sr = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    const Arcane::Collider2D* out = reg.GetComponent<Arcane::Collider2D>(sr->entity);
    REQUIRE(out != nullptr);
    REQUIRE(out->fixtures.size() == 2);
    CHECK(out->fixtures[0].kind == Manifold2D::Physics::ShapeKind::Aabb);
    CHECK(out->fixtures[0].halfH == Catch::Approx(0.25f));
    CHECK(out->fixtures[0].friction == Catch::Approx(0.7f));
    CHECK(out->fixtures[1].kind == Manifold2D::Physics::ShapeKind::Circle);
    CHECK(out->fixtures[1].localPos.x == Catch::Approx(1.0f));
    CHECK(out->fixtures[1].isSensor);
    const Arcane::RigidBody2D* rb = reg.GetComponent<Arcane::RigidBody2D>(sr->entity);
    REQUIRE(rb != nullptr);
    CHECK(rb->type == Manifold2D::Physics::BodyType::Dynamic);
}
```

(Add `#include <Arcane/Scene/PhysicsComponents.hpp>` and `#include <catch2/catch_approx.hpp>` to `SceneJsonTest.cpp` if absent.)

`SceneAssetTest.cpp` — add directly after "a v3 scene still loads after the v4 bump", the same shape with a literal 4:

```cpp
TEST_CASE("a v4 scene still loads after the v5 bump", "[scene][json]")
{
    // v5 (2026-09-11) is ADDITIVE like v4: Collider2D::fixtures now writes as
    // an array a v4 engine would refuse on read, so the number moved; nothing a
    // v4 file already said changed, so v4 keeps loading. LITERAL 4 -- see the
    // v3 case above for why not the symbolic constant.
    const std::filesystem::path dir  = TempDir("arcane_scene_asset_v4");
    const std::filesystem::path file = dir / ("legacy4" + std::string(Arcane::Scene::kSceneExt));
    const std::string tName(Astra::GetMeta<Arcane::Transform>()->typeName);

    nlohmann::json e0;
    e0["components"][tName]["position"] = { 7.0, 0.0, 0.0 };
    e0["parent"] = -1;
    nlohmann::json doc;
    doc["id"]       = "00000000-0000-0000-0000-000000000002";
    doc["version"]  = 4;   // LITERAL
    doc["assets"]   = nlohmann::json::array();
    doc["entities"] = nlohmann::json::array({ e0 });
    std::ofstream(file, std::ios::binary) << doc.dump();

    std::string err;
    const auto read = Arcane::Scene::ReadSceneFile(file, &err);
    REQUIRE(read.has_value());
    CHECK(err.empty());
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry fresh{components};
    Arcane::RegisterSceneComponents(fresh);
    REQUIRE(Arcane::Scene::ApplySceneDocument(*read, fresh));
    const Arcane::SceneRoot* sr = fresh.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    CHECK(fresh.GetComponent<Arcane::Transform>(sr->entity)->position.x == 7.0f);
}
```

- [ ] **Step 2: Build `Arcane.slnx` Debug; run `"[json]"` — expect FAIL** (the Bag case: writer latches "unsupported field type"; the Collider2D case: `fixtures` absent — it was `Serializable(false)`).
- [ ] **Step 3: `ReflectionJson.hpp`.** (a) `Detail::IsHandledType` — first line of the body:

```cpp
            // A std::vector of a REFLECTED STRUCT is handled (2026-09-11
            // container branch); any other element kind is not -- see
            // ReflectionJsonWriter::WriteVector for the why.
            if (f.isVector)
            {
                const Astra::TypeMeta* em = Astra::GetMeta(f.elementTypeHash);
                return em != nullptr && em->GetEnumInfo() == nullptr && static_cast<bool>(f.vectorElement);
            }
```

(b) `ReflectionJsonWriter::Visit` — first statement, before `nlohmann::json value;`:

```cpp
            if (field.isVector) { WriteVector(field, instance); return; }
```

and a new private member beside `CollectAssetGuid`:

```cpp
        // std::vector<T> where T is a REFLECTED STRUCT -- the only element kind
        // any roster field has today (Collider2D::fixtures, MeshAssetData::
        // slots): a JSON array, one OBJECT per element, each walked by a
        // sub-writer over the element type's reflected fields exactly as a
        // nested-struct field is (so a guid nested in an element still reaches
        // the asset sink). Vectors of scalars / glm / enums stay "unsupported
        // field type" -- fail loud, never a partial array -- until a roster
        // field needs them (spec 2026-09-11-physics-2d-wiring s7.2).
        void WriteVector(const Astra::FieldInfo& field, void* instance)
        {
            const Astra::TypeMeta* em = Astra::GetMeta(field.elementTypeHash);
            if (!em || em->GetEnumInfo() || !field.vectorSize || !field.vectorElement)
            {
                Fail(Detail::UnsupportedFieldMessage(field));
                return;
            }
            nlohmann::json arr = nlohmann::json::array();
            const std::size_t n = field.vectorSize(instance);
            for (std::size_t i = 0; i < n; ++i)
            {
                nlohmann::json elem = nlohmann::json::object();
                ReflectionJsonWriter subWriter(elem, m_assetGuidSink);
                void* elemInstance = field.vectorElement(instance, i);
                for (const Astra::FieldInfo& nf : em->fields)
                    if (nf.IsSerializable())
                        subWriter.Visit(nf, elemInstance);
                if (subWriter.HasError()) { Fail(subWriter.Error()); return; }
                arr.push_back(std::move(elem));
            }
            m_out[std::string(field.name)] = std::move(arr);
        }
```

(c) `ReflectionJsonReader::Visit` — directly after `if (!node) return;`:

```cpp
            if (field.isVector) { ReadVector(field, instance, *node); return; }
```

and a new private member beside `Find`:

```cpp
        // The read half of WriteVector. SHAPE is checked for every element
        // BEFORE the live vector is touched, so a malformed document leaves
        // the component exactly as it was; a malformed SUB-FIELD inside an
        // element (wrong-arity vec2, non-string enum) latches like any other
        // and the vector is emptied rather than left half-read -- the
        // component's load has already failed at that point, this is hygiene.
        void ReadVector(const Astra::FieldInfo& field, void* instance, const nlohmann::json& node)
        {
            const Astra::TypeMeta* em = Astra::GetMeta(field.elementTypeHash);   // vetted by IsHandledType
            if (!node.is_array()) { Fail(Detail::MalformedFieldMessage(field)); return; }
            for (const nlohmann::json& elem : node)
                if (!elem.is_object()) { Fail(Detail::MalformedFieldMessage(field)); return; }
            field.vectorResize(instance, node.size());
            for (std::size_t i = 0; i < node.size(); ++i)
            {
                ReflectionJsonReader subReader(node[i]);
                void* elemInstance = field.vectorElement(instance, i);
                for (const Astra::FieldInfo& nf : em->fields)
                    if (nf.IsSerializable())
                        subReader.Visit(nf, elemInstance);
                if (subReader.HasError())
                {
                    field.vectorResize(instance, 0);
                    Fail(subReader.Error());
                    return;
                }
            }
        }
```

- [ ] **Step 4: `PhysicsComponents.hpp`** — the `Collider2D` reflect block: delete `ASTRA_REFLECT_ATTR(Serializable, false)` and replace the whole preceding comment (from "Collider2D: reflects the fixture list field." through "...this attribute comes off in the same change.") with:

```cpp
    // Collider2D: reflects the fixture list. Serializable on BOTH paths since
    // 2026-09-11 (2D physics wiring): the reflection->JSON bridge grew a
    // container branch (ReflectionJson.hpp, WriteVector/ReadVector) that walks
    // each Fixture's reflected fields, so a fixture list authored in the
    // Inspector or by hand in an .arcscene round-trips -- scene schema v5.
    // Before that the field was Serializable(false) because the bridge could
    // neither write nor read a container and a saved Collider2D could never be
    // opened again (SceneJsonTest pins the fix). The BINARY path was always
    // fine: Collider2D::Serialize(Archive&) carries the vector directly.
```

- [ ] **Step 5: `SceneSerializer.hpp`** — `kSceneJsonVersion = 5;` and append to its comment, before the constant:

```cpp
    // v5 (2026-09-11, 2D physics wiring Plan 1, engine ABI 28, spec s7.2) is
    // ADDITIVE like v4: Collider2D::fixtures now writes as a JSON array a v4
    // engine would refuse on read (its bridge had no container branch), so
    // the number says so; nothing a v4 file already said changed, and v4 (and
    // v3) keep loading -- kSceneJsonVersionMin stays 3.
```

- [ ] **Step 6: Build `Arcane.slnx` Debug (no game-module header changed — `SceneSerializer.hpp`/`ReflectionJson.hpp`/`PhysicsComponents.hpp` are not on the surface); run `"[json]"`, `"[scene]"`, `"[reflection]"` — PASS.** Then full `"~[gpu]"` — green. Delta: **+3 cases** (`ReflectionJsonTest` Bag, `SceneJsonTest` Collider2D, `SceneAssetTest` v4). Commit — `feat(serialization): reflection->JSON container branch for vectors of reflected structs; Collider2D::fixtures serialises; scene schema v5` + trailer.

---

### Task 4: `PhysicsSettings` component + `.arcproj` `"physics"` block (spec §5)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/Components.hpp` (struct after `MeshRenderer`; reflect block at the end of the reflect section), `ArcaneClient/src/Arcane/Scene/SceneModule.hpp` (`RegisterSceneComponents`: append after `MeshRenderer`), `ArcaneClient/src/Arcane/Base/Runtime.cpp` (the `Register<…>()` roster ~:171: append `PhysicsSettings` after `MeshRenderer`, i.e. between `MeshRenderer` and `RigidBody2D`? — NO: see the note below), `ArcaneClient/src/Arcane/Project/ProjectManifest.hpp` (`PhysicsConfig` + member), `ArcaneClient/src/Arcane/Project/ProjectManifest.cpp` (`FromJson` lenient parse), `ArcaneClient/src/Arcane/Project/Project.cpp` (`Create` writes the block)
- Test: `ArcaneTests/src/SceneComponentsTest.cpp` (one case), `ArcaneTests/src/ProjectManifestTest.cpp` (two cases), `ArcaneTests/src/SceneJsonTest.cpp` (one case)

**Roster order note.** Runtime's roster is "EXACTLY the order RegisterSceneComponents + RegisterPhysicsComponents register in" — scene roster first (`Transform … MeshRenderer`), then physics (`RigidBody2D, Collider2D, PhysicsBodyRef`). Appending `PhysicsSettings` to `RegisterSceneComponents` (after `MeshRenderer`) therefore means Runtime's list becomes `… MeshRenderer, PhysicsSettings, RigidBody2D, Collider2D, PhysicsBodyRef` — the three physics ids shift by one. That is the same in-process-only shift Task 2's ledger already records; ids are never persisted. Keep the two lists identical in order.

**Interfaces:**
- Produces: `struct Arcane::PhysicsSettings { glm::vec2 gravity{0.0f, 9.81f}; };` (reflected, roster-registered, user-addable); `ProjectManifest::PhysicsConfig { glm::vec2 gravity{0.0f, 9.81f}; } physics;` parsed from `"physics": { "gravity": [x, y] }`. Task 6 reads both.

- [ ] **Step 1: Write the failing tests.** `SceneComponentsTest.cpp`:

```cpp
TEST_CASE("PhysicsSettings is a reflected, roster-registered scene component", "[scene][physics]")
{
    // Spec 2026-09-11-physics-2d-wiring s5: the per-scene gravity override
    // rides on the scene-root entity as an ordinary component, so the
    // Inspector, JSON, undo and the catalog all get it for free.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg{components};
    Arcane::RegisterSceneComponents(reg);
    const Astra::TypeMeta* meta = Astra::GetMeta<Arcane::PhysicsSettings>();
    REQUIRE(meta != nullptr);
    REQUIRE(meta->fields.size() == 1);
    CHECK(meta->fields[0].name == "gravity");
    CHECK(meta->fields[0].IsSerializable());
    REQUIRE(components->GetComponentDescriptor(Astra::TypeID<Arcane::PhysicsSettings>::Value()) != nullptr);
    Arcane::PhysicsSettings def;
    CHECK(def.gravity.x == 0.0f);
    CHECK(def.gravity.y == Catch::Approx(9.81f));   // +Y is down
}
```

`ProjectManifestTest.cpp`:

```cpp
TEST_CASE("a manifest physics block sets gravity; absent keeps the default", "[project]")
{
    const auto with = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 28 },
        "physics": { "gravity": [0.0, 12.5] }
    })"));
    REQUIRE(with.has_value());
    CHECK(with->physics.gravity.x == 0.0f);
    CHECK(with->physics.gravity.y == Catch::Approx(12.5f));

    const auto without = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(R"({
        "formatVersion": 1, "name": "T", "engine": { "abi": 28 }
    })"));
    REQUIRE(without.has_value());
    CHECK(without->physics.gravity.y == Catch::Approx(9.81f));
}

TEST_CASE("a malformed physics gravity leaves the default rather than failing the manifest", "[project]")
{
    // Same lenient spirit as splash.backgroundColor: present-but-malformed
    // (wrong type, too short, a non-number element) keeps the default.
    for (const char* body : { R"("gravity": 5)", R"("gravity": [1.0])", R"("gravity": [1.0, "x"])" })
    {
        const auto m = Arcane::ProjectManifest::FromJson(nlohmann::json::parse(
            std::string(R"({"formatVersion": 1, "name": "T", "engine": { "abi": 28 }, "physics": {)") + body + "}}"));
        REQUIRE(m.has_value());
        CHECK(m->physics.gravity.y == Catch::Approx(9.81f));
    }
}
```

`SceneJsonTest.cpp`:

```cpp
TEST_CASE("PhysicsSettings on the scene root round-trips through JSON", "[json][scene][physics]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
        Arcane::PhysicsSettings ps; ps.gravity = glm::vec2(0.0f, 3.0f);
        reg.AddComponent<Arcane::PhysicsSettings>(root, ps);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        doc = Arcane::Scene::SaveJson(reg);
    }
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));
    const Arcane::SceneRoot* sr = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(sr != nullptr);
    const Arcane::PhysicsSettings* ps = reg.GetComponent<Arcane::PhysicsSettings>(sr->entity);
    REQUIRE(ps != nullptr);
    CHECK(ps->gravity.y == Catch::Approx(3.0f));
}
```

- [ ] **Step 2: Build — expect FAIL** (no `PhysicsSettings`, no `physics` member).
- [ ] **Step 3: `Components.hpp`** — after the `MeshRenderer` struct:

```cpp
    // PhysicsSettings (2026-09-11, 2D physics wiring, spec s5): the PER-SCENE
    // override of the project's physics block. Read by Runtime::EnsurePhysics
    // from the SCENE-ROOT entity only (SceneRoot resource) -- beside Camera
    // and PostProcess, where scene-level facts already live; on any other
    // entity it is ignored (pinned by RuntimeTest). Presence IS the override:
    // add it to change gravity for this scene, remove it to fall back to the
    // project. +Y is DOWN (screen-space world, Manifold2D's y-down default).
    struct PhysicsSettings
    {
        glm::vec2 gravity{0.0f, 9.81f};   // m/s^2; +Y down
    };
```

and at the end of the reflect section (after `MeshRenderer`'s block):

```cpp
    ASTRA_REFLECT_TYPE(PhysicsSettings)
        ASTRA_REFLECT_FIELD(PhysicsSettings, gravity)
            ASTRA_REFLECT_ATTR(Tooltip, "Gravity for THIS scene (m/s^2, +Y is down). Meaningful on the scene root only; overrides the project's physics block while present.")
    ASTRA_END_REFLECT_TYPE()
```

`SceneModule.hpp` — after `creg.RegisterComponent<MeshRenderer>();` add `creg.RegisterComponent<PhysicsSettings>();   // 2026-09-11 physics wiring -- APPENDED (see the note above)`. `Runtime.cpp` roster — `… Camera, MeshRenderer, PhysicsSettings, RigidBody2D, Collider2D, PhysicsBodyRef>();` and extend the order comment with one line: `// 2026-09-11: PhysicsSettings appended to the scene roster (after MeshRenderer), so the three physics ids shifted up by one -- in-process only, as ever.`
- [ ] **Step 4: `ProjectManifest.hpp`** — beside `SplashConfig`:

```cpp
        // The project-wide physics defaults (2026-09-11, spec s5). A scene
        // overrides them with a PhysicsSettings component on its root. Every
        // field has a default so an absent "physics" block behaves exactly
        // like this struct -- FromJson's lenient parse, as for splash.
        struct PhysicsConfig
        {
            glm::vec2 gravity{0.0f, 9.81f};   // m/s^2; +Y is down
        };
```

plus `PhysicsConfig physics;` after `splash;` (add `#include <glm/vec2.hpp>`). `ProjectManifest.cpp` `FromJson`, after the splash block:

```cpp
        // physics (2026-09-11): optional block; gravity is an array field with
        // the same lenient rule as splash.backgroundColor -- present but
        // malformed leaves the default, and BOTH elements must be numbers.
        if (doc.contains("physics") && doc["physics"].is_object())
        {
            const auto& ph = doc["physics"];
            ProjectManifest::PhysicsConfig cfg;   // defaults
            if (ph.contains("gravity") && ph["gravity"].is_array() && ph["gravity"].size() >= 2
                && ph["gravity"][0].is_number() && ph["gravity"][1].is_number())
            {
                cfg.gravity = glm::vec2(ph["gravity"][0].get<float>(), ph["gravity"][1].get<float>());
            }
            m.physics = cfg;
        }
```

`Project.cpp` `Create` — after `manifestJson["bootScene"] = "";` add `manifestJson["physics"] = { { "gravity", { 0.0, 9.81 } } };   // stamped at birth with the engine default (+Y down)`.
- [ ] **Step 5: Build order** (`Components.hpp` / `SceneModule.hpp` are game-module headers): `GenerateProjects.bat` → `ReferenceProject.slnx` Debug `/t:Rebuild` → `Arcane.slnx` Debug. Run `"[scene]"`, `"[project]"`, `"[json]"`, `"[runtime]"`, `"[editor]"` — PASS (the catalog tests must still pass: `PhysicsSettings` is addable, not hidden, not locked — no catalog change is wanted).
- [ ] **Step 6: Full `~[gpu]` — green.** Delta: **+4 cases**. Commit — `feat(scene): PhysicsSettings scene-root component + .arcproj physics block (project default, per-scene override)` + trailer.

---

### Task 5: `PhysicsSystem` becomes schedulable; the paused-pass fixes; `PhysicsBodyRef` auto-added (spec §4.1, §4.1a)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Scene/PhysicsSystem.hpp` (include; trait block ~:246-252; PASS 1 ~:272-296; a PASS 1.5 before PASS 2; PASS 4 ~:508-528; the tick-contract note ~:557-564; the header banner's PASS list :13-60), `ArcaneClient/src/Arcane/Scene/PhysicsComponents.hpp` (`RigidBody2D` ~:60, `Collider2D` ~:127: `AstraChangeTracked`)
- Test: `ArcaneTests/src/AuthoredTransformSyncTest.cpp` (four new `[transform-sync]` cases reusing `BuildAabbBody` / `kDt`)

**Interfaces:**
- Consumes: Task 4's roster (unchanged here).
- Produces (for Task 6): `PhysicsSystem` carries `static constexpr bool RequiresExclusive = true;` and `Astra::Before<TransformPropagationSystem>` in its trait list; `PhysicsSystem{fixedDt, /*stepWorld*/false}` is the edit pass; an entity with `RigidBody2D` + `Collider2D` but no `PhysicsBodyRef` gets one added by PASS 1.5 and is minted the same pass.

- [ ] **Step 1: Write the failing tests** — append to `AuthoredTransformSyncTest.cpp`:

```cpp
// ---- 2D physics wiring Plan 1 Task 5: the paused-pass fixes ----------------

TEST_CASE("a paused pass writes back nothing: Transform and RigidBody2D stay unstamped", "[transform-sync]")
{
    // Spec s4.1: PASS 4 is gated on stepWorld. Before, every paused pass wrote
    // Transform + velocity for every body, stamping every physics entity
    // changed each Edit frame (propagation recomposed them all) and flattening
    // an authored out-of-plane rotation to its Z turn.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {1,2}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    // An authored tilt OUT of the XY plane: the 2D solver has no state for it,
    // and a paused pass must leave it alone.
    const glm::quat tilt = glm::angleAxis(0.3f, glm::normalize(glm::vec3(1.0f, 0.0f, 0.0f)));
    reg.GetComponent<Transform>(e)->rotation = tilt;

    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);                                   // mints the body
    const Astra::Tick afterMint = reg.CurrentTick();
    paused(reg);                                   // a second paused pass: nothing authored changed
    paused(reg);

    // Nothing stamped since the mint pass: a Changed<Transform> view since
    // afterMint is empty, and so is one over RigidBody2D.
    int changedT = 0, changedRb = 0;
    reg.CreateView<const Transform, Astra::Changed<Transform>>().Since(afterMint)
        .ForEach([&](Astra::Entity, const Transform&) { ++changedT; });
    reg.CreateView<const RigidBody2D, Astra::Changed<RigidBody2D>>().Since(afterMint)
        .ForEach([&](Astra::Entity, const RigidBody2D&) { ++changedRb; });
    CHECK(changedT == 0);
    CHECK(changedRb == 0);
    // The tilt survived (PASS 4 used to overwrite rotation with RotationAboutZ).
    const glm::quat& r = reg.GetComponent<Transform>(e)->rotation;
    CHECK(std::abs(glm::dot(r, tilt)) == Approx(1.0f).margin(1e-5f));
}

TEST_CASE("a paused Collider2D edit re-mints the body with the new shape", "[transform-sync]")
{
    // Spec s4.1a: Collider2D is tracked; a paused PASS 1 destroys a body whose
    // Collider2D changed since lastReconcile and PASS 2 re-mints it the same
    // pass. Observable as the fixture's world half-extents.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle before = res->entityToBody.at(e);
    CHECK(Fixture0HalfExtents(reg).x == Approx(0.5f).margin(1e-4f));

    for (int i = 0; i < 3; ++i) paused(reg);       // untouched: the body is NOT re-minted
    CHECK(res->entityToBody.at(e) == before);

    reg.GetComponent<Collider2D>(e)->fixtures[0].halfW = 1.5f;   // the Inspector's edit, stamped by Mut
    paused(reg);
    REQUIRE(res->entityToBody.count(e) == 1);
    CHECK(res->world->IsValid(res->entityToBody.at(e)));
    CHECK_FALSE(res->world->IsValid(before));       // the old body is gone
    CHECK(Fixture0HalfExtents(reg).x == Approx(1.5f).margin(1e-4f));
}

TEST_CASE("a paused RigidBody2D type edit re-mints; removing Collider2D destroys", "[transform-sync]")
{
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Kinematic);
    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle first = res->entityToBody.at(e);
    CHECK(res->world->TypeSlot(first.index) == Phys::BodyType::Kinematic);

    reg.GetComponent<RigidBody2D>(e)->type = Phys::BodyType::Static;
    paused(reg);
    const Phys::BodyHandle second = res->entityToBody.at(e);
    CHECK_FALSE(res->world->IsValid(first));
    CHECK(res->world->TypeSlot(second.index) == Phys::BodyType::Static);

    reg.RemoveComponent<Collider2D>(e);
    paused(reg);
    CHECK(res->entityToBody.count(e) == 0);
    CHECK_FALSE(res->world->IsValid(second));
}

TEST_CASE("a stepping pass never re-mints on its own velocity write-back", "[transform-sync]")
{
    // The re-mint criteria are PAUSED-ONLY: PASS 4 writes RigidBody2D::velocity
    // every step, which would otherwise read as an author edit next pass.
    Astra::Registry reg;
    Astra::Entity e = BuildAabbBody(reg, {0,0}, {0.5f,0.5f}, Phys::BodyType::Dynamic);
    PhysicsSystem stepping(kDt, /*stepWorld=*/true);
    stepping(reg);
    auto* res = reg.GetResource<PhysicsResource>();
    const Phys::BodyHandle h = res->entityToBody.at(e);
    for (int i = 0; i < 10; ++i) stepping(reg);
    CHECK(res->entityToBody.at(e) == h);            // same body across ten steps
    CHECK(res->world->IsValid(h));
}

TEST_CASE("an entity authored without PhysicsBodyRef is minted anyway", "[transform-sync]")
{
    // The Inspector can never add PhysicsBodyRef (it is structure-locked), so
    // PASS 1.5 adds it for any RigidBody2D + Collider2D entity that lacks it.
    Astra::Registry reg;
    RegisterSceneComponents(reg);
    RegisterPhysicsComponents(reg);
    Phys::WorldDef wd; wd.gravityX = 0.0f; wd.gravityY = 0.0f;
    reg.SetResource(PhysicsResource{ std::make_unique<Phys::PhysicsWorld>(wd), {} });
    Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Transform>(e, Transform{});
    reg.AddComponent<WorldTransform>(e, WorldTransform{});
    RigidBody2D rb; rb.type = Phys::BodyType::Kinematic;
    reg.AddComponent<RigidBody2D>(e, rb);
    Collider2D col; Fixture fx; fx.kind = Phys::ShapeKind::Circle; fx.radius = 0.5f; col.fixtures.push_back(fx);
    reg.AddComponent<Collider2D>(e, col);
    REQUIRE_FALSE(reg.HasComponent<PhysicsBodyRef>(e));

    PhysicsSystem paused(kDt, /*stepWorld=*/false);
    paused(reg);
    REQUIRE(reg.HasComponent<PhysicsBodyRef>(e));
    auto* res = reg.GetResource<PhysicsResource>();
    REQUIRE(res->entityToBody.count(e) == 1);
    CHECK(res->world->IsValid(res->entityToBody.at(e)));
}

TEST_CASE("PhysicsSystem declares the scheduler contract: exclusive, before propagation", "[transform-sync]")
{
    STATIC_REQUIRE(PhysicsSystem::RequiresExclusive);
    STATIC_REQUIRE(std::tuple_size_v<PhysicsSystem::BeforeTypes> == 1);
    STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, PhysicsSystem::BeforeTypes>, TransformPropagationSystem>);
}
```

(Add `#include <Arcane/Scene/TransformSystems.hpp>`, `#include <glm/gtc/quaternion.hpp>`, `#include <tuple>`, `#include <type_traits>` to the test TU as needed.)

- [ ] **Step 2: Build `Arcane.slnx` Debug; run `"[transform-sync]"` — expect FAIL** (no `RequiresExclusive`; the paused pass stamps; no re-mint; no auto-add).
- [ ] **Step 3: `PhysicsComponents.hpp`** — in `RigidBody2D` and in `Collider2D`, first member, mirroring `Transform`:

```cpp
        // Astra change tracking (2026-09-11 physics wiring, spec s4.1a): a
        // paused PhysicsSystem pass re-mints a body whose RigidBody2D or
        // Collider2D changed since the last reconcile, so an Inspector edit
        // (or its undo) reaches the world without an exact compare per body.
        static constexpr bool AstraChangeTracked = true;
```

- [ ] **Step 4: `PhysicsSystem.hpp`.** (a) `#include <Arcane/Scene/TransformSystems.hpp>` beside the other `Arcane/Scene` includes (Manifold2D-free; no cycle — it does not include this header). (b) The trait block:

```cpp
    struct PhysicsSystem
        : Astra::SystemTraits<Astra::Reads<Collider2D>,
                              Astra::Writes<Transform, PhysicsBodyRef, RigidBody2D>,
                              Astra::Before<TransformPropagationSystem>>
    {
        // Scheduled by Runtime::InstallEngineSystems into fixedUpdate (2026-09-11
        // physics wiring, spec s4.1). EXCLUSIVE: this pass advances the registry
        // tick and mutates structurally (PASS 1.5 adds PhysicsBodyRef), so it
        // owns its scheduler segment like TransformPropagationSystem does.
        // BEFORE propagation: PASS 4's write-back must be what propagation
        // composes this step, whichever order the module and the engine
        // inserted their systems.
        static constexpr bool RequiresExclusive = true;
```

(c) PASS 1 — replace the block with:

```cpp
            // ------------------------------------------------------------------
            // PASS 1: DESTROY -- remove body rows for dead or un-physicised
            // entities, and (PAUSED passes only, spec s4.1a) for entities whose
            // RigidBody2D or Collider2D changed since the last reconcile -- an
            // author edit of shape, mass or body type -- so PASS 2 re-mints them
            // from the current components in this same pass. Stepping passes
            // skip the change criteria: PASS 4 writes velocity every step, which
            // would otherwise read as an edit. Collect first; erase after (map
            // iteration) -- and the change views are read BEFORE any erase so
            // nothing is invalidated under them.
            //
            // Implicit assumption: a handle becomes invalid ONLY through this
            // pass. The CREATE pass self-heals any stale handle by overwriting
            // the map entry when it calls AddBody for the same entity.
            // ------------------------------------------------------------------
            {
                std::vector<Astra::Entity> toRemove;
                if (!m_stepWorld)
                {
                    reg.CreateView<const PhysicsBodyRef, Astra::Changed<Collider2D>, Astra::With<RigidBody2D>>()
                        .Since(res->lastReconcile)
                        .ForEach([&](Astra::Entity entity, const PhysicsBodyRef&) { toRemove.push_back(entity); });
                    reg.CreateView<const PhysicsBodyRef, Astra::Changed<RigidBody2D>, Astra::With<Collider2D>>()
                        .Since(res->lastReconcile)
                        .ForEach([&](Astra::Entity entity, const PhysicsBodyRef&) { toRemove.push_back(entity); });
                }
                for (auto& [entity, handle] : entityToBody)
                {
                    const bool dead       = !reg.IsValid(entity);
                    const bool noBody     = !dead && !reg.HasComponent<RigidBody2D>(entity);
                    const bool noCollider = !dead && !reg.HasComponent<Collider2D>(entity);
                    if (dead || noBody || noCollider)
                        toRemove.push_back(entity);
                }
                for (Astra::Entity e : toRemove)
                {
                    auto it = entityToBody.find(e);
                    if (it == entityToBody.end()) continue;   // listed twice, or never minted
                    if (world.IsValid(it->second))
                        world.RemoveBody(it->second);
                    entityToBody.erase(it);
                }
            }

            // ------------------------------------------------------------------
            // PASS 1.5: ENSURE PhysicsBodyRef. The Inspector can never add one
            // (ComponentCatalog structure-locks it), so an editor-authored
            // RigidBody2D + Collider2D entity would otherwise never match PASS 2's
            // view. Collected, then added -- AddComponent moves the entity
            // between archetypes, never inside a ForEach.
            // ------------------------------------------------------------------
            {
                std::vector<Astra::Entity> missing;
                reg.CreateView<const RigidBody2D, const Collider2D, Astra::Not<PhysicsBodyRef>>()
                    .ForEach([&](Astra::Entity entity, const RigidBody2D&, const Collider2D&) { missing.push_back(entity); });
                for (Astra::Entity e : missing)
                    reg.AddComponent<PhysicsBodyRef>(e, PhysicsBodyRef{});
            }
```

(d) PASS 4 — wrap the existing write-back block in `if (m_stepWorld)` and replace its comment's lead with:

```cpp
            // ------------------------------------------------------------------
            // PASS 4: WRITE-BACK -- STEPPING passes only (spec s4.1, 2026-09-11).
            // A paused pass has nothing to reflect: the author owns the pose and
            // PASS 3.5 already pushed edits body-ward. The unconditional write
            // this replaced stamped every physics entity's Transform changed on
            // every Edit frame (propagation recomposed them all, defeating the
            // change detection for exactly the entities physics touches) and
            // flattened an authored out-of-plane rotation to its Z turn.
            // ------------------------------------------------------------------
            if (m_stepWorld)
            {
                ... (the existing view + ForEach, unchanged) ...
            }
```

(e) The tick-contract note near the end ("PhysicsSystem is never AddSystem'd anywhere (map B3) … it needs RequiresExclusive") → rewrite: `// Time base for the paused reconcile and the re-mint criteria (PhysicsResource::lastReconcile): AFTER PASS 4, so a stepping pass's own write-back marks are never newer than it. Scheduled (Runtime::InstallEngineSystems) or bare (Runtime::PhysicsEditPass, tests) alike -- the advance-after-every-pass contract, spec 2026-09-11-astra-adoption s6.3.` (f) The header banner (:13-60): add a PASS 1.5 line after PASS 1's; in PASS 1's description add "or, paused, changed RigidBody2D/Collider2D (re-mint)"; in PASS 4's, "STEPPING passes only"; delete the "(Epic 04.2, opt-in) if the entity carries a PreviousTransform…" remnants if any survived Task 2 of the previous plan (they should not).

- [ ] **Step 5: Build `Arcane.slnx` Debug; run `"[transform-sync]"`, `"[physics]"`, `"[interp]"`, `"[scene]"` — PASS.** (If `Astra::Changed<Collider2D>` refuses to compile because the type is not tracked at registration, the static member was missed — it must be on the struct, not the reflect block.)
- [ ] **Step 6: Full `~[gpu]` — green.** Delta: **+6 cases**. Commit — `feat(physics): PhysicsSystem schedulable (RequiresExclusive, Before<propagation>); paused passes write back nothing and re-mint on RigidBody2D/Collider2D edits; PhysicsBodyRef auto-added` + trailer.

---

### Task 6: `Runtime` facade — `InstallEngineSystems`, `EnsurePhysics`, `PhysicsEditPass`; both hosts call it (spec §4.2–§4.4, §5)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Base/Runtime.hpp` (three public methods beside `ClearSystems`), `ArcaneClient/src/Arcane/Base/Runtime.cpp` (include `<Arcane/Scene/PhysicsSystem.hpp>`; ctor tail; `ClearSystems` re-installs; the three bodies), `ArcaneRuntime/src/RuntimeFrame.cpp` (~:275, before `Loop().Advance`), `ArcaneEditor/src/App/EditorAppFrame.cpp` (`AdvanceSim` ~:1226, before `Loop().Advance`)
- Test: `ArcaneTests/src/RuntimeTest.cpp` (five new cases; one existing case amended)

**Interfaces:**
- Produces (Tasks 7, 8, 9): 
  ```cpp
  void Runtime::InstallEngineSystems();   // idempotent; PhysicsSystem into fixedUpdate
  void Runtime::EnsurePhysics();          // per frame, before Advance: mint/refresh PhysicsResource + PhysicsInterpBuffer
  void Runtime::PhysicsEditPass();        // PhysicsSystem{1/fixedHz, stepWorld=false}(registry), bare
  glm::vec2 Runtime::ResolvedGravity() const;   // project block, overridden by the scene-root PhysicsSettings
  ```
  `ClearSystems()` now re-installs the engine systems after clearing (the module's are gone, the engine's are back) — every `PluginHost` path is covered without touching it.

- [ ] **Step 1: Write the failing tests** — `RuntimeTest.cpp` (add `#include <Arcane/Scene/PhysicsSystem.hpp>`, `<Arcane/Scene/TransformSystems.hpp>`, `<Arcane/Scene/SceneModule.hpp>`, `<Manifold2D/Physics/PhysicsWorld.hpp>`):

```cpp
// ---- 2D physics wiring Plan 1 Task 6: the engine-owned physics facade ------

TEST_CASE("Runtime installs PhysicsSystem into fixedUpdate and re-installs after ClearSystems", "[runtime][physics]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    CHECK_FALSE(rt.Schedulers().update.HasSystem<Arcane::PhysicsSystem>());
    rt.InstallEngineSystems();                                   // idempotent
    CHECK(rt.Schedulers().fixedUpdate.Size() == 1);
    REQUIRE(rt.Schedulers().fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>().IsOk());   // "the module's"
    rt.ClearSystems();                                           // what PluginHost does on every unload/reload
    CHECK(rt.Schedulers().fixedUpdate.HasSystem<Arcane::PhysicsSystem>());
    CHECK_FALSE(rt.Schedulers().fixedUpdate.HasSystem<Arcane::TransformPropagationSystem>());
    CHECK(rt.Schedulers().update.Empty());
    CHECK(rt.Schedulers().render.Empty());
}

TEST_CASE("EnsurePhysics mints a world once and again after RestoreRegistry", "[runtime][physics]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    CHECK(rt.Registry().GetResource<Arcane::PhysicsResource>() == nullptr);
    rt.EnsurePhysics();
    const auto* res = rt.Registry().GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    REQUIRE(res->world != nullptr);
    REQUIRE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer>() != nullptr);
    CHECK_FALSE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer>()->captured);
    const Manifold2D::Physics::PhysicsWorld* first = res->world.get();
    rt.EnsurePhysics();                                          // same frame, same settings: no re-mint
    CHECK(rt.Registry().GetResource<Arcane::PhysicsResource>()->world.get() == first);
    CHECK(static_cast<float>(first->Gravity().y) == Catch::Approx(9.81f));   // the engine default, no project

    auto bytes = rt.SnapshotRegistry();
    REQUIRE(bytes.IsOk());
    REQUIRE(rt.RestoreRegistry(bytes.Value()));                  // replaces the registry: resources are gone
    CHECK(rt.Registry().GetResource<Arcane::PhysicsResource>() == nullptr);
    rt.EnsurePhysics();
    REQUIRE(rt.Registry().GetResource<Arcane::PhysicsResource>() != nullptr);
    CHECK(rt.Registry().GetResource<Arcane::PhysicsResource>()->world.get() != first);
}

TEST_CASE("ResolvedGravity: the engine default, then the scene-root PhysicsSettings override", "[runtime][physics]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    CHECK(rt.ResolvedGravity().y == Catch::Approx(9.81f));      // no project open: PhysicsConfig's default

    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root  = reg.CreateEntity();
    const Astra::Entity other = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    Arcane::PhysicsSettings ps; ps.gravity = glm::vec2(0.0f, 2.0f);
    reg.AddComponent<Arcane::PhysicsSettings>(other, ps);        // NOT the root: ignored
    CHECK(rt.ResolvedGravity().y == Catch::Approx(9.81f));
    reg.AddComponent<Arcane::PhysicsSettings>(root, ps);
    CHECK(rt.ResolvedGravity().y == Catch::Approx(2.0f));

    rt.EnsurePhysics();
    const auto* res = rt.Registry().GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    CHECK(static_cast<float>(res->world->Gravity().y) == Catch::Approx(2.0f));
    // A gravity edit re-mints the world (no SetGravity on the vendored
    // PhysicsWorld; spec s4.3 amended): the next Ensure carries it.
    const auto* before = res->world.get();
    reg.GetComponent<Arcane::PhysicsSettings>(root)->gravity.y = 5.0f;
    rt.EnsurePhysics();
    res = rt.Registry().GetResource<Arcane::PhysicsResource>();
    CHECK(res->world.get() != before);
    CHECK(static_cast<float>(res->world->Gravity().y) == Catch::Approx(5.0f));
}

TEST_CASE("PhysicsEditPass mints bodies, moves none, captures nothing", "[runtime][physics]")
{
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root = reg.CreateEntity();
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f, -1.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Manifold2D::Physics::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture fx; fx.kind = Manifold2D::Physics::ShapeKind::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);

    rt.EnsurePhysics();
    for (int i = 0; i < 5; ++i) rt.PhysicsEditPass();
    const auto* res = rt.Registry().GetResource<Arcane::PhysicsResource>();
    REQUIRE(res->entityToBody.count(e) == 1);                    // minted (PhysicsBodyRef auto-added)
    CHECK(reg.GetComponent<Arcane::Transform>(e)->position.y == Catch::Approx(-1.0f));   // did not fall
    CHECK_FALSE(rt.Registry().GetResource<Arcane::PhysicsInterpBuffer>()->captured);
}

TEST_CASE("fixedUpdate runs physics BEFORE propagation whichever was added first", "[runtime][physics]")
{
    // Behavioural pin of the Before<> edge: after one fixed step the entity's
    // WorldTransform carries the POST-step position PASS 4 wrote back. If
    // propagation ran first it would lag one step behind.
    Arcane::Runtime rt(&Arcane::Test::SharedTypeContext());
    // The module's insertion order: propagation AFTER the engine's physics
    // (the ctor installed it) -- and the plan must not depend on that.
    REQUIRE(rt.Schedulers().fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>().IsOk());
    Astra::Registry& reg = rt.Registry();
    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(e, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Manifold2D::Physics::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture fx; fx.kind = Manifold2D::Physics::ShapeKind::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.SetParent(e, root);

    rt.EnsurePhysics();
    rt.Loop().SetPaused(false);
    rt.Loop().Advance(1.0 / 60.0);                               // exactly one fixed step at 60 Hz
    const float y  = reg.GetComponent<Arcane::Transform>(e)->position.y;
    const float wy = reg.GetComponent<Arcane::WorldTransform>(e)->matrix[3].y;
    CHECK(y > 0.0f);                                             // it fell (+Y down)
    CHECK(wy == Catch::Approx(y).margin(1e-6f));                 // propagation saw this step's write-back
}
```

Amend the existing `"Runtime ClearSystems empties all phase schedulers"` case: after `rt.ClearSystems();` the `fixedUpdate` check becomes `CHECK(rt.Schedulers().fixedUpdate.Size() == 1);   // the engine-owned PhysicsSystem is re-installed (2026-09-11); the module's NoOpSystem is gone` + `CHECK_FALSE(rt.Schedulers().fixedUpdate.HasSystem<NoOpSystem>());`; `update`/`render` stay `Empty()`. Rename the case to `"Runtime ClearSystems empties the module's systems and re-installs the engine's"`.

- [ ] **Step 2: Build — expect FAIL** (no `InstallEngineSystems` etc.).
- [ ] **Step 3: `Runtime.hpp`** — beside `ClearSystems`:

```cpp
        // --- engine-owned physics (2026-09-11, spec docs/specs/2026-09-11-physics-2d-wiring-design.md s4-s5) ---
        // Manifold2D-free surface: hosts and modules never see PhysicsSystem or
        // PhysicsWorld. InstallEngineSystems adds the engine's own systems
        // (today: PhysicsSystem into fixedUpdate, Before<TransformPropagation
        // System>); the ctor calls it, and ClearSystems calls it again after
        // clearing, so every PluginHost load/reload/unload path keeps it.
        // Idempotent. EnsurePhysics runs once per frame before Loop().Advance
        // (beside SetRenderContext): it mints PhysicsResource + PhysicsInterp
        // Buffer when the current registry lacks them -- scene open,
        // RestoreRegistry (Play -> Stop, structural undo) and hot reload all
        // replace the registry, and the next frame's Ensure is the reset --
        // and re-mints the world when ResolvedGravity changed (the vendored
        // PhysicsWorld has no SetGravity; a settings edit is authoring, not
        // gameplay). PhysicsEditPass is Edit mode's only physics: a bare
        // stepWorld=false pass (mint / destroy / reconcile, no step).
        void      InstallEngineSystems();
        void      EnsurePhysics();
        void      PhysicsEditPass();
        // Scene-root PhysicsSettings when present, else the project's physics
        // block, else PhysicsConfig's default (0, 9.81; +Y down).
        [[nodiscard]] glm::vec2 ResolvedGravity() const;
```

(`<glm/vec2.hpp>` is already reachable through `SceneResources.hpp`'s include chain in this header; add it explicitly if the build says otherwise.)
- [ ] **Step 4: `Runtime.cpp`.** Include `<Arcane/Scene/PhysicsSystem.hpp>` (Arcane.dll compiles Manifold2D). At the END of the ctor body (after `InstallMosaicHandler();`): `InstallEngineSystems();`. Then:

```cpp
    void Runtime::InstallEngineSystems()
    {
        auto& fixed = m_impl->schedulers->fixedUpdate;
        if (fixed.HasSystem<PhysicsSystem>()) return;
        const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
        // AlreadyRegistered is the only failure and HasSystem just excluded it.
        std::ignore = fixed.AddSystem<PhysicsSystem>(fixedDt, /*stepWorld*/ true);
    }

    void Runtime::ClearSystems()
    {
        m_impl->schedulers->fixedUpdate.Clear();
        m_impl->schedulers->update.Clear();
        m_impl->schedulers->render.Clear();
        InstallEngineSystems();   // the module's systems are gone; the engine's are back
    }

    glm::vec2 Runtime::ResolvedGravity() const
    {
        glm::vec2 g = ProjectManifest::PhysicsConfig{}.gravity;
        if (m_impl->project)
            g = m_impl->project->Manifest().physics.gravity;
        if (const SceneRoot* sr = m_impl->registry->GetResource<SceneRoot>())
            if (const PhysicsSettings* ps = std::as_const(*m_impl->registry).GetComponent<PhysicsSettings>(sr->entity))
                g = ps->gravity;
        return g;
    }

    void Runtime::EnsurePhysics()
    {
        Astra::Registry& reg = *m_impl->registry;
        const glm::vec2 g = ResolvedGravity();
        PhysicsResource* res = reg.GetResource<PhysicsResource>();
        if (res && res->world)
        {
            const auto cur = res->world->Gravity();
            if (static_cast<float>(cur.x) == g.x && static_cast<float>(cur.y) == g.y)
                return;
            // Gravity changed: replace the world. Bodies re-mint from their
            // current Transforms on the next pass (PASS 1 sees every handle
            // invalid against the new world; PASS 2 self-heals).
        }
        Manifold2D::Physics::WorldDef wd;
        wd.gravityX = g.x;
        wd.gravityY = g.y;
        reg.SetResource(PhysicsResource{ std::make_unique<Manifold2D::Physics::PhysicsWorld>(wd), {} });
        reg.SetResource(PhysicsInterpBuffer{});
    }

    void Runtime::PhysicsEditPass()
    {
        const float fixedDt = static_cast<float>(1.0 / m_impl->loopCfg.fixedHz);
        PhysicsSystem{ fixedDt, /*stepWorld*/ false }(*m_impl->registry);
    }
```

(`std::as_const` needs `<utility>`; `ProjectManifest` needs `<Arcane/Project/ProjectManifest.hpp>` — `Project.hpp` already includes it.) Note on the re-mint branch: after `SetResource(PhysicsResource{...})` replaces the resource, the old `entityToBody` map is gone too, so PASS 2's "already tracked" test (`entityToBody.count(entity)`) fails for every body and each is re-minted — the stale `PhysicsBodyRef::handle` is overwritten (PASS 2 "self-heals any stale handle"). Handles that point at the OLD world are never dereferenced against the new one: PASS 1 iterates the (empty) new map.
- [ ] **Step 5: The hosts.** `RuntimeFrame.cpp`, directly before `io.runtime->Loop().Advance(simDt, …)`: `io.runtime->EnsurePhysics();   // engine-owned physics (spec s4.3): mint/refresh the world before the step`. `EditorAppFrame.cpp` `AdvanceSim`, directly before `m_runtime->Loop().Advance(simDt, …)`: `m_runtime->EnsurePhysics();   // engine-owned physics (spec s4.3); Edit mode's pass is EditModeSchedule's (Task 7)`.
- [ ] **Step 6: Build `Arcane.slnx` Debug (Runtime.hpp is not a game-module header — `Runtime.hpp` IS included by `Aphelyon.cpp`/`ReferenceGame.cpp`! It is on the surface: `GenerateProjects.bat` → `ReferenceProject.slnx` Debug `/t:Rebuild` → `Arcane.slnx` Debug).** Run `"[runtime]"`, `"[physics]"`, `"[transform-sync]"` — PASS. Full `~[gpu]` — green. Delta: **+5 cases**. Commit — `feat(runtime): engine-owned physics -- InstallEngineSystems / EnsurePhysics / PhysicsEditPass / ResolvedGravity; both hosts Ensure before Advance` + trailer.

---

### Task 7: Editor — the Edit-mode pass before propagation; Play → Stop returns bodies (spec §6.1, §6.2)

**Files:**
- Modify: `ArcaneEditor/src/Scene/EditModeSchedule.hpp` (an injected `std::function<void()> editPass` seam + setter), `ArcaneEditor/src/Scene/EditModeSchedule.cpp` (`RunFrame` calls it before `Execute`), `ArcaneEditor/src/App/EditorApp.cpp` (bind `m_editSchedule.SetPhysicsEditPass([this]{ m_runtime->PhysicsEditPass(); })` where `m_runtime` is created — find the ctor/Init site that owns `m_runtime`)
- Test: `ArcaneTests/src/EditModeScheduleTest.cpp` (one case), `ArcaneTests/src/EditorPlayModeTest.cpp` (one case)

**Interfaces:**
- Consumes: Task 6's `Runtime::PhysicsEditPass`, `EnsurePhysics`.
- Produces: `void EditModeSchedule::SetPhysicsEditPass(std::function<void()> pass)`; `RunFrame` invokes it (if set) before propagation, Edit mode only.

- [ ] **Step 1: Write the failing tests.** `EditModeScheduleTest.cpp`:

```cpp
TEST_CASE("the physics edit pass runs before propagation, once per Edit frame, never in Play", "[editor][physics]")
{
    // Spec s6.1: EditModeSchedule owns Edit mode's ONLY physics -- the injected
    // pass (EditorApp binds Runtime::PhysicsEditPass) runs before the
    // propagation it feeds. Device-less: the seam is a callable, so this test
    // links no Manifold2D.
    auto s = BuildScene();                          // this file's existing fixture
    Arcane::Editor::EditModeSchedule schedule;
    std::vector<std::string> order;
    schedule.SetPhysicsEditPass([&] { order.push_back("physics"); });
    // Observe propagation through its own counter (TransformOrder::runs).
    auto runsNow = [&] { const auto* c = s.reg.template GetResource<Arcane::TransformOrder>(); return c ? c->runs : 0u; };
    const auto before = runsNow();
    REQUIRE(schedule.RunFrame(s.reg, /*inPlayMode*/ false));
    REQUIRE(order.size() == 1);
    CHECK(runsNow() == before + 1);
    CHECK_FALSE(schedule.RunFrame(s.reg, /*inPlayMode*/ true));
    CHECK(order.size() == 1);                       // Play: no edit pass
    REQUIRE(schedule.RunFrame(s.reg, false));
    CHECK(order.size() == 2);
}
```

(Check the file's fixture name — it has a `BuildScene`-like helper returning a struct with `reg`; use whatever it is called. The ordering claim "before propagation" is pinned by the pass being invoked from `RunFrame` before `m_schedule.Execute`; to make it observable, have the injected pass record `runsNow()` at call time: `schedule.SetPhysicsEditPass([&]{ order.push_back("physics"); CHECK(runsNow() == before); });`.)

`EditorPlayModeTest.cpp`:

```cpp
TEST_CASE("Play lets a body fall; Stop returns it to the authored pose with a fresh world", "[editor][physics]")
{
    Arcane::Runtime runtime(&Arcane::Test::SharedTypeContext(), /*enableAudioDevice*/false);
    Astra::Registry& reg = runtime.Registry();
    Arcane::RegisterSceneComponents(reg);
    Arcane::RegisterPhysicsComponents(reg);
    const Astra::Entity root = reg.CreateEntity();
    reg.AddComponent<Arcane::Transform>(root, Arcane::Transform{});
    reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
    const Astra::Entity e = reg.CreateEntity();
    Arcane::Transform lt; lt.position = glm::vec3(0.0f, -1.0f, 0.0f);
    reg.AddComponent<Arcane::Transform>(e, lt);
    reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
    Arcane::RigidBody2D rb; rb.type = Manifold2D::Physics::BodyType::Dynamic;
    reg.AddComponent<Arcane::RigidBody2D>(e, rb);
    Arcane::Collider2D col; Arcane::Fixture fx; fx.kind = Manifold2D::Physics::ShapeKind::Circle; fx.radius = 0.5f;
    col.fixtures.push_back(fx);
    reg.AddComponent<Arcane::Collider2D>(e, col);
    reg.SetParent(e, root);

    // Edit mode: the host's per-frame Ensure + the schedule's edit pass.
    runtime.EnsurePhysics();
    runtime.PhysicsEditPass();
    const Manifold2D::Physics::PhysicsWorld* editWorld = runtime.Registry().GetResource<Arcane::PhysicsResource>()->world.get();

    Arcane::Editor::PlaySession play;
    REQUIRE(play.Play(runtime));
    for (int i = 0; i < 30; ++i) { runtime.EnsurePhysics(); runtime.Loop().Advance(1.0 / 60.0); }
    {
        Astra::Registry& live = runtime.Registry();
        float y = -1.0f;
        for (Astra::Entity le : live.GetEntityManager())
            if (const auto* rbp = live.GetComponent<Arcane::RigidBody2D>(le))
                if (rbp->type == Manifold2D::Physics::BodyType::Dynamic)
                    y = live.GetComponent<Arcane::Transform>(le)->position.y;
        CHECK(y > -0.5f);                           // it fell during Play
    }

    REQUIRE(play.Stop(runtime));                    // restore: the registry is replaced
    CHECK(runtime.Registry().GetResource<Arcane::PhysicsResource>() == nullptr);
    runtime.EnsurePhysics();                        // the next Edit frame
    runtime.PhysicsEditPass();
    const auto* res = runtime.Registry().GetResource<Arcane::PhysicsResource>();
    REQUIRE(res != nullptr);
    CHECK(res->world.get() != editWorld);           // a fresh world
    CHECK_FALSE(runtime.Registry().GetResource<Arcane::PhysicsInterpBuffer>()->captured);
    Astra::Registry& restored = runtime.Registry();
    float y = 0.0f; int dynamic = 0;
    for (Astra::Entity le : restored.GetEntityManager())
        if (const auto* rbp = restored.GetComponent<Arcane::RigidBody2D>(le))
            if (rbp->type == Manifold2D::Physics::BodyType::Dynamic)
            { ++dynamic; y = restored.GetComponent<Arcane::Transform>(le)->position.y; }
    REQUIRE(dynamic == 1);
    CHECK(y == Catch::Approx(-1.0f));               // the authored pose
    CHECK(res->entityToBody.size() == 1);           // re-minted from it
}
```

- [ ] **Step 2: Build — expect FAIL** (no `SetPhysicsEditPass`).
- [ ] **Step 3: `EditModeSchedule.hpp`** — add `#include <functional>`; in the class: 

```cpp
        // Edit mode's ONLY physics (spec 2026-09-11-physics-2d-wiring s6.1):
        // an injected callable -- EditorApp binds Runtime::PhysicsEditPass --
        // run by RunFrame BEFORE propagation, so bodies exist in the editor
        // world without simulating and every authored edit reaches them the
        // frame after it lands. A callable rather than the system itself so
        // this class (and its device-less test) links no Manifold2D.
        void SetPhysicsEditPass(std::function<void()> pass) { m_physicsEditPass = std::move(pass); }
```

and a member `std::function<void()> m_physicsEditPass;`. `.cpp` `RunFrame`: after the `inPlayMode` early return, before `m_schedule.Execute(reg)`: `if (m_physicsEditPass) m_physicsEditPass();   // mint / destroy / reconcile; propagation composes the result`.
- [ ] **Step 4: `EditorApp.cpp`** — at the site where `m_runtime` is constructed (grep `m_runtime = std::make_unique<Arcane::Runtime>` or the equivalent), immediately after it: `m_editSchedule.SetPhysicsEditPass([this] { m_runtime->PhysicsEditPass(); });`. If the runtime is re-created on project switch, the binding through `this` stays valid (it dereferences `m_runtime` at call time).
- [ ] **Step 5: Build `Arcane.slnx` Debug (editor + tests only); run `"[editor]"` — PASS.** Full `~[gpu]` — green. Delta: **+2 cases**. Commit — `feat(editor): Edit-mode physics pass runs before propagation; Play/Stop returns bodies to authored poses through a fresh world` + trailer.

---

### Task 8: The overlay — selected-body outline in Edit mode, View → Physics Overlay (spec §6.3)

**Files:**
- Modify: `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.hpp` (`PhysicsDebugDrawOptions::onlyBody`), `ArcaneClient/src/Arcane/Render/PhysicsDebugDraw.cpp` (the body loop ~:332 and the contacts block ~:491 honour it), `ArcaneEditor/src/Scene/PhysicsOverlay.hpp` (NEW, header-only: the pure decision), `ArcaneEditor/src/App/EditorApp.hpp` (`bool m_physicsOverlay = false;`), `ArcaneEditor/src/App/EditorAppFrame.cpp` (`SubmitSceneToBatcher`: the draw call after `SubmitRender()`, before the gizmo), `ArcaneEditor/src/Panels/EditorPanels.hpp` (`MenuRequests::togglePhysicsOverlay`; `BeginDockSpace` gains `bool physicsOverlayOn`), `ArcaneEditor/src/Panels/EditorPanels.cpp` (a new `View` menu between `Assets` and `Window`), the `BeginDockSpace` call site in `EditorAppFrame.cpp` (pass `m_physicsOverlay`; act on the request)
- Test: `ArcaneTests/src/PhysicsDebugRichTest.cpp` (one case: `onlyBody`), `ArcaneTests/src/PhysicsOverlayPlanTest.cpp` (NEW TU: the decision; `GenerateProjects.bat`)

**Interfaces:**
- Produces: `std::optional<Manifold2D::Physics::BodyHandle> PhysicsDebugDrawOptions::onlyBody;` — when set, only that body's outline is drawn and every non-outline overlay (contacts, AABBs, velocity, COM, orientation, manifolds) is skipped. `Arcane::Editor::PhysicsOverlayPlan PlanPhysicsOverlay(bool inPlayMode, bool overlayToggled, bool hasSelectedBody)` → `{ bool draw; bool wholeWorld; }`: `wholeWorld` iff toggled; `draw` iff toggled or (Edit mode and a selected body).

- [ ] **Step 1: Write the failing tests.** `PhysicsOverlayPlanTest.cpp` (NEW):

```cpp
// The overlay decision, spec 2026-09-11-physics-2d-wiring s6.3, kept pure so
// the ImGui-side call (EditorAppFrame::SubmitSceneToBatcher) has nothing to
// decide: selected-body outline always in Edit mode; the whole world only
// behind View -> Physics Overlay, in Edit and Play alike.
#include <catch2/catch_test_macros.hpp>
#include "Scene/PhysicsOverlay.hpp"
using Arcane::Editor::PlanPhysicsOverlay;
TEST_CASE("PlanPhysicsOverlay: selection outline in Edit; whole world only when toggled", "[editor][physics]")
{
    // (inPlayMode, toggled, hasSelectedBody) -> (draw, wholeWorld)
    CHECK_FALSE(PlanPhysicsOverlay(false, false, false).draw);
    CHECK(PlanPhysicsOverlay(false, false, true).draw);
    CHECK_FALSE(PlanPhysicsOverlay(false, false, true).wholeWorld);
    CHECK_FALSE(PlanPhysicsOverlay(true, false, true).draw);      // Play: no selection outline
    CHECK(PlanPhysicsOverlay(true, true, false).draw);            // Play + toggle: whole world
    CHECK(PlanPhysicsOverlay(true, true, false).wholeWorld);
    CHECK(PlanPhysicsOverlay(false, true, true).wholeWorld);      // toggle wins over selection
}
```

`PhysicsDebugRichTest.cpp` — a case in that file's idiom (it already builds a world `w` with several bodies and a counting batcher; reuse its helpers):

```cpp
TEST_CASE("onlyBody draws exactly one outline and no other overlay", "[physics][debug]")
{
    // Two circles; filter to the second: one Circle call, zero Lines (no
    // contacts, velocity rays, COM crosses or orientation ticks).
    namespace P = Manifold2D::Physics;
    P::WorldDef wd; P::PhysicsWorld w(wd);
    P::BodyDef a; a.type = P::BodyType::Static;  a.position = P::Vec2(0, 0); a.shape = P::MakeCircle(P::Real(0.5)); a.density = P::Real(1);
    P::BodyDef b; b.type = P::BodyType::Dynamic; b.position = P::Vec2(3, 0); b.shape = P::MakeCircle(P::Real(0.5)); b.density = P::Real(1);
    w.AddBody(a);
    const P::BodyHandle hb = w.AddBody(b);
    CountingBatcher rec;                                    // this file's recording mock
    Arcane::PhysicsDebugDrawOptions opts;                   // defaults: contacts/velocity/COM/orientation ON
    opts.onlyBody = hb;
    Arcane::DrawPhysicsDebug(w, rec, opts);
    CHECK(rec.circles == 1);
    CHECK(rec.lines == 0);
    CHECK(rec.lastCircleCenter.x == Catch::Approx(3.0f));
}
```

- [ ] **Step 2: `GenerateProjects.bat`; build — expect FAIL** (no `onlyBody`, no `PhysicsOverlay.hpp`).
- [ ] **Step 3: `PhysicsDebugDraw.hpp`** — in `PhysicsDebugDrawOptions`, after `alpha`:

```cpp
        // ---- one-body filter (2026-09-11 physics wiring, spec s6.3) ---------
        // When set, ONLY this body's shape outline is drawn -- no contacts,
        // AABBs, velocity rays, COM crosses, orientation ticks or manifolds --
        // so the editor can outline the SELECTED entity's collider in Edit mode
        // without the whole-world overlay (the Unity collider gizmo). Null (the
        // default) is every existing caller: the whole world, every flag honoured.
        std::optional<Manifold2D::Physics::BodyHandle> onlyBody;
```

(`#include <optional>`; `BodyHandle` is a Manifold2D type — this header already includes `<Manifold2D/Physics/PhysicsTypes.hpp>` or forward-references it; use whichever the header already does — a full include is fine, the editor and tests both carry the Manifold2D row.) `.cpp`: in the body loop, after `if (!world.Alive(i)) continue;` add `if (opts.onlyBody && *opts.onlyBody != world.HandleOf(i)) continue;`; inside the loop, gate every non-outline emission (AABB, velocity, COM, orientation) with `!opts.onlyBody &&` in addition to its flag; wrap the contacts block and the manifold/broadphase blocks in `if (!opts.onlyBody)`.
- [ ] **Step 4: `ArcaneEditor/src/Scene/PhysicsOverlay.hpp`** (NEW):

```cpp
#pragma once
// The overlay decision (spec 2026-09-11-physics-2d-wiring s6.3), pure so it
// is testable device-less and the ImGui frame has nothing to decide:
//   * selected-body outline -- ALWAYS in Edit mode when the primary selection
//     carries a live body (the authoring feedback; Unity's collider gizmo);
//   * whole world (outlines + contacts) -- only behind View -> Physics Overlay,
//     Edit and Play alike; session state, never persisted (ruling R5).
namespace Arcane::Editor
{
    struct PhysicsOverlayPlan
    {
        bool draw       = false;
        bool wholeWorld = false;
    };

    [[nodiscard]] constexpr PhysicsOverlayPlan PlanPhysicsOverlay(bool inPlayMode, bool overlayToggled,
                                                                  bool hasSelectedBody) noexcept
    {
        PhysicsOverlayPlan p;
        p.wholeWorld = overlayToggled;
        p.draw       = overlayToggled || (!inPlayMode && hasSelectedBody);
        return p;
    }
}
```

- [ ] **Step 5: The editor.** `EditorApp.hpp`: `bool m_physicsOverlay = false;   // View -> Physics Overlay (spec s6.3, session-only)` beside `m_editSchedule`. `EditorPanels.hpp`: `bool togglePhysicsOverlay = false;   // View -> Physics Overlay` in `MenuRequests`; `BeginDockSpace(...)` gains a `bool physicsOverlayOn` parameter after `hasAssetSelection`. `EditorPanels.cpp`: between the `Assets` and `Window` menus:

```cpp
            if (ImGui::BeginMenu("View"))
            {
                // The whole-world physics overlay (outlines + contacts), Edit
                // and Play alike -- spec 2026-09-11-physics-2d-wiring s6.3. The
                // SELECTED body's outline needs no toggle: it is always drawn
                // in Edit mode. Session state, deliberately not persisted.
                if (ImGui::MenuItem("Physics Overlay", nullptr, physicsOverlayOn))
                    requests.togglePhysicsOverlay = true;
                ImGui::EndMenu();
            }
```

`EditorAppFrame.cpp`: pass `m_physicsOverlay` at the `BeginDockSpace` call; where the other `requests.*` are acted on: `if (requests.togglePhysicsOverlay) m_physicsOverlay = !m_physicsOverlay;`. In `SubmitSceneToBatcher`, directly after `m_runtime->Loop().SubmitRender();` and before the gizmo comment:

```cpp
        // Physics overlay (spec 2026-09-11-physics-2d-wiring s6.3), drawn AFTER
        // the sprites and BEFORE the gizmo, through the SAME camera and the
        // SAME interp buffer + alpha the sprites used, so overlay and sprite
        // agree to the bit. The decision is PlanPhysicsOverlay (pure, tested).
        {
            Astra::Registry& reg = m_runtime->Registry();
            const Arcane::PhysicsResource* phys = reg.GetResource<Arcane::PhysicsResource>();
            std::optional<Manifold2D::Physics::BodyHandle> selectedBody;
            if (m_selection.HasSelection())
                if (const auto* ref = std::as_const(reg).GetComponent<Arcane::PhysicsBodyRef>(m_selection.Primary()))
                    if (phys && phys->world && phys->world->IsValid(ref->handle))
                        selectedBody = ref->handle;
            const Arcane::Editor::PhysicsOverlayPlan plan =
                Arcane::Editor::PlanPhysicsOverlay(InPlayMode(), m_physicsOverlay, selectedBody.has_value());
            if (plan.draw && phys && phys->world)
            {
                const Arcane::RenderContext2D* ctx = reg.GetResource<Arcane::RenderContext2D>();
                Arcane::PhysicsDebugDrawOptions opts;
                if (ctx) { opts.cameraOffset = ctx->cameraOffset; opts.zoom = ctx->zoom; opts.alpha = ctx->alpha; }
                opts.interp = reg.GetResource<Arcane::PhysicsInterpBuffer>();
                opts.drawVelocities = opts.drawComMarkers = opts.drawOrientations = false;   // outlines + contacts (spec)
                opts.drawContacts   = plan.wholeWorld;
                if (!plan.wholeWorld) opts.onlyBody = selectedBody;
                Arcane::DrawPhysicsDebug(*phys->world, b, opts);
            }
        }
```

(Includes in `EditorAppFrame.cpp`: `<Arcane/Render/PhysicsDebugDraw.hpp>`, `<Arcane/Scene/PhysicsSystem.hpp>` (for `PhysicsResource`), `"Scene/PhysicsOverlay.hpp"`, `<optional>`, `<utility>`.) `RenderContext2D` is set by `SetRenderContext(&b)` two lines above, so `ctx` carries this frame's camera and alpha.
- [ ] **Step 6: Build `Arcane.slnx` Debug; run `"[editor]"`, `"[physics]"` — PASS.** Then ONE observable launch: `bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject --frames 60` (absolute path; exit 0; no `AbiMismatch`; no `plugin: initial load failed`). Full `~[gpu]` — green. Delta: **+2 cases**. Commit — `feat(editor): physics overlay -- selected-body outline in Edit mode, View -> Physics Overlay for the whole world; DrawPhysicsDebug onlyBody filter` + trailer.

---

### Task 9: `physics.arcscene` + the falling-body witness (spec §8)

**Files:**
- Create: `ReferenceProject/Content/scenes/physics.arcscene`
- Test: `ArcaneTests/src/WitnessScenariosTest.cpp` (one new `[witness][gpu]` case)

**Interfaces:** Consumes Task 3's v5 format, Task 4's `PhysicsSettings`, Task 5/6's runtime. The scene's Guid is **`4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90`** (fixed; the asset registry scans `Content/` and registers it by the embedded `id`). It is **not** the boot scene.

**Geometry (world +Y down; camera `orthographicSize` 2.0 at the origin; the headless host is 1280×720 so `zoom = 360 / 2 = 180 px/m` and world (0,0) is pixel (640, 360)):**
- `Ground` — static `Aabb` halfW 3.0 / halfH 0.25 at (0, +1.0): spans y ∈ [0.75, 1.25] m → pixels y 495..585. Rect sprite scale (6, 0.5, 1).
- `Crate` — dynamic `Aabb` halfW 0.5 / halfH 0.5 (`fixedRotation: true` — the Manifold2D dynamic-AABB invariant) at (0, −1.0): authored span y ∈ [−1.5, −0.5] → pixels 90..270. It falls 1.25 m (rest centre y = 0.75 − 0.5 = 0.25 → pixel 405; span 315..495). Fall time √(2·1.25/9.81) ≈ 0.50 s = 30 steps; 60 steps is ample. Rect sprite scale (1, 1, 1).
- `Ball` — dynamic `Circle` r 0.5 at (−2, −1.0), Circle sprite scale (1,1,1). `Pill` — dynamic `Capsule` r 0.25 / halfLen 0.5 at (+2, −1.0), Capsule sprite scale (1.5, 0.5, 1). Neither is probed.
- Scene root carries `PhysicsSettings { gravity: [0, 9.81] }` — the override path exercised by content (same value as the default, deliberately: the witness must not depend on it).

- [ ] **Step 1: The scene** — write `ReferenceProject/Content/scenes/physics.arcscene` (Identity ids are fresh 64-bit pairs; `parent: 0` = the root at index 0, `-1` = none):

```json
{
  "assets": [],
  "entities": [
    {
      "components": {
        "Arcane::Camera": { "active": true, "orthographicSize": 2.0 },
        "Arcane::Identity": { "id": { "hi": 4980164511003216701, "lo": 13316602907131203921 }, "name": "PhysicsScene" },
        "Arcane::PhysicsSettings": { "gravity": [0.0, 9.81] },
        "Arcane::Transform": { "position": [0.0, 0.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0] }
      },
      "parent": -1
    },
    {
      "components": {
        "Arcane::Identity": { "id": { "hi": 4980164511003216702, "lo": 13316602907131203922 }, "name": "Ground" },
        "Arcane::RigidBody2D": { "bullet": false, "fixedRotation": false, "linearDamping": 0.0, "mass": 0.0, "type": "Static", "velocity": [0.0, 0.0] },
        "Arcane::Collider2D": { "fixtures": [
          { "categoryBits": 1, "density": 1.0, "friction": 0.6, "halfH": 0.25, "halfLen": 0.0, "halfW": 3.0, "isSensor": false, "kind": "Aabb", "localAngle": 0.0, "localPos": [0.0, 0.0], "maskBits": 4294967295, "radius": 0.5, "restitution": 0.0 }
        ] },
        "Arcane::SpriteRenderer": { "material": { "hi": 0, "lo": 0 }, "orderInLayer": 0, "shape": "Rect", "sortingLayer": 0, "sprite": { "hi": 0, "lo": 0 }, "tint": [0.35, 0.35, 0.4, 1.0] },
        "Arcane::Transform": { "position": [0.0, 1.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [6.0, 0.5, 1.0] }
      },
      "parent": 0
    },
    {
      "components": {
        "Arcane::Identity": { "id": { "hi": 4980164511003216703, "lo": 13316602907131203923 }, "name": "Crate" },
        "Arcane::RigidBody2D": { "bullet": false, "fixedRotation": true, "linearDamping": 0.0, "mass": 0.0, "type": "Dynamic", "velocity": [0.0, 0.0] },
        "Arcane::Collider2D": { "fixtures": [
          { "categoryBits": 1, "density": 1.0, "friction": 0.6, "halfH": 0.5, "halfLen": 0.0, "halfW": 0.5, "isSensor": false, "kind": "Aabb", "localAngle": 0.0, "localPos": [0.0, 0.0], "maskBits": 4294967295, "radius": 0.5, "restitution": 0.0 }
        ] },
        "Arcane::SpriteRenderer": { "material": { "hi": 0, "lo": 0 }, "orderInLayer": 1, "shape": "Rect", "sortingLayer": 0, "sprite": { "hi": 0, "lo": 0 }, "tint": [0.9, 0.6, 0.2, 1.0] },
        "Arcane::Transform": { "position": [0.0, -1.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0] }
      },
      "parent": 0
    },
    {
      "components": {
        "Arcane::Identity": { "id": { "hi": 4980164511003216704, "lo": 13316602907131203924 }, "name": "Ball" },
        "Arcane::RigidBody2D": { "bullet": false, "fixedRotation": false, "linearDamping": 0.0, "mass": 0.0, "type": "Dynamic", "velocity": [0.0, 0.0] },
        "Arcane::Collider2D": { "fixtures": [
          { "categoryBits": 1, "density": 1.0, "friction": 0.6, "halfH": 0.5, "halfLen": 0.0, "halfW": 0.5, "isSensor": false, "kind": "Circle", "localAngle": 0.0, "localPos": [0.0, 0.0], "maskBits": 4294967295, "radius": 0.5, "restitution": 0.2 }
        ] },
        "Arcane::SpriteRenderer": { "material": { "hi": 0, "lo": 0 }, "orderInLayer": 1, "shape": "Circle", "sortingLayer": 0, "sprite": { "hi": 0, "lo": 0 }, "tint": [0.3, 0.7, 0.9, 1.0] },
        "Arcane::Transform": { "position": [-2.0, -1.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0] }
      },
      "parent": 0
    },
    {
      "components": {
        "Arcane::Identity": { "id": { "hi": 4980164511003216705, "lo": 13316602907131203925 }, "name": "Pill" },
        "Arcane::RigidBody2D": { "bullet": false, "fixedRotation": false, "linearDamping": 0.0, "mass": 0.0, "type": "Dynamic", "velocity": [0.0, 0.0] },
        "Arcane::Collider2D": { "fixtures": [
          { "categoryBits": 1, "density": 1.0, "friction": 0.6, "halfH": 0.5, "halfLen": 0.5, "halfW": 0.5, "isSensor": false, "kind": "Capsule", "localAngle": 0.0, "localPos": [0.0, 0.0], "maskBits": 4294967295, "radius": 0.25, "restitution": 0.0 }
        ] },
        "Arcane::SpriteRenderer": { "material": { "hi": 0, "lo": 0 }, "orderInLayer": 1, "shape": "Capsule", "sortingLayer": 0, "sprite": { "hi": 0, "lo": 0 }, "tint": [0.7, 0.9, 0.4, 1.0] },
        "Arcane::Transform": { "position": [2.0, -1.0, 0.0], "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.5, 0.5, 1.0] }
      },
      "parent": 0
    }
  ],
  "id": "4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90",
  "version": 5
}
```

(Field names come from the reflect blocks: `RigidBody2D{type, velocity, mass, linearDamping, fixedRotation, bullet}`, `Fixture{kind, radius, halfLen, halfW, halfH, localPos, localAngle, density, friction, restitution, categoryBits, maskBits, isSensor}`; enum values by name. If `LoadJson` refuses a field name, the reflect block is the authority — fix the file, not the code.)

- [ ] **Step 2: Prove it loads in-process first.** Open it through the editor: `bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Arcane\ReferenceProject --scene 4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90 --frames 60 --screenshot D:\dev\starworks\Arcane\bin\t9-physics.png` — exit 0, no load error in the log, and the PNG shows four sprites (a wide grey ground, an orange square, a blue disc, a green pill) — describe it in the report, then delete the PNG. (Editor = Edit mode: nothing moves; the selected-body outline needs a selection, so none is expected here.)
- [ ] **Step 3: The witness** — append to `WitnessScenariosTest.cpp`:

```cpp
TEST_CASE("W4: a dynamic body authored in physics.arcscene falls under the runtime host",
          "[witness][gpu]")
{
    // The whole chain, observed from OUTSIDE: the v5 scene with Collider2D
    // fixtures loads, Runtime installs PhysicsSystem and mints the world,
    // fixedUpdate steps it 60 times at 1/60 s (--headless pins the sim dt),
    // PASS 4 writes the pose back, propagation composes it, the sprite
    // draws where the body ended. Geometry (physics.arcscene, +Y down,
    // 1280x720, ortho 2.0 -> 180 px/m, origin at (640,360)): the Crate is
    // authored spanning pixels y 90..270 and comes to rest on the Ground
    // spanning 315..495, so a pick at its authored centre finds NOTHING and
    // a pick at its resting centre finds "Crate". No render change is
    // claimed: this scene is not the boot scene and no golden covers it.
    WitnessScratch scratch(StagedRuntimeDir(), "w4-physics-falls");
    WitnessRun run = RunWitness(HostInv(scratch,
        { "--scene", "4f6a1c2e-7b3d-4e8a-9c1f-2d5b6e7a8f90",
          "--probe", "pick@640,120", "--probe", "pick@640,405" }));
    INFO("host stdout: " << run.stdoutPath.string());
    INFO("host stderr: " << run.stderrPath.string());
    INFO("exit " << run.exitCode << ", wall " << run.wallMs << " ms, timedOut " << run.timedOut);
    REQUIRE_FALSE(GradeProcessFacts(run).has_value());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report["exitReason"].get<std::string>() == "frames-complete");
    REQUIRE(run.report.contains("probes"));
    REQUIRE(run.report["probes"].size() == 2);
    const nlohmann::json& authored = run.report["probes"][0];
    const nlohmann::json& resting  = run.report["probes"][1];
    REQUIRE(authored["kind"] == "pick");
    REQUIRE(resting["kind"]  == "pick");
    CHECK(authored["entity"].is_null());                         // it left
    REQUIRE(resting["entity"].is_string());
    CHECK(resting["entity"].get<std::string>() == "Crate");      // and landed here
}
```

- [ ] **Step 4: Build order (a Content change is staged by `Arcane.slnx`'s post-build; no header changed): `msbuild Arcane.slnx /p:Configuration=Debug /m`.** Run `"[witness]"` FROM the exe dir — W1, W3 and W4 pass. If W4's picks disagree with the arithmetic above, read the report's `probes` entries and the host log FIRST (a wrong pixel is a geometry slip in this task; a null at BOTH pixels means physics did not run — that is a Task 5/6 defect, report it, do not tune pixels around it).
- [ ] **Step 5: Debug UNFILTERED suite — all pass.** Release build order (`ReferenceProject.slnx` `/t:Rebuild` → `Arcane.slnx`) and Release `~[gpu]` — same counts as Debug `~[gpu]`. Delta: **+1 case** (W4 is `[gpu]`: outside `~[gpu]`, so the `~[gpu]` count is unchanged here and the unfiltered count rises by one). Commit — `feat(reference): physics.arcscene (ground + three dynamic bodies, scene-root PhysicsSettings) + W4 falling-body witness` + trailer.

---

### Task 10: Gacha — restamp `Aphelyon.arcproj` 27 → 28 and rebuild `Aphelyon.dll` (spec §9)

**Files:** Modify (Gacha repo): `Game/Aphelyon.arcproj` (`"abi": 27` → `28`). Rebuild: `Game/Binaries/Aphelyon.dll` (untracked output).

- [ ] **Step 1:** `Game/Aphelyon.arcproj` → `"abi": 28`. (No `Aphelyon.cpp` change: the grep in Task 2's ledger is what proves the module compiles unchanged — re-run it here and paste the empty result in the report.)
- [ ] **Step 2: Rebuild** (`ARCANE_SDK=D:\dev\starworks\Arcane`, set per invocation if the process env is stale): `cd D:\dev\starworks\Gacha\Game && msbuild Aphelyon.slnx /p:Configuration=Debug /m /t:Rebuild` — 0 warnings / 0 errors; `Game/Binaries/Aphelyon.dll` timestamp is now. (No premake regenerate: the include surface is unchanged.)
- [ ] **Step 3: One launch in the v28 host:** `D:\dev\starworks\Arcane\bin\Debug-windows-x86_64-md\ArcaneEditor\ArcaneEditor.exe --project D:\dev\starworks\Gacha\Game --frames 60` (the Debug host must be the staged config — Task 9 ended in Release: rebuild `ReferenceProject.slnx` Debug `/t:Rebuild` first, then this launch). Exit 0; `plugin loaded (gen 1)`; no `AbiMismatch`; no `plugin: initial load failed`.
- [ ] **Step 4: Commit (Gacha)** — `git add Game/Aphelyon.arcproj` — `chore(game): restamp Aphelyon.arcproj to engine ABI 28` + trailer (`Game/Binaries/` is untracked; never add it).

---

### Task 11: Plan 1 close — sweep, golden gate, final counts, handoff

- [ ] **Step 1: Sweeps.** (a) `git grep -n "PhysicsSystem\|PhysicsResource\|PhysicsInterpBuffer" -- ReferenceProject/Source build/arcane.lua` returns NOTHING (the module surface stayed Manifold2D-free). (b) The v28 ledger's grep over both game-module trees is still empty. (c) `git grep -riw "Serializable, false" -- ArcaneClient/src/Arcane/Scene/PhysicsComponents.hpp` shows `PhysicsBodyRef`'s two rows only (Collider2D's is gone).
- [ ] **Step 2: Build order, both configs, foreground, `/t:Rebuild` on ReferenceProject for each** (Debug first): Debug unfiltered + `~[gpu]` (+ a `-r json -o D:\dev\starworks\Arcane\bin\t11-debug-nogpu.json` run for the guard); Release `~[gpu]` (+ `-r json`). `scripts/check-baselines.ps1 -ReportPath <json> -Configuration <cfg> -Invocation "~[gpu]"` — a RISE over 56216/1632 on both metrics, exit 0, both configs. Derive the final counts; attribute: T2 +1, T3 +3, T4 +4, T5 +6, T6 +5, T7 +2, T8 +2 = **+23 `~[gpu]` cases** expected, T9 +1 unfiltered-only; state the measured assertion delta. Then `powershell -ExecutionPolicy Bypass -File scripts\golden-gate.ps1 -Configuration Release` — **4/4 lanes `diffCount=0`, exit 0** (the gate rebuilds ReferenceProject Release itself).
- [ ] **Step 3: Book the rise** into `scripts/automation-baselines.json` (Debug + Release rows to the measured figures; a dated `///` paragraph in `note` in the file's voice attributing by task and case name; the `measured` sentence rewritten with the previous one pushed into "Prior measurements:"; Dist untouched and said so). Re-run the guard: `+0/+0`, exit 0, both configs. Commit — `chore(tests): baselines -- 2D physics wiring plan 1 (56216 -> <n> attributed)` + trailer.
- [ ] **Step 4: Closeout notes** — append `## Closeout (2026-09-11)` to this plan doc in the shape of `docs/plans/2026-09-11-astra-adoption-plan2-interpolation.md`'s: HEAD range; state handed off (ABI 28; physics engine-owned and always on; `PhysicsSettings` + `.arcproj` block; the paused-pass fixes; the overlay; `physics.arcscene` + W4; Gacha at 28); the final counts with seeds and the attribution table; the guard and golden-gate outcomes; **the handoff to Plan 2** (the Inspector `FieldKind::Vector` editor, spec §7.3 — until it lands, fixtures are authored by hand in `.arcscene`); follow-ups recorded not actioned: `SetGravity` upstream in Manifold2D (so a gravity edit stops re-minting the world), vectors of scalar elements in the JSON bridge; every controller `Ruling:` transcribed from the SDD ledger; deferred minors listed. Commit — `docs: 2D physics wiring plan 1 closeout notes` + trailer.

---

## Self-review record (run at authoring time)

**Spec coverage — every clause to a task:**

| Spec | Task |
|---|---|
| §4.1 `RequiresExclusive`, `Before<TransformPropagationSystem>`, PASS 4 gated on `stepWorld` | T5 |
| §4.1a tracked `Collider2D`/`RigidBody2D`; paused PASS 1 re-mint criteria; `Modified` stamps | T5 (PASS 1.5 `PhysicsBodyRef` auto-add is an addition the code exposed: the Inspector can never add it) |
| §4.2 `InstallEngineSystems`, ctor + every PluginHost path | T6 (via `ClearSystems` re-installing — one site instead of three) |
| §4.3 `EnsurePhysics` per frame in both hosts; mint-if-absent; gravity refresh | T6 (refresh = world re-mint; the vendored `PhysicsWorld` has no `SetGravity` — spec amended) |
| §4.4 `PhysicsEditPass` | T6, called by T7 |
| §4.5 interpolation self-wiring | T6 (buffer set by Ensure), pinned in T7's Play test (`captured` false after Stop) |
| §5 `.arcproj` block, `PhysicsSettings` on the scene root, resolution | T4 (data), T6 (`ResolvedGravity`) |
| §6.1 edit pass before propagation | T7 |
| §6.2 Play/Stop/Pause/Step | T7 (test), nothing new in code |
| §6.3 overlay, both visibilities, `onlyBody` | T8 |
| §7.1 Astra `FieldInfo` | T1, vendored T2 |
| §7.2 container branch, `Serializable(false)` off, v5 | T3 (reflected-struct elements only — spec amended) |
| §7.3 Inspector | **Plan 2** |
| §8 scene + witness + per-piece pins | T9; pins across T3–T8; Plan 2's owed α=0.25 case rides in T6's ordering pin (it asserts the composed world position, not the blend direction — the α=0.25 pin is deferred to Plan 2 with the integration test it belongs to; recorded) |
| §9 ABI 28, build ritual, baseline, Gacha | T2, every task, T11, T10 |

**Type consistency:** `FieldInfo::{elementTypeHash, elementSize, vectorSize, vectorResize, vectorElement, vectorErase, vectorInsert}` (T1) are the names T2's smoke case and T3's `WriteVector`/`ReadVector` use. `PhysicsSettings{gravity}` and `ProjectManifest::PhysicsConfig{gravity}` / `.physics` (T4) are what T6's `ResolvedGravity` reads and T9's scene writes. `Runtime::{InstallEngineSystems, EnsurePhysics, PhysicsEditPass, ResolvedGravity}` (T6) are what T7 binds and T8 relies on (`PhysicsResource`/`PhysicsInterpBuffer` present). `PhysicsDebugDrawOptions::onlyBody` and `PlanPhysicsOverlay(bool, bool, bool)` (T8) are used only in T8. `PhysicsSystem::BeforeTypes` is Astra's `SystemTraits` alias (`System.hpp:81`), so T5's static pin compiles.

**Contradictions found in the code, resolved:**
1. Spec §4.3 "push gravity to the world": the vendored `PhysicsWorld` exposes `Gravity()` but no setter → T6 re-mints the world on a gravity change; `SetGravity` upstream is a recorded follow-up.
2. Spec §7.2 "each element goes through the same dispatch a field does — arithmetic, glm, enums, structs": the scalar helpers key off a `FieldInfo`, elements have none; no roster field is a vector of scalars → T3 handles reflected-struct elements only, fails loud otherwise; the existing `HasVector{vector<int>}` "unsupported" pin stays true by design.
3. Spec §9 "`PhysicsComponents.hpp` is on the game-module include surface": it is not (neither module includes it; it needs Manifold2D). The v28 ledger says so correctly; the spec line is amended.
4. PASS 2 requires a `PhysicsBodyRef` the Inspector can never add (structure-locked) → T5's PASS 1.5.

**Known intentional gaps, each with its owner:** the Inspector vector editor (Plan 2); `SetGravity` upstream (Manifold2D follow-up); scalar-element vectors in the bridge (follow-up, no consumer); the α=0.25 blend-direction pin (Plan 2, with the Inspector-era integration case); Gacha's own scenes carry no physics content (the user's call — the block and roster are there).
