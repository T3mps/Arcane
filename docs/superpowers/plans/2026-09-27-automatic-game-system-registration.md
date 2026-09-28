# Automatic Game-System Registration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make editor-created game systems register automatically with their owning game module while keeping execution phase, network role, and scheduler ordering explicit.

**Architecture:** A new header-only `GameSystems.hpp` maintains a trivially destructible, module-local registrar list and shares one factory-construction helper with manual `GameModule::RegisterSystem`. The game-module prologue traverses that list inside the existing DLL-owner bracket before `OnInit`; the editor generates a system `.cpp` containing `ARCANE_SYSTEM` from phase and role choices collected by the Create C++ Class dialog.

**Tech Stack:** C++23, ArcaneCore plugin SDK, Astra system scheduler/ECS, Dear ImGui editor, Catch2, Premake/MSBuild.

**Spec:** `docs/superpowers/specs/2026-09-27-automatic-game-system-registration-design.md`

## Global Constraints

- Preserve `/MD` across engine, tests, fixtures, and game modules.
- Keep `GameSystems.hpp` header-only and module-local; do not add exported registrar state to `ArcaneCore.dll`.
- Registrar nodes must remain trivially destructible and must not own `std::function`, strings, component handles, or other live cleanup.
- Register factories only while `PluginHost` has an image-owner bracket open.
- Never use translation-unit/static-initialization order as scheduler order; semantic order belongs in `Astra::Before<>` and `Astra::After<>`.
- `ARCANE_SYSTEM` supports default-constructible systems only; keep manual `GameModule::RegisterSystem` for constructor arguments and conditional registration.
- Do not bump the game-plugin ABI: exports and `EngineContext` remain unchanged.
- Preserve all unrelated dirty-tree changes. Stage only the files named by each task, and never stage `ReferenceProject/Source/Game/TestComponent.*` or unrelated renderer/crash work.
- Follow strict red-green-refactor: every production behavior is preceded by a focused failing test or an existing test deliberately made red by removing its old manual wiring.

## Review Focus

- Registering one type as both `Server` and `Client` in the same phase must be rejected because `Standalone` and `ListenServer` match both roles; Task 1 tests the deterministic drop and unchanged table size.
- Registering one type in two different phases must remain legal; Task 1 tests that both entries survive and instantiate into separate schedulers.
- Invalid/stale dialog indices or enum values must generate `Both` + `FixedUpdate`, never malformed C++; Task 3 tests both normalization boundaries.
- `Update` and `Render` templates must not acquire a meaningless fixed-step transform edge; Task 3 tests includes and traits for all three phases.
- Failed initialization and unload must remove automatic factories before their DLL is unmapped; Task 2 runs the existing init-failure and unload tests after moving one fixture system to `ARCANE_SYSTEM`.

## File Structure

- Create `ArcaneCore/src/Arcane/Plugin/GameSystems.hpp`: automatic registrar nodes, module-local list, shared factory helper, traversal, and `ARCANE_SYSTEM`.
- Modify `ArcaneCore/src/Arcane/Plugin/GameModule.hpp`: delegate manual registration to the shared helper and traverse automatic registrars before `OnInit`.
- Modify `ArcaneCore/src/Arcane/Plugin/GameComponents.hpp`: correct the public explanation now that systems can self-register while scheduler dependencies remain explicit.
- Modify `ArcaneCore/src/Arcane/Plugin/SystemFactory.cpp`: reject duplicate type/phase entries owned by the same DLL.
- Modify `ArcaneTests/src/RoleMaskTest.cpp`: unit coverage for registrar discovery, phase/role routing, and duplicates.
- Modify `ArcaneTests/plugins/HotReloadPlugin.cpp`: make `ServerOnlyTick` automatic while leaving `ClientOnlyTick` manual, exercising both paths in one real DLL.
- Verify with `ArcaneTests/src/PluginHostTest.cpp` and `RoleMaskTest.cpp`; their existing behavior/count assertions remain unchanged.
- Modify `ArcaneEditor/src/Project/ClassTemplates.hpp` and `.cpp`: typed system options, choice mappings, phase-specific headers, and generated registrar source.
- Modify `ArcaneTests/src/ClassTemplatesTest.cpp`: exhaustive generated-text and normalization coverage.
- Modify `ArcaneEditor/src/Panels/CreateAssetDialog.hpp` and `.cpp`: store and draw phase/role choices, then return them with the result.
- Modify `ArcaneTests/src/CreateAssetDialogTest.cpp`: pin fresh-dialog defaults.
- Modify `ArcaneEditor/src/App/EditorApp.hpp`, `EditorAppFrame.cpp`, and `EditorAppProject.cpp`: carry the two choices into `ClassTemplates::Render`.
- Create `ReferenceProject/Source/Game/PlayerController2DSystem.cpp`: reference-project `ARCANE_SYSTEM` declaration.
- Modify `ReferenceProject/Source/Game/PlayerController2DSystem.hpp` and `ReferenceGame.cpp`: explain automatic wiring and remove the temporary manual module registration.

