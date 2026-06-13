# Arcane M4 — Simulation Substrate + Scene Vertical Slice — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the engine's simulation substrate (Astra Registry + enkiTS + per-phase
schedulers + RunLoop) and a minimal formal scene (transform hierarchy + sprite submission),
gated by an Astra 3.2 release that makes serialization format-flexible via a reflection-driven
field-visitor seam.

**Architecture:** Astra gains ONE format-agnostic `IFieldVisitor` slot on
`ComponentRegistry` (binary path untouched, no JSON dep) + an `AliasName` attribute, released
as 3.2 and re-vendored. Arcane wraps enkiTS as an `Astra::IWorkScheduler` (DLL-owned), then
the host owns an `Astra::Registry` driven by three per-phase `SystemScheduler`s and an
`Arcane::RunLoop`; scene components/systems/serializers are header-only and instantiate in the
host. A Playground slice and a `[gpu]` test render a moving parent/child sprite.

**Tech Stack:** C++23 (Arcane) / C++20 (Astra); Astra ECS (header-only); enkiTS; NVRHI +
Batcher2D; glm; nlohmann/json (Arcane only); GoogleTest (Astra) + Catch2 (Arcane); premake5
(Astra vs2022, Arcane vs2026); msbuild.

---

## Design notes (read before Task 1)

**Spec:** `docs/superpowers/specs/2026-06-13-arcane-m4-sim-substrate-scene.md`. This plan
implements it. Two refinements decided during planning (do not re-litigate):

1. **Module ownership rule — each `Astra::Registry` is touched by exactly ONE module.**
   Astra is header-only; `TypeID<T>::Value()` (ComponentID) is a per-module counter and
   `MetaRegistry::Instance()` is a per-module singleton, so a registry created in module A
   must not be mutated/queried from module B (IDs would disagree). Therefore:
   - **Header-only, host-instantiated:** `RunLoop`, `SystemSchedulers`, `Simulation`, and the
     entire **Scene** module (components, systems, registration, serializers). The host
     (Playground exe / a test exe) owns the registry; reflection + IDs are self-consistent
     within it. `MetaRegistry::Register` is idempotent (first-wins by hash), so the
     `ASTRA_REFLECT` blocks may live in a header and register once per module.
   - **DLL-exported (`ARCANE_API`):** only the **Jobs** layer (`JobSystem`), which owns the
     single process enkiTS `TaskScheduler` and hands out a
     `std::shared_ptr<Astra::IWorkScheduler>`. The DLL never touches the host's registry — it
     only runs the `std::function`s Astra's `ParallelExecutor` submits (safe across the /MD
     shared heap). This refines the spec's "ARCANE_API RunLoop" wording: header-only RunLoop
     keeps hosts equally thin (they include the header) and sidesteps the cross-module split.

2. **Render phase is single-threaded.** `RunLoop::Advance` runs FixedUpdate + Update through
   the enkiTS `ParallelExecutor`; `RunLoop::SubmitRender` runs the Render scheduler with the
   inline `SequentialExecutor` (the no-executor `Execute(reg)` overload), because `Batcher2D`
   is not thread-safe and must be recorded on the thread that called `Begin`.

**Branches & repos:**
- Phase 0 is in `D:\dev\starworks\Astra` on branch `dev`; release via `D:\dev\github\Astra`
  (branch `main` -> cut `3.2`). Both clone `github.com/T3mps/Astra.git`.
- Phases 1-5 are in `D:\dev\starworks\Gacha` on a NEW branch `feature/arcane-m4-sim-scene`
  off `main`.

**Build commands (both repos use the VS2026 MSBuild):** `msbuild` below means the VS MSBuild
(e.g. `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe`)
— use the full path if `msbuild` is not on PATH.
- **Astra regen:** from `D:/dev/starworks/Astra`, run `premake5 vs2022` **directly** (the
  `scripts\generate_vs2022.bat` shim ends in `pause` and will hang). If `premake5` is not on
  PATH, use `D:/dev/starworks/Gacha/ThirdParty/premake5/premake5.exe vs2022`.
- **Astra build/run:** `msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Debug /m`
  then `"D:/dev/starworks/Astra/bin/Debug-windows-x86_64/AstraTest/AstraTest.exe"`. (premake's
  `vs2022` action writes `Astra.sln`; if only `Astra.slnx` is present, build that.)
- **Arcane regen:** from `D:/dev/starworks/Gacha/Arcane`, run
  `../ThirdParty/premake5/premake5.exe vs2026` **directly** (`GenerateProjects.bat` pauses).
- **Arcane build:** `msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m`
  (and `/p:Configuration=Release`).
- **Arcane tests:** run from the output dir (the exe loads `Arcane.dll` beside it):
  `D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe "<filter>"`.
  Exclude GPU on headless machines with `"~[gpu]"`.

**Engine rules:** /MD, no `/fp:fast` in Arcane, UTF-8 without BOM, ASCII in comments, C++23
(Astra stays C++20). Every engine component is `ASTRA_REFLECT`-annotated. Commit trailer on
every commit: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`. NEVER run `db-reset`,
`clean --deep`, `docker compose down -v`, or `tauri dev`.

---

# PHASE 0 — Astra 3.2 (serialization flexibility)

All Phase 0 work is in `D:\dev\starworks\Astra`.

## Task A0: Prepare the Astra `dev` branch

**Files:** none (git only).

- [ ] **Step 1: Switch to / create `dev` and sync with main**

```bash
git -C "D:/dev/starworks/Astra" fetch origin
git -C "D:/dev/starworks/Astra" checkout dev || git -C "D:/dev/starworks/Astra" checkout -b dev
git -C "D:/dev/starworks/Astra" status
```

Expected: on branch `dev`, working tree clean (or only expected local state).

- [ ] **Step 2: Confirm baseline builds + tests green BEFORE changes**

Regenerate then build+run (full path msbuild as noted above):

```bash
cd "D:/dev/starworks/Astra" && premake5 vs2022
msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Debug /m
"D:/dev/starworks/Astra/bin/Debug-windows-x86_64/AstraTest/AstraTest.exe"
```

Expected: build succeeds; AstraTest reports `[  PASSED  ]` for all tests, 0 failed. Record the
passing test count (baseline for the regression check in Task A3).

## Task A1: The `IFieldVisitor` seam (TDD)

**Files:**
- Test: `D:\dev\starworks\Astra\tests\Reflection\FieldVisitorTest.cpp` (create)
- Create: `D:\dev\starworks\Astra\include\Astra\Reflection\FieldVisitor.hpp`
- Modify: `D:\dev\starworks\Astra\include\Astra\Component\Component.hpp` (add slot + fwd decl)
- Modify: `D:\dev\starworks\Astra\include\Astra\Component\ComponentRegistry.hpp` (populate + helper)

- [ ] **Step 1: Write the failing test**

Create `tests/Reflection/FieldVisitorTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>
#include <Astra/Reflection/FieldVisitor.hpp>

#include <any>
#include <map>
#include <string>
#include <vector>

namespace
{
    struct Stats
    {
        int   hp = 0;
        float speed = 0.0f;
        bool  dead = false;
        int   derived = 0;   // opted OUT of serialization below
    };

    struct NoReflect   // registered as a component but never reflected
    {
        int x = 0;
    };

    // Collects the names of visited fields (write direction).
    class RecordingVisitor : public Astra::IFieldVisitor
    {
    public:
        std::vector<std::string> names;
        void Visit(const Astra::FieldInfo& field, void* /*instance*/) override
        {
            names.emplace_back(field.name);
        }
        bool IsWriting() const noexcept override { return true; }
    };

    // Writes each field's value into a name-keyed map (format = std::map; no JSON in Astra).
    class MapWriteVisitor : public Astra::IFieldVisitor
    {
    public:
        std::map<std::string, std::any>& out;
        explicit MapWriteVisitor(std::map<std::string, std::any>& o) : out(o) {}
        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            out[std::string(field.name)] = field.GetAny(instance);
        }
        bool IsWriting() const noexcept override { return true; }
    };

    // Reads each field's value back from the map into the instance.
    class MapReadVisitor : public Astra::IFieldVisitor
    {
    public:
        const std::map<std::string, std::any>& in;
        explicit MapReadVisitor(const std::map<std::string, std::any>& i) : in(i) {}
        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            auto it = in.find(std::string(field.name));
            if (it != in.end())
                field.SetAny(instance, it->second);
        }
        bool IsWriting() const noexcept override { return false; }
    };
}

ASTRA_REFLECT_TYPE(Stats)
    ASTRA_REFLECT_FIELD(Stats, hp)
    ASTRA_REFLECT_FIELD(Stats, speed)
    ASTRA_REFLECT_FIELD(Stats, dead)
    ASTRA_REFLECT_FIELD(Stats, derived)
        ASTRA_REFLECT_ATTR(Serializable, false)
ASTRA_END_REFLECT_TYPE()

TEST(FieldVisitor, EnumeratesSerializableFieldsInOrder)
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Stats>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Stats>::Value());
    ASSERT_NE(desc, nullptr);
    ASSERT_NE(desc->visitFields, nullptr);

    Stats s{};
    RecordingVisitor rec;
    desc->visitFields(&s, rec);

    // "derived" is Serializable(false) and must be skipped; order is declaration order.
    ASSERT_EQ(rec.names.size(), 3u);
    EXPECT_EQ(rec.names[0], "hp");
    EXPECT_EQ(rec.names[1], "speed");
    EXPECT_EQ(rec.names[2], "dead");
}

TEST(FieldVisitor, RoundTripsThroughAFormatAgnosticMap)
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Stats>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Stats>::Value());
    ASSERT_NE(desc, nullptr);

    Stats a{};
    a.hp = 42; a.speed = 3.5f; a.dead = true; a.derived = 99;

    std::map<std::string, std::any> blob;
    MapWriteVisitor w(blob);
    desc->visitFields(&a, w);

    Stats b{};
    MapReadVisitor r(blob);
    desc->visitFields(&b, r);

    EXPECT_EQ(b.hp, 42);
    EXPECT_FLOAT_EQ(b.speed, 3.5f);
    EXPECT_TRUE(b.dead);
    EXPECT_EQ(b.derived, 0);   // skipped on both sides -> stays default
}

TEST(FieldVisitor, UnreflectedComponentHasNullSlot)
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<NoReflect>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<NoReflect>::Value());
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->visitFields, nullptr);
}
```

- [ ] **Step 2: Regenerate (pick up the new test file) and build — expect FAIL**

```bash
cd "D:/dev/starworks/Astra" && premake5 vs2022
msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Debug /m
```

Expected: compile error — `Astra/Reflection/FieldVisitor.hpp` not found and
`desc->visitFields` has no member.

- [ ] **Step 3: Add `include/Astra/Reflection/FieldVisitor.hpp`**

```cpp
#pragma once

#include "FieldInfo.hpp"

namespace Astra
{
    // Format-agnostic field visitor. An end user (e.g. the Arcane engine)
    // implements this to drive ANY serialization format by walking reflection.
    // Astra ships NO format backend beyond the built-in binary path -- a JSON,
    // protobuf, or editor backend lives entirely in the consumer.
    class IFieldVisitor
    {
    public:
        virtual ~IFieldVisitor() = default;

