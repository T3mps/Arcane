# Editor Undo/Redo Command Foundation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A generic reflection-based undo/redo command stack in Arcane, wired so every Grimoire Inspector edit is undoable (Ctrl+Z / Ctrl+Y), transaction-grouped so gizmos plug in later.

**Architecture:** A new `Arcane/Edit/` module (`ARCANE_API`): `ICommand`, a `ComponentEditCommand` that snapshots a whole component to a byte blob via the Astra `descriptor->serialize/deserialize` seam (before→after) and re-resolves the live component by `(Entity, descriptor->hash)` at undo/redo time, and a `CommandStack` whose undo unit is a **Transaction** of 1..N commands (idempotent snapshot-on-first-touch, à la Unreal `Modify()`). Grimoire owns one stack and brackets Inspector gestures (`IsItemActivated` → snapshot before; `IsItemDeactivatedAfterEdit` → commit).

**Tech Stack:** C++23, Astra ECS (reflection + `BinaryWriter`/`BinaryReader`), Dear ImGui (Grimoire), Catch2. Spec: `docs/superpowers/specs/2026-07-20-arcane-edit-undo-command-design.md`.

## Global Constraints

- **`ARCANE_API`, editor-free engine:** the command module lives in Arcane; Grimoire only consumes it. No editor state in Arcane.
- **/MD everywhere; no `/fp:fast`; UTF-8 without BOM; ASCII comments.**
- **Headless tests are CPU-only** (tag `[edit]`, NO `[gpu]` tag) — they run anywhere, no graphics device.
- **No cross-DLL TypeContext pin needed** for `[edit]` tests: the command is hash/descriptor-driven (`GetComponentByHash` + `descriptor->serialize`), never a type-based `CreateView<T>`, so it resolves identically across modules.
- **Build (PowerShell, VS18 MSBuild):** `& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "Arcane\Arcane.slnx" /t:<Target> /p:Configuration=Debug /m`. New files → run `& "Arcane\GenerateProjects.bat"` once (premake globs `Arcane/**`).
- **Run headless tests** from the exe dir: `cd Arcane\bin\Debug-windows-x86_64-md\ArcaneTests` then `.\ArcaneTests.exe "[edit]"`.
- **Commits:** `type(scope): summary`, NO AI trailers.
- **Baseline:** `~[gpu]` 27785/329 (CPU floor, must not drop); `[edit]` count grows with the new cases.

---

## File Structure

| File | Change | Responsibility |
|---|---|---|
| `Arcane/Arcane/src/Arcane/Edit/Command.hpp` | Create | `ARCANE_API ICommand` interface. |
| `Arcane/Arcane/src/Arcane/Edit/ComponentEditCommand.hpp` | Create | `ComponentEditCommand` interface + `Snapshot` helper. |
| `Arcane/Arcane/src/Arcane/Edit/ComponentEditCommand.cpp` | Create | Impl: snapshot/restore via the descriptor serialize seam. |
| `Arcane/Arcane/src/Arcane/Edit/CommandStack.hpp` | Create | `CommandStack` + `ScopedTransaction` interfaces. |
| `Arcane/Arcane/src/Arcane/Edit/CommandStack.cpp` | Create | Impl: transactions, undo/redo deques, depth cap. |
| `Arcane/Tests/src/CommandStackTest.cpp` | Create | `[edit]` headless tests (command round-trip + stack + transactions). |
| `Arcane/Grimoire/src/EditorPanels.hpp` | Modify | `DrawInspectorPanel` gains a `CommandStack&`; toolbar gains Undo/Redo. |
| `Arcane/Grimoire/src/EditorPanels.cpp` | Modify | Visitor brackets edits into the stack; toolbar buttons. |
| `Arcane/Grimoire/src/GrimoireApp.hpp` | Modify | Own an `Arcane::CommandStack m_undo`. |
| `Arcane/Grimoire/src/GrimoireApp.cpp` | Modify | Construct the stack; Ctrl+Z/Y keybinds; Play clears the stack; pass the stack to the Inspector + toolbar. |

---

## Task 1: `ComponentEditCommand` — reflection snapshot/restore (headless)

The generic before→after component command. Snapshots a whole component to a byte blob via the descriptor serialize seam; undo/redo re-resolve the live component by `(Entity, descriptor->hash)` and deserialize.

**Files:** Create `Command.hpp`, `ComponentEditCommand.{hpp,cpp}`, `CommandStackTest.cpp`.