---

### Task 1: Core Automatic Registrar and Duplicate-Safe Factory Table

**Files:**
- Create: `ArcaneCore/src/Arcane/Plugin/GameSystems.hpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/GameModule.hpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/GameComponents.hpp`
- Modify: `ArcaneCore/src/Arcane/Plugin/SystemFactory.cpp`
- Modify/Test: `ArcaneTests/src/RoleMaskTest.cpp`

**Interfaces:**
- Consumes: `Arcane::SystemFactoryTable`, `RoleMask`, `SystemPhase`, `Astra::SystemScheduler::AddSystem<T>`, and the existing `PluginHost::BeginOwner/EndOwner` bracket.
- Produces: `Arcane::Game::SystemRegistrar`, `LinkSystemRegistrar(SystemRegistrar&)`, `SystemRegistrars()`, `RegisterSystems(SystemFactoryTable&)`, `Detail::AddSystemFactory<System>(...)`, and `ARCANE_SYSTEM(Type, Role, Phase)`.

- [ ] **Step 1: Write failing registrar and duplicate tests**

Add `Arcane/Plugin/GameSystems.hpp` and `Arcane/Sim/SystemSchedulers.hpp` includes to `RoleMaskTest.cpp`. Define one namespace-scope probe and registrar:

```cpp
namespace
{
    struct AutoUpdateProbe
    {
        void operator()(Astra::Registry&) {}
    };
}

ARCANE_SYSTEM(AutoUpdateProbe,
              Arcane::RoleMask::Client,
              Arcane::SystemPhase::Update)
```

Add tests that exercise real table and scheduler behavior:

```cpp
TEST_CASE("automatic system registrars add a role-masked factory to the selected phase",
          "[runtime][netmode][systems]")
{
    Arcane::SystemFactoryTable table;
    const int owner = 1;
    table.BeginOwner(&owner);
    CHECK(Arcane::Game::RegisterSystems(table) == 1);
    table.EndOwner();

    Arcane::SystemSchedulers client(nullptr);
    CHECK(table.InstantiateInto(client, Arcane::NetMode::Client) == 1);
    CHECK(client.update.HasSystem<AutoUpdateProbe>());
    CHECK_FALSE(client.fixedUpdate.HasSystem<AutoUpdateProbe>());
    CHECK_FALSE(client.render.HasSystem<AutoUpdateProbe>());

    Arcane::SystemSchedulers server(nullptr);
    CHECK(table.InstantiateInto(server, Arcane::NetMode::DedicatedServer) == 0);
    CHECK_FALSE(server.update.HasSystem<AutoUpdateProbe>());
}

TEST_CASE("one module cannot register the same system twice in one phase",
          "[runtime][netmode][systems]")
{
    Arcane::SystemFactoryTable table;
    const int owner = 2;
    const auto entry = [](Arcane::RoleMask mask, Arcane::SystemPhase phase)
    {
        return Arcane::SystemFactoryEntry{
            "DuplicateProbe", mask, phase,
            [](Astra::SystemScheduler&) {}, nullptr };
    };

    table.BeginOwner(&owner);
    table.Add(entry(Arcane::RoleMask::Server, Arcane::SystemPhase::FixedUpdate));
    table.Add(entry(Arcane::RoleMask::Client, Arcane::SystemPhase::FixedUpdate));
    CHECK(table.Size() == 1);

    table.Add(entry(Arcane::RoleMask::Client, Arcane::SystemPhase::Update));
    CHECK(table.Size() == 2);
    table.EndOwner();
}
```