        // Invoked once per serializable reflected field of a component instance.
        // `instance` is the component base pointer; read or write the field via
        // field.GetAny(instance) / field.SetAny(instance, value), the typed
        // field.Get<T>/Set<T>, or field.GetPtr<T>(instance). For a nested
        // reflected struct, look its type up by field.typeHash in MetaRegistry
        // and recurse (the consumer owns recursion policy and POD-math fallbacks).
        virtual void Visit(const FieldInfo& field, void* instance) = 0;

        // Direction hint so a single visitor type can serve read and write.
        ASTRA_NODISCARD virtual bool IsWriting() const noexcept = 0;
    };
}
```

- [ ] **Step 4: Add the slot to `ComponentDescriptor` (`Component/Component.hpp`)**

Find the forward declarations near the top of `namespace Astra`:

```cpp
    class BinaryWriter;
    class BinaryReader;
    class TypeMeta;  // Forward declaration for reflection integration
```

Add one line after them:

```cpp
    class IFieldVisitor;  // Forward declaration for the reflection-driven visitor seam
```

In the `ComponentDescriptor` `using` block (after `DeserializeVersionedFn`), add:

```cpp
        using VisitFieldsFn = void(void* instance, IFieldVisitor& visitor);  // reflection-driven, format-agnostic
```

In the member list, immediately after `const TypeMeta* meta = nullptr;`, add:

```cpp
        // Reflection-driven, format-agnostic serialization seam. Populated from
        // TypeMeta at registration; null when the type is not reflected. The
        // binary path (serialize/deserialize above) is unaffected.
        VisitFieldsFn* visitFields = nullptr;
```

- [ ] **Step 5: Populate the slot in `ComponentRegistry.hpp`**

Add the include near the existing reflection include:

```cpp
#include "../Reflection/FieldVisitor.hpp"
```

In `RegisterComponentImpl<T>`, immediately after the existing
`desc.meta = MetaRegistry::Instance().Get<T>();` line, add:

```cpp
            // Reflection-driven visitor slot: null unless the type is reflected.
            desc.visitFields = desc.meta ? &VisitFields<T> : nullptr;
```

Add the static helper alongside the existing `Serialize<T>` family (in the `private:` section):

```cpp
        template<typename T>
        static void VisitFields(void* instance, IFieldVisitor& visitor)
        {
            const TypeMeta* meta = MetaRegistry::Instance().Get<T>();
            if (!meta) return;
            for (const FieldInfo& field : meta->fields)
            {
                if (!field.IsSerializable()) continue;  // honors Serializable(false)
                visitor.Visit(field, instance);
            }
        }
```

- [ ] **Step 6: Build + run the new tests — expect PASS**

```bash
msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Debug /m
"D:/dev/starworks/Astra/bin/Debug-windows-x86_64/AstraTest/AstraTest.exe" --gtest_filter=FieldVisitor.*
```

Expected: 3 tests pass, 0 fail.

- [ ] **Step 7: Commit**

```bash
git -C "D:/dev/starworks/Astra" add include/Astra/Reflection/FieldVisitor.hpp include/Astra/Component/Component.hpp include/Astra/Component/ComponentRegistry.hpp tests/Reflection/FieldVisitorTest.cpp
git -C "D:/dev/starworks/Astra" commit -m "feat(reflection): add format-agnostic IFieldVisitor seam to ComponentRegistry

One generic visitFields slot on ComponentDescriptor, populated from TypeMeta
in RegisterComponentImpl. Binary path untouched; no external dependency. Lets a
consumer drive any serialization format by walking reflection.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task A2: `AliasName` attribute (TDD)

**Files:**
- Modify: `D:\dev\starworks\Astra\include\Astra\Reflection\Attribute.hpp`
- Modify: `D:\dev\starworks\Astra\tests\Reflection\FieldVisitorTest.cpp`

- [ ] **Step 1: Add the failing test (append to `FieldVisitorTest.cpp`)**

Add a reflected type with an alias and a test. After the `Stats` reflect block add:

```cpp
namespace { struct Renamed { float value = 0.0f; }; }

ASTRA_REFLECT_TYPE(Renamed)
    ASTRA_REFLECT_FIELD(Renamed, value)
        ASTRA_REFLECT_ATTR(AliasName, "amount")
ASTRA_END_REFLECT_TYPE()
```

And append the test:

```cpp
TEST(FieldVisitor, AliasNameAttributeIsQueryable)
{
    const Astra::TypeMeta* meta = Astra::GetMeta<Renamed>();
    ASSERT_NE(meta, nullptr);
    const Astra::FieldInfo* field = meta->GetField("value");
    ASSERT_NE(field, nullptr);

    std::vector<std::string> aliases;
    field->ForEachAttribute<Astra::AliasName>(
        [&](const Astra::AliasName& a) { aliases.emplace_back(a.name); });

    ASSERT_EQ(aliases.size(), 1u);
    EXPECT_EQ(aliases[0], "amount");
}
```

- [ ] **Step 2: Build — expect FAIL** (`AliasName` undefined)

```bash
msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Debug /m
```

Expected: error — `AliasName` is not a member of `Astra`.

- [ ] **Step 3: Add `AliasName` to `Attribute.hpp`** (after the `Deprecated` attribute)

```cpp
    /**
     * Records a former serialized name for a field so name-keyed format loaders
     * can find a value written under the old name after a rename. Multiple
     * AliasName attributes may be attached (a field renamed more than once).
     * The built-in binary path is unaffected (it uses SerializationTraits Version).
     */
    struct AliasName : AttributeBase<AliasName>
    {
        std::string_view name;

        constexpr explicit AliasName(std::string_view formerName) noexcept
            : name(formerName) {}
    };
```

Add `#include <string_view>` at the top if not already present (it is used by other
attributes, so it should already be included).

- [ ] **Step 4: Build + run — expect PASS**

