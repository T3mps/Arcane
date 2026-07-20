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