**Interfaces:**
- Produces: `class ARCANE_API ICommand { virtual void Undo()=0; virtual void Redo()=0; virtual const char* Label() const=0; virtual ~ICommand()=default; };`
- Produces: `class ARCANE_API ComponentEditCommand final : public ICommand` with ctor `(Astra::Registry&, Astra::Entity, const Astra::ComponentDescriptor*, std::vector<std::byte> before, std::vector<std::byte> after, std::string label)`, `Undo()`/`Redo()`/`Label()`, and `static std::vector<std::byte> Snapshot(Astra::Registry&, Astra::Entity, const Astra::ComponentDescriptor*)`.

- [ ] **Step 1: Write `Command.hpp`.**

```cpp
#pragma once

// Arcane/Edit: editor command foundation. ICommand is the undo/redo unit --
// the forward edit already happened (live), so a command only reverses/replays.
// ARCANE_API generic capability; Grimoire consumes it (no editor state here).

#include <Arcane/Base/Api.hpp>

namespace Arcane
{
    class ARCANE_API ICommand
    {
    public:
        virtual ~ICommand() = default;
        virtual void Undo() = 0;
        virtual void Redo() = 0;
        virtual const char* Label() const = 0;   // for UI / debug
    };
}
```

- [ ] **Step 2: Write the failing test** — `CommandStackTest.cpp`:

```cpp
// Arcane editor undo/redo commands + stack ([edit], CPU-only). No TypeContext
// pin: ComponentEditCommand is hash/descriptor-driven (GetComponentByHash +
// descriptor->serialize), never a type-based CreateView<T>.

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Astra/Registry/Registry.hpp>

#include <Arcane/Edit/Command.hpp>
#include <Arcane/Edit/ComponentEditCommand.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneModule.hpp>

namespace
{
    // Fresh registry with the scene components registered.
    std::unique_ptr<Astra::Registry> MakeReg()
    {
        auto components = std::make_shared<Astra::ComponentRegistry>();
        auto reg = std::make_unique<Astra::Registry>(components);
        Arcane::RegisterSceneComponents(*reg);
        return reg;
    }

    // The component descriptor for `typeName` on `entity`, via the same
    // InspectEntity path the Inspector uses.
    const Astra::ComponentDescriptor* DescriptorFor(Astra::Registry& reg,
                                                    Astra::Entity e, const char* typeName)
    {
        for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
            if (ci.meta && ci.meta->typeName == typeName)
                return ci.descriptor;
        return nullptr;
    }
}

TEST_CASE("ComponentEditCommand restores a component before/after via reflection", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::LocalTransform lt;
    lt.position = glm::vec2(1.0f, 2.0f);
    reg->AddComponent<Arcane::LocalTransform>(e, lt);

    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "LocalTransform");
    REQUIRE(desc != nullptr);

    // before = current; mutate; after = mutated.
    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    reg->GetComponent<Arcane::LocalTransform>(e)->position = glm::vec2(9.0f, 9.0f);
    std::vector<std::byte> after = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    REQUIRE(before != after);

    Arcane::ComponentEditCommand cmd(*reg, e, desc, before, after, "Edit LocalTransform");

    cmd.Undo();
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 1.0f);
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.y == 2.0f);

    cmd.Redo();
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 9.0f);
}

TEST_CASE("ComponentEditCommand no-ops on a deleted entity", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "LocalTransform");
    REQUIRE(desc != nullptr);

    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    Arcane::ComponentEditCommand cmd(*reg, e, desc, before, before, "noop");

    reg->DestroyEntity(e);
    CHECK_NOTHROW(cmd.Undo());   // re-resolve returns null -> safe no-op
    CHECK_NOTHROW(cmd.Redo());
}
```

- [ ] **Step 3: Run it, verify it fails** (no `ComponentEditCommand`). Regenerate first (new files): `& "Arcane\GenerateProjects.bat"`, build, then from the exe dir `.\ArcaneTests.exe "[edit]"`. Expected: link/compile error.

- [ ] **Step 4: Write `ComponentEditCommand.hpp`.**