```bash
msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Debug /m
"D:/dev/starworks/Astra/bin/Debug-windows-x86_64/AstraTest/AstraTest.exe" --gtest_filter=FieldVisitor.*
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Astra" add include/Astra/Reflection/Attribute.hpp tests/Reflection/FieldVisitorTest.cpp
git -C "D:/dev/starworks/Astra" commit -m "feat(reflection): add AliasName attribute for field-rename migration

Name-keyed format loaders query it via FieldInfo::ForEachAttribute<AliasName>
to fall back to a former field name. Binary path unaffected.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task A3: Binary regression + Release build gate

**Files:** none (verification only).

- [ ] **Step 1: Full AstraTest run (Debug) — no regressions**

```bash
"D:/dev/starworks/Astra/bin/Debug-windows-x86_64/AstraTest/AstraTest.exe"
```

Expected: all tests pass; the total `[ PASSED ]` count equals the Task A0 baseline + 4 new
tests. Pay attention that `Serialization/*` and `Registry*` suites are still green (binary path
unchanged).

- [ ] **Step 2: Release build + run (proves no NDEBUG-only break)**

```bash
msbuild "D:/dev/starworks/Astra/Astra.sln" /p:Configuration=Release /m
"D:/dev/starworks/Astra/bin/Release-windows-x86_64/AstraTest/AstraTest.exe"
```

Expected: build + all tests pass. (No commit — verification only.)

## Task A4: Release — merge to main + cut `3.2` + bump version

**Files:**
- Modify: `D:\dev\starworks\Astra\include\Astra\Core\Version.hpp` (bump done on `dev` first)

- [ ] **Step 1: Bump the version on `dev`**

Edit `D:\dev\starworks\Astra\include\Astra\Core\Version.hpp`: change

```cpp
#define ASTRA_VERSION_MINOR 1
```

to

```cpp
#define ASTRA_VERSION_MINOR 2
```

(leave `MAJOR 3`, `PATCH 0`).

- [ ] **Step 2: Commit + push `dev`**

```bash
git -C "D:/dev/starworks/Astra" add include/Astra/Core/Version.hpp
git -C "D:/dev/starworks/Astra" commit -m "chore: bump Astra to 3.2.0

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git -C "D:/dev/starworks/Astra" push origin dev
```

- [ ] **Step 3: In the github clone, merge dev -> main and cut the `3.2` release branch**

```bash
git -C "D:/dev/github/Astra" fetch origin
git -C "D:/dev/github/Astra" checkout main
git -C "D:/dev/github/Astra" merge --no-ff origin/dev -m "merge: Astra 3.2 (format-agnostic field-visitor seam + AliasName)"
git -C "D:/dev/github/Astra" push origin main
git -C "D:/dev/github/Astra" checkout -b 3.2
git -C "D:/dev/github/Astra" push -u origin 3.2
git -C "D:/dev/github/Astra" checkout main
```

Expected: `main` advanced; branch `3.2` pushed. (If `D:/dev/github/Astra` has no `origin/dev`
ref yet, `git -C "D:/dev/github/Astra" fetch origin dev` first.)

## Task A5: Re-vendor Astra 3.2 into the Gacha repo

**Files:**
- Modify (overwrite): `D:\dev\starworks\Gacha\ThirdParty\Astra\include\Astra\**` (plain copy)

- [ ] **Step 1: Copy the updated headers over the vendored tree**

```bash
cp -r "D:/dev/starworks/Astra/include/Astra/." "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/"
```

- [ ] **Step 2: Verify the vendored copy is 3.2 and has the new seam**

```bash
grep -n "ASTRA_VERSION_MINOR" "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/Core/Version.hpp"
ls "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/Reflection/FieldVisitor.hpp"
grep -n "visitFields" "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/Component/Component.hpp"
grep -n "struct AliasName" "D:/dev/starworks/Gacha/ThirdParty/Astra/include/Astra/Reflection/Attribute.hpp"
```

Expected: `ASTRA_VERSION_MINOR 2`; `FieldVisitor.hpp` exists; `visitFields` present;
`AliasName` present.

- [ ] **Step 3: Commit the re-vendor in the Gacha repo** (still on whatever branch is current;
  the Phase-1 branch is created in Task B0, so commit this on `main` or stage it — prefer
  creating the branch first if not already on it. If still on `feature/setup-wizard` or
  `main`, switch to the new branch from Task B0 Step 1, then commit here.)

```bash
git -C "D:/dev/starworks/Gacha" add ThirdParty/Astra/include/Astra
git -C "D:/dev/starworks/Gacha" commit -m "chore(thirdparty): re-vendor Astra 3.2 (IFieldVisitor seam + AliasName)

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 1 — Jobs: enkiTS wired into Arcane

All remaining phases are in `D:\dev\starworks\Gacha`.

## Task B0: Feature branch + premake wiring

**Files:**
- Modify: `D:\dev\starworks\Gacha\Arcane\premake5.lua`

- [ ] **Step 1: Create the feature branch off main**

```bash
git -C "D:/dev/starworks/Gacha" checkout main
git -C "D:/dev/starworks/Gacha" pull
git -C "D:/dev/starworks/Gacha" checkout -b feature/arcane-m4-sim-scene
```

(If the Task A5 re-vendor commit landed on `main`/another branch, cherry-pick or ensure it is
present on this branch: `git -C "D:/dev/starworks/Gacha" log --oneline -1 -- ThirdParty/Astra`.)

- [ ] **Step 2: Add Astra + enkiTS to the `Arcane` (DLL) project and Astra to `Playground`**

In `Arcane/premake5.lua`, project `"Arcane"` `includedirs { ... }`, add after
`"%{IncludeDir.imgui}",`:

```lua
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
```

In project `"Arcane"` `links { "Core", "nvrhi", "msdfgen", "freetype", "imgui" }`, change to:

```lua
    links { "Core", "nvrhi", "msdfgen", "freetype", "imgui", "enkiTS" }
```

In project `"Playground"` `includedirs { ... }`, add after `"%{IncludeDir.imgui}",`:

```lua
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.glm}",
```

(`glm` may already be present in Playground includedirs; if so, do not duplicate.)

- [ ] **Step 3: Regenerate + build (no new code yet) — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

Expected: solution regenerates; full build succeeds (wiring is inert until used).

- [ ] **Step 4: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/premake5.lua
git -C "D:/dev/starworks/Gacha" commit -m "build(arcane): wire Astra + enkiTS into Arcane.dll and Playground

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task B1: `JobSystem` + enkiTS `IWorkScheduler` adapter (TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\JobSchedulerTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Jobs\JobSystem.hpp`
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Jobs\JobSystem.cpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/JobSchedulerTest.cpp`:

```cpp
// JobSystem hands Astra an enkiTS-backed IWorkScheduler. A registry parallel
// pass must visit every entity exactly once across worker threads.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>

#include <Astra/Registry/Registry.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace
{
    struct Counter { int value = 0; };
}

TEST_CASE("enkiTS work scheduler drives Astra parallel iteration", "[jobs]")
{
    Arcane::JobSystem jobs;
    std::shared_ptr<Astra::IWorkScheduler> sched = jobs.WorkScheduler();
    REQUIRE(sched != nullptr);
    REQUIRE(sched->WorkerCount() >= 1);

    Astra::Registry::Config cfg;
    cfg.workScheduler = sched;
    Astra::Registry reg(cfg);
    reg.GetComponentRegistry()->RegisterComponent<Counter>();

    constexpr int kN = 4096;
    for (int i = 0; i < kN; ++i)
        reg.CreateEntityWith(Counter{i});

    std::atomic<int> visited{0};
    auto view = reg.CreateView<Counter>();
    view.ParallelForEach([&](Astra::Entity, Counter& c)
    {
        c.value += 1;
        visited.fetch_add(1, std::memory_order_relaxed);
    });

    CHECK(visited.load() == kN);
}
```

- [ ] **Step 2: Build — expect FAIL** (`Arcane/Jobs/JobSystem.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

Expected: compile error — cannot open `Arcane/Jobs/JobSystem.hpp`.

- [ ] **Step 3: Create `Arcane/Jobs/JobSystem.hpp`**

```cpp
#pragma once

// Jobs: the engine owns ONE enkiTS TaskScheduler per process and exposes it to
// Astra through the IWorkScheduler seam. Astra creates no threads; this adapter
// is the only thread source for the simulation. Lives in Arcane.dll so the
// scheduler is a single shared instance; hosts receive a shared_ptr to inject
// into Registry::Config and Astra::ParallelExecutor.

#include <Arcane/Base/Api.hpp>

#include <Astra/Core/WorkScheduler.hpp>

#include <cstdint>
#include <memory>

namespace Arcane
{
    class ARCANE_API JobSystem
    {
    public:
        // threads == 0 -> enkiTS hardware default (GetNumHardwareThreads()).
        explicit JobSystem(uint32_t threads = 0);
        ~JobSystem();

        JobSystem(const JobSystem&) = delete;
        JobSystem& operator=(const JobSystem&) = delete;

        // The shared enkiTS-backed scheduler. Inject the SAME pointer into every
        // module / registry / executor that needs parallelism.
        std::shared_ptr<Astra::IWorkScheduler> WorkScheduler() const;

        uint32_t WorkerCount() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
}
```

- [ ] **Step 4: Create `Arcane/Jobs/JobSystem.cpp`**

```cpp
#include <Arcane/Jobs/JobSystem.hpp>

#include <TaskScheduler.h>

#include <functional>

namespace Arcane
{
    namespace
    {
        // Adapts enkiTS to Astra::IWorkScheduler. Internal to this TU: consumers
        // only ever see Astra::IWorkScheduler via JobSystem::WorkScheduler().
        class EnkiWorkScheduler : public Astra::IWorkScheduler
        {
        public:
            explicit EnkiWorkScheduler(enki::TaskScheduler& ts) : m_ts(ts) {}

            void ParallelFor(size_t count, size_t minBatch,
                             const std::function<void(size_t, size_t)>& fn) override
            {
                if (count == 0)
                    return;

                enki::TaskSet task(
                    static_cast<uint32_t>(count),
                    [&fn](enki::TaskSetPartition range, uint32_t /*threadnum*/)
                    {
                        fn(range.start, range.end);
                    });
                task.m_MinRange = static_cast<uint32_t>(minBatch == 0 ? 1 : minBatch);

                m_ts.AddTaskSetToPipe(&task);
                m_ts.WaitforTask(&task);   // calling thread participates in the work
            }

            size_t WorkerCount() const noexcept override
            {
                return static_cast<size_t>(m_ts.GetNumTaskThreads());
            }

        private:
            enki::TaskScheduler& m_ts;
        };
    }

    struct JobSystem::Impl
    {
        enki::TaskScheduler ts;
        std::shared_ptr<Astra::IWorkScheduler> adapter;
    };

    JobSystem::JobSystem(uint32_t threads) : m_impl(std::make_unique<Impl>())
    {
        if (threads == 0)
            m_impl->ts.Initialize();
        else
            m_impl->ts.Initialize(threads);
        m_impl->adapter = std::make_shared<EnkiWorkScheduler>(m_impl->ts);
    }

    JobSystem::~JobSystem()
    {
        // Drop the adapter before the scheduler shuts down (adapter holds a ref).
        m_impl->adapter.reset();
        m_impl->ts.WaitforAll();
    }

    std::shared_ptr<Astra::IWorkScheduler> JobSystem::WorkScheduler() const
    {
        return m_impl->adapter;
    }

    uint32_t JobSystem::WorkerCount() const noexcept
    {
        return m_impl->ts.GetNumTaskThreads();
    }
}
```

- [ ] **Step 5: Regenerate + build + run — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[jobs]"
```

Expected: 1 test, all assertions pass.

- [ ] **Step 6: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Jobs/JobSystem.hpp Arcane/Arcane/src/Arcane/Jobs/JobSystem.cpp Arcane/Tests/src/JobSchedulerTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/jobs): enkiTS-backed Astra::IWorkScheduler adapter

JobSystem owns the one-per-process enki::TaskScheduler and exposes it as a
shared Astra::IWorkScheduler. Test drives Astra ParallelForEach across workers.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 2 — Simulation substrate: schedulers + RunLoop

## Task C1: `SystemSchedulers` + `RunLoop` (header-only, TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\RunLoopTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Sim\SystemSchedulers.hpp`
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Sim\RunLoop.hpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/RunLoopTest.cpp`:

```cpp
// RunLoop: a fixed-timestep accumulator drives the FixedUpdate scheduler N times
// per real frame and the Update scheduler once, exposing a render alpha in [0,1).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

namespace
{
    struct Ticks { int fixed = 0; };
}

TEST_CASE("RunLoop runs a fixed-rate scheduler and clamps spikes", "[sim][runloop]")
{
    Astra::Registry reg;  // sequential fallback scheduler -- no jobs needed here
    reg.SetResource<Ticks>(Ticks{});

    Arcane::SystemSchedulers schedulers(nullptr);  // null -> sequential executor
    schedulers.fixedUpdate.AddSystem([](Astra::Registry& r)
    {
        r.GetResource<Ticks>()->fixed += 1;
    });

    Arcane::RunLoop::Config cfg;   // 60 Hz, maxStepsPerFrame default 5
    Arcane::RunLoop loop(reg, schedulers, cfg);

    // Advance ~1 second of wall time in 1/60 s frames -> ~60 fixed steps.
    for (int i = 0; i < 60; ++i)
    {
        double alpha = loop.Advance(1.0 / 60.0);
        CHECK(alpha >= 0.0);
        CHECK(alpha < 1.0);
    }
    const int afterOneSecond = reg.GetResource<Ticks>()->fixed;
    CHECK(afterOneSecond >= 58);
    CHECK(afterOneSecond <= 62);

    // A huge dt spike must be clamped to maxStepsPerFrame (no spiral of death).
    const int before = reg.GetResource<Ticks>()->fixed;
    loop.Advance(10.0);  // would be 600 steps unclamped
    const int stepsTaken = reg.GetResource<Ticks>()->fixed - before;
    CHECK(stepsTaken == cfg.maxStepsPerFrame);
}
```

- [ ] **Step 2: Build — expect FAIL** (headers missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

Expected: cannot open `Arcane/Sim/RunLoop.hpp` / `SystemSchedulers.hpp`.

- [ ] **Step 3: Create `Arcane/Sim/SystemSchedulers.hpp`**

```cpp
#pragma once

// The engine's phase layer over Astra's flat SystemScheduler. Astra has no
// stage/phase concept; the engine maintains one scheduler per phase and runs
// them in order. All share one (optional) enkiTS-backed executor.
//
// Header-only by design: the simulation Registry is owned by the host module,
// and Astra's TypeID/MetaRegistry are per-module -- a registry must be touched
// by exactly one module, so the schedulers that touch it live where it lives.

#include <Astra/System/SystemScheduler.hpp>
#include <Astra/System/SystemExecutor.hpp>
#include <Astra/Core/WorkScheduler.hpp>

#include <memory>

namespace Arcane
{
    struct SystemSchedulers
    {
        Astra::SystemScheduler fixedUpdate;   // fixed-rate sim (60 Hz)
        Astra::SystemScheduler update;        // once per rendered frame (variable dt)
        Astra::SystemScheduler render;        // render submission (single-threaded)
        Astra::ParallelExecutor executor;     // enkiTS-backed; null scheduler -> sequential

        // sched may be null (sequential inline execution -- fine for tests/headless).
        explicit SystemSchedulers(std::shared_ptr<Astra::IWorkScheduler> sched)
            : executor(std::move(sched)) {}
    };
}
```

- [ ] **Step 4: Create `Arcane/Sim/RunLoop.hpp`**

```cpp
#pragma once

// RunLoop: fixed-timestep accumulator + render alpha. Mirrors the proven Lua
// Application cadence (fixed 60 UPS, alpha on draw). Header-only so it operates
// on the host-owned Registry (see SystemSchedulers.hpp for the ownership rule).
//
// Advance() runs >=0 FixedUpdate steps then Update once, through the parallel
// executor. SubmitRender() runs the Render scheduler SEQUENTIALLY on the calling
// thread (Batcher2D is not thread-safe and must be recorded where Begin was
// called). The host brackets SubmitRender() between Batcher2D::Begin/End.