The first test also carries a compile-time check:

```cpp
STATIC_REQUIRE(std::is_trivially_destructible_v<Arcane::Game::SystemRegistrar>);
```

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```powershell
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m
```

Expected: compilation fails because `Arcane/Plugin/GameSystems.hpp`, `ARCANE_SYSTEM`, and `RegisterSystems` do not exist.

- [ ] **Step 3: Implement the registrar and shared factory helper**

Create `GameSystems.hpp` with detailed contract comments and these shapes:

Include `Arcane/Plugin/SystemFactory.hpp`, `Astra/Core/TypeID.hpp`,
`Astra/System/SystemScheduler.hpp`, and the standard `<cstddef>`, `<string>`,
and `<tuple>` headers. Add `#include <Arcane/Plugin/GameSystems.hpp>` to
`GameModule.hpp`; do not rely on an accidental transitive include.

```cpp
namespace Arcane::Game
{
    struct SystemRegistrar
    {
        void (*registerFn)(SystemFactoryTable&, RoleMask, SystemPhase);
        const char*      typeName;
        RoleMask        mask;
        SystemPhase     phase;
        SystemRegistrar* next;
    };

    namespace Detail
    {
        inline SystemRegistrar*& SystemRegistrarHead()
        {
            static SystemRegistrar* head = nullptr;
            return head;
        }

        template <class System, class... Args>
        void AddSystemFactory(SystemFactoryTable& table,
                              RoleMask mask,
                              SystemPhase phase,
                              Args... args)
        {
            table.Add(SystemFactoryEntry{
                std::string(Astra::TypeID<System>::Name()), mask, phase,
                [args...](Astra::SystemScheduler& scheduler)
                {
                    std::ignore = scheduler.AddSystem<System>(args...);
                },
                nullptr });
        }

        template <class System>
        void AddDefaultSystemFactory(SystemFactoryTable& table,
                                     RoleMask mask,
                                     SystemPhase phase)
        {
            AddSystemFactory<System>(table, mask, phase);
        }
    }
}
```

Implement `LinkSystemRegistrar`, `SystemRegistrars`, and `RegisterSystems`. `RegisterSystems` records `table.Size()` before traversal and returns the accepted-entry delta so rejected duplicates are not reported as registered.

Define `ARCANE_SYSTEM` through a two-stage `__COUNTER__` macro, following `ARCANE_COMPONENT`, with an anonymous-namespace node and linked `const bool`. Store `&Detail::AddDefaultSystemFactory<T>` in the node; do not store a `std::function` in static registrar state.

- [ ] **Step 4: Integrate the traversal and reject duplicates**

In `GameModule::RegisterSystem`, replace the duplicated `SystemFactoryEntry` construction with:

```cpp
Game::Detail::AddSystemFactory<System>(
    Process().SystemFactories(), mask, phase, args...);
```

In `GameModuleDetail::State::Init`, after constructing/binding `instance` and before `instance->OnInit(*c)`, call:

```cpp
const std::size_t systemCount = Game::RegisterSystems(c->process->SystemFactories());
ARC_INFO("{}: registered {} automatic module system(s)", name, systemCount);
```