```cpp
#pragma once

// Generic reflection-based component edit command: a whole-component before/
// after byte snapshot (via the Astra descriptor serialize seam). Undo/Redo
// re-resolve the LIVE component by (Entity, descriptor->hash) so it survives
// archetype moves and no-ops if the entity/component is gone. Restoring a
// LocalTransform reflects to the physics body via SPEC #1's polling reconcile.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
    class ARCANE_API ComponentEditCommand final : public ICommand
    {
    public:
        ComponentEditCommand(Astra::Registry& registry, Astra::Entity entity,
                             const Astra::ComponentDescriptor* descriptor,
                             std::vector<std::byte> before,
                             std::vector<std::byte> after,
                             std::string label);

        void Undo() override;   // deserialize `before` into the live component
        void Redo() override;   // deserialize `after`
        const char* Label() const override { return m_label.c_str(); }

        // Serialize the live component for (registry, entity, descriptor) to a
        // blob. Empty if the entity/component is not present.
        static std::vector<std::byte> Snapshot(Astra::Registry& registry,
                                               Astra::Entity entity,
                                               const Astra::ComponentDescriptor* descriptor);

    private:
        void Restore(const std::vector<std::byte>& blob);

        Astra::Registry&                   m_registry;
        Astra::Entity                      m_entity;
        const Astra::ComponentDescriptor*  m_descriptor;
        std::vector<std::byte>             m_before;
        std::vector<std::byte>             m_after;
        std::string                        m_label;
    };
}
```

- [ ] **Step 5: Write `ComponentEditCommand.cpp`.**

```cpp
#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <Astra/Component/Component.hpp>
#include <Astra/Registry/Registry.hpp>
#include <Astra/Serialization/BinaryReader.hpp>
#include <Astra/Serialization/BinaryWriter.hpp>

#include <span>
#include <utility>

namespace Arcane
{
    ComponentEditCommand::ComponentEditCommand(Astra::Registry& registry, Astra::Entity entity,
                                               const Astra::ComponentDescriptor* descriptor,
                                               std::vector<std::byte> before,
                                               std::vector<std::byte> after,
                                               std::string label)
        : m_registry(registry), m_entity(entity), m_descriptor(descriptor),
          m_before(std::move(before)), m_after(std::move(after)), m_label(std::move(label))
    {
    }

    std::vector<std::byte> ComponentEditCommand::Snapshot(Astra::Registry& registry,
                                                          Astra::Entity entity,
                                                          const Astra::ComponentDescriptor* descriptor)
    {
        std::vector<std::byte> blob;
        if (!descriptor || !descriptor->serialize)
            return blob;
        void* instance = registry.GetComponentByHash(entity, descriptor->hash);
        if (!instance)
            return blob;
        {
            Astra::BinaryWriter writer(blob, /*reserveSize*/ 256);
            descriptor->serialize(writer, instance);
        } // writer dtor flushes into `blob`
        return blob;
    }

    void ComponentEditCommand::Restore(const std::vector<std::byte>& blob)
    {
        if (!m_descriptor || !m_descriptor->deserialize || blob.empty())
            return;
        void* instance = m_registry.GetComponentByHash(m_entity, m_descriptor->hash);
        if (!instance)
            return;   // entity/component gone -> safe no-op
        Astra::BinaryReader reader(std::span<const std::byte>(blob));
        m_descriptor->deserialize(reader, instance);
    }

    void ComponentEditCommand::Undo() { Restore(m_before); }
    void ComponentEditCommand::Redo() { Restore(m_after); }
}
```

- [ ] **Step 6: Regenerate + build + run at, verify PASS.** `& "Arcane\GenerateProjects.bat"`, build ArcaneTests, then `.\ArcaneTests.exe "[edit]"`. Expected: 2 cases pass.

- [ ] **Step 7: Commit** — `feat(arcane): ComponentEditCommand -- reflection before/after component snapshot`.

---

## Task 2: `CommandStack` + `ScopedTransaction` (headless)

The undo/redo history. Its unit is a **Transaction** of 1..N `ComponentEditCommand`s. `Begin`/`SnapshotComponent`(idempotent)/`Commit`/`Cancel` grouping; depth cap; redo cleared on commit.

**Files:** Create `CommandStack.{hpp,cpp}`. Modify `CommandStackTest.cpp` (append).

**Interfaces:**
- Consumes: `ComponentEditCommand` (Task 1).
- Produces: `class ARCANE_API CommandStack` — ctor `(Astra::Registry&, std::size_t maxDepth = 100)`; `Begin(std::string)`, `SnapshotComponent(Astra::Entity, const Astra::ComponentDescriptor*)`, `Commit()`, `Cancel()`, `Undo()`, `Redo()`, `bool CanUndo()/CanRedo() const`, `const char* UndoLabel()/RedoLabel() const`, `void Clear()`.
- Produces: `class ARCANE_API ScopedTransaction { ScopedTransaction(CommandStack&, std::string); ~ScopedTransaction(); void Snapshot(Astra::Entity, const Astra::ComponentDescriptor*); void Cancel(); }`.

- [ ] **Step 1: Write the failing tests** (append to `CommandStackTest.cpp`):