#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

namespace Arcane
{
    class RunLoop
    {
    public:
        struct Config
        {
            double fixedHz = 60.0;
            int    maxStepsPerFrame = 5;   // clamp to avoid the spiral of death
        };

        RunLoop(Astra::Registry& registry, SystemSchedulers& schedulers, Config cfg = {})
            : m_registry(&registry), m_schedulers(&schedulers), m_cfg(cfg) {}

        // Advance one real frame. Returns the render alpha in [0,1) for interpolation.
        double Advance(double realDt)
        {
            const double fixedDt = 1.0 / m_cfg.fixedHz;
            m_accumulator += realDt;

            int steps = 0;
            while (m_accumulator >= fixedDt && steps < m_cfg.maxStepsPerFrame)
            {
                m_schedulers->fixedUpdate.Execute(*m_registry, &m_schedulers->executor);
                m_accumulator -= fixedDt;
                ++steps;
            }
            if (steps == m_cfg.maxStepsPerFrame && m_accumulator > fixedDt)
                m_accumulator = 0.0;  // dropped the backlog; do not accumulate debt

            m_schedulers->update.Execute(*m_registry, &m_schedulers->executor);

            m_alpha = m_accumulator / fixedDt;
            return m_alpha;
        }

        // Run the Render scheduler on the calling thread (single-threaded).
        void SubmitRender()
        {
            m_schedulers->render.Execute(*m_registry);
        }

        double Alpha() const noexcept { return m_alpha; }

    private:
        Astra::Registry*  m_registry;
        SystemSchedulers* m_schedulers;
        Config m_cfg;
        double m_accumulator = 0.0;
        double m_alpha = 0.0;
    };
}
```

- [ ] **Step 5: Build + run — expect PASS**

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[runloop]"
```

Expected: 1 test passes (the headers are new but `**.hpp` is globbed; if the test cannot find
them, regenerate with premake first).

- [ ] **Step 6: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Sim/SystemSchedulers.hpp Arcane/Arcane/src/Arcane/Sim/RunLoop.hpp Arcane/Tests/src/RunLoopTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/sim): per-phase SystemSchedulers + fixed-timestep RunLoop

Header-only phase layer over Astra's flat scheduler (Fixed/Update/Render).
RunLoop accumulates fixed steps + exposes render alpha; render phase runs
single-threaded. Test covers fixed-rate stepping and spike clamping.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 3 — Minimal formal scene (native Astra relations)

## Task D1: Scene components + resources (TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\SceneComponentsTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Scene\Components.hpp`
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Scene\SceneResources.hpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/SceneComponentsTest.cpp`:

```cpp
// Scene components are plain reflected data. ToMatrix() builds a 2D TRS matrix;
// reflected components expose a non-null visitFields slot (Astra 3.2 seam).

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>

#include <Astra/Component/ComponentRegistry.hpp>

#include <glm/gtc/epsilon.hpp>
#include <cmath>

TEST_CASE("LocalTransform::ToMatrix composes translation/scale", "[scene]")
{
    Arcane::LocalTransform t;
    t.position = glm::vec2(10.0f, 20.0f);
    t.scale = glm::vec2(2.0f, 3.0f);
    t.rotation = 0.0f;

    const glm::mat3 m = t.ToMatrix();
    // column-major: m[2] is the translation column; m[0]/m[1] basis * scale.
    CHECK(glm::epsilonEqual(m[2].x, 10.0f, 1e-5f));
    CHECK(glm::epsilonEqual(m[2].y, 20.0f, 1e-5f));
    CHECK(glm::epsilonEqual(glm::length(glm::vec2(m[0])), 2.0f, 1e-5f));
    CHECK(glm::epsilonEqual(glm::length(glm::vec2(m[1])), 3.0f, 1e-5f));
}

TEST_CASE("scene components are reflected (visitFields slot populated)", "[scene]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Arcane::LocalTransform>();
    creg.RegisterComponent<Arcane::SpriteRenderer>();

    const auto* lt = creg.GetComponentDescriptor(Astra::TypeID<Arcane::LocalTransform>::Value());
    const auto* sr = creg.GetComponentDescriptor(Astra::TypeID<Arcane::SpriteRenderer>::Value());
    REQUIRE(lt != nullptr);
    REQUIRE(sr != nullptr);
    CHECK(lt->visitFields != nullptr);
    CHECK(sr->visitFields != nullptr);
}
```

- [ ] **Step 2: Build — expect FAIL** (`Arcane/Scene/Components.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

- [ ] **Step 3: Create `Arcane/Scene/Components.hpp`**

```cpp
#pragma once

// Scene components: plain reflected data. Every engine component is
// ASTRA_REFLECT-annotated from day one (design rule) so the editor/JSON/server
// path stays open at ~zero cost. Header-only + reflected here: the reflection
// registers once per module (MetaRegistry::Register is idempotent by hash), and
// the simulation Registry is owned by the host module that includes this header.

#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace Arcane
{
    struct LocalTransform
    {
        glm::vec2 position{0.0f, 0.0f};
        float     rotation = 0.0f;          // radians
        glm::vec2 scale{1.0f, 1.0f};

        // 2D TRS in column-major homogeneous mat3 (translation in column 2).
        glm::mat3 ToMatrix() const
        {
            const float c = std::cos(rotation);
            const float s = std::sin(rotation);
            glm::mat3 m(1.0f);
            m[0] = glm::vec3(c * scale.x,  s * scale.x, 0.0f);
            m[1] = glm::vec3(-s * scale.y, c * scale.y, 0.0f);
            m[2] = glm::vec3(position.x,   position.y,  1.0f);
            return m;
        }
    };

    struct WorldTransform
    {
        glm::mat3 matrix{1.0f};             // computed by TransformPropagationSystem; never authored
    };

    struct SpriteRenderer
    {
        uint32_t  textureId = 0;            // 0 => untextured tinted quad; resolved via TextureTable
        glm::vec2 size{32.0f, 32.0f};       // base pixel size before world scale
        glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
        int32_t   sortingLayer = 0;
        int32_t   orderInLayer = 0;
    };
}

// Reflection blocks at namespace scope (NOT anonymous). WorldTransform::matrix is
// derived state -> Serializable(false) so name-keyed formats skip it (it is
// recomputed on load; the binary trivially-copyable path still round-trips it).
namespace Arcane
{
    ASTRA_REFLECT_TYPE(LocalTransform)
        ASTRA_REFLECT_FIELD(LocalTransform, position)
        ASTRA_REFLECT_FIELD(LocalTransform, rotation)
            ASTRA_REFLECT_ATTR(AngleFormat, Astra::AngleFormat::Unit::Radians)
        ASTRA_REFLECT_FIELD(LocalTransform, scale)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(WorldTransform)
        ASTRA_REFLECT_FIELD(WorldTransform, matrix)
            ASTRA_REFLECT_ATTR(Serializable, false)
    ASTRA_END_REFLECT_TYPE()

    ASTRA_REFLECT_TYPE(SpriteRenderer)
        ASTRA_REFLECT_FIELD(SpriteRenderer, textureId)
        ASTRA_REFLECT_FIELD(SpriteRenderer, size)
        ASTRA_REFLECT_FIELD(SpriteRenderer, tint)
        ASTRA_REFLECT_FIELD(SpriteRenderer, sortingLayer)
        ASTRA_REFLECT_FIELD(SpriteRenderer, orderInLayer)
    ASTRA_END_REFLECT_TYPE()
}
```

- [ ] **Step 4: Create `Arcane/Scene/SceneResources.hpp`**

```cpp
#pragma once

// Registry resources (singletons) for the scene slice. RenderContext2D and
// TextureTable are set by the host each frame; SceneRoot marks the subtree that
// IS the scene.

#include <Astra/Entity/Entity.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <unordered_map>

namespace Arcane
{
    struct SceneRoot { Astra::Entity entity; };

    struct RenderContext2D
    {
        class Batcher2D* batcher = nullptr;   // set by the host between Begin/End
        glm::vec2        cameraOffset{0.0f, 0.0f};  // world->screen; world unit == canvas px
    };

    struct TextureTable
    {
        // textureId 0 is reserved for "untextured". Full Assets integration deferred.
        std::unordered_map<uint32_t, nvrhi::ITexture*> textures;

        nvrhi::ITexture* Resolve(uint32_t id) const
        {
            if (id == 0) return nullptr;
            auto it = textures.find(id);
            return it != textures.end() ? it->second : nullptr;
        }
    };
}
```

- [ ] **Step 5: Regenerate + build + run — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[scene]"
```

Expected: 2 tests pass.

- [ ] **Step 6: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Scene/Components.hpp Arcane/Arcane/src/Arcane/Scene/SceneResources.hpp Arcane/Tests/src/SceneComponentsTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/scene): reflected LocalTransform/WorldTransform/SpriteRenderer + resources

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task D2: `TransformPropagationSystem` + binary scene save/load (TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\TransformPropagationTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Scene\TransformSystems.hpp`
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Scene\SceneModule.hpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/TransformPropagationTest.cpp`:

```cpp
// BFS transform propagation: a child's world translation is parent.world * child.local.
// Reversed insertion order must give the same result (order-independence). Binary
// scene save/load round-trips transforms + relations.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>

#include <Astra/Registry/Registry.hpp>

#include <filesystem>
#include <memory>

namespace
{
    Astra::Entity MakeNode(Astra::Registry& reg, glm::vec2 pos)
    {
        Astra::Entity e = reg.CreateEntity();
        Arcane::LocalTransform lt;
        lt.position = pos;
        reg.AddComponent<Arcane::LocalTransform>(e, lt);
        reg.AddComponent<Arcane::WorldTransform>(e, Arcane::WorldTransform{});
        return e;
    }
}

TEST_CASE("transform propagation composes the parent chain in BFS order", "[scene]")
{
    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);

    Astra::Entity root = MakeNode(reg, glm::vec2(100.0f, 0.0f));
    Astra::Entity child = MakeNode(reg, glm::vec2(10.0f, 5.0f));
    Astra::Entity grandchild = MakeNode(reg, glm::vec2(1.0f, 2.0f));
    reg.SetParent(grandchild, child);   // deliberately wire deepest first
    reg.SetParent(child, root);
    reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

    Arcane::TransformPropagationSystem propagate;
    propagate(reg);

    auto worldPos = [&](Astra::Entity e)
    {
        const glm::mat3& m = reg.GetComponent<Arcane::WorldTransform>(e)->matrix;
        return glm::vec2(m[2].x, m[2].y);
    };
    CHECK(worldPos(root).x == Catch::Approx(100.0f));
    CHECK(worldPos(child).x == Catch::Approx(110.0f));
    CHECK(worldPos(child).y == Catch::Approx(5.0f));
    CHECK(worldPos(grandchild).x == Catch::Approx(111.0f));
    CHECK(worldPos(grandchild).y == Catch::Approx(7.0f));
}