In `SystemFactoryTable::Add`, after validating `m_openOwner` and before stamping/pushing the entry, find an existing entry with the same `owner`, `name`, and `phase`. Log an `ARC_ERROR` naming the system and phase, then return without pushing it. Do not include mask in the duplicate predicate because separate Server/Client entries both match combined-role runtimes. Keep this recoverable so a game-module wiring mistake cannot terminate the editor during load.

Update `GameComponents.hpp` and `GameModule.hpp` comments to explain why automatic discovery is safe only when semantic ordering is expressed through traits, why registrar nodes are trivial, and when manual registration remains required.

- [ ] **Step 5: Run focused and neighboring tests and verify GREEN**

Run from the test executable directory after the build:

```powershell
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[runtime][netmode][systems]"
.\ArcaneTests.exe "[base][assert]"
```

Expected: all selected cases pass; the duplicate case leaves one fixed-update entry plus the legal update entry, and the test process remains alive.

- [ ] **Step 6: Commit the core contract**

```powershell
git add ArcaneCore/src/Arcane/Plugin/GameSystems.hpp ArcaneCore/src/Arcane/Plugin/GameModule.hpp ArcaneCore/src/Arcane/Plugin/GameComponents.hpp ArcaneCore/src/Arcane/Plugin/SystemFactory.cpp ArcaneTests/src/RoleMaskTest.cpp
git commit -m "feat(systems): add automatic game-module registration"
```

### Task 2: Prove Automatic Registration Through a Real Plugin Lifecycle

**Files:**
- Modify/Test fixture: `ArcaneTests/plugins/HotReloadPlugin.cpp`
- Verify: `ArcaneTests/src/PluginHostTest.cpp` (existing cases)
- Verify: `ArcaneTests/src/RoleMaskTest.cpp` (existing cases)

**Interfaces:**
- Consumes: `ARCANE_SYSTEM` and `ARCANE_GAME_MODULE_ABI` from Task 1.
- Produces: a real DLL fixture with one automatically registered server system and one manually registered client system; the total remains two factories.

- [ ] **Step 1: Make the existing lifecycle tests fail by removing the old server wiring**

Delete only this line from `HotReloadPlugin.cpp`:

```cpp
RegisterSystem<ServerOnlyTick>(Arcane::RoleMask::Server,
                               Arcane::SystemPhase::FixedUpdate);
```

Do not add the macro yet. Update no assertions.

- [ ] **Step 2: Rebuild and verify RED through observable runtime behavior**

Run:

```powershell
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "two Runtimes, one module*"
```

Expected: FAIL because the dedicated-server scheduler lacks `ServerOnlyTick` and its counter remains zero.

- [ ] **Step 3: Register the server system automatically**

Beside the two `ARCANE_COMPONENT` declarations, add:

```cpp
ARCANE_SYSTEM(Arcane::HotReloadTest::ServerOnlyTick,
              Arcane::RoleMask::Server,
              Arcane::SystemPhase::FixedUpdate)
```

Leave `ClientOnlyTick` in `Module::OnInit` as the manual-path control. Rewrite the fixture comments to state that the same DLL deliberately exercises automatic and manual factory registration, and that the init-failure build accumulates both before returning false.

- [ ] **Step 4: Run all lifecycle cases and verify GREEN**

Run:

```powershell
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[runtime][netmode][hotreload]"
.\ArcaneTests.exe "[hotreload]"
```

Expected: all cases pass. In particular, the factory-table size remains `before + 2` while loaded and returns to `before` after unload; the init-failure case leaves the table at `before`; server/client role behavior remains distinct; reload still swaps V1/V2 behavior.

- [ ] **Step 5: Commit the real-DLL proof**

```powershell
git add ArcaneTests/plugins/HotReloadPlugin.cpp
git commit -m "test(systems): prove automatic registration lifecycle"
```

### Task 3: Generate Registered Systems for Every Phase and Role

**Files:**
- Modify: `ArcaneEditor/src/Project/ClassTemplates.hpp`
- Modify: `ArcaneEditor/src/Project/ClassTemplates.cpp`
- Modify/Test: `ArcaneTests/src/ClassTemplatesTest.cpp`