```cpp
#include <Arcane/Edit/CommandStack.hpp>

TEST_CASE("CommandStack: one-gesture transaction undo/redo + redo cleared on new edit", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::LocalTransform lt; lt.position = glm::vec2(0.0f, 0.0f);
    reg->AddComponent<Arcane::LocalTransform>(e, lt);
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "LocalTransform");

    Arcane::CommandStack stack(*reg);

    // Gesture: Begin -> Snapshot (before) -> mutate -> Commit (after).
    stack.Begin("move");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::LocalTransform>(e)->position = glm::vec2(5.0f, 0.0f);
    stack.Commit();

    REQUIRE(stack.CanUndo());
    REQUIRE_FALSE(stack.CanRedo());

    stack.Undo();
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 0.0f);
    REQUIRE(stack.CanRedo());

    stack.Redo();
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 5.0f);

    // A new edit after an undo clears the redo stack.
    stack.Undo();                         // back to x=0, redo available
    REQUIRE(stack.CanRedo());
    stack.Begin("move2"); stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::LocalTransform>(e)->position = glm::vec2(7.0f, 0.0f);
    stack.Commit();
    CHECK_FALSE(stack.CanRedo());
}

TEST_CASE("CommandStack: empty / no-op transaction is not pushed", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "LocalTransform");

    Arcane::CommandStack stack(*reg);
    stack.Begin("noop");
    stack.SnapshotComponent(e, desc);     // no mutation
    stack.Commit();                        // before == after -> nothing pushed
    CHECK_FALSE(stack.CanUndo());

    stack.Begin("cancelled");
    stack.SnapshotComponent(e, desc);
    stack.Cancel();
    CHECK_FALSE(stack.CanUndo());
}

TEST_CASE("CommandStack: a transaction groups two components into one undo step", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    reg->AddComponent<Arcane::SpriteRenderer>(e, Arcane::SpriteRenderer{});
    const Astra::ComponentDescriptor* dLt = DescriptorFor(*reg, e, "LocalTransform");
    const Astra::ComponentDescriptor* dSr = DescriptorFor(*reg, e, "SpriteRenderer");

    Arcane::CommandStack stack(*reg);
    stack.Begin("multi");
    stack.SnapshotComponent(e, dLt);
    stack.SnapshotComponent(e, dSr);
    stack.SnapshotComponent(e, dLt);      // idempotent: second touch is a no-op
    reg->GetComponent<Arcane::LocalTransform>(e)->position = glm::vec2(3.0f, 0.0f);
    reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer = 4;
    stack.Commit();

    stack.Undo();                          // ONE undo reverts BOTH
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 0.0f);
    CHECK(reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer == 0);
}

TEST_CASE("CommandStack: depth cap drops the oldest", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "LocalTransform");

    Arcane::CommandStack stack(*reg, /*maxDepth*/ 2);
    for (int i = 1; i <= 3; ++i)
    {
        stack.Begin("e");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::LocalTransform>(e)->position = glm::vec2((float)i, 0.0f);
        stack.Commit();
    }
    // Cap 2: only the last two edits are undoable (3->2, 2->1); the 0->1 op was dropped.
    stack.Undo(); stack.Undo();
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 1.0f);
    CHECK_FALSE(stack.CanUndo());          // the oldest (would restore x=0) was evicted
}
```

- [ ] **Step 2: Run headless, verify fail** (`.\ArcaneTests.exe "[edit]"` from the exe dir): compile/link error (no `CommandStack`).

- [ ] **Step 3: Write `CommandStack.hpp`.**