TEST_CASE("binary scene save/load round-trips transforms and relations", "[scene]")
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "arcane_scene_roundtrip.bin";

    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);
        Astra::Entity root = MakeNode(reg, glm::vec2(100.0f, 0.0f));
        Astra::Entity child = MakeNode(reg, glm::vec2(10.0f, 5.0f));
        reg.SetParent(child, root);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        auto saved = Arcane::Scene::SaveBinary(reg, path);
        REQUIRE(saved.IsOk());
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    // The destination ComponentRegistry must know the scene types before Load.
    Arcane::RegisterSceneComponents(*components);
    auto loaded = Arcane::Scene::LoadBinary(path, components);
    REQUIRE(loaded.IsOk());
    std::unique_ptr<Astra::Registry> reg = std::move(loaded).Unwrap();

    int spatial = 0;
    glm::vec2 childLocal{0.0f};
    auto view = reg->CreateView<Arcane::LocalTransform>();
    view.ForEach([&](Astra::Entity, Arcane::LocalTransform& lt)
    {
        ++spatial;
        if (std::abs(lt.position.x - 10.0f) < 1e-4f) childLocal = lt.position;
    });
    CHECK(spatial == 2);
    CHECK(childLocal.y == Catch::Approx(5.0f));

    std::filesystem::remove(path);
}
```

Note: `RegisterSceneComponents` is overloaded for both `Registry&` and `ComponentRegistry&`
(see Step 3). If `Result::Unwrap()` is not the exact Astra spelling, use the project's
accessor (`loaded.GetValue()` / `*loaded`); confirm against `Astra/Core/Result.hpp` when
implementing and adjust both the test and SceneModule to match.

- [ ] **Step 2: Build — expect FAIL** (`TransformSystems.hpp` / `SceneModule.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

- [ ] **Step 3: Create `Arcane/Scene/SceneModule.hpp`** (registration + binary save/load)

```cpp
#pragma once

// Scene registration + binary persistence. Binary save/load is the scene's
// primary runtime persistence (the future hot-reload path); it is one call over
// Astra's integrated, CRC'd, versioned Registry::Save/Load.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/SerializationError.hpp>

#include <filesystem>
#include <memory>

namespace Arcane
{
    inline void RegisterSceneComponents(Astra::ComponentRegistry& creg)
    {
        creg.RegisterComponent<LocalTransform>();
        creg.RegisterComponent<WorldTransform>();
        creg.RegisterComponent<SpriteRenderer>();
    }

    inline void RegisterSceneComponents(Astra::Registry& reg)
    {
        RegisterSceneComponents(*reg.GetComponentRegistry());
    }

    namespace Scene
    {
        inline Astra::Result<void, Astra::SerializationError>
            SaveBinary(const Astra::Registry& reg, const std::filesystem::path& path)
        {
            return reg.Save(path);
        }

        inline Astra::Result<std::unique_ptr<Astra::Registry>, Astra::SerializationError>
            LoadBinary(const std::filesystem::path& path,
                       std::shared_ptr<Astra::ComponentRegistry> components)
        {
            return Astra::Registry::Load(path, std::move(components));
        }
    }
}
```

- [ ] **Step 4: Create `Arcane/Scene/TransformSystems.hpp`**

```cpp
#pragma once

// TransformPropagationSystem: BFS world-matrix propagation over the scene root's
// subtree. Correctness relies on BFS pre-order (parent before child) guaranteed
// by Astra's RelationshipGraph TraversalCache. The root is computed first (no
// parent); every descendant reads its already-computed parent's world matrix.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

namespace Arcane
{
    struct TransformPropagationSystem
        : Astra::SystemTraits<Astra::Reads<LocalTransform>, Astra::Writes<WorldTransform>>
    {
        void operator()(Astra::Registry& reg)
        {
            const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
            if (!sceneRoot) return;
            const Astra::Entity root = sceneRoot->entity;

            // Root: world = local (no parent).
            if (auto* rootLocal = reg.GetComponent<LocalTransform>(root))
                if (auto* rootWorld = reg.GetComponent<WorldTransform>(root))
                    rootWorld->matrix = rootLocal->ToMatrix();

            reg.GetRelations(root).ForEachDescendant(
                [&](Astra::Entity e, size_t /*depth*/)
                {
                    auto* local = reg.GetComponent<LocalTransform>(e);
                    auto* world = reg.GetComponent<WorldTransform>(e);
                    if (!local || !world) return;   // skip non-spatial nodes

                    const Astra::Entity parent = reg.GetParent(e);
                    const WorldTransform* parentWorld = reg.GetComponent<WorldTransform>(parent);
                    const glm::mat3 parentMat = parentWorld ? parentWorld->matrix : glm::mat3(1.0f);
                    world->matrix = parentMat * local->ToMatrix();
                });
        }
    };
}
```

- [ ] **Step 5: Build + run — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[scene]"
```

Expected: all `[scene]` tests pass (4 now). If `ForEachDescendant`'s callback arity differs
(some Astra builds pass only `Entity`), adjust the lambda to `[&](Astra::Entity e){...}` per
`Relations.hpp`.

- [ ] **Step 6: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Scene/TransformSystems.hpp Arcane/Arcane/src/Arcane/Scene/SceneModule.hpp Arcane/Tests/src/TransformPropagationTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/scene): BFS TransformPropagationSystem + binary scene save/load

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 4 — Reflection -> JSON bridge (the seam's first consumer)

## Task F1: `ReflectionJsonWriter`/`Reader` (TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\ReflectionJsonTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Serialization\ReflectionJson.hpp`

Note: this bridge is header-only (templated on the host's reflection); it lives in Arcane but
is consumed by the host module that owns the registry. nlohmann/json is in the Arcane include
path for all consumers.

- [ ] **Step 1: Write the failing test**

Create `Tests/src/ReflectionJsonTest.cpp`:

```cpp
// The reflection->JSON bridge drives the Astra 3.2 visitFields seam to round-trip
// a component through nlohmann::json, with no format knowledge in Astra. glm math
// types serialize as arrays; AliasName recovers a renamed field.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Component/ComponentRegistry.hpp>
#include <Astra/Reflection/Reflection.hpp>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace
{
    struct Widget
    {
        int       count = 0;
        float     ratio = 0.0f;
        glm::vec2 offset{0.0f, 0.0f};
        glm::vec4 color{0.0f, 0.0f, 0.0f, 0.0f};
    };

    struct Versioned { int amount = 0; };
}

ASTRA_REFLECT_TYPE(Widget)
    ASTRA_REFLECT_FIELD(Widget, count)
    ASTRA_REFLECT_FIELD(Widget, ratio)
    ASTRA_REFLECT_FIELD(Widget, offset)
    ASTRA_REFLECT_FIELD(Widget, color)
ASTRA_END_REFLECT_TYPE()

ASTRA_REFLECT_TYPE(Versioned)
    ASTRA_REFLECT_FIELD(Versioned, amount)
        ASTRA_REFLECT_ATTR(AliasName, "value")   // renamed from "value"
ASTRA_END_REFLECT_TYPE()

TEST_CASE("component round-trips through JSON via the visitFields seam", "[json]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Widget>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Widget>::Value());
    REQUIRE(desc != nullptr);
    REQUIRE(desc->visitFields != nullptr);

    Widget a;
    a.count = 7; a.ratio = 1.25f; a.offset = glm::vec2(3, 4); a.color = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);

    nlohmann::json j;
    Arcane::ReflectionJsonWriter writer(j);
    desc->visitFields(&a, writer);

    CHECK(j["count"].get<int>() == 7);
    CHECK(j["offset"].is_array());
    CHECK(j["offset"][1].get<float>() == Catch::Approx(4.0f));

    Widget b;
    Arcane::ReflectionJsonReader reader(j);
    desc->visitFields(&b, reader);
    CHECK(b.count == 7);
    CHECK(b.ratio == Catch::Approx(1.25f));
    CHECK(b.offset.x == Catch::Approx(3.0f));
    CHECK(b.color.w == Catch::Approx(1.0f));
}

TEST_CASE("AliasName recovers a value written under the old field name", "[json]")
{
    Astra::ComponentRegistry creg;
    creg.RegisterComponent<Versioned>();
    const auto* desc = creg.GetComponentDescriptor(Astra::TypeID<Versioned>::Value());
    REQUIRE(desc != nullptr);

    nlohmann::json j;
    j["value"] = 55;   // legacy key

    Versioned v;
    Arcane::ReflectionJsonReader reader(j);
    desc->visitFields(&v, reader);
    CHECK(v.amount == 55);
}
```

- [ ] **Step 2: Build — expect FAIL** (`ReflectionJson.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

- [ ] **Step 3: Create `Arcane/Serialization/ReflectionJson.hpp`**

```cpp
#pragma once

// Reflection -> JSON bridge: the first consumer of Astra 3.2's format-agnostic
// IFieldVisitor seam. Drives any reflected component to/from nlohmann::json by
// walking FieldInfo. Astra owns no JSON; this lives entirely in Arcane.
//
// Value dispatch: arithmetic/bool/string via typed accessors; enums by name via
// EnumInfo; glm::vec2/3/4 as JSON arrays (POD-math fallback); nested reflected
// structs recurse via MetaRegistry. Reader falls back to AliasName for renames;
// a missing key leaves the default-constructed value (forward/back compatible).

#include <Astra/Reflection/FieldVisitor.hpp>
#include <Astra/Reflection/FieldInfo.hpp>
#include <Astra/Reflection/MetaRegistry.hpp>
#include <Astra/Core/TypeID.hpp>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace Arcane
{
    namespace Detail
    {
        // Math-type hashes computed once (XXHash64 of the type name, cross-module stable).
        inline uint64_t Vec2Hash() { static const uint64_t h = Astra::TypeID<glm::vec2>::Hash(); return h; }
        inline uint64_t Vec3Hash() { static const uint64_t h = Astra::TypeID<glm::vec3>::Hash(); return h; }
        inline uint64_t Vec4Hash() { static const uint64_t h = Astra::TypeID<glm::vec4>::Hash(); return h; }

        inline bool WriteScalar(const Astra::FieldInfo& f, void* inst, nlohmann::json& out)
        {
            const uint64_t h = f.typeHash;
            if (h == Astra::TypeID<bool>::Hash())     { out = f.Get<bool>(inst);     return true; }
            if (h == Astra::TypeID<int>::Hash())      { out = f.Get<int>(inst);      return true; }
            if (h == Astra::TypeID<int32_t>::Hash())  { out = f.Get<int32_t>(inst);  return true; }
            if (h == Astra::TypeID<uint32_t>::Hash()) { out = f.Get<uint32_t>(inst); return true; }
            if (h == Astra::TypeID<int64_t>::Hash())  { out = f.Get<int64_t>(inst);  return true; }
            if (h == Astra::TypeID<uint64_t>::Hash()) { out = f.Get<uint64_t>(inst); return true; }
            if (h == Astra::TypeID<float>::Hash())    { out = f.Get<float>(inst);    return true; }
            if (h == Astra::TypeID<double>::Hash())   { out = f.Get<double>(inst);   return true; }
            if (h == Astra::TypeID<std::string>::Hash()) { out = f.Get<std::string>(inst); return true; }
            return false;
        }

        inline bool ReadScalar(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Astra::TypeID<bool>::Hash())     { f.Set<bool>(inst, in.get<bool>());         return true; }
            if (h == Astra::TypeID<int>::Hash())      { f.Set<int>(inst, in.get<int>());           return true; }
            if (h == Astra::TypeID<int32_t>::Hash())  { f.Set<int32_t>(inst, in.get<int32_t>());   return true; }
            if (h == Astra::TypeID<uint32_t>::Hash()) { f.Set<uint32_t>(inst, in.get<uint32_t>()); return true; }
            if (h == Astra::TypeID<int64_t>::Hash())  { f.Set<int64_t>(inst, in.get<int64_t>());   return true; }
            if (h == Astra::TypeID<uint64_t>::Hash()) { f.Set<uint64_t>(inst, in.get<uint64_t>()); return true; }
            if (h == Astra::TypeID<float>::Hash())    { f.Set<float>(inst, in.get<float>());       return true; }
            if (h == Astra::TypeID<double>::Hash())   { f.Set<double>(inst, in.get<double>());     return true; }
            if (h == Astra::TypeID<std::string>::Hash()) { f.Set<std::string>(inst, in.get<std::string>()); return true; }
            return false;
        }

        inline bool WriteGlm(const Astra::FieldInfo& f, void* inst, nlohmann::json& out)
        {
            const uint64_t h = f.typeHash;
            if (h == Vec2Hash()) { auto v = f.Get<glm::vec2>(inst); out = {v.x, v.y};            return true; }
            if (h == Vec3Hash()) { auto v = f.Get<glm::vec3>(inst); out = {v.x, v.y, v.z};       return true; }
            if (h == Vec4Hash()) { auto v = f.Get<glm::vec4>(inst); out = {v.x, v.y, v.z, v.w};  return true; }
            return false;
        }

        inline bool ReadGlm(const Astra::FieldInfo& f, void* inst, const nlohmann::json& in)
        {
            const uint64_t h = f.typeHash;
            if (h == Vec2Hash()) { f.Set<glm::vec2>(inst, glm::vec2(in[0], in[1]));                 return true; }
            if (h == Vec3Hash()) { f.Set<glm::vec3>(inst, glm::vec3(in[0], in[1], in[2]));          return true; }
            if (h == Vec4Hash()) { f.Set<glm::vec4>(inst, glm::vec4(in[0], in[1], in[2], in[3]));   return true; }
            return false;
        }
    }

    class ReflectionJsonWriter : public Astra::IFieldVisitor
    {
    public:
        explicit ReflectionJsonWriter(nlohmann::json& out) : m_out(out) {}

        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            nlohmann::json value;
            if (Detail::WriteScalar(field, instance, value) ||
                Detail::WriteGlm(field, instance, value))
            {
                m_out[std::string(field.name)] = std::move(value);
                return;
            }
            // Enum: emit the value's name.
            if (field.isEnum)
            {
                const Astra::TypeMeta* em = Astra::GetMeta(field.typeHash);
                if (em && em->GetEnumInfo())
                {
                    const int64_t raw = static_cast<int64_t>(field.Get<int>(instance));
                    if (auto name = em->EnumToString(raw))
                    {
                        m_out[std::string(field.name)] = std::string(*name);
                        return;
                    }
                }
            }
            // Nested reflected struct: recurse over the sub-instance.
            if (const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash))
            {
                nlohmann::json sub;
                ReflectionJsonWriter subWriter(sub);
                void* subInstance = static_cast<std::byte*>(instance) + field.offset;
                for (const Astra::FieldInfo& nf : nested->fields)
                    if (nf.IsSerializable())
                        subWriter.Visit(nf, subInstance);
                m_out[std::string(field.name)] = std::move(sub);
            }
            // else: unsupported field type for JSON -> silently skipped.
        }

        bool IsWriting() const noexcept override { return true; }

    private:
        nlohmann::json& m_out;
    };

    class ReflectionJsonReader : public Astra::IFieldVisitor
    {
    public:
        explicit ReflectionJsonReader(const nlohmann::json& in) : m_in(in) {}

        void Visit(const Astra::FieldInfo& field, void* instance) override
        {
            const nlohmann::json* node = Find(field);
            if (!node) return;   // missing -> keep default

            if (Detail::ReadScalar(field, instance, *node) ||
                Detail::ReadGlm(field, instance, *node))
                return;

            if (field.isEnum && node->is_string())
            {
                const Astra::TypeMeta* em = Astra::GetMeta(field.typeHash);
                if (em)
                    if (auto v = em->EnumFromString(node->get<std::string>()))
                        field.Set<int>(instance, static_cast<int>(*v));
                return;
            }
            if (const Astra::TypeMeta* nested = Astra::GetMeta(field.typeHash))
            {
                ReflectionJsonReader subReader(*node);
                void* subInstance = static_cast<std::byte*>(instance) + field.offset;
                for (const Astra::FieldInfo& nf : nested->fields)
                    if (nf.IsSerializable())
                        subReader.Visit(nf, subInstance);
            }
        }

        bool IsWriting() const noexcept override { return false; }

    private:
        // Try the current name, then any AliasName (former names).
        const nlohmann::json* Find(const Astra::FieldInfo& field) const
        {
            auto it = m_in.find(std::string(field.name));
            if (it != m_in.end()) return &(*it);

            const nlohmann::json* found = nullptr;
            field.ForEachAttribute<Astra::AliasName>([&](const Astra::AliasName& a)
            {
                if (!found)
                {
                    auto ai = m_in.find(std::string(a.name));
                    if (ai != m_in.end()) found = &(*ai);
                }
            });
            return found;
        }

        const nlohmann::json& m_in;
    };
}
```

- [ ] **Step 4: Build + run — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[json]"
```

