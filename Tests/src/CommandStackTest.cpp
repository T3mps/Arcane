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
#include <Arcane/Edit/CommandStack.hpp>
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
    // InspectEntity path the Inspector uses. NOTE: Astra::TypeMeta::typeName is
    // the __FUNCSIG__/__PRETTY_FUNCTION__-derived name, which is namespace-
    // qualified (e.g. "Arcane::LocalTransform"), not the bare type name --
    // adapted from the brief's literal "LocalTransform" after verifying against
    // the real TypeID<T>::Name() implementation (Astra/Core/TypeID.hpp).
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

    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");
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
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");
    REQUIRE(desc != nullptr);

    std::vector<std::byte> before = Arcane::ComponentEditCommand::Snapshot(*reg, e, desc);
    Arcane::ComponentEditCommand cmd(*reg, e, desc, before, before, "noop");

    reg->DestroyEntity(e);
    CHECK_NOTHROW(cmd.Undo());   // re-resolve returns null -> safe no-op
    CHECK_NOTHROW(cmd.Redo());
}

TEST_CASE("CommandStack: one-gesture transaction undo/redo + redo cleared on new edit", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    Arcane::LocalTransform lt; lt.position = glm::vec2(0.0f, 0.0f);
    reg->AddComponent<Arcane::LocalTransform>(e, lt);
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

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
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

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
    const Astra::ComponentDescriptor* dLt = DescriptorFor(*reg, e, "Arcane::LocalTransform");
    const Astra::ComponentDescriptor* dSr = DescriptorFor(*reg, e, "Arcane::SpriteRenderer");

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
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

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