**Interfaces:**
- Consumes: `Arcane::SystemPhase`, `Arcane::RoleMask`, and `ARCANE_SYSTEM` from Task 1.
- Produces: `ClassTemplates::SystemOptions`, `kSystemPhaseChoiceCount`, `kSystemRoleChoiceCount`, `SystemPhaseChoiceLabel(int)`, `SystemRoleChoiceLabel(int)`, `SystemOptionsForChoiceIndices(int, int)`, and `Render(..., SystemOptions options = {})`.

- [ ] **Step 1: Replace the old header-only system expectations with failing generated-pair tests**

Update `ClassTemplatesTest.cpp` so the default system expects `Movement.hpp` plus `Movement.cpp`, an `ARCANE_SYSTEM` call, `Both`, `FixedUpdate`, and the fixed-update transform edge. Add table-driven coverage:

```cpp
struct PhaseCase
{
    int index;
    const char* spelling;
    bool transformAnchor;
};

for (const PhaseCase c : {
         PhaseCase{0, "Arcane::SystemPhase::FixedUpdate", true},
         PhaseCase{1, "Arcane::SystemPhase::Update", false},
         PhaseCase{2, "Arcane::SystemPhase::Render", false},
     })
{
    const auto options = ClassTemplates::SystemOptionsForChoiceIndices(c.index, 0);
    const auto rendered = ClassTemplates::Render(
        ClassTemplates::Kind::System, "Movement", "Aphelyon", options);
    CHECK(Has(rendered.source, c.spelling));
    CHECK(Has(rendered.header, "Astra::Before<Arcane::TransformPropagationSystem>")
          == c.transformAnchor);
    CHECK(Has(rendered.header, "#include <Arcane/Scene/TransformSystems.hpp>")
          == c.transformAnchor);
}
```

Add equivalent role coverage with literal expected spellings:

```cpp
struct RoleCase { int index; const char* spelling; };
for (const RoleCase c : {
         RoleCase{0, "Arcane::RoleMask::Both"},
         RoleCase{1, "Arcane::RoleMask::Server"},
         RoleCase{2, "Arcane::RoleMask::Client"},
     })
{
    const auto options = ClassTemplates::SystemOptionsForChoiceIndices(0, c.index);
    const auto rendered = ClassTemplates::Render(
        ClassTemplates::Kind::System, "Movement", "Aphelyon", options);
    CHECK(Has(rendered.source, c.spelling));
}
```

Test `SystemOptionsForChoiceIndices(-1, 99)` yields `FixedUpdate` + `Both`, and call `Render` with invalid cast enum values to prove it independently normalizes them. Check every generated file ends in a newline, contains no `{{` token, and contains comments explaining registrar discovery and ordering.

Use this independent invalid-enum assertion rather than deriving expected text from the renderer:

```cpp
ClassTemplates::SystemOptions invalid;
invalid.phase = static_cast<Arcane::SystemPhase>(255);
invalid.role  = static_cast<Arcane::RoleMask>(0);
const auto normalized = ClassTemplates::Render(
    ClassTemplates::Kind::System, "Movement", "Aphelyon", invalid);
CHECK(Has(normalized.source, "Arcane::SystemPhase::FixedUpdate"));
CHECK(Has(normalized.source, "Arcane::RoleMask::Both"));
```

- [ ] **Step 2: Run the editor template cases and verify RED**

Run:

```powershell
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "ClassTemplates::*"
```

Expected: compilation fails because the option and choice APIs are absent, or the old renderer fails because it emits no system source and still instructs manual `OnInit` registration.

- [ ] **Step 3: Add typed choices and normalization**

In `ClassTemplates.hpp`, include `Arcane/Plugin/SystemFactory.hpp` and define:

```cpp
inline constexpr int kSystemPhaseChoiceCount = 3;
inline constexpr int kSystemRoleChoiceCount  = 3;

struct SystemOptions
{
    Arcane::SystemPhase phase = Arcane::SystemPhase::FixedUpdate;
    Arcane::RoleMask    role  = Arcane::RoleMask::Both;
};

[[nodiscard]] const char* SystemPhaseChoiceLabel(int index) noexcept;
[[nodiscard]] const char* SystemRoleChoiceLabel(int index) noexcept;
[[nodiscard]] SystemOptions SystemOptionsForChoiceIndices(
    int phaseIndex, int roleIndex) noexcept;
```

Extend `Render` with a defaulted `SystemOptions` argument so existing component/plain-class callers remain source-compatible. Implement exhaustive switches/arrays in `ClassTemplates.cpp`; any invalid index or enum normalizes to the defaults.

- [ ] **Step 4: Emit phase-specific headers and the registrar source**

Split the system header template into a fixed-update variant and an unanchored update/render variant. The fixed variant includes `TransformSystems.hpp` and `Before<TransformPropagationSystem>` with a reason comment. The other variant omits both code elements and comments that fixed-step propagation is not installed in these schedulers, so the developer must add a relevant `Before<>`/`After<>` dependency when ordering matters.

Add a system source template:

```cpp
#include "{{CLASS}}.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// ARCANE_GAME_MODULE discovers this declaration while its DLL-owner bracket
// is open. Phase and role are explicit; scheduler order belongs in traits.
ARCANE_SYSTEM(
    {{NS}}::{{CLASS}},
    {{ROLE}},
    {{PHASE}})
```

Extend template filling for `{{ROLE}}` and `{{PHASE}}`. For `Kind::System`, assign both `sourceName` and `source`; remove the old paste-ready `OnInit` instructions.

- [ ] **Step 5: Run template and create-dialog pure tests and verify GREEN**

Run:

```powershell
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "ClassTemplates::*"
.\ArcaneTests.exe "[editor][create]"
```

Expected: all cases pass; existing component and plain-class output is byte-for-byte unchanged except for any deliberately updated surrounding documentation assertions.

- [ ] **Step 6: Commit template generation**

```powershell
git add ArcaneEditor/src/Project/ClassTemplates.hpp ArcaneEditor/src/Project/ClassTemplates.cpp ArcaneTests/src/ClassTemplatesTest.cpp
git commit -m "feat(editor): generate self-registering systems"
```

### Task 4: Expose Phase and Role in the Create C++ Class Dialog