Expected: 2 tests pass.

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Serialization/ReflectionJson.hpp Arcane/Tests/src/ReflectionJsonTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/serialization): reflection->JSON bridge over the Astra 3.2 seam

ReflectionJsonWriter/Reader drive visitFields to round-trip components through
nlohmann::json; glm math as arrays, AliasName fallback. Astra owns no JSON.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task F2: Scene JSON save/load (TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\SceneJsonTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Serialization\SceneSerializer.hpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/SceneJsonTest.cpp`:

```cpp
// Scene JSON: save the slice's entities (LocalTransform + SpriteRenderer) + parent
// links to JSON via the seam, then load into a fresh registry (typed roster) and
// assert transforms + hierarchy survived. Proves the north-star path end to end.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

TEST_CASE("scene round-trips through JSON (typed roster)", "[json][scene]")
{
    nlohmann::json doc;
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        Astra::Registry reg(components);
        Arcane::RegisterSceneComponents(reg);

        Astra::Entity root = reg.CreateEntity();
        Arcane::LocalTransform rootT; rootT.position = glm::vec2(100, 0);
        reg.AddComponent<Arcane::LocalTransform>(root, rootT);
        reg.AddComponent<Arcane::SpriteRenderer>(root, Arcane::SpriteRenderer{});

        Astra::Entity child = reg.CreateEntity();
        Arcane::LocalTransform childT; childT.position = glm::vec2(10, 5);
        reg.AddComponent<Arcane::LocalTransform>(child, childT);
        Arcane::SpriteRenderer sr; sr.tint = glm::vec4(0.5f, 0.6f, 0.7f, 1.0f); sr.sortingLayer = 2;
        reg.AddComponent<Arcane::SpriteRenderer>(child, sr);

        reg.SetParent(child, root);
        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});

        doc = Arcane::Scene::SaveJson(reg);
    }

    auto components = std::make_shared<Astra::ComponentRegistry>();
    Astra::Registry reg(components);
    Arcane::RegisterSceneComponents(reg);
    REQUIRE(Arcane::Scene::LoadJson(reg, doc));

    int sprites = 0;
    float maxLayer = -1.0f;
    auto view = reg.CreateView<Arcane::LocalTransform, Arcane::SpriteRenderer>();
    view.ForEach([&](Astra::Entity, Arcane::LocalTransform&, Arcane::SpriteRenderer& sr)
    {
        ++sprites;
        if (sr.sortingLayer > maxLayer) maxLayer = (float)sr.sortingLayer;
    });
    CHECK(sprites == 2);
    CHECK(maxLayer == Catch::Approx(2.0f));

    // The child must still be parented under some entity.
    const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
    REQUIRE(root != nullptr);
    CHECK(reg.GetChildCount(root->entity) == 1);
}
```

- [ ] **Step 2: Build — expect FAIL** (`SceneSerializer.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

- [ ] **Step 3: Create `Arcane/Serialization/SceneSerializer.hpp`**

```cpp
#pragma once

// Scene JSON (M4 scope: the slice's known component roster -- LocalTransform +
// SpriteRenderer -- plus parent links). General schema-driven scene JSON for
// arbitrary component rosters is a Grimoire-era concern. Binary (SceneModule)
// remains the primary runtime persistence; JSON is the inspectable/editable peer.

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>

#include <Astra/Registry/Registry.hpp>

#include <nlohmann/json.hpp>

#include <vector>

namespace Arcane::Scene
{
    // Emits { "entities": [ { "local": {...}, "sprite": {...}, "parent": <index|-1> } ] }
    // ordered root-first (BFS), so parent indices always refer to an earlier entry.
    inline nlohmann::json SaveJson(const Astra::Registry& reg)
    {
        nlohmann::json doc;
        doc["entities"] = nlohmann::json::array();

        const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
        if (!sceneRoot) return doc;

        // Collect root-first ordering.
        std::vector<Astra::Entity> order;
        order.push_back(sceneRoot->entity);
        reg.GetRelations(sceneRoot->entity).ForEachDescendant(
            [&](Astra::Entity e, size_t) { order.push_back(e); });

        auto indexOf = [&](Astra::Entity e) -> int
        {
            for (size_t i = 0; i < order.size(); ++i)
                if (order[i] == e) return static_cast<int>(i);
            return -1;
        };

        auto& mutableReg = const_cast<Astra::Registry&>(reg);  // GetComponent is non-const
        for (Astra::Entity e : order)
        {
            nlohmann::json entry;
            if (auto* lt = mutableReg.GetComponent<LocalTransform>(e))
            {
                nlohmann::json j;
                ReflectionJsonWriter w(j);
                ForEachReflectedField<LocalTransform>(lt, w);
                entry["local"] = std::move(j);
            }
            if (auto* sr = mutableReg.GetComponent<SpriteRenderer>(e))
            {
                nlohmann::json j;
                ReflectionJsonWriter w(j);
                ForEachReflectedField<SpriteRenderer>(sr, w);
                entry["sprite"] = std::move(j);
            }
            entry["parent"] = indexOf(mutableReg.GetParent(e));
            doc["entities"].push_back(std::move(entry));
        }
        return doc;
    }

    inline bool LoadJson(Astra::Registry& reg, const nlohmann::json& doc)
    {
        if (!doc.contains("entities")) return false;
        const auto& entities = doc["entities"];

        std::vector<Astra::Entity> created;
        created.reserve(entities.size());

        for (const auto& entry : entities)
        {
            Astra::Entity e = reg.CreateEntity();
            if (entry.contains("local"))
            {
                LocalTransform lt;
                ReflectionJsonReader r(entry["local"]);
                ForEachReflectedField<LocalTransform>(&lt, r);
                reg.AddComponent<LocalTransform>(e, lt);
                reg.AddComponent<WorldTransform>(e, WorldTransform{});
            }
            if (entry.contains("sprite"))
            {
                SpriteRenderer sr;
                ReflectionJsonReader r(entry["sprite"]);
                ForEachReflectedField<SpriteRenderer>(&sr, r);
                reg.AddComponent<SpriteRenderer>(e, sr);
            }
            created.push_back(e);
        }

        for (size_t i = 0; i < entities.size(); ++i)
        {
            const int parent = entities[i].value("parent", -1);
            if (parent >= 0 && parent < static_cast<int>(created.size()))
                reg.SetParent(created[i], created[static_cast<size_t>(parent)]);
        }

        if (!created.empty())
            reg.SetResource<SceneRoot>(SceneRoot{created.front()});
        return true;
    }

    // Walks a type's serializable reflected fields, driving the given visitor.
    // (The seam's per-type walk, reused outside ComponentRegistry for known types.)
    template<typename T>
    inline void ForEachReflectedField(void* instance, Astra::IFieldVisitor& visitor)
    {
        const Astra::TypeMeta* meta = Astra::GetMeta<T>();
        if (!meta) return;
        for (const Astra::FieldInfo& f : meta->fields)
            if (f.IsSerializable())
                visitor.Visit(f, instance);
    }
}
```

