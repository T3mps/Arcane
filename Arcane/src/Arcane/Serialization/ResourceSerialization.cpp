#include <Arcane/Serialization/ResourceSerialization.hpp>

#include <Arcane/Scene/SceneResources.hpp>   // SceneRoot

#include <Astra/Core/TypeID.hpp>
#include <Astra/Entity/Entity.hpp>

#include <cstdint>

// SerializableResources() lives in Arcane.dll so BOTH the engine (Runtime's
// snapshot path) and any consumer (a test/host) resolve the SAME instance
// through the exported accessor, and the default SceneRoot codec is instantiated
// against this module's shared TypeContext.

namespace Arcane::Serialization
{
    namespace
    {
        // SceneRoot is a plain Astra::Entity id. Entity ids survive Registry::
        // Save/Load intact (EntityManager serializes exact (id,version) pairs), so
        // persisting the raw handle is sufficient -- after restore it resolves to
        // the same entity + components in the loaded registry.
        bool SaveSceneRoot(const Astra::Registry& reg, Astra::BinaryWriter& w)
        {
            const SceneRoot* root = reg.GetResource<SceneRoot>();
            if (!root) return false;   // absent -> skip
            const uint64_t raw = static_cast<uint64_t>(root->entity.GetValue());
            w(raw);
            return true;
        }

        bool LoadSceneRoot(Astra::Registry& reg, Astra::BinaryReader& r)
        {
            uint64_t raw = 0;
            r(raw);
            if (r.HasError()) return false;

            const Astra::Entity entity(static_cast<Astra::Entity::StorageType>(raw));
            if (!reg.IsValid(entity))
                return false;   // corrupt/dangling entity id -- do not install an invalid SceneRoot

            reg.SetResource<SceneRoot>(SceneRoot{ entity });
            return true;
        }
    }

    ResourceSerializerRegistry& SerializableResources()
    {
        // Function-local static: initialized on first access (runtime, after the
        // host installed the shared TypeContext), never at static-init time.
        static ResourceSerializerRegistry s_registry = []
        {
            ResourceSerializerRegistry r;
            r.Register(ResourceCodec{
                Astra::TypeID<SceneRoot>::Hash(), &SaveSceneRoot, &LoadSceneRoot });
            return r;
        }();
        return s_registry;
    }
}
