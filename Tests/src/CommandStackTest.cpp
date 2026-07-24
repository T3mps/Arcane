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

    Arcane::ComponentEditCommand cmd([&reg]() -> Astra::Registry& { return *reg; }, e, desc, before, after, "Edit LocalTransform");

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
    Arcane::ComponentEditCommand cmd([&reg]() -> Astra::Registry& { return *reg; }, e, desc, before, before, "noop");

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

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

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

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
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

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    stack.Begin("multi");
    stack.SnapshotComponent(e, dLt);      // captures before = 0
    stack.SnapshotComponent(e, dSr);
    // Discriminating idempotency check: mutate BETWEEN the two dLt touches. If
    // SnapshotComponent were NOT idempotent (re-captured on the second touch),
    // the recorded `before` would be this x=3 value and Undo would restore
    // x=3 instead of the true original x=0.
    reg->GetComponent<Arcane::LocalTransform>(e)->position.x = 3.0f;
    stack.SnapshotComponent(e, dLt);      // idempotent: MUST NOT re-capture x=3
    reg->GetComponent<Arcane::LocalTransform>(e)->position.x = 7.0f;
    reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer = 4;
    stack.Commit();

    stack.Undo();                          // ONE undo reverts BOTH
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 0.0f);   // fails if 2nd touch re-snapshotted
    CHECK(reg->GetComponent<Arcane::SpriteRenderer>(e)->sortingLayer == 0);
}

TEST_CASE("CommandStack: survives a registry object swap (resolver, no dangling Registry&)", "[edit]")
{
    // The regression guard for the Critical defect: a command committed
    // BEFORE a registry swap must not hold a dangling Astra::Registry&. The
    // resolver re-reads the CURRENT `reg` pointer on every call, so it always
    // targets whichever registry object is live, mirroring how a real
    // Runtime::RestoreRegistry/ResetRegistry swap replaces the registry object
    // out from under a long-lived CommandStack/ComponentEditCommand.
    //
    // Deliberately NOT MakeReg() here: that helper builds a brand-new
    // Astra::ComponentRegistry per call, and this test's pre-swap command
    // captures a `desc` pointer into the OLD registry's descriptor array via
    // `m_descriptor`. `reg = MakeReg()` would free that ComponentRegistry
    // (the Registry is its sole owner) out from under the still-live
    // `m_descriptor`, so the CHECK_NOTHROW(stack.Undo()) below would read
    // freed memory -- UB, and harsher than production. Production's
    // Runtime::ResetRegistry/RestoreRegistry keep the SAME shared
    // Astra::ComponentRegistry across a registry-object swap (only the
    // Registry object is replaced; see Runtime.cpp), so descriptors never
    // dangle there. Mirror that here: one shared ComponentRegistry, two
    // Registry objects. RegisterComponent<T> is idempotent (guarded by a
    // per-id atomic flag living on the ComponentRegistry -- see
    // ComponentRegistry.hpp), so registering scene components once, before
    // the swap, is enough; the post-swap Registry inherits the registration
    // through the shared ComponentRegistry, exactly as ResetRegistry does not
    // re-register component types after swapping in a fresh Registry object.
    auto components = std::make_shared<Astra::ComponentRegistry>();
    auto reg = std::make_unique<Astra::Registry>(components);
    Arcane::RegisterSceneComponents(*reg);
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });

    // Commit one edit on the pre-swap registry.
    stack.Begin("pre-swap edit");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::LocalTransform>(e)->position = glm::vec2(1.0f, 0.0f);
    stack.Commit();
    REQUIRE(stack.CanUndo());

    // Swap: frees the OLD registry OBJECT the pre-swap command captured
    // through the resolver (still the regression guard for the dangling-
    // Registry& defect). The SAME shared ComponentRegistry survives the swap,
    // so `desc`/`m_descriptor` stay valid -- no freed-descriptor read. Under
    // the old cached-Registry& design, the next Undo would dereference freed
    // memory (UAF). Under the resolver design it resolves the NEW registry.
    reg = std::make_unique<Astra::Registry>(components);

    // The pre-swap entity does not exist in the new registry -> GetComponentByHash
    // returns null -> Restore is a safe no-op. No crash = the win.
    CHECK_NOTHROW(stack.Undo());

    // Positive path: the stack must operate on the SWAPPED-IN registry, not a
    // stale one.
    const Astra::Entity e2 = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e2, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc2 = DescriptorFor(*reg, e2, "Arcane::LocalTransform");

    stack.Begin("post-swap edit");
    stack.SnapshotComponent(e2, desc2);
    reg->GetComponent<Arcane::LocalTransform>(e2)->position.x = 5.0f;
    stack.Commit();

    stack.Undo();
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e2)->position.x == 0.0f);
}