Note: `ForEachReflectedField` is declared after its uses in this header; move its definition
above `SaveJson` (or forward-declare it) so it compiles — place the template helper first in
the file when implementing.

- [ ] **Step 4: Build + run — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[json][scene]"
```

Expected: the scene JSON round-trip test passes.

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Serialization/SceneSerializer.hpp Arcane/Tests/src/SceneJsonTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/serialization): scene JSON save/load over the reflection bridge

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# PHASE 5 — Render submission + vertical slice + GPU verification

## Task E1: `RenderSubmissionSystem` + `[gpu]` scene-slice test (TDD)

**Files:**
- Test: `D:\dev\starworks\Gacha\Arcane\Tests\src\SceneSliceTest.cpp` (create)
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Scene\RenderSystems.hpp`

- [ ] **Step 1: Write the failing test**

Create `Tests/src/SceneSliceTest.cpp`:

```cpp
// End-to-end slice: substrate (Registry+schedulers+RunLoop) advances a parented
// scene, then the render phase submits sprites to a Batcher2D into an offscreen
// HDR canvas. Asserts both sprites are submitted and NVRHI validation is silent.

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Render/Canvas.hpp>
#include <Arcane/Render/Device.hpp>
#include <Arcane/Render/ShaderLibrary.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>

#include <memory>

namespace
{
    void RunSlice(Arcane::GraphicsBackend backend)
    {
        Arcane::RenderDeviceDesc desc;
        desc.backend = backend;
        auto device = Arcane::RenderDevice::Create(desc);
        REQUIRE(device != nullptr);
        auto shaders = Arcane::ShaderLibrary::Create(device->Nvrhi(), backend, "shaders");
        REQUIRE(shaders != nullptr);
        auto canvas = Arcane::CreateCanvas(device->Nvrhi(), 64, 64);
        REQUIRE(canvas != nullptr);
        auto batcher = Arcane::Batcher2D::Create(device->Nvrhi(), *shaders);
        REQUIRE(batcher != nullptr);

        Arcane::JobSystem jobs;
        auto sched = jobs.WorkScheduler();

        Astra::Registry::Config cfg;
        cfg.workScheduler = sched;
        Astra::Registry reg(cfg);
        Arcane::RegisterSceneComponents(reg);

        Arcane::SystemSchedulers schedulers(sched);
        schedulers.fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
        schedulers.render.AddSystem<Arcane::RenderSubmissionSystem>();

        // Tiny scene: root + parent sprite + child sprite.
        Astra::Entity root = reg.CreateEntity();
        reg.AddComponent<Arcane::LocalTransform>(root, Arcane::LocalTransform{});
        reg.AddComponent<Arcane::WorldTransform>(root, Arcane::WorldTransform{});

        Astra::Entity parent = reg.CreateEntity();
        Arcane::LocalTransform pT; pT.position = glm::vec2(10, 10);
        reg.AddComponent<Arcane::LocalTransform>(parent, pT);
        reg.AddComponent<Arcane::WorldTransform>(parent, Arcane::WorldTransform{});
        reg.AddComponent<Arcane::SpriteRenderer>(parent, Arcane::SpriteRenderer{});
        reg.SetParent(parent, root);

        Astra::Entity child = reg.CreateEntity();
        Arcane::LocalTransform cT; cT.position = glm::vec2(5, 5);
        reg.AddComponent<Arcane::LocalTransform>(child, cT);
        reg.AddComponent<Arcane::WorldTransform>(child, Arcane::WorldTransform{});
        Arcane::SpriteRenderer csr; csr.tint = glm::vec4(0.2f, 0.8f, 0.3f, 1.0f);
        reg.AddComponent<Arcane::SpriteRenderer>(child, csr);
        reg.SetParent(child, parent);

        reg.SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{root});
        reg.SetResource<Arcane::TextureTable>(Arcane::TextureTable{});

        Arcane::RunLoop loop(reg, schedulers);
        for (int i = 0; i < 4; ++i)
            loop.Advance(1.0 / 60.0);   // FixedUpdate: transform propagation

        nvrhi::CommandListHandle cl = device->Nvrhi()->createCommandList();
        cl->open();
        cl->clearTextureFloat(canvas->Texture(), nvrhi::AllSubresources,
                              nvrhi::Color(0, 0, 0, 1));
        batcher->Begin(cl, canvas->Framebuffer(), canvas->Width(), canvas->Height());
        reg.SetResource<Arcane::RenderContext2D>(
            Arcane::RenderContext2D{batcher.get(), glm::vec2(0, 0)});
        loop.SubmitRender();   // RenderSubmissionSystem -> batcher
        batcher->End();
        cl->close();
        device->Nvrhi()->executeCommandList(cl);
        device->Nvrhi()->waitForIdle();

        CHECK(batcher->Stats().quads >= 2);
        CHECK(Arcane::RenderErrorCount() == 0);
        device->Nvrhi()->runGarbageCollection();
    }
}

TEST_CASE("d3d12: scene slice submits sprites with no validation errors", "[gpu][d3d12][scene]")
{
    RunSlice(Arcane::GraphicsBackend::D3D12);
}

TEST_CASE("vulkan: scene slice submits sprites with no validation errors", "[gpu][vulkan][scene]")
{
    RunSlice(Arcane::GraphicsBackend::Vulkan);
}
```

- [ ] **Step 2: Build — expect FAIL** (`RenderSystems.hpp` missing)

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
```

- [ ] **Step 3: Create `Arcane/Scene/RenderSystems.hpp`**

```cpp
#pragma once

// RenderSubmissionSystem: reads WorldTransform + SpriteRenderer, submits one quad
// per sprite to the Batcher2D held in the RenderContext2D resource. Read-only
// w.r.t. ECS; the side effect (batcher submission) is external. Runs in the
// Render phase, single-threaded (Batcher2D is not thread-safe). Quads are axis
// aligned for the slice (rotation is ignored by Batcher2D::Quad).

#include <Arcane/Render/Batcher2D.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/System/System.hpp>

#include <glm/glm.hpp>

namespace Arcane
{
    struct RenderSubmissionSystem
        : Astra::SystemTraits<Astra::Reads<WorldTransform, SpriteRenderer>>
    {
        void operator()(Astra::Registry& reg)
        {
            RenderContext2D* ctx = reg.GetResource<RenderContext2D>();
            if (!ctx || !ctx->batcher) return;
            const TextureTable* textures = reg.GetResource<TextureTable>();

            auto view = reg.CreateView<WorldTransform, SpriteRenderer>();
            view.ForEach([&](Astra::Entity, WorldTransform& world, SpriteRenderer& sprite)
            {
                const glm::mat3& m = world.matrix;
                const glm::vec2 worldPos(m[2].x, m[2].y);
                const glm::vec2 worldScale(glm::length(glm::vec2(m[0])),
                                           glm::length(glm::vec2(m[1])));
                const glm::vec2 dstPos = worldPos + ctx->cameraOffset;
                const glm::vec2 dstSize = sprite.size * worldScale;

                ctx->batcher->SetLayer(static_cast<uint16_t>(sprite.sortingLayer),
                                       static_cast<uint16_t>(sprite.orderInLayer));

                nvrhi::ITexture* tex = textures ? textures->Resolve(sprite.textureId) : nullptr;
                if (tex)
                    ctx->batcher->Quad(dstPos, dstSize, tex,
                                       glm::vec2(0, 0), glm::vec2(1, 1), sprite.tint);
                else
                    ctx->batcher->Rect(dstPos, dstSize, sprite.tint);
            });
        }
    };
}
```

- [ ] **Step 4: Build + run the GPU test — expect PASS**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[scene][gpu]"
```

Expected: both d3d12 + vulkan scene-slice tests pass; quads >= 2; `RenderErrorCount() == 0`.
(On a machine without a capable GPU, run `"[scene]~[gpu]"` for the headless subset; CI's
`windows-1` runs the `[gpu]` set.)

- [ ] **Step 5: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Scene/RenderSystems.hpp Arcane/Tests/src/SceneSliceTest.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/scene): RenderSubmissionSystem + [gpu] scene-slice test

Substrate advances a parented scene; render phase submits sprites to Batcher2D
into an offscreen canvas; asserts RenderErrorCount()==0 on both backends.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

## Task E2: Playground vertical slice

**Files:**
- Create: `D:\dev\starworks\Gacha\Arcane\Arcane\src\Arcane\Sim\Simulation.hpp`
- Modify: `D:\dev\starworks\Gacha\Arcane\Playground\src\main.cpp`

- [ ] **Step 1: Create the header-only `Simulation` facade**

Create `Arcane/Sim/Simulation.hpp`:

```cpp
#pragma once

// Simulation: the substrate's host-facing entry. Owns the Registry (configured
// with the injected scheduler) and the per-phase SystemSchedulers. Header-only
// so the host module owns the registry (single-module ownership rule).

#include <Arcane/Sim/SystemSchedulers.hpp>

#include <Astra/Registry/Registry.hpp>
#include <Astra/Core/WorkScheduler.hpp>

#include <memory>
#include <utility>

namespace Arcane
{
    class Simulation
    {
    public:
        explicit Simulation(std::shared_ptr<Astra::IWorkScheduler> sched)
            : m_sched(std::move(sched))
            , m_registry(MakeConfig(m_sched))
            , m_schedulers(m_sched) {}

        Astra::Registry&  Registry() noexcept    { return m_registry; }
        SystemSchedulers&  Schedulers() noexcept { return m_schedulers; }

    private:
        static Astra::Registry::Config MakeConfig(std::shared_ptr<Astra::IWorkScheduler> s)
        {
            Astra::Registry::Config cfg;
            cfg.workScheduler = std::move(s);
            return cfg;
        }

        std::shared_ptr<Astra::IWorkScheduler> m_sched;
        Astra::Registry  m_registry;
        SystemSchedulers m_schedulers;
    };
}
```

