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