**Files:**
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.hpp`
- Modify: `ArcaneEditor/src/Panels/CreateAssetDialog.cpp`
- Modify/Test: `ArcaneTests/src/CreateAssetDialogTest.cpp`
- Modify: `ArcaneEditor/src/App/EditorApp.hpp`
- Modify: `ArcaneEditor/src/App/EditorAppFrame.cpp`
- Modify: `ArcaneEditor/src/App/EditorAppProject.cpp`

**Interfaces:**
- Consumes: the choice counts, labels, `SystemOptionsForChoiceIndices`, and `Render` overload from Task 3.
- Produces: `CreateDialogState::systemPhaseIndex/systemRoleIndex`, matching `CreateAssetResult` fields, and `MintCppClass(..., int templateKind, int systemPhaseIndex, int systemRoleIndex)`.

- [ ] **Step 1: Write failing fresh-state tests**

Add to `CreateAssetDialogTest.cpp`:

```cpp
TEST_CASE("Create C++ System options start at Fixed Update and Both",
          "[editor][create]")
{
    const CreateDialogState state;
    const CreateAssetResult result;
    CHECK(state.systemPhaseIndex == 0);
    CHECK(state.systemRoleIndex == 0);
    CHECK(result.systemPhaseIndex == 0);
    CHECK(result.systemRoleIndex == 0);

    const auto options = ClassTemplates::SystemOptionsForChoiceIndices(
        state.systemPhaseIndex, state.systemRoleIndex);
    CHECK(options.phase == Arcane::SystemPhase::FixedUpdate);
    CHECK(options.role == Arcane::RoleMask::Both);
}
```

Include `Project/ClassTemplates.hpp` for the consumer-visible conversion assertion.

- [ ] **Step 2: Build and verify RED**

Run:

```powershell
msbuild Arcane.slnx /t:ArcaneTests /p:Configuration=Debug /m
```

Expected: compilation fails because the dialog state/result fields do not exist.

- [ ] **Step 3: Add dialog state, result propagation, and system-only controls**

Add zero-initialized `systemPhaseIndex` and `systemRoleIndex` fields to both `CreateDialogState` and `CreateAssetResult`. Aggregate reset in `BeginCreateAsset` then restores `FixedUpdate` + `Both` automatically.

In `CreateAssetDialog.cpp`, add a small indexed-combo helper and draw these controls only when the clamped class template is `Kind::System`:

```text
Execution Phase: Fixed Update | Update | Render
Network Role:    Both | Server | Client
```

Use Task 3's counts and label functions. Clamp stale indices before displaying them and assign the clamped values back to state. Replace the old “Add its AddSystem line” helper with text that says the generated `.cpp` registers the system on **Rebuild Game Module** and that ordering is declared in traits. Copy both indices into `CreateAssetResult` on Create.

- [ ] **Step 4: Carry selections through the dispatcher and mint**

Change the declaration and definition to:

```cpp
Arcane::Guid MintCppClass(const std::filesystem::path& headerTarget,
                          const std::string& className,
                          int templateKind,
                          int systemPhaseIndex,
                          int systemRoleIndex);
```

Pass the result fields from `ConsumeCreateResult`. In `MintCppClass`, convert them once:

```cpp
const ClassTemplates::SystemOptions systemOptions =
    ClassTemplates::SystemOptionsForChoiceIndices(
        systemPhaseIndex, systemRoleIndex);
const ClassTemplates::Rendered files =
    ClassTemplates::Render(kind, className,
                           proj->Manifest().name, systemOptions);
```

Do not add a second file-writing path: the existing optional-source transaction already refuses collisions before writing, removes the header if source creation fails, registers both files, regenerates the solution, and opens the source.

- [ ] **Step 5: Run pure tests, build the editor, and desk-check the ImGui branch**

Run:

```powershell
msbuild Arcane.slnx "/t:ArcaneTests;ArcaneEditor" /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe "[editor][create]"
.\ArcaneTests.exe "ClassTemplates::*"
```

Expected: all selected tests pass and `ArcaneEditor` builds. Desk-check in the editor: System shows both combos; Component and Plain Class hide them; cancel/reopen restores Fixed Update + Both; selecting Render + Client produces a `.cpp` containing those exact enums and a header without the transform-propagation edge.

- [ ] **Step 6: Commit dialog plumbing**

```powershell
git add ArcaneEditor/src/Panels/CreateAssetDialog.hpp ArcaneEditor/src/Panels/CreateAssetDialog.cpp ArcaneTests/src/CreateAssetDialogTest.cpp ArcaneEditor/src/App/EditorApp.hpp ArcaneEditor/src/App/EditorAppFrame.cpp ArcaneEditor/src/App/EditorAppProject.cpp
git commit -m "feat(editor): configure created system phase and role"
```

### Task 5: Migrate the Reference System and Run the Complete Gate

**Files:**
- Create: `ReferenceProject/Source/Game/PlayerController2DSystem.cpp`
- Modify: `ReferenceProject/Source/Game/PlayerController2DSystem.hpp`
- Modify: `ReferenceProject/Source/Game/ReferenceGame.cpp`
- Verify only: all files from Tasks 1-4

**Interfaces:**
- Consumes: `ARCANE_SYSTEM`, generated-code conventions, and the game-module automatic traversal.
- Produces: a reference game whose player-controller system is active without any module-specific include or `OnInit` registration.

- [ ] **Step 1: Make the reference build red by removing manual module registration**

Return `ReferenceGame.cpp` to the minimal module:

```cpp
#include <Arcane/Plugin/GameModule.hpp>