```cpp
#pragma once

// Arcane/Edit: undo/redo history. The undo unit is a Transaction of 1..N
// ComponentEditCommands (Unreal FTransaction model). Begin/SnapshotComponent
// (idempotent snapshot-on-first-touch)/Commit/Cancel groups a gesture into one
// step. ARCANE_API; Grimoire owns one and brackets its Inspector edits.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Edit/Command.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane
{
    class ARCANE_API CommandStack
    {
    public:
        explicit CommandStack(Astra::Registry& registry, std::size_t maxDepth = 100);

        // Transaction grouping.
        void Begin(std::string label);   // open a transaction (no-op if one is open)
        // Idempotent before-snapshot of (entity, descriptor) into the open
        // transaction. Call BEFORE the live edit mutates the component.
        void SnapshotComponent(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        void Commit();   // capture afters; push if any changed; clear redo; close
        void Cancel();   // discard the open transaction (no push, no revert)

        void Undo();
        void Redo();
        [[nodiscard]] bool CanUndo() const noexcept { return !m_undo.empty(); }
        [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.empty(); }
        [[nodiscard]] const char* UndoLabel() const noexcept;
        [[nodiscard]] const char* RedoLabel() const noexcept;
        void Clear() noexcept;

    private:
        struct Transaction
        {
            std::string label;
            std::vector<std::unique_ptr<ICommand>> commands;
        };
        struct Pending
        {
            Astra::Entity                     entity;
            const Astra::ComponentDescriptor* descriptor;
            std::vector<std::byte>            before;
        };

        Astra::Registry& m_registry;
        std::size_t      m_maxDepth;

        std::deque<Transaction> m_undo;
        std::deque<Transaction> m_redo;

        bool                 m_open = false;
        std::string          m_openLabel;
        std::vector<Pending> m_pending;
    };

    // RAII form for single-scope edits (gizmo drag-commit, programmatic
    // multi-edit). Commits in the dtor unless Cancel() was called. NOT for the
    // Inspector -- its gesture spans frames, so it uses explicit Begin/Commit.
    class ARCANE_API ScopedTransaction
    {
    public:
        ScopedTransaction(CommandStack& stack, std::string label);
        ~ScopedTransaction();
        void Snapshot(Astra::Entity entity, const Astra::ComponentDescriptor* descriptor);
        void Cancel() noexcept { m_cancelled = true; }

        ScopedTransaction(const ScopedTransaction&) = delete;
        ScopedTransaction& operator=(const ScopedTransaction&) = delete;

    private:
        CommandStack& m_stack;
        bool          m_cancelled = false;
    };
}
```

- [ ] **Step 4: Write `CommandStack.cpp`.**

```cpp
#include <Arcane/Edit/CommandStack.hpp>

#include <Arcane/Edit/ComponentEditCommand.hpp>

#include <utility>

namespace Arcane
{
    CommandStack::CommandStack(Astra::Registry& registry, std::size_t maxDepth)
        : m_registry(registry), m_maxDepth(maxDepth ? maxDepth : 1)
    {
    }

    void CommandStack::Begin(std::string label)
    {
        if (m_open)
            return;   // already open (exclusive gestures); keep the first
        m_open = true;
        m_openLabel = std::move(label);
        m_pending.clear();
    }

    void CommandStack::SnapshotComponent(Astra::Entity entity,
                                         const Astra::ComponentDescriptor* descriptor)
    {
        if (!m_open || !descriptor)
            return;
        // Idempotent: first touch of (entity, descriptor) snapshots; later ones no-op.
        for (const Pending& p : m_pending)
            if (p.entity == entity && p.descriptor == descriptor)
                return;
        m_pending.push_back(Pending{
            entity, descriptor,
            ComponentEditCommand::Snapshot(m_registry, entity, descriptor) });
    }

    void CommandStack::Commit()
    {
        if (!m_open)
            return;
        Transaction txn;
        txn.label = m_openLabel;
        for (Pending& p : m_pending)
        {
            std::vector<std::byte> after =
                ComponentEditCommand::Snapshot(m_registry, p.entity, p.descriptor);
            if (after == p.before)
                continue;   // unchanged -> drop
            txn.commands.push_back(std::make_unique<ComponentEditCommand>(
                m_registry, p.entity, p.descriptor,
                std::move(p.before), std::move(after), m_openLabel));
        }
        m_open = false;
        m_pending.clear();
        if (txn.commands.empty())
            return;   // nothing changed -> no history entry

        m_undo.push_back(std::move(txn));
        m_redo.clear();
        while (m_undo.size() > m_maxDepth)
            m_undo.pop_front();   // drop the oldest
    }

    void CommandStack::Cancel()
    {
        m_open = false;
        m_pending.clear();
    }

    void CommandStack::Undo()
    {
        if (m_undo.empty())
            return;
        Transaction txn = std::move(m_undo.back());
        m_undo.pop_back();
        for (auto it = txn.commands.rbegin(); it != txn.commands.rend(); ++it)
            (*it)->Undo();   // reverse order
        m_redo.push_back(std::move(txn));
    }

    void CommandStack::Redo()
    {
        if (m_redo.empty())
            return;
        Transaction txn = std::move(m_redo.back());
        m_redo.pop_back();
        for (auto& c : txn.commands)
            c->Redo();       // forward order
        m_undo.push_back(std::move(txn));
    }

    const char* CommandStack::UndoLabel() const noexcept
    {
        return m_undo.empty() ? "" : m_undo.back().label.c_str();
    }
    const char* CommandStack::RedoLabel() const noexcept
    {
        return m_redo.empty() ? "" : m_redo.back().label.c_str();
    }

    void CommandStack::Clear() noexcept
    {
        m_undo.clear();
        m_redo.clear();
        m_open = false;
        m_pending.clear();
    }

    // ---- ScopedTransaction --------------------------------------------------
    ScopedTransaction::ScopedTransaction(CommandStack& stack, std::string label)
        : m_stack(stack)
    {
        m_stack.Begin(std::move(label));
    }
    ScopedTransaction::~ScopedTransaction()
    {
        if (m_cancelled) m_stack.Cancel();
        else             m_stack.Commit();
    }
    void ScopedTransaction::Snapshot(Astra::Entity entity,
                                     const Astra::ComponentDescriptor* descriptor)
    {
        m_stack.SnapshotComponent(entity, descriptor);
    }
}
```