namespace
{
    // Generic ICommand fake: counts Undo/Redo calls (the material-param /
    // future-graph edit path through CommandStack::Push).
    struct CountingCommand final : Arcane::ICommand
    {
        int* undos;
        int* redos;
        std::string label;

        CountingCommand(int* u, int* r, std::string l)
            : undos(u), redos(r), label(std::move(l)) {}
        void Undo() override { ++*undos; }
        void Redo() override { ++*redos; }
        const char* Label() const override { return label.c_str(); }
    };
}

TEST_CASE("CommandStack::Push: standalone step, transaction join, redo-clear", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int undos = 0, redos = 0;

    SECTION("outside a gesture, Push is its own labeled undo step")
    {
        stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "Edit Speed"));
        REQUIRE(stack.CanUndo());
        CHECK(std::string(stack.UndoLabel()) == "Edit Speed");
        stack.Undo();
        CHECK(undos == 1);
        stack.Redo();
        CHECK(redos == 1);
    }

    SECTION("inside a gesture, Push joins the transaction (one undo step)")
    {
        stack.Begin("combo");
        stack.SnapshotComponent(e, desc);
        reg->GetComponent<Arcane::LocalTransform>(e)->position.x = 4.0f;
        stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "generic"));
        stack.Commit();

        stack.Undo();   // ONE undo reverts the component edit AND the generic
        CHECK(undos == 1);
        CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 0.0f);
        CHECK_FALSE(stack.CanUndo());
    }

    SECTION("a cancelled gesture discards joined generics")
    {
        stack.Begin("cancelled");
        stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "generic"));
        stack.Cancel();
        CHECK_FALSE(stack.CanUndo());
    }
}

TEST_CASE("CommandStack::Clear discards generics pushed into an open gesture", "[edit]")
{
    // Review M9 regression: Begin -> Push -> Clear used to leave the pushed
    // command queued in m_pendingGeneric; the NEXT unrelated commit silently
    // spliced it in and undoing that transaction replayed a pre-Clear edit.
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; });
    int undos = 0, redos = 0;

    stack.Begin("doomed");
    stack.Push(std::make_unique<CountingCommand>(&undos, &redos, "leaky"));
    stack.Clear();   // e.g. project switch mid-gesture

    stack.Begin("fresh");
    stack.SnapshotComponent(e, desc);
    reg->GetComponent<Arcane::LocalTransform>(e)->position.x = 2.0f;
    stack.Commit();

    stack.Undo();
    CHECK(undos == 0);   // the cleared generic must NOT replay
    CHECK(reg->GetComponent<Arcane::LocalTransform>(e)->position.x == 0.0f);
}

TEST_CASE("CommandStack: depth cap drops the oldest", "[edit]")
{
    auto reg = MakeReg();
    const Astra::Entity e = reg->CreateEntity();
    reg->AddComponent<Arcane::LocalTransform>(e, Arcane::LocalTransform{});
    const Astra::ComponentDescriptor* desc = DescriptorFor(*reg, e, "Arcane::LocalTransform");

    Arcane::CommandStack stack([&reg]() -> Astra::Registry& { return *reg; }, /*maxDepth*/ 2);
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