- [ ] **Step 2: Add the scene path to Playground `main.cpp`**

Add includes near the existing Arcane includes at the top of `Playground/src/main.cpp`:

```cpp
#include <Arcane/Jobs/JobSystem.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/RenderSystems.hpp>
#include <Arcane/Scene/SceneModule.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Scene/TransformSystems.hpp>
#include <Arcane/Sim/RunLoop.hpp>
#include <Arcane/Sim/Simulation.hpp>
```

After the input subsystem is created (just before `bool showStats = true;`), build the
simulation + scene:

```cpp
    // --- M4 scene slice: substrate + a moving parent/child sprite ---
    Arcane::JobSystem jobs;
    Arcane::Simulation sim(jobs.WorkScheduler());
    Arcane::RegisterSceneComponents(sim.Registry());
    sim.Schedulers().fixedUpdate.AddSystem<Arcane::TransformPropagationSystem>();
    sim.Schedulers().render.AddSystem<Arcane::RenderSubmissionSystem>();

    Astra::Entity sceneRoot = sim.Registry().CreateEntity();
    sim.Registry().AddComponent<Arcane::LocalTransform>(sceneRoot, Arcane::LocalTransform{});
    sim.Registry().AddComponent<Arcane::WorldTransform>(sceneRoot, Arcane::WorldTransform{});

    Astra::Entity orbiter = sim.Registry().CreateEntity();
    {
        Arcane::LocalTransform t; t.position = glm::vec2((float)canvas->Width() * 0.5f,
                                                         (float)canvas->Height() * 0.5f);
        sim.Registry().AddComponent<Arcane::LocalTransform>(orbiter, t);
        sim.Registry().AddComponent<Arcane::WorldTransform>(orbiter, Arcane::WorldTransform{});
        Arcane::SpriteRenderer sr; sr.size = glm::vec2(48.0f); sr.tint = glm::vec4(0.9f, 0.7f, 0.2f, 1.0f);
        sim.Registry().AddComponent<Arcane::SpriteRenderer>(orbiter, sr);
        sim.Registry().SetParent(orbiter, sceneRoot);
    }
    Astra::Entity moon = sim.Registry().CreateEntity();
    {
        Arcane::LocalTransform t; t.position = glm::vec2(80.0f, 0.0f);
        sim.Registry().AddComponent<Arcane::LocalTransform>(moon, t);
        sim.Registry().AddComponent<Arcane::WorldTransform>(moon, Arcane::WorldTransform{});
        Arcane::SpriteRenderer sr; sr.size = glm::vec2(20.0f); sr.tint = glm::vec4(0.4f, 0.8f, 1.0f, 1.0f);
        sim.Registry().AddComponent<Arcane::SpriteRenderer>(moon, sr);
        sim.Registry().SetParent(moon, orbiter);
    }
    sim.Registry().SetResource<Arcane::SceneRoot>(Arcane::SceneRoot{sceneRoot});
    sim.Registry().SetResource<Arcane::TextureTable>(Arcane::TextureTable{});

    // Orbit the parent each fixed step so transform propagation visibly moves the moon.
    double simTime = 0.0;
    sim.Schedulers().fixedUpdate.AddSystem([&simTime, orbiter](Astra::Registry& r)
    {
        simTime += 1.0 / 60.0;
        if (auto* lt = r.GetComponent<Arcane::LocalTransform>(orbiter))
            lt->rotation = (float)simTime;
    });

    Arcane::RunLoop runLoop(sim.Registry(), sim.Schedulers());
```

Inside the frame loop, AFTER the input block computes `frameDt` (reuse it) and BEFORE drawing,
advance the sim. Add right after the input `{ ... }` block:

```cpp
        // Advance the M4 simulation (FixedUpdate: orbit + transform propagation).
        {
            const double simDt = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - lastFrameTime).count();
            runLoop.Advance(simDt > 0.25 ? 0.25 : simDt);
        }
```

Then, inside the existing `batcher->Begin(...)` ... `batcher->End()` block, AFTER `Begin` and
before the existing demo primitives, submit the scene:

```cpp
        sim.Registry().SetResource<Arcane::RenderContext2D>(
            Arcane::RenderContext2D{batcher.get(), glm::vec2(0, 0)});
        runLoop.SubmitRender();
```

(The existing bouncing rects / circle / HUD remain; the scene sprites render alongside them,
proving the substrate path coexists with direct batcher use.)

- [ ] **Step 3: Build + run both backends with `--frames`**

```bash
cd "D:/dev/starworks/Gacha/Arcane" && ../ThirdParty/premake5/premake5.exe vs2026
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Debug /m
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/Playground/Playground.exe" --backend dx12 --frames 120
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/Playground/Playground.exe" --backend vulkan --frames 120
```

Expected: both exit with code 0 after 120 frames; window shows the orbiting parent sprite with
the moon revolving around it (run without `--frames` to watch interactively).

- [ ] **Step 4: Commit**

```bash
git -C "D:/dev/starworks/Gacha" add Arcane/Arcane/src/Arcane/Sim/Simulation.hpp Arcane/Playground/src/main.cpp
git -C "D:/dev/starworks/Gacha" commit -m "feat(arcane/playground): M4 scene slice -- moving parent/child sprite via RunLoop

Simulation facade owns the Registry + schedulers; Playground builds a tiny
parented scene and renders it through RunLoop + Batcher2D on both backends.

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

---

# WRAP-UP

## Task Z1: Release build sweep + full test run

**Files:** none (verification).

- [ ] **Step 1: Release build both configs and run the full Arcane suite (headless subset)**

```bash
msbuild "D:/dev/starworks/Gacha/Arcane/Arcane.slnx" /p:Configuration=Release /m
"D:/dev/starworks/Gacha/Arcane/bin/Release-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "~[gpu]"
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "~[gpu]"
```

Expected: Release build succeeds; all non-GPU tests pass in both configs.

- [ ] **Step 2: GPU suite (both backends) on the GPU machine**

```bash
"D:/dev/starworks/Gacha/Arcane/bin/Debug-windows-x86_64-md/ArcaneTests/ArcaneTests.exe" "[gpu]"
```

Expected: all `[gpu]` tests pass (existing + the new scene slice), `RenderErrorCount()==0`.

## Task Z2: Roadmap + memory updates

**Files:**
- Modify: `D:\dev\starworks\Gacha\docs\superpowers\specs\2026-06-11-engine-architecture-design.md`
- Modify: `C:\Users\Ethan Temprovich\.claude\projects\D--dev-starworks-Gacha\memory\project_arcane_next_milestone.md`
  and `MEMORY.md` (a fresh `project_arcane_m4_complete.md` once merged)

- [ ] **Step 1: Renumber the engine-arch bring-up table**

In `2026-06-11-engine-architecture-design.md`, the "Bring-up order" section: insert this M4 as
"M4 — Simulation substrate (Astra Registry + enkiTS + per-phase schedulers + RunLoop) + minimal
formal scene (transform hierarchy + sprite submission); Astra 3.2 serialization seam (Phase 0)";
renumber the existing M4 (Plugin module) -> M5, M5 (physics) -> M6, M6 (Grimoire) -> M7. Add a
one-line note that hot-reload state uses `registry.Save(buffer)`/`Load`, not per-component
BinaryWriter loops (the spec-deviation recorded in the M4 spec's Roadmap section).

- [ ] **Step 2: Commit the doc update**

```bash
git -C "D:/dev/starworks/Gacha" add docs/superpowers/specs/2026-06-11-engine-architecture-design.md
git -C "D:/dev/starworks/Gacha" commit -m "docs(engine): renumber bring-up order -- M4 = sim substrate, plugin host -> M5

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

- [ ] **Step 3: Memory** — after the branch merges, record `project_arcane_m4_complete.md`
  (substrate + scene slice + Astra 3.2 seam; the single-module ownership rule; key gotchas) and
  update `project_arcane_next_milestone.md` to point at M5 (plugin host). Add the MEMORY.md
  pointer line. (Do this at merge time, not before.)

## Task Z3: Finish the development branch

- [ ] **Step 1: Use the finishing-a-development-branch skill**

Invoke `superpowers:finishing-a-development-branch` to choose merge/PR/cleanup for
`feature/arcane-m4-sim-scene`. Push the branch; let CI go green; then merge to `main` and push
(the established push-branch-then-merge workflow). Astra's `dev`/`main`/`3.2` are already pushed
(Task A4).

---

## Self-review checklist (run before executing)

- **Phase 0 (Astra 3.2):** seam header (A1), descriptor slot (A1), registry population (A1),
  AliasName (A2), AstraTest coverage incl. map round-trip + null-slot + alias (A1/A2), binary
  regression (A3), dev->main->3.2 + version bump (A4), re-vendor (A5). Binary path untouched
  (no edits to BinaryWriter/Reader/Registry::Save). No JSON dep in Astra (JSON is Arcane-only,
  Phase 4). No FArchive.
- **Phase 1 (Jobs):** premake wiring (B0), JobSystem + enkiTS adapter + ParallelForEach test (B1).
- **Phase 2 (Substrate):** SystemSchedulers + RunLoop + fixed-step/clamp test (C1).
- **Phase 3 (Scene):** components+resources+reflection (D1), TransformPropagationSystem +
  binary save/load (D2).
- **Phase 4 (JSON bridge):** ReflectionJsonWriter/Reader (F1), scene JSON (F2).
- **Phase 5 (Slice):** RenderSubmissionSystem + `[gpu]` test (E1), Playground slice (E2).
- **Design rule:** every engine component (LocalTransform/WorldTransform/SpriteRenderer) is
  ASTRA_REFLECT-annotated (D1). **Single-module ownership** honored: substrate/scene header-only,
  only Jobs DLL-exported.
- **Type consistency:** `RegisterSceneComponents` overloaded `Registry&`/`ComponentRegistry&`
  (D2 SceneModule) and used both ways (D2 test, E1 test). `Scene::SaveBinary/LoadBinary`,
  `Scene::SaveJson/LoadJson`, `ForEachReflectedField<T>`, `RenderContext2D`/`TextureTable`/
  `SceneRoot` names match across tasks. `RunLoop::Advance/SubmitRender`, `SystemSchedulers`
  fields (`fixedUpdate`/`update`/`render`/`executor`) consistent.
- **API-spelling guards to verify at implementation time (adjust test + code together if they
  differ in the vendored Astra):** `Result::Unwrap()` (D2) vs the actual accessor in
  `Core/Result.hpp`; `Relations::ForEachDescendant` callback arity `(Entity, size_t)` vs
  `(Entity)` in `Relations.hpp`; `Registry::GetChildCount` (F2 test) presence; `AddSystem<T>()`
  for trait structs + `AddSystem(lambda)` for lambdas in `SystemScheduler.hpp`. These are
  confirmed against the audited 3.1 API in the research doc but should be re-checked once 3.2 is
  vendored.