- [ ] **Step 5: Regenerate + build + run, verify PASS.** `& "Arcane\GenerateProjects.bat"`, build ArcaneTests, `.\ArcaneTests.exe "[edit]"`. Expected: all `[edit]` cases pass (Task 1's 2 + Task 2's 4).

- [ ] **Step 6: Commit** — `feat(arcane): CommandStack + ScopedTransaction -- transaction-grouped undo/redo`.

---

## Task 3: Grimoire integration — Inspector capture + keybinds + toolbar

Grimoire owns one `CommandStack`, brackets Inspector gestures into it, maps Ctrl+Z/Y, shows Undo/Redo toolbar buttons, and clears the stack on Play. Not headless-testable (ImGui); verified by build + desk.

**Files:** Modify `GrimoireApp.{hpp,cpp}`, `EditorPanels.{hpp,cpp}`.

**Interfaces:**
- Consumes: `Arcane::CommandStack` (Task 2).
- Produces: `Grimoire::DrawInspectorPanel(Astra::Registry&, const SelectionContext&, Arcane::CommandStack&)`; `Grimoire::DrawSimTimeToolbar(...)` gains a `CommandStack&` param.

- [ ] **Step 0 (read-first):** re-read `Arcane/Grimoire/src/EditorPanels.cpp` lines 126-209 (the `ImGuiFieldVisitor` + `DrawInspectorPanel`) and `GrimoireApp.cpp` lines 155-260 (input sample + the Play/Stop toolbar + panel draws) so the edits below drop into the exact current structure.

- [ ] **Step 1: Own the stack in Grimoire.** `GrimoireApp.hpp`: add `#include <Arcane/Edit/CommandStack.hpp>`, forward nothing new, and add a member beside `m_selection`:

```cpp
        // Editor undo/redo history (Edit-mode; cleared on Play). Constructed in
        // Init once the runtime's registry exists. optional so it can be built
        // after m_runtime (a CommandStack holds a Registry&).
        std::optional<Arcane::CommandStack> m_undo;
```

`GrimoireApp.cpp` in `Init()`, AFTER `m_runtime.emplace(...)` and the registry is live (right after the plugin load / before returning true):

```cpp
        m_undo.emplace(m_runtime->Registry());
```

- [ ] **Step 2: Bracket Inspector edits.** In `EditorPanels.cpp`, extend `ImGuiFieldVisitor` to hold the stack + entity + descriptor, and snapshot BEFORE apply / commit on release. Replace the `ImGuiFieldVisitor` struct + its use in `DrawInspectorPanel` with:

