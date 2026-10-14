#include <Arcane/Scene/EngineResourceTypes.hpp>

#include <Arcane/Scene/PhysicsSystem.hpp>       // Arcane::PhysicsWorld2D
#include <Arcane/Scene/SceneResources.hpp>      // SceneRoot / Arcane::PhysicsInterpBuffer2D / the four tables
#include <Arcane/Scene/TransformSystems.hpp>    // TransformOrder

#include <Astra/Core/TypeID.hpp>

namespace Arcane
{
    void PrewarmEngineResourceTypes()
    {
        // Order is not load-bearing (resource ids are not serialized anywhere and
        // nothing indexes by them); being FIRST is. Grouped as declared.
        (void)Astra::TypeID<SceneRoot>::Value();
        (void)Astra::TypeID<Arcane::PhysicsInterpBuffer2D>::Value();
        (void)Astra::TypeID<SpriteTable>::Value();
        (void)Astra::TypeID<SpriteMaterialTable>::Value();
        (void)Astra::TypeID<MeshTable>::Value();
        (void)Astra::TypeID<MeshMaterialTable>::Value();
        (void)Astra::TypeID<Arcane::PhysicsWorld2D>::Value();
        (void)Astra::TypeID<TransformOrder>::Value();
    }
}