namespace ReferenceGame
{
    struct Module final : Arcane::GameModule {};
}

ARCANE_GAME_MODULE(ReferenceGame::Module)
```

Do not add `PlayerController2DSystem.cpp` yet. Rebuild and launch a short reference runtime/editor smoke if the scene contains a matching entity; the system is absent. At minimum, inspect the built plugin log/factory count to confirm no player-controller factory is declared. This is the deliberate RED created by removing the legacy wiring; compilation alone is not the behavioral assertion.

- [ ] **Step 2: Add the reference registrar and update its reasoning comments**

Create:

```cpp
#include "PlayerController2DSystem.hpp"

#include <Arcane/Plugin/GameSystems.hpp>

// The module prologue discovers this default-constructible system. It runs in
// fixed simulation before transform propagation; both roles are intentional
// for this reference input/movement probe.
ARCANE_SYSTEM(
    ReferenceProject::PlayerController2DSystem,
    Arcane::RoleMask::Both,
    Arcane::SystemPhase::FixedUpdate)
```

Replace the header's manual-`OnInit` instructions with comments pointing to the `.cpp`, explaining why fixed update receives the transform-propagation edge, why registrar order is irrelevant, and when a project should choose Server or Client instead of Both. Do not change the controller's movement semantics or touch `TestComponent.*`.

- [ ] **Step 3: Regenerate and build the external-SDK-style reference project**

Run:

```powershell
cd ReferenceProject
..\ThirdParty\premake5\premake5.exe vs2026
msbuild ReferenceProject.slnx /p:Configuration=Debug /m
```

Expected: `Binaries/ReferenceGame.dll` links with zero errors; generated project files include `PlayerController2DSystem.cpp`; there is no manual registration in `ReferenceGame.cpp`.

- [ ] **Step 4: Run the full engine verification gate**

From the repository root:

```powershell
GenerateProjects.bat
msbuild Arcane.slnx /p:Configuration=Debug /m
cd bin\Debug-windows-x86_64-md\ArcaneTests
.\ArcaneTests.exe
```

Expected: the complete unfiltered suite reports zero failed test cases. Capture the random seed if anything fails and rerun that failure with `--rng-seed <seed>` before changing code.

Then run the scripted reference-project smoke from the repository root:

```powershell
bin\Debug-windows-x86_64-md\ArcaneRuntime\ArcaneRuntime.exe --project ReferenceProject --frames 180
```

Expected: the game module loads, reports its automatic system count, runs 180 frames, and exits without validation/render errors. If the current reference scene has no matching `PlayerController2D + Transform` entity, factory/scheduler presence is the acceptance signal and the empty ECS query is expected.

- [ ] **Step 5: Inspect the final diff for scope and stale instructions**

Run:

```powershell
git diff --check
rg -n "Add its AddSystem|paste-ready RegisterSystem|systems stay EXPLICIT|PlayerController2DSystem>" ArcaneCore ArcaneEditor ArcaneTests ReferenceProject
git status --short
```

Expected: `git diff --check` is clean; no user-facing stale manual-wiring instruction remains; any remaining `RegisterSystem` occurrences are intentional manual API documentation/tests; unrelated dirty files remain unstaged and unchanged.

- [ ] **Step 6: Commit the reference migration**

```powershell
git add ReferenceProject/Source/Game/PlayerController2DSystem.cpp ReferenceProject/Source/Game/PlayerController2DSystem.hpp ReferenceProject/Source/Game/ReferenceGame.cpp
git commit -m "refactor(reference): adopt automatic system registration"
```

- [ ] **Step 7: Record verification evidence**

In the implementation handoff, report:

- the five task commit hashes;
- the exact ArcaneTests pass/fail totals and random seed;
- the ReferenceProject build warning/error totals;
- the 180-frame runtime exit result;
- any pre-existing dirty files that were deliberately left untouched.