```cpp
        struct ImGuiFieldVisitor : Astra::IFieldVisitor
        {
            Arcane::CommandStack*             stack = nullptr;
            Astra::Entity                     entity{};
            const Astra::ComponentDescriptor* descriptor = nullptr;
            std::string                       typeName;

            bool IsWriting() const noexcept override { return true; }

            // Open a transaction + snapshot the component the first frame this
            // widget activates -- BEFORE the edit applies (so a same-frame
            // click+drag still captures the true 'before').
            void BeginGestureIfActivated(const std::string& field)
            {
                if (stack && ImGui::IsItemActivated())
                {
                    stack->Begin("Edit " + typeName + "." + field);
                    stack->SnapshotComponent(entity, descriptor);
                }
            }

            void Visit(const Astra::FieldInfo& f, void* instance) override
            {
                ImGui::PushID(static_cast<int>(f.nameHash));
                const std::string label(f.name);
                switch (Grimoire::ClassifyField(f))
                {
                    case Grimoire::FieldKind::Bool:
                    {
                        bool v = f.Get<bool>(instance);
                        bool changed = ImGui::Checkbox(label.c_str(), &v);
                        BeginGestureIfActivated(label);
                        if (changed) Grimoire::ApplyBoolEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Int32:
                    {
                        int v = f.Get<int32_t>(instance);
                        bool changed = ImGui::DragInt(label.c_str(), &v);
                        BeginGestureIfActivated(label);
                        if (changed) Grimoire::ApplyIntEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Float:
                    {
                        float v = f.Get<float>(instance);
                        bool changed = ImGui::DragFloat(label.c_str(), &v, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) Grimoire::ApplyFloatEdit(f, instance, v);
                        break;
                    }
                    case Grimoire::FieldKind::Vec2:
                    {
                        glm::vec2 v = f.Get<glm::vec2>(instance);
                        bool changed = ImGui::DragFloat2(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) if (glm::vec2* p = f.GetPtr<glm::vec2>(instance)) *p = v;
                        break;
                    }
                    case Grimoire::FieldKind::Vec3:
                    {
                        glm::vec3 v = f.Get<glm::vec3>(instance);
                        bool changed = ImGui::DragFloat3(label.c_str(), &v.x, 0.1f);
                        BeginGestureIfActivated(label);
                        if (changed) if (glm::vec3* p = f.GetPtr<glm::vec3>(instance)) *p = v;
                        break;
                    }
                    case Grimoire::FieldKind::ReadOnly:
                    default:
                        ImGui::BeginDisabled();
                        ImGui::Text("%s (unsupported)", label.c_str());
                        ImGui::EndDisabled();
                        break;
                }
                // Close the gesture: commit if the value changed, else discard
                // the (pure-click) open transaction so it never leaks.
                if (stack)
                {
                    if (ImGui::IsItemDeactivatedAfterEdit()) stack->Commit();
                    else if (ImGui::IsItemDeactivated())     stack->Cancel();
                }
                ImGui::PopID();
            }
        };
```

And `DrawInspectorPanel` signature + per-component wiring:

```cpp
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel,
                            Arcane::CommandStack& undo)
    {
        ImGui::Begin("Inspector");
        if (!sel.HasSelection())
        {
            ImGui::TextDisabled("No selection");
            ImGui::End();
            return;
        }
        for (const Astra::Registry::ComponentInfo& ci : registry.InspectEntity(sel.selected))
        {
            if (!ci.descriptor || !ci.descriptor->visitFields || !ci.data) continue;
            const std::string typeName = ci.meta ? std::string(ci.meta->typeName) : std::string("<unreflected>");
            if (ImGui::CollapsingHeader(typeName.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGuiFieldVisitor visitor;
                visitor.stack      = &undo;
                visitor.entity     = sel.selected;
                visitor.descriptor = ci.descriptor;
                visitor.typeName   = typeName;
                ci.descriptor->visitFields(ci.data, visitor);
            }
        }
        ImGui::End();
    }
```

Update the declaration in `EditorPanels.hpp` to `void DrawInspectorPanel(Astra::Registry&, const SelectionContext&, Arcane::CommandStack&);` and add `#include <Arcane/Edit/CommandStack.hpp>` there. Add `#include <string>` to `EditorPanels.cpp` if not present.

- [ ] **Step 3: Call site + keybinds + Play-clears + toolbar.** In `GrimoireApp.cpp`:

Update the Inspector call:
```cpp
            Grimoire::DrawInspectorPanel(m_runtime->Registry(), m_selection, *m_undo);
```

Add Ctrl+Z / Ctrl+Y in the input block (Edit-mode only; skip while a text field has keyboard focus). After `m_gpu->Input().Update(frameDt, snap);` and where `!m_play.IsPlaying()` is known (Edit mode), add:
```cpp
            // Undo/redo (Edit mode only; not while typing in an ImGui text field).
            if (!m_play.IsPlaying() && !m_gpu->Imgui().WantCaptureKeyboard())
            {
                const bool ctrl = snap.KeyDown(Arcane::Key::LeftCtrl) || snap.KeyDown(Arcane::Key::RightCtrl);
                const bool shift = snap.KeyDown(Arcane::Key::LeftShift) || snap.KeyDown(Arcane::Key::RightShift);
                if (ctrl && snap.KeyPressed(Arcane::Key::Z))
                    (shift ? m_undo->Redo() : m_undo->Undo());
                else if (ctrl && snap.KeyPressed(Arcane::Key::Y))
                    m_undo->Redo();
            }
```
(At Step 0, confirm the exact `InputSnapshot` key API — `KeyDown`/`KeyPressed` + the `Arcane::Key` enum names; adjust the calls to match. The intent: Ctrl+Z undo, Ctrl+Shift+Z / Ctrl+Y redo, edge-triggered.)

