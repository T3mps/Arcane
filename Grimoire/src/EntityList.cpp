#include "EntityList.hpp"

#include <Astra/Registry/Registry.hpp>

namespace Grimoire
{
    std::vector<Astra::Entity> CollectEntities(Astra::Registry& registry)
    {
        std::vector<Astra::Entity> out;
        out.reserve(registry.Size());
        for (Astra::Entity e : registry.GetEntityManager())
            out.push_back(e);
        return out;
    }
}