Clear the stack on the Edit→Play transition. Find where Play starts (the toolbar's Play button / `m_play` state change) and add `m_undo->Clear();` when entering Play.

- [ ] **Step 4: Toolbar Undo/Redo buttons.** Pass the stack to `DrawSimTimeToolbar` and add two buttons (enabled from `CanUndo/CanRedo`). In `EditorPanels.cpp` `DrawSimTimeToolbar`, add after the existing controls:
```cpp
        ImGui::SameLine();
        ImGui::BeginDisabled(!undo.CanUndo());
        if (ImGui::Button("Undo")) undo.Undo();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && undo.CanUndo()) ImGui::SetTooltip("Undo %s", undo.UndoLabel());
        ImGui::SameLine();
        ImGui::BeginDisabled(!undo.CanRedo());
        if (ImGui::Button("Redo")) undo.Redo();
        ImGui::EndDisabled();
```
Update `DrawSimTimeToolbar`'s signature (hpp + cpp + call site in `GrimoireApp.cpp`) to take `Arcane::CommandStack& undo`.

- [ ] **Step 5: Regenerate + build Grimoire + ArcaneTests.** `& "Arcane\GenerateProjects.bat"` (no new files, but safe), build `/t:ArcaneTests;Grimoire`. Expected: clean build, no errors.

- [ ] **Step 6: Desk-verify** (interactive Grimoire): select an entity; drag a `LocalTransform` field; `Ctrl+Z` reverts it (and the body follows for a physics entity); `Ctrl+Y` / `Ctrl+Shift+Z` re-applies; the toolbar Undo/Redo buttons enable/disable + show the label tooltip; press Play → history clears (Undo disabled). Report results.

- [ ] **Step 7: Commit** — `feat(grimoire): undoable Inspector edits (Ctrl+Z/Y, toolbar) on the Arcane command stack`.

---

## Task 4: Gate + desk-verify

- [ ] **Step 1: Headless gate** (runs here): from the exe dir `.\ArcaneTests.exe "~[gpu]"` — the new `[edit]` cases pass; the CPU floor (27785/329 + the new `[edit]` cases) does not drop. Record the new count.
- [ ] **Step 2: Desk interactive** (Grimoire): the Task 3 Step 6 checklist end-to-end, plus: undo across two different components on one entity, undo after re-selecting a different entity, and an edit → undo → new edit clears redo. Confirm no NVRHI/validation noise in the console.
- [ ] **Step 3:** Append a completion note to `.superpowers/sdd/progress.md`.

---

## Self-Review

**Spec coverage:** §4 architecture (Arcane/Edit module) → T1/T2; §5 reflection command (snapshot/restore, re-resolve, no-op-on-gone) → T1; §6 transaction model (Begin/Snapshot/Commit/Cancel, idempotent, compound undo, ScopedTransaction) → T2; §7 Inspector capture (gesture bracketing, snapshot-before-apply, no-op skip) → T3; §8 keybinds + toolbar → T3; §9 edge cases (Play clears → T3; deleted entity → T1; redo-invalidation + depth cap + no-op → T2; PostEditUndo via polling reconcile → inherent, no code) → covered; §10 testing → T1/T2 (headless) + T3/T4 (desk); §11 non-goals untouched; §12 impl-time points → T3 Step 0 (component-resolution confirmed = `GetComponentByHash`; BinaryWriter/Reader confirmed; ImGui signals + keybind API confirmed at read-first). Covered.

**Placeholder scan:** the two flagged items — the exact `InputSnapshot` key API (T3 Step 3) and the Play-transition call site (T3 Step 3) — are read-first confirmations against named files, not deferred TODOs; the code shows the exact intent to adapt. No "TBD"/"add error handling"/uncoded steps.

**Type consistency:** `ICommand{Undo,Redo,Label}`, `ComponentEditCommand(Registry&, Entity, const ComponentDescriptor*, before, after, label)` + `Snapshot(...)`, `CommandStack(Registry&, maxDepth)` + `Begin/SnapshotComponent/Commit/Cancel/Undo/Redo/CanUndo/CanRedo/UndoLabel/RedoLabel/Clear`, `ScopedTransaction(CommandStack&, label)` + `Snapshot/Cancel`, `DrawInspectorPanel(Registry&, SelectionContext&, CommandStack&)`, `DrawSimTimeToolbar(..., CommandStack&)` — consistent across tasks.
